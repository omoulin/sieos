/*
 * task.c - Processes, threads, the scheduler, sleeping, and starting a
 * program from an ELF file.
 *
 * A process is an address space and an identity (uid, gid); its threads
 * ("tasks") are what the scheduler runs. Threads and processes share one
 * numbering: a process's pid is the id of its first thread. Ids are never
 * reused; hash tables find a thread or a process from its id.
 *
 * Scheduling is round-robin: the ready threads wait in one queue shared by
 * all CPUs. A thread runs until it waits (for a message, a reply, a
 * timer...), or until its time slice ends while others wait for a CPU (the
 * timer is only set then: timer_update below). A waiting thread is in no
 * queue and costs no CPU time. A CPU with nothing to run sleeps; when a
 * thread becomes ready, a sleeping CPU is woken (arch_kick: on x86-64, an
 * inter-processor interrupt).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"

proc_t *all_procs;                   /* every process, in pid order */
static proc_t *all_tail;
int nprocs, nthreads;
proc_t *super;                       /* the supervisor: init, the first boot module */
static task_t *rq_head, *rq_tail;    /* the run queue */
static task_t *sleepq;               /* sleeping threads, the earliest wake-up first */
#define NTID 512                     /* hash buckets: a few entries each even with thousands of threads */
#define NPID 256
static task_t *tid_hash[NTID];
static proc_t *pid_hash[NPID];
static int next_id = 1;

task_t *task_find(int tid)
{
    for (task_t *t = tid_hash[tid % NTID]; t; t = t->hnext)
        if (t->tid == tid) return t;
    return 0;
}

proc_t *proc_find(int pid)
{
    for (proc_t *p = pid_hash[pid % NPID]; p; p = p->hnext)
        if (p->pid == pid) return p;
    return 0;
}

int runq_empty(void) { return !rq_head; }

/* Make t runnable. If this CPU is idle and t is alone in the queue, its
 * idle loop will run it; otherwise wake a sleeping CPU, if any. (If all are busy, this CPU's
 * timer, set on the way out of the kernel, ends its slice: t waits at most
 * one slice.) */
void ready(task_t *t)
{
    t->state = TS_READY;
    t->next = 0;
    if (rq_tail) rq_tail->next = t; else rq_head = t;
    rq_tail = t;
    cpu_t *me = this_cpu();
    if (!me->running && rq_head == t) return;   /* we are idle: our idle loop takes it */
    for (int i = 0; i < ncpu; i++)
        if (cpus[i]->idle && cpus[i] != me) {
            cpus[i]->idle = 0;
            arch_kick(cpus[i]);
            return;
        }
}

/* Make t the very next thread to run on this CPU, waking no other CPU:
 * for IPC, when the caller is about to wait (so t gets this CPU at once,
 * without an inter-processor interrupt and its delay). t continues the
 * caller's time slice rather than starting a new one: a client and a server
 * passing the CPU back and forth cannot keep the other ready threads waiting
 * beyond one slice. */
void ready_first(task_t *t)
{
    this_cpu()->handoff = t;
    t->state = TS_READY;
    t->next = rq_head;
    rq_head = t;
    if (!rq_tail) rq_tail = t;
}

/* Give this CPU to the next ready thread (or to its idle loop). The caller
 * holds the kernel lock and has already put cur to sleep or back in the
 * queue. Nothing here may use `c` after switch_to: when this thread runs
 * again, it may be on another CPU.
 * A CPU holds a process's address space only while it runs one of its
 * threads (idle runs on the kernel's): tlb_shootdown relies on it. */
void schedule(void)
{
    cpu_t *c = this_cpu();
    task_t *prev = c->running, *next = rq_head;
    if (next && !(rq_head = next->next)) rq_tail = 0;
    if (next && next != c->handoff) c->slice_end = ticks() + ns_to_ticks(SLICE_NS);
    c->handoff = 0;
    if (next == prev) { prev->state = TS_RUN; return; }
    c->running = next;
    uint64_t *save = prev ? &prev->ksp : &c->idle_ksp;
    if (!next) {
        fpu_switch(c, prev, 0);
        if (c->cur_as != kernel_as) arch_as_load(c->cur_as = kernel_as);
        switch_to(save, c->idle_ksp);
        return;
    }
    fpu_switch(c, prev, next);           /* the floating-point registers: prev's out, next's in */
    next->state = TS_RUN;
    next->cpu = c->id;
    arch_tls_switch(&c->a, next->fs_base);   /* its thread-local storage */
    if (next->proc->as != c->cur_as) arch_as_load(c->cur_as = next->proc->as);
    arch_set_kstack(c, next->kstack + KSTACK, next->proc->driver);
    switch_to(save, next->ksp);
}

void preempt(void) { if (cur && rq_head) { ready(cur); schedule(); } }

/* Free the stack of a thread that exited on this CPU. It cannot be freed
 * in thread_exit, which still runs on it; it is done once we have left it. */
void reap(void)
{
    cpu_t *c = this_cpu();
    if (c->dead) { page_free(V2P(c->dead), KSTACK / PAGE); c->dead = 0; }
}

/* Leaving the kernel (entry.S): a thread of an ending process ends here;
 * otherwise tidy up, set this CPU's timer, release the kernel lock. */
void kernel_exit(void)
{
    if (cur && cur->proc->exiting) thread_exit();
    reap();
    timer_update(this_cpu());
    kunlock();
}

/* Decide when this CPU's timer must fire next (called on the way out of
 * the kernel, and by idle before halting): at the end of the running
 * thread's slice if others wait for a processor; on CPU 0, at the next
 * sleeper's wake-up (one CPU keeps the sleepers, so idle CPUs need none).
 * Tickless: with nothing to wait for, no timer at all. */
void timer_update(cpu_t *c)
{
    uint64_t d = NEVER;
    if (c->running && !runq_empty()) d = c->slice_end;
    if (c->id == 0 && sleepq && sleepq->wake < d) d = sleepq->wake;
    if (d != c->armed) { arch_timer_set(d); c->armed = d; }
}

/* An interrupt of the timer (timer = 1) or a "reschedule" request reached
 * this CPU from user mode or idle: wake the sleepers whose time has come
 * (CPU 0), and end the running thread's slice if it is over and others wait. */
void core_tick(cpu_t *c, int timer)
{
    if (timer) {
        c->armed = NEVER;                /* it fired; kernel_exit sets the next one */
        if (c->id == 0) wake_sleepers();
    }
    if (c->running && !runq_empty() && ticks() >= c->slice_end) preempt();   /* its time is up */
}

/* Each CPU's idle loop, entered with the kernel lock held: run threads, or
 * sleep. The lock is released while sleeping, so other CPUs can work.
 * arch_wait_irq cannot miss a wake-up (on x86-64, "sti; hlt": sti takes
 * effect after the next instruction, so an interrupt arriving in between
 * still wakes the hlt). */
void idle(void)
{
    for (;;) {
        reap();
        if (rq_head) { schedule(); continue; }
        cpu_t *c = this_cpu();
        c->idle = 1;
        timer_update(c);
        kunlock();
        arch_wait_irq();
        klock();
        c->idle = 0;
    }
}

/* ---- Sleeping. Only CPU 0 sets its timer for the sleepers (timer_update);
 * a sleeper that becomes the earliest is announced to it (arch_kick). */

long sleep_ns(uint64_t ns)
{
    task_t *t = cur, **q = &sleepq;
    if (t->woken) { t->woken = 0; return 1; }               /* woken before it slept */
    if (!ns) { ready(t); schedule(); return 0; }
    t->wake = ticks() + ns_to_ticks(ns);
    while (*q && (*q)->wake <= t->wake) q = &(*q)->next;
    t->next = *q;
    *q = t;
    t->state = TS_SLEEP;
    if (sleepq == t && this_cpu()->id != 0) arch_kick(cpus[0]);
    schedule();
    return t->ret;
}

/* SYS_WAKE: end the sleep of another thread of this process early (its
 * sleep returns 1); if it is not asleep yet, its next sleep ends at once,
 * so a wake-up is never lost. */
long sleep_wake(int tid)
{
    task_t *t = task_find(tid);
    if (!t || t->proc != cur->proc) return -ESRCH;
    if (t->state != TS_SLEEP) { t->woken = 1; return 0; }
    for (task_t **q = &sleepq; *q; q = &(*q)->next)
        if (*q == t) { *q = t->next; break; }
    t->ret = 1;
    ready(t);
    return 0;
}

void wake_sleepers(void)
{
    uint64_t now = ticks();
    while (sleepq && sleepq->wake <= now) {
        task_t *t = sleepq;
        sleepq = t->next;
        t->ret = 0;
        ready(t);
    }
}

/* ---- Threads. */
static void list_add(proc_t *p)
{
    p->hnext = pid_hash[p->pid % NPID];
    pid_hash[p->pid % NPID] = p;
    p->aprev = all_tail;
    if (all_tail) all_tail->anext = p; else all_procs = p;
    all_tail = p;
    nprocs++;
}

static void proc_free(proc_t *p)
{
    for (proc_t **h = &pid_hash[p->pid % NPID]; *h; h = &(*h)->hnext)
        if (*h == p) { *h = p->hnext; break; }
    if (p->aprev) p->aprev->anext = p->anext; else all_procs = p->anext;
    if (p->anext) p->anext->aprev = p->aprev; else all_tail = p->aprev;
    nprocs--;
    kfree(p);
}

#ifdef STACKCHECK
#define PAINT 0x5AFE5AFE5AFE5AFEUL
static uint64_t deepest;                 /* bytes, over all threads that ended */
void stack_paint(void *lo, void *hi) { for (uint64_t *w = lo; w < (uint64_t *)hi; w++) *w = PAINT; }
static uint64_t stack_used(void *lo, void *hi)
{
    uint64_t *w = lo;
    while (w < (uint64_t *)hi && *w == PAINT) w++;
    return (char *)hi - (char *)w;
}
static void stack_note(task_t *t) { uint64_t u = stack_used(t + 1, (char *)t + KSTACK); if (u > deepest) deepest = u; }
void stack_report(void)
{
    uint64_t idle = 0;
    for (proc_t *p = all_procs; p; p = p->anext)
        for (task_t *t = p->threads; t; t = t->tnext) stack_note(t);
    for (int i = 0; i < ncpu; i++) {
        uint64_t u = stack_used(cpus[i]->stack_lo, cpus[i]->stack_hi);
        if (u > idle) idle = u;
    }
    kprintf("mk: deepest stack use: threads %lu bytes (of %lu), idle loops %lu bytes\n",
            deepest, KSTACK - sizeof(task_t), idle);
}
#endif

_Static_assert(sizeof(task_t) <= 256, "task_t shares the kernel stack's page");

static task_t *thread_new(proc_t *p)
{
    uint64_t ks;
    if (nthreads >= MAXTASK || !(ks = page_alloc(KSTACK / PAGE))) return 0;
    task_t *t = P2V(ks);
#ifdef STACKCHECK
    stack_paint(t + 1, (char *)t + KSTACK);
#endif
    t->kstack = (uint64_t)t;
    t->proc = p;
    t->tid = next_id++;
    t->hnext = tid_hash[t->tid % NTID];
    tid_hash[t->tid % NTID] = t;
    t->tnext = p->threads;
    p->threads = t;
    p->nthreads++;
    nthreads++;
    return t;
}

/* A new thread: its first switch_to enters user mode at pc, with the
 * stack sp and arg as its first argument (arch_thread_start); then it may run. */
static void thread_start(task_t *t, uint64_t pc, uint64_t sp, uint64_t arg)
{
    arch_thread_start(t, pc, sp, arg);
    ready(t);
}

long thread_create(uint64_t entry, uint64_t stack, uint64_t arg)
{
    if (entry >= USER_TOP || stack >= USER_TOP || stack < PAGE) return -EINVAL;
    task_t *t = thread_new(cur->proc);
    if (!t) return -ENOMEM;
    thread_start(t, entry, (stack & ~15UL) - 8, arg);   /* -8: as if called (the ABI's alignment) */
    return t->tid;
}

/* Take t out of whatever it waits in, and make it run: it is part of an
 * ending process and will end itself on its way out of the kernel. */
static void unblock(task_t *t)
{
    if (t->state == TS_SLEEP)
        for (task_t **q = &sleepq; *q; q = &(*q)->next)
            if (*q == t) { *q = t->next; break; }
    if (t->state == TS_SEND || t->state == TS_RECV || t->state == TS_LOOKUP) ipc_unblock(t);
    t->ret = -EPIPE;
    ready(t);
}

/* The last thread of p is ending (on this CPU): free its memory; it stays
 * as a "zombie" (its exit status) until its parent collects it with
 * SYS_WAIT, unless there is no parent to do so. */
static void proc_end(proc_t *p)
{
    if (!p->exiting) { p->exiting = 1; ipc_exit(p); }
    cpu_t *c = this_cpu();
    arch_as_load(c->cur_as = kernel_as); /* leave its address space before freeing it */
    vm_free(p->as);
    p->as = 0;
    p->pages = 0;
    if (p == super) panic("init, the supervisor, ended (status %d)\n", p->status);
    /* Its children are adopted by the supervisor, which then learns when
     * they end (a session's shell whose login program died, for example). */
    int adopted = 0;
    for (proc_t *q = all_procs, *n; q; q = n) {
        n = q->anext;
        if (q->ppid != p->pid) continue;
        if (super && !super->exiting) { q->ppid = super->pid; adopted |= q->zombie; }
        else if (q->zombie) proc_free(q);
        else q->ppid = 0;
    }
    if (adopted && super->child_port) port_notify(super->child_port, super, NOTE_CHILD);
    proc_t *parent = p->ppid ? proc_find(p->ppid) : 0;
    if (!parent || parent->exiting) { proc_free(p); return; }
    p->zombie = 1;
    for (task_t *w = parent->threads; w; w = w->tnext)
        if (w->state == TS_WAIT && (w->wpid == -1 || w->wpid == p->pid)) { w->ret = 0; ready(w); }
    if (parent->child_port) port_notify(parent->child_port, parent, NOTE_CHILD);
}

void thread_exit(void)
{
    task_t *t = cur;
    proc_t *p = t->proc;
    for (task_t **q = &p->threads; *q; q = &(*q)->tnext)
        if (*q == t) { *q = t->tnext; break; }
    for (task_t **h = &tid_hash[t->tid % NTID]; *h; h = &(*h)->hnext)
        if (*h == t) { *h = t->hnext; break; }
    p->nthreads--;
    nthreads--;
    fpu_free(t);                         /* (its registers are not saved: it is gone) */
#ifdef STACKCHECK
    stack_note(t);
#endif
    if (!p->nthreads) proc_end(p);
    t->state = TS_DEAD;
    reap();                              /* (a previous one, if any) */
    this_cpu()->dead = t;                /* its stack goes once we are off it */
    schedule();
    __builtin_unreachable();
}

/* Make process p end with status code: all its threads. Those waiting are
 * woken, those running on other CPUs interrupted; each then ends itself in
 * kernel_exit (the last one frees the process). */
static void proc_stop(proc_t *p, int code)
{
    if (p->exiting) return;
    if (code) kprintf("mk: %s (pid %d) %s %d\n", p->name, p->pid, code == EXIT_KILLED ? "killed, status" : "exited with", code);
    p->exiting = 1;
    p->status = code;
    ipc_exit(p);                         /* its ports, and whoever waits on it */
    for (task_t *t = p->threads; t; t = t->tnext) {
        if (t == cur || t->state == TS_READY) continue;
        if (t->state == TS_RUN) arch_kick(cpus[t->cpu]);
        else unblock(t);
    }
}

void proc_exit(int code)
{
    proc_stop(cur->proc, code);
    thread_exit();
}

/* SYS_KILL: root may end any process but the supervisor; others only their own. */
long proc_kill(int pid)
{
    proc_t *p = proc_find(pid), *me = cur->proc;
    if (!p || p->zombie || p->exiting) return -ESRCH;
    if (p == super || (me->uid && me->uid != p->uid)) return -EPERM;
    if (p == me) proc_exit(EXIT_KILLED);
    proc_stop(p, EXIT_KILLED);
    return 0;
}

/* SYS_WAIT: collect an ended child (pid, or any if -1). */
long proc_wait(int pid, int *ustatus, int flags)
{
    proc_t *me = cur->proc;
    for (;;) {
        int found = 0;
        for (proc_t *q = all_procs; q; q = q->anext) {
            if (q->ppid != me->pid || (pid > 0 && q->pid != pid)) continue;
            found = 1;
            if (!q->zombie) continue;
            int st = q->status, id = q->pid;
            proc_free(q);
            if (ustatus && vm_copy(me->as, ustatus, 0, &st, sizeof st)) return -EFAULT;
            return id;
        }
        if (!found) return -ECHILD;
        if (flags & WAIT_NOHANG) return 0;
        cur->wpid = pid;
        cur->state = TS_WAIT;
        schedule();
        if (cur->ret < 0) return cur->ret;
    }
}

/* ---- Starting a program. An ELF executable lists "segments": pieces of
 * the file to place at given addresses, with their rights (read, write,
 * execute). We copy each into fresh pages of a new address space and add a
 * stack. args ("name arg1 arg2") is put on its stack, and its address given
 * to the program as its first argument. The file is read from address space src_as (0:
 * the kernel's, for the boot modules), through vm_copy: a bad file or
 * pointer from a program gives an error, never a crash. */
typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version;
                 uint64_t entry, phoff, shoff; uint32_t flags;
                 uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } elf_hdr;
typedef struct { uint32_t type, flags; uint64_t off, vaddr, paddr, filesz, memsz, align; } elf_ph;
#define STACK_PAGES 8                    /* 32 KiB */

int proc_spawn(uint64_t src_as, const uint8_t *img, uint64_t len, const char *args, int uid, int gid,
               const mk_groups_t *groups, int driver, uint64_t quota)
{
    elf_hdr h;
    if (len < sizeof h || vm_copy(0, &h, src_as, img, sizeof h)) return -EFAULT;
    if (memcmp(h.ident, "\177ELF\2", 5) || h.machine != ELF_MACHINE || h.type != 2 || h.phnum > 32 ||
        h.phoff + h.phnum * sizeof(elf_ph) > len)
        return -EINVAL;                  /* not a 64-bit executable for this processor */
    uint64_t n = strlen(args) + 1;
    if (n > 256) return -EINVAL;
    quarantine_release();                /* dead drivers' DMA buffers, once safe */
    proc_t *p = kzalloc(sizeof *p);
    if (!p) return -ENOMEM;
    p->quota = quota;
    long err = -ENOMEM;
    if (!(p->as = vm_new())) goto fail;

    uint64_t end = 0;
    for (int i = 0; i < h.phnum; i++) {
        elf_ph ph;
        if (vm_copy(0, &ph, src_as, img + h.phoff + i * sizeof ph, sizeof ph)) { err = -EFAULT; goto fail; }
        if (ph.type != 1) continue;      /* PT_LOAD */
        if (ph.off + ph.filesz > len || ph.filesz > ph.memsz || ph.vaddr + ph.memsz > MMIO_BASE) { err = -EINVAL; goto fail; }
        uint64_t fl = VM_U | (ph.flags & 2 ? VM_W : 0) | (ph.flags & 1 ? 0 : VM_NX);
        for (uint64_t va = ph.vaddr & ~(PAGE - 1); va < ph.vaddr + ph.memsz; va += PAGE) {
            uint64_t pa = (!quota || p->pages < quota) ? page_alloc(1) : 0;
            if (!pa) goto fail;
            if (vm_map(p->as, va, pa, fl)) { page_free(pa, 1); err = -EINVAL; goto fail; }
            p->pages++;
        }
        /* the file's bytes; the rest stays zero (.bss) */
        if (vm_load(p->as, (void *)ph.vaddr, src_as, img + ph.off, ph.filesz)) { err = -EFAULT; goto fail; }
        if (ph.vaddr + ph.memsz > end) end = ph.vaddr + ph.memsz;
    }
    for (uint64_t va = STACK_TOP - STACK_PAGES * PAGE; va < STACK_TOP; va += PAGE) {
        uint64_t pa = (!quota || p->pages < quota) ? page_alloc(1) : 0;
        if (!pa) goto fail;
        if (vm_map(p->as, va, pa, VM_U | VM_W | VM_NX)) { page_free(pa, 1); goto fail; }
        p->pages++;
    }
    uint64_t sp = (STACK_TOP - n) & ~15UL;
    vm_copy(p->as, (void *)sp, 0, args, n);

    task_t *t = thread_new(p);
    if (!t) goto fail;
    p->pid = t->tid;
    p->ppid = cur ? cur->proc->pid : 0;
    p->uid = uid; p->gid = gid; p->driver = driver;
    if (groups) p->groups = *groups;
    p->brk_base = p->brk = (end + PAGE - 1) & ~(PAGE - 1);
    p->mmio = MMIO_BASE;
    for (int i = 0; i < 15 && args[i] && args[i] != ' '; i++) p->name[i] = args[i];
    list_add(p);
    thread_start(t, h.entry, sp, sp);
    return p->pid;
fail:
    if (p->as) vm_free(p->as);
    kfree(p);
    return err;
}
