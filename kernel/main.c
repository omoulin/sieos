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
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"

module_t modules[MAXMOD];
int nmodules;
cpu_t *cpus[MAXCPU];
int ncpu;

/* ---- The kernel lock. On several processors, two CPUs must never change
 * the kernel's data at the same time. SIEOS does what seL4 does: one lock
 * for the whole kernel, taken on every entry (system call, interrupt) and
 * released on the way out. It sounds coarse, but a microkernel spends very
 * little time in the kernel: a system call is a few hundred instructions.
 * The servers and programs, where the real work happens, run in user mode
 * and truly in parallel, one per CPU. While it waits, a CPU still does
 * what its architecture needs (arch_lock_wait: on x86-64, answering TLB
 * shootdowns, which the holder may be waiting for). */
static spinlock_t big_lock;
void klock(void)
{
    cpu_t *c = this_cpu();
    uint32_t t = __atomic_fetch_add(&big_lock.next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&big_lock.owner, __ATOMIC_ACQUIRE) != t) arch_lock_wait(c);
}
void kunlock(void) { spin_unlock(&big_lock); }

void kernel_main(boot_info_t *bi)
{
    kprintf("\nSIEOS microkernel\n");
    mem_init(bi);
    cpu_init();
    rand_init();
    smp_init();
    klock();                     /* from now on, the kernel lock */

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
    int pid = proc_spawn(0, P2V(modules[0].pa), modules[0].len, modules[0].name, 0, 0, 0, 0, 0);
    if (pid < 0) panic("cannot start %s (error %d)\n", modules[0].name, pid);
    super = proc_find(pid);
    super->super = 1;
    kprintf("mk: started %s (pid %d), the supervisor; %d more boot modules\n", modules[0].name, pid, nmodules - 1);
    mem_boot_done();             /* the boot loader's information is read: free it */
    smp_start();
    kprintf("mk: %lu MiB of memory free of %lu MiB\n", pages_free / 256, pages_total / 256);
    idle();
}
