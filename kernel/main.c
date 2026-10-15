/*
 * main.c - Where the portable kernel starts: the architecture's boot code
 * (arch/NAME/) finds the memory and the boot modules, fills a boot_info_t
 * and calls kernel_main.
 *
 * The kernel does little by itself: it prepares the memory and the
 * processors, starts the first boot module (init, the supervisor, which
 * starts everything else), starts the other processors, and from then on
 * only runs the processes, on every processor.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"

module_t modules[MAXMOD];
int nmodules;
cpu_t *cpus[MAXCPU];
int ncpu;
int opt_pcores;

/* ---- Locks. Every CPU may be in the kernel at once: each piece of shared
 * data has its own lock (spinlock_t, kernel.h), held for a few hundred
 * instructions at most and never while copying a message. A message
 * round trip takes three uncontended locks (the port's bucket twice, the
 * client's in the thread table once to reply) and no full barrier: the
 * IPC fast path stays light. A CPU holding a lock may take one further
 * down this list, never one above it (so no two CPUs can wait for each
 * other):
 *
 *   proc_lock         the process table: pids, parents and children (task.c)
 *   p->lock           a process: its threads, its ports, its heap, exiting
 *   names_lock        port names, threads waiting in port_lookup (ipc.c)
 *   irq_lock          which ports each interrupt line goes to (ipc.c)
 *   port bucket       a bucket of the port table: its ports and their queues
 *   tid bucket        a bucket of the thread table (task.c)
 *   sleep_lock        the sleeping threads (task.c)
 *   rq_lock           a CPU's run queue (one at a time)
 *   quarantine        dead drivers' DMA pages (mem.c)
 *   slab_lock         small kernel objects (mem.c)
 *   page_lock         the physical page bitmap (mem.c)
 *   rand, log, print  the random pool, the log, the serial port
 *   (arch)            x86-64: the I/O APICs; PCI configuration
 *
 * Threads move between states without a lock of their own: a waiting
 * thread is woken by whoever takes it off the queue it waits in (under that
 * queue's lock), or, waiting for a reply, by whoever sets its `server` to
 * 0 (under its tid bucket's lock): one waker, never two. A woken thread
 * may still be on its old CPU's stack for a moment: on_cpu says so, and
 * ready() waits for it (no lock is needed to finish a switch, so that wait
 * is short and cannot deadlock). Ending a process races with its threads
 * going to sleep: each sleeper checks `exiting` once queued; the killer
 * sets it, waits until every other CPU has answered a shootdown, then
 * looks at each thread (proc_stop): one of them always sees the other,
 * with no barrier on the message path. Memory is freed only once no CPU
 * can use it: user pages after a shootdown that every CPU answers
 * (tlb_shootdown), a thread's stack once its CPU has left it.
 *
 * Ordering on AArch64: locks are acquire/release (ldaxr/stlr); a flag
 * another CPU acts on is published with a release store or an atomic, and
 * read with an acquire load; store_mb/smp_mb (a full barrier, dmb ish)
 * where a store must be seen before a later load (claiming idle CPUs,
 * timers). */
void spin_wait(spinlock_t *l, uint32_t t)
{
    cpu_t *c = this_cpu();
    while (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) != t) arch_lock_wait(c);
}

/* The boot options, on the kernel's command line (x86-64: multiboot's;
 * arm64: the device tree's /chosen bootargs). One so far:
 *   pcores=N   pretend that CPUs 0..N-1 are P-cores and the others E-cores
 *              (to test the scheduler's placement where all cores are alike) */
static void options(const char *s)
{
    while (s && *s) {                    /* s: at the start of a word */
        int n = 0;
        while (s[n] && s[n] != ' ') n++;
        if (n > 7 && !memcmp(s, "pcores=", 7))
            for (int i = 7; i < n && s[i] >= '0' && s[i] <= '9'; i++) opt_pcores = opt_pcores * 10 + s[i] - '0';
        for (s += n; *s == ' '; s++) ;
    }
}

void kernel_main(boot_info_t *bi)
{
    kprintf("\nSIEOS microkernel\n");
    options(bi->cmdline);
    mem_init(bi);
    cpu_init();
    rand_init();
    smp_init();

    /* The boot modules: "path/name.elf [arguments]" each. The first is init,
     * the supervisor: the kernel starts it alone; init starts the others
     * (SPAWN_MODULE) and starts them again if they fail, so their images
     * stay in memory (where the boot loader put them) for the whole run. */
    for (int i = 0; i < bi->nmod && i < MAXMOD; i++) {
        const char *s = bi->mod[i].cmdline, *w = s;
        char *name = modules[i].name;
        int n = 0;
        for (; *s && *s != ' '; s++) if (*s == '/') w = s + 1;   /* w: the file name */
        for (; w < s && *w != '.' && n < 15; w++) name[n++] = *w; /* without ".elf" */
        modules[i].pa = bi->mod[i].pa;
        modules[i].len = bi->mod[i].len;
        nmodules++;
    }
    if (!nmodules) panic("no boot modules: nothing to run\n");
    long err;
    proc_t *p = proc_new(0, P2V(modules[0].pa), modules[0].len, modules[0].name, 0, 0, 0, 0, 0, &err);
    if (!p) panic("cannot start %s (error %ld)\n", modules[0].name, err);
    super = p;
    p->super = 1;
    int pid = proc_start(p, 0);
    kprintf("mk: started %s (pid %d), the supervisor; %d more boot modules\n", modules[0].name, pid, nmodules - 1);
    mem_boot_done();             /* the boot loader's information is read: free it */
    smp_start();
    sched_cpus();
    kprintf("mk: %lu MiB of memory free of %lu MiB\n", pages_free / 256, pages_total / 256);
    idle();
}
