/*
 * disp.c - The dispatcher: switching between LWPs, the idle loop, kernel
 * threads (the clock's, the device interrupts', the network's, fsflush).
 *
 * The dispatcher's lock (disp_enter) protects every LWP's state and the
 * choice of the next LWP to run.  A processor switching away from an LWP
 * holds it across switch_context; the LWP switched to releases it
 * (switch_finish, forkret, kthread_start).  An LWP stays `oncpu` until the
 * switch away from it is complete: another processor never runs it before,
 * even when a wake-up made it runnable meanwhile (it then keeps its
 * processor), and its stack is not freed before.
 *
 * Kernel threads are LWPs of p0 (proc_table[0], "sched") that run kernel
 * code only, in the SYS class above time-sharing.  Device interrupts run in
 * the interrupt thread: the trap masks the line, acknowledges it and posts
 * it; the thread runs the handlers and unmasks it.  The clock interrupt
 * keeps the time, wakes the LWPs whose deadline passed and accounts the
 * processors' time; the rest of a tick (interval timers, the network's and
 * the drivers' polls, the load average) runs in the clock thread.  Until
 * the boot is over (kernel_running) all of it runs in the interrupts.
 * The fsflush thread commits old ext4 transactions and reads the disks that
 * came after the boot, once a second.
 *
 * There is no big kernel lock: every subsystem has its own locks (sync.h;
 * their order: docs/locking.md).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "proc.h"
#include "smp.h"
#include "mm.h"
#include "arch.h"
#include "net.h"
#include "ddi.h"
#include "power.h"
#include "jbd2.h"
#include "blkdev.h"
#include "sieos/priocntl.h"

#define UNBLOCKABLE (KSIGBIT(SIGKILL) | KSIGBIT(SIGSTOP))

static struct spinlock disp_lk;
struct proc *p0 = &proc_table[0];

void disp_enter(void)
{
    spin_lock(&disp_lk);
}

void disp_exit(void)
{
    spin_unlock(&disp_lk);
}

/* ------------------------------------------------------------------ */
/* Switching                                                           */
/* ------------------------------------------------------------------ */

static bool may_run_on(struct lwp *l, struct cpu *c)
{
    return (!l->bound || l->bound == c->id + 1) && (!l->affinity || (l->affinity & (1ULL << c->id)));
}

/* An idle processor that may run l (the one it last ran on first): told to look. */
static void kick_idle_for(struct lwp *l)
{
    if (!lapic_ok || ncpu < 2)
        return;
    struct cpu *me = mycpu();
    if (l->cpu >= 0 && l->cpu < ncpu) {
        struct cpu *c = &cpus[l->cpu];
        if (c != me && c->online && !c->offline && c->lwp == c->idle && may_run_on(l, c)) {
            smp_resched(c);
            return;
        }
    }
    for (int i = 0; i < ncpu; i++) {
        struct cpu *c = &cpus[i];
        if (c != me && c->online && !c->offline && c->lwp == c->idle && may_run_on(l, c)) {
            smp_resched(c);
            return;
        }
    }
}

/*
 * With the dispatcher's lock held, and the calling LWP's state set (running
 * to keep competing, or sleeping, stopped...): run the runnable LWP of
 * highest priority, round robin among equals.  Returns when the caller runs
 * again, the lock released: true if another LWP ran meanwhile.
 */
bool swtch(void)
{
    struct cpu *c = mycpu();
    struct lwp *cur = c->lwp, *next = NULL;
    int best = -1;
    c->need_resched = false;
    if (cur->state == LWP_RUNNABLE)
        cur->state = LWP_RUNNING;               /* woken before it could switch away */
    for (int i = 1; i <= NLWP && !c->offline; i++) {   /* highest priority first, round robin among equals */
        int idx = (c->rr + i) % NLWP;
        struct lwp *l = &lwp_table[idx];
        if (l->state == LWP_RUNNABLE && !l->oncpu && may_run_on(l, c)) {
            int pri = sched_gpri(l);
            if (pri > best) {
                best = pri;
                next = l;
            }
        }
    }
    bool cur_ok = cur->state == LWP_RUNNING && !cur->is_idle && !c->offline && may_run_on(cur, c);
    if (cur_ok && (!next || sched_gpri(cur) > best || (sched_gpri(cur) == best && !c->slice_expired))) {
        c->slice_expired = false;
        disp_exit();                            /* it keeps the CPU: nobody ranks higher */
        return false;
    }
    c->slice_expired = false;
    if (next)
        c->rr = next - lwp_table;
    else
        next = c->idle;
    if (next == cur) {
        disp_exit();
        return false;
    }
    if (!cur->is_idle) {
        if (cur->state == LWP_RUNNING)
            cur->nivcsw++;
        else
            cur->nvcsw++;
    }
    if (cur->state == LWP_RUNNING)
        cur->state = LWP_RUNNABLE;
    next->state = LWP_RUNNING;
    next->oncpu = 1;
    next->cpu = c->id;
    c->lwp = next;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* c->lwp before CR3: against tlb_shootdown's scan */
    if (!next->is_idle)
        tss_set_rsp0(next->kstack + KSTACK_SIZE);
    uint64_t cr3 = next->proc->pml4 ? next->proc->pml4 : kernel_pml4_phys;
    if (read_cr3() != cr3) {
        c->tlb_flush = 0;                       /* (loading CR3 flushes) */
        write_cr3(cr3);
    } else {
        tlb_service();
    }
    fpu_save(&cur->fpu);                        /* user FPU/SSE state follows the LWP */
    fpu_restore(&next->fpu);
    wrmsr(MSR_FS_BASE, next->fsbase);           /* its TLS pointer */
    c->slice_start = ticks;
    next->last_run = ticks;
    c->prev = cur;
    switch_context(&cur->ctx_rsp, next->ctx_rsp);
    /* Running again, perhaps on another processor. */
    switch_finish();
    return true;
}

/* In the LWP switched to: the previous one may run elsewhere now; the dispatcher's lock goes. */
void switch_finish(void)
{
    struct cpu *c = mycpu();
    struct lwp *prev = c->prev;
    c->prev = NULL;
    bool kick = prev && !prev->is_idle && prev->state == LWP_RUNNABLE;
    if (prev)
        __atomic_store_n(&prev->oncpu, 0, __ATOMIC_RELEASE);
    if (kick)
        kick_idle_for(prev);                    /* (a processor may have passed it over while it was oncpu) */
    disp_exit();
}

void preempt(void)
{
    disp_enter();
    swtch();
}

void lwp_wait_offcpu(struct lwp *l)
{
    while (__atomic_load_n(&l->oncpu, __ATOMIC_ACQUIRE))
        cpu_relax();
}

/* Per-CPU idle loop. */
void cpu_idle(void)
{
    for (;;) {
        disp_enter();
        if (swtch())
            continue;
        power_idle();                        /* HLT, or MWAIT into a deep C-state */
        cli();
    }
}

/* ------------------------------------------------------------------ */
/* Waking                                                              */
/* ------------------------------------------------------------------ */

void setrun_locked(struct lwp *l)
{
    if (l->state != LWP_SLEEPING)
        return;
    l->state = LWP_RUNNABLE;
    sched_woke(l);
    if (!l->oncpu)
        kick_idle_for(l);
}

/* An embryo, stopped or suspended LWP runs; a sleeping one wakes if its sleep is interruptible. */
void make_runnable(struct lwp *l)
{
    if (sleepq_unsleep(l, true, NULL))
        return;
    disp_enter();
    if (l->state == LWP_EMBRYO || l->state == LWP_STOPPED || l->state == LWP_SUSPENDED) {
        l->state = LWP_RUNNABLE;
        sched_woke(l);
        if (!l->oncpu)
            kick_idle_for(l);
    }
    disp_exit();
}

bool lwp_sig_pending(struct lwp *l)
{
    if (l->must_exit)
        return true;
    ksigset_t pend = l->sig_pending | l->proc->sig_pending;
    return (pend & ~(l->sig_blocked & ~UNBLOCKABLE)) || (pend & l->sig_waiting);
}

bool sleep_due(struct lwp *l)
{
    if (l->sleep_sig && lwp_sig_pending(l))
        return true;
    if (l->wake_tick && ticks >= l->wake_tick)
        return true;
    return l->wake_ns && hrtime() >= l->wake_ns;
}

void lwp_wake_sig(struct lwp *l)
{
    sleepq_unsleep(l, true, NULL);
}

static bool tick_due(struct lwp *l)
{
    return l->wake_tick && l->wake_tick <= ticks;
}

/* The timer interrupt: LWPs whose tick deadline passed. */
void clock_wake_sleepers(void)
{
    uint64_t now = ticks;
    for (int i = 0; i < NLWP; i++) {
        struct lwp *l = &lwp_table[i];
        if (l->wchan && l->wake_tick && l->wake_tick <= now)
            sleepq_unsleep(l, false, tick_due);
    }
}

static bool ns_due(struct lwp *l)
{
    return l->wake_ns && l->wake_ns <= hrtime();
}

/* Wake the high-resolution sleepers that are due; returns the earliest deadline left. */
uint64_t hr_wake(uint64_t now)
{
    uint64_t next = ~0UL;
    for (int i = 0; i < NLWP; i++) {
        struct lwp *l = &lwp_table[i];
        uint64_t w = l->wake_ns;
        if (!w || !l->wchan)
            continue;
        if (w <= now)
            sleepq_unsleep(l, false, ns_due);
        else if (w < next)
            next = w;
    }
    return next;
}

/* ------------------------------------------------------------------ */
/* Kernel threads                                                      */
/* ------------------------------------------------------------------ */

extern void kthread_start(void);

struct lwp *kthread_create(void (*fn)(void *), void *arg, const char *name, int pri)
{
    if (!p0->pid && p0->state != PSTATE_RUNNING) {
        p0->state = PSTATE_RUNNING;
        strlcpy(p0->name, "sched", sizeof(p0->name));
        p0->sid = p0->pgid = 0;
    }
    struct lwp *l = lwp_alloc(p0);
    if (!l)
        panic("kthread_create: no LWP for %s", name);
    l->kthread = true;
    l->kpri = pri;
    l->cid = SIEOS_CID_SYS;
    strlcpy(l->name, name, sizeof(l->name));
    /* a context that "returns" to kthread_start with r12 = fn, r13 = arg */
    uint64_t *sp = (uint64_t *)(l->kstack + KSTACK_SIZE - 64);
    *--sp = (uint64_t)kthread_start;
    *--sp = 0;                                   /* rbp */
    *--sp = 0;                                   /* rbx */
    *--sp = (uint64_t)fn;                        /* r12 */
    *--sp = (uint64_t)arg;                       /* r13 */
    *--sp = 0;                                   /* r14 */
    *--sp = 0;                                   /* r15 */
    l->ctx_rsp = (uint64_t)sp;
    l->tf = NULL;
    make_runnable(l);
    return l;
}

void kthread_exit(void)
{
    panic("kernel thread %s returned", curlwp->name);
}

/* The clock thread: the tick's work beyond the interrupt's. */
static kmutex_t clock_mx = MUTEX_SPIN_INITIALIZER;
static kcondvar_t clock_cv;
static uint64_t clock_pending;
static bool threads_up;

void clock_thread_kick(void)
{
    mutex_enter(&clock_mx);
    clock_pending++;
    cv_signal(&clock_cv);
    mutex_exit(&clock_mx);
}

static void clock_thread(void *arg)
{
    (void)arg;
    uint64_t last = ticks;
    mutex_enter(&clock_mx);
    for (;;) {
        while (!clock_pending)
            cv_wait(&clock_cv, &clock_mx);
        clock_pending = 0;
        mutex_exit(&clock_mx);
        uint64_t now = ticks, n = now - last;
        last = now;
        if (n)
            clock_tick(n);
        netisr_kick();                           /* the network's timers and polled cards */
        ddi_poll();                              /* the drivers' polled devices (USB, I2C HID, Wi-Fi) */
        power_tick();
        mutex_enter(&clock_mx);
    }
}

/* The interrupt thread: the device interrupts posted by the trap. */
static kmutex_t intr_mx = MUTEX_SPIN_INITIALIZER;
static kcondvar_t intr_cv;
static uint32_t intr_pending, intr_masked;

void intr_thread_post(int irq, bool masked)
{
    mutex_enter(&intr_mx);
    intr_pending |= 1u << irq;
    if (masked)
        intr_masked |= 1u << irq;
    cv_signal(&intr_cv);
    mutex_exit(&intr_mx);
}

static void intr_thread(void *arg)
{
    (void)arg;
    mutex_enter(&intr_mx);
    for (;;) {
        while (!intr_pending)
            cv_wait(&intr_cv, &intr_mx);
        uint32_t todo = intr_pending, masked = intr_masked;
        intr_pending = intr_masked = 0;
        mutex_exit(&intr_mx);
        for (int irq = 0; irq < 16; irq++)
            if (todo & (1u << irq)) {
                irq_dispatch(irq);
                if (masked & (1u << irq))
                    irq_unmask(irq);
            }
        mutex_enter(&intr_mx);
    }
}

/* fsflush: old ext4 transactions committed, disks that came after the boot read, once a second. */
static void fsflush(void *arg)
{
    (void)arg;
    static kcondvar_t never;
    static kmutex_t mx;
    for (;;) {
        mutex_enter(&mx);
        cv_timedwait(&never, &mx, ticks + TIMER_HZ);
        mutex_exit(&mx);
        if (fs_commit_deadline && ticks >= fs_commit_deadline)
            ext4_journal_tick(false);
        if (blk_late_pending)
            blk_scan_late();
    }
}

bool kthreads_running(void)
{
    return threads_up;
}

void kthreads_start(void)
{
    kthread_create(intr_thread, NULL, "intr", 165);
    kthread_create(clock_thread, NULL, "clock", 164);
    netisr_start();
    kthread_create(fsflush, NULL, "fsflush", 160);
    threads_up = true;
}
