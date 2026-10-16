/*
 * task.c - Processes, threads, the scheduler, sleeping, and starting a
 * program from an ELF file.
 *
 * A process is an address space and an identity (uid, gid); its threads
 * ("tasks") are what the scheduler runs. Threads and processes share one
 * numbering: a process's pid is the id of its first thread. Ids are never
 * reused; hash tables find a thread or a process from its id.
 *
 * Scheduling is round-robin, per CPU: each CPU has its own queue of ready
 * threads. A thread runs until it waits (for a message, a reply, a
 * timer...), or until its time slice ends while others wait for its CPU
 * (the timer is only set then: timer_update below). A waiting thread is in
 * no queue and costs no CPU time. A CPU with nothing to run takes a thread
 * from another CPU's queue, or sleeps; when a thread becomes ready, a
 * sleeping CPU is woken for it (arch_kick: an inter-processor interrupt).
 *
 * P-cores and E-cores (hybrid processors: Intel's since Alder Lake, ARM's
 * big.LITTLE): speed first. Threads are normal (interactive work, servers,
 * the assistant) or background (SYS_PLACE, SPAWN_BACKGROUND). Normal ones
 * go to P-cores, background ones to E-cores; a thread goes to the other
 * kind only when a CPU of that kind is idle (normal work overflowing to
 * E-cores, background work using an idle P-core). A normal thread that
 * finds every P-core busy with background work takes one of them (that
 * work moves away); an idle P-core takes over a normal thread running on
 * an E-core, an idle E-core background work running on a P-core. Where
 * all cores are alike, nothing of this applies.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"

spinlock_t proc_lock;                /* the process table (lock order: main.c) */
proc_t *all_procs;                   /* every process, in pid order */
static proc_t *all_tail;
int nprocs, nthreads;
proc_t *super;                       /* the supervisor: init, the first boot module */
int ecores;                          /* E-cores (0: all cores alike) */
static int hybrid;                   /* P-cores and E-cores: where a thread runs matters */
static spinlock_t sleep_lock;
static task_t *sleepq;               /* sleeping threads, the earliest wake-up first */
static volatile uint64_t sleep_next = NEVER;   /* (ticks) the first one's wake-up, for CPU 0's timer */
#define NTID 512                     /* hash buckets: a few entries each even with thousands of threads */
#define NPID 256
static struct { spinlock_t lock; task_t *head; } tid_hash[NTID];
static proc_t *pid_hash[NPID];       /* (under proc_lock) */
static int next_id = 1;
typedef struct sigctx { regs_t r; void *fpu; } sigctx_t;   /* in a signal's function (Signals, below) */

/* The thread with id tid, its bucket locked: it cannot end until
 * task_unlock. */
task_t *task_lock(int tid)
{
    if (tid <= 0) return 0;
    spin_lock(&tid_hash[tid % NTID].lock);
    for (task_t *t = tid_hash[tid % NTID].head; t; t = t->hnext)
        if (t->tid == tid) return t;
    spin_unlock(&tid_hash[tid % NTID].lock);
    return 0;
}
void task_unlock(task_t *t) { spin_unlock(&tid_hash[t->tid % NTID].lock); }

proc_t *proc_find(int pid)
{
    for (proc_t *p = pid_hash[pid % NPID]; p; p = p->hnext)
        if (p->pid == pid) return p;
    return 0;
}

/* t (the running thread) waits now, in state; its caller has just queued
 * it, under that queue's lock. If its process is ending it must not wait:
 * -EPIPE (the caller takes it off the queue again); nor if a signal it
 * catches came: -EINTR. No barrier is needed between the two (proc_stop,
 * sig_post): no CPU answers a shootdown between them. */
static int unblocked(task_t *t);
int blocked(task_t *t, int state)
{
    __atomic_store_n(&t->state, state, __ATOMIC_RELEASE);
    return may_stop(t) ? unblocked(t) : 0;
}

/* ---- The run queues. */
static void rq_add(cpu_t *c, task_t *t)   /* (under c->rq_lock) */
{
    t->next = 0;
    if (c->rq_tail) c->rq_tail->next = t; else c->rq_head = t;
    c->rq_tail = t;
    c->nready++;
}

/* An idle CPU (cpu_t.idle) polls its queue for a while, then sleeps.
 * A waker claims it for the thread it queues there: a sleeping one is
 * woken (arch_kick); a polling one sees the thread at once, with no
 * interrupt (in a virtual machine, an interrupt between CPUs costs
 * microseconds). */
enum { BUSY, POLLING, SLEEPING, CLAIMED };
#define IDLE_POLL_NS 50000UL                 /* 50 us */

/* Claim CPU c if it is idle: its idle loop runs what we queue for it
 * next. -> 0: not idle; SLEEPING: kick it once queued; POLLING: then
 * unclaim it (it waits for that). */
static int claim(cpu_t *c)
{
    int s = c->idle;
    if (s != POLLING && s != SLEEPING) return 0;
    return __atomic_compare_exchange_n(&c->idle, &s, s == POLLING ? CLAIMED : BUSY, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_RELAXED) ? s : 0;
}
static void claimed(cpu_t *c, int how)       /* its thread is queued */
{
    if (how == POLLING) __atomic_store_n(&c->idle, BUSY, __ATOMIC_RELEASE);
    else if (how == SLEEPING && c != this_cpu()) arch_kick(c);
}

/* Where a thread that becomes ready goes (see the top): an idle CPU of
 * the kind it prefers, the waker's own first; else an idle one of the
 * other kind; else, for a normal thread, a P-core running background work
 * (asked to move it away: *how = -1, kick it); else the shortest queue of
 * its kind, its last CPU on a tie (*how = 0). */
static cpu_t *place_on(task_t *t, cpu_t *me, int *how)
{
    int n = ncpus(), want = hybrid && t->bg ? CPU_E : CPU_P;
    cpu_t *last = cpus[t->cpu < n ? t->cpu : 0], *best = last->type == want ? last : 0;
    if (best && (*how = claim(last))) return last;
    for (int other = 0; other < 2; other++)
        for (int i = 0; i < n; i++) {
            cpu_t *c = cpus[(me->id + i) % n];
            if ((c->type != want) == other && (*how = claim(c))) return c;
        }
    *how = -1;
    if (want == CPU_P && hybrid)
        for (int i = 0; i < n; i++) {
            cpu_t *c = cpus[(me->id + i) % n];
            int zero = 0;
            if (c->type == CPU_P && c->running_bg &&
                __atomic_compare_exchange_n(&c->evict, &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return c;
        }
    for (int i = 0; i < n; i++)
        if (cpus[i]->type == want && (!best || cpus[i]->nready < best->nready)) best = cpus[i];
    *how = 0;
    return best;
}

/* Make t runnable: it is ours (taken off whatever it waited in). If it
 * is still on its old CPU's stack (it went to sleep there a moment ago),
 * wait until that CPU is off it. Then queue it where place_on says. A
 * busy CPU is interrupted only if its timer would not end its running
 * thread's slice (timer_update: the barrier pairs with its own): t waits
 * one slice at most. */
void ready(task_t *t)
{
    cpu_t *me = this_cpu();
    while (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) arch_lock_wait(me);
    int how;
    cpu_t *c = place_on(t, me, &how);
    t->state = TS_READY;
    spin_lock(&c->rq_lock);
    rq_add(c, t);
    spin_unlock(&c->rq_lock);
    /* (t may run, even end, from here on: it is not touched again) */
    if (how > 0) { claimed(c, how); return; }
    if (c == me) return;                 /* (our timer is set on the way out of the kernel) */
    smp_mb();                            /* (pairs with idle()'s and timer_update's) */
    int h = claim(c);                    /* c went idle since we looked: it must run t */
    if (h) { claimed(c, h); return; }
    if (how < 0 || (c->running && c->armed > c->slice_end)) arch_kick(c);
    if (!c->running) return;             /* (in its idle loop, not asleep: it sees t) */
    for (int i = 0, n = ncpus(); i < n; i++)     /* another CPU that went idle takes t from c's queue */
        if ((h = claim(cpus[i]))) { claimed(cpus[i], h); return; }
}

/* Make t the very next thread to run on this CPU, waking no other CPU:
 * for IPC, when the caller is about to wait (so t gets this CPU at once,
 * without an inter-processor interrupt and its delay). t continues the
 * caller's time slice rather than starting a new one: a client and a server
 * passing the CPU back and forth cannot keep the other ready threads waiting
 * beyond one slice. It is in no queue: no other CPU can take it. */
void ready_first(task_t *t)
{
    cpu_t *me = this_cpu();
    while (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) arch_lock_wait(me);
    t->state = TS_READY;
    me->handoff = t;
}

/* Right after a switch, on the new stack: the thread this CPU left may
 * now run elsewhere (on_cpu 0), move (it wants another CPU), or, if it
 * ended, its stack is freed. */
static void finish_switch(cpu_t *c)
{
    task_t *t = c->last;
    if (!t) return;
    c->last = 0;
    if (c->last_dead) { c->last_dead = 0; page_free(V2P(t), KSTACK / PAGE); return; }
    __atomic_store_n(&t->on_cpu, 0, __ATOMIC_RELEASE);
    if (c->last_ready) { c->last_ready = 0; ready(t); }
}

/* Give this CPU to the next ready thread (or to its idle loop). The
 * running thread is either waiting now (its state says so, its waker will
 * make it ready), or still TS_RUN: it yields, or its slice is over, and
 * goes back in this CPU's queue (or, if it should move, where place_on
 * says). Nothing here may use `c` after switch_to: when this thread runs
 * again, it may be on another CPU.
 * A CPU holds a process's address space only while it runs one of its
 * threads (idle runs on the kernel's). */
void schedule(void)
{
    cpu_t *c = this_cpu();
    task_t *prev = c->running, *next = c->handoff;
    int again = prev && prev->state == TS_RUN;
    if (again && prev->move) { prev->move = 0; prev->state = TS_READY; c->last_ready = 1; again = 0; }
    if (again || !next) {
        spin_lock(&c->rq_lock);
        if (again) { prev->state = TS_READY; rq_add(c, prev); }
        if (!next && (next = c->rq_head)) {
            if (!(c->rq_head = next->next)) c->rq_tail = 0;
            c->nready--;
        }
        spin_unlock(&c->rq_lock);
    }
    if (next && next != c->handoff) c->slice_end = ticks() + ns_to_ticks(SLICE_NS);
    c->handoff = 0;
    if (next == prev) { if (prev) prev->state = TS_RUN; return; }
    c->running = next;
    c->running_bg = next && next->bg;
    if (prev) { c->last = prev; c->last_dead = prev->state == TS_DEAD; }
    uint64_t *save = prev ? &prev->ksp : &c->idle_ksp;
    if (!next) {
        fpu_switch(c, prev, 0);
        if (c->cur_as != kernel_as) arch_as_load(c->cur_as = kernel_as);
        switch_to(save, c->idle_ksp);
    } else {
        fpu_switch(c, prev, next);       /* the floating-point registers: prev's out, next's in */
        next->cpu = c->id;
        next->on_cpu = 1;
        next->state = TS_RUN;
        arch_tls_switch(&c->a, next->fs_base);   /* its thread-local storage */
        if (next->proc->as != c->cur_as) arch_as_load(c->cur_as = next->proc->as);
        arch_set_kstack(c, next->kstack + KSTACK, next->proc->driver);
        switch_to(save, next->ksp);
    }
    finish_switch(this_cpu());
}

void preempt(void) { if (cur) schedule(); }

/* Asked to move the running thread away (place_on, steal): it goes if it
 * suits the other kind of core better. 1 if it went (and came back,
 * maybe on another CPU). */
static int evicted(cpu_t *c)
{
    if (!__atomic_exchange_n(&c->evict, 0, __ATOMIC_ACQ_REL)) return 0;
    task_t *t = c->running;
    if (!t || !hybrid || t->bg == (c->type == CPU_E)) return 0;
    t->move = 1;
    schedule();
    return 1;
}

static int idle_kind(int type)
{
    for (int i = 0, n = ncpus(); i < n; i++) {
        int s = __atomic_load_n(&cpus[i]->idle, __ATOMIC_RELAXED);
        if (cpus[i]->type == type && (s == POLLING || s == SLEEPING)) return 1;
    }
    return 0;
}

static void leaving(cpu_t *c, task_t *t);

/* Leaving the kernel (entry.S): a thread of an ending process ends here
 * (every thread that runs again passes here: proc_stop relies on it);
 * otherwise set this CPU's timer. */
void kernel_exit(void)
{
    cpu_t *c = this_cpu();
    finish_switch(c);                    /* (a new thread comes here straight from switch_to) */
    if (c->evict && evicted(c)) c = this_cpu();
    if (hybrid && c->running && c->running_bg != (c->type == CPU_E) && idle_kind(c->running_bg ? CPU_E : CPU_P)) {
        c->running->move = 1;            /* on the wrong kind of core (it came by a handoff, say) */
        schedule();                      /* while one of its kind idles, which may sleep and not look: */
        c = this_cpu();                  /* it goes there (place_on claims it) */
    }
    task_t *t = c->running;
    if (t && may_stop(t)) leaving(c, t);
    else timer_update(c);
}

/* Decide when this CPU's timer must fire next (called on the way out of
 * the kernel, and by idle before halting): at the end of the running
 * thread's slice if others wait for this CPU; on CPU 0, at the next
 * sleeper's wake-up (one CPU keeps the sleepers, so idle CPUs need none).
 * Tickless: with nothing to wait for, no timer at all. A CPU putting its
 * timer later while it runs a thread looks again at its queue after
 * saying so (store_mb): ready() did not interrupt it if the timer it saw
 * was to end the slice anyway. */
void timer_update(cpu_t *c)
{
    uint64_t d = NEVER;
    if (c->running && c->nready) d = c->slice_end;
    if (c->id == 0 && sleep_next < d) d = sleep_next;
    if (d == c->armed) return;
    if (d > c->armed && c->running) {
        store_mb(&c->armed, d);
        if (c->nready && c->slice_end < d) d = c->slice_end;
    }
    arch_timer_set(d);
    c->armed = d;
}

/* An interrupt of the timer (timer = 1) or a "reschedule" request reached
 * this CPU from user mode or idle: wake the sleepers whose time has come
 * (CPU 0); move the running thread if asked to and it suits another kind
 * of core better; end its slice if it is over and others wait. */
void core_tick(cpu_t *c, int timer)
{
    if (timer) {
        store_mb(&c->armed, NEVER);      /* it fired; kernel_exit sets the next one (seeing what ready() queued) */
        if (c->id == 0) wake_sleepers();
    }
    if (c->evict && evicted(c)) return;
    if (c->running && c->nready && ticks() >= c->slice_end) schedule();    /* its time is up */
}

/* An idle CPU takes a thread waiting in another CPU's queue behind the
 * thread it runs (an idle CPU runs its queue itself), from a CPU of its
 * own kind first; one still on its CPU's stack is left. Else it asks a
 * CPU of the other kind running a thread that prefers this kind to hand
 * it over (core_tick there moves it; place_on then finds this CPU idle):
 * a normal thread on an E-core, a background one on a P-core (it went
 * there when the P-core was idle; P-cores stay free for normal work). */
static int waits(cpu_t *v) { return v->nready && v->running; }
static int steal(cpu_t *c)
{
    int n = ncpus();
    for (int other = 0; other < 2; other++)
        for (int i = 1; i < n; i++) {
            cpu_t *v = cpus[(c->id + i) % n];
            if ((v->type != c->type) != other || !waits(v)) continue;
            task_t *t = 0, *prev = 0;
            spin_lock(&v->rq_lock);
            for (task_t **q = &v->rq_head; *q; prev = *q, q = &(*q)->next)
                if (!__atomic_load_n(&(*q)->on_cpu, __ATOMIC_ACQUIRE)) {
                    t = *q;
                    *q = t->next;
                    if (v->rq_tail == t) v->rq_tail = prev;
                    v->nready--;
                    break;
                }
            spin_unlock(&v->rq_lock);
            if (!t) continue;
            spin_lock(&c->rq_lock);
            rq_add(c, t);
            spin_unlock(&c->rq_lock);
            return 1;
        }
    for (int i = 0; hybrid && i < n; i++) {
        cpu_t *v = cpus[i];
        int zero = 0;
        if (v->type != c->type && v->running && v->running_bg == (c->type == CPU_E) &&
            __atomic_compare_exchange_n(&v->evict, &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            arch_kick(v);
            break;
        }
    }
    return 0;
}

static int work_elsewhere(cpu_t *c)
{
    for (int i = 0, n = ncpus(); i < n; i++) if (cpus[i] != c && waits(cpus[i])) return 1;
    return 0;
}

/* Each CPU's idle loop: run threads, or poll a while (letting interrupts
 * in), then sleep. Each step is announced first (idle: a waker may then
 * claim it), and the queues are looked at after that, with a barrier
 * between: a thread queued meanwhile is never left waiting. arch_wait_irq
 * cannot miss a wake-up (on x86-64, "sti; hlt": sti takes effect after the
 * next instruction, so an interrupt arriving in between still wakes the
 * hlt). */
void idle(void)
{
    for (;;) {
        cpu_t *c = this_cpu();
        if (c->nready || steal(c)) { schedule(); continue; }
        store_mb(&c->idle, POLLING);
        int work = 0;
        for (uint64_t i = 0, end = ticks() + ns_to_ticks(IDLE_POLL_NS); c->idle == POLLING; i++) {
            if ((work = c->nready || (i % 64 == 0 && work_elsewhere(c))) || ticks() >= end) break;
            arch_irq_window();
            arch_lock_wait(c);
        }
        int s = POLLING;
        if (!__atomic_compare_exchange_n(&c->idle, &s, work ? BUSY : SLEEPING, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            while (c->idle != BUSY) arch_lock_wait(c);   /* claimed: its thread is being queued */
            continue;
        }
        if (work) continue;
        smp_mb();
        if (!c->nready && !work_elsewhere(c)) {
            timer_update(c);
            arch_wait_irq();
        }
        __atomic_store_n(&c->idle, BUSY, __ATOMIC_RELAXED);
    }
}

/* The CPUs have started (smp_start): their kinds (cpu_setup read each
 * one's from the processor; "pcores=N" overrides), and the boot line. All
 * of one kind (the Pi 4's A72s, the Pi 5's A76s, most PCs) is no hybrid:
 * all are treated as P-cores. */
void sched_cpus(void)
{
    int n = ncpus(), k[2] = { 0, 0 };
    for (int i = 0; i < n; i++) {
        if (opt_pcores) cpus[i]->type = i < opt_pcores ? CPU_P : CPU_E;
        k[cpus[i]->type]++;
    }
    hybrid = k[CPU_P] && k[CPU_E];
    ecores = hybrid ? k[CPU_E] : 0;
    if (!hybrid) for (int i = 0; i < n; i++) cpus[i]->type = CPU_P;
    if (hybrid) kprintf("mk: %d CPUs running: %d P-cores, %d E-cores%s\n", n, k[CPU_P], k[CPU_E],
                        opt_pcores ? " (pcores=: pretended)" : "");
    else kprintf("mk: %d CPU%s running\n", n, n > 1 ? "s" : "");
}

/* SYS_PLACE: where a thread of this process should run: normal (0: P-cores)
 * or background (1: E-cores). tid 0: the caller, which moves at once if it
 * is on the wrong kind; -1: the whole process, its later threads too; else
 * that thread, from its next wake-up. */
long place(int tid, int bg)
{
    task_t *me = cur;
    proc_t *p = me->proc;
    if (bg != 0 && bg != 1) return -EINVAL;
    if (tid == -1) {
        spin_lock(&p->lock);
        p->bg = bg;
        for (task_t *t = p->threads; t; t = t->tnext) t->bg = bg;
        spin_unlock(&p->lock);
    } else if (tid && tid != me->tid) {
        task_t *t = task_lock(tid);
        if (t && t->proc == p) t->bg = bg;
        if (t) task_unlock(t);
        return t && t->proc == p ? 0 : -ESRCH;
    } else me->bg = bg;
    cpu_t *c = this_cpu();
    c->running_bg = me->bg;
    if (hybrid && me->bg != (c->type == CPU_E)) { me->move = 1; schedule(); }
    return 0;
}

/* ---- Sleeping. Only CPU 0 sets its timer for the sleepers (timer_update);
 * a sleeper that becomes the earliest is announced to it (arch_kick). */

static void sleep_remove(task_t *t)  /* (under sleep_lock) */
{
    for (task_t **q = &sleepq; *q; q = &(*q)->next)
        if (*q == t) { *q = t->next; break; }
    sleep_next = sleepq ? sleepq->wake : NEVER;
}

long sleep_ns(uint64_t ns)
{
    task_t *t = cur, **q = &sleepq;
    spin_lock(&sleep_lock);
    if (t->woken) { t->woken = 0; spin_unlock(&sleep_lock); return 1; }   /* woken before it slept */
    if (!ns) { spin_unlock(&sleep_lock); schedule(); return 0; }
    t->wake = ticks() + ns_to_ticks(ns);
    while (*q && (*q)->wake <= t->wake) q = &(*q)->next;
    t->next = *q;
    *q = t;
    sleep_next = sleepq->wake;
    int first = sleepq == t, e = blocked(t, TS_SLEEP);
    if (e) sleep_remove(t);
    spin_unlock(&sleep_lock);
    if (e) return e;
    if (first && this_cpu()->id != 0) arch_kick(cpus[0]);
    schedule();
    return t->ret;
}

/* SYS_WAKE: end the sleep of another thread of this process early (its
 * sleep returns 1); if it is not asleep yet, its next sleep ends at once,
 * so a wake-up is never lost. */
long sleep_wake(int tid)
{
    task_t *t = task_lock(tid);
    if (!t) return -ESRCH;
    if (t->proc != cur->proc) { task_unlock(t); return -ESRCH; }
    spin_lock(&sleep_lock);
    int asleep = t->state == TS_SLEEP;
    if (asleep) { sleep_remove(t); t->state = TS_READY; } else t->woken = 1;
    spin_unlock(&sleep_lock);
    task_unlock(t);
    if (asleep) { t->ret = 1; ready(t); }
    return 0;
}

void wake_sleepers(void)
{
    uint64_t now = ticks();
    task_t *list = 0, **end = &list;
    spin_lock(&sleep_lock);
    while (sleepq && sleepq->wake <= now) {
        sleepq->state = TS_READY;        /* (ours now: stop_thread leaves it) */
        *end = sleepq;
        end = &sleepq->next;
        sleepq = sleepq->next;
    }
    *end = 0;
    sleep_next = sleepq ? sleepq->wake : NEVER;
    spin_unlock(&sleep_lock);
    for (task_t *t = list, *n; t; t = n) {
        n = t->next;
        t->ret = 0;
        ready(t);
    }
}

/* ---- Threads. */
static void list_add(proc_t *p)          /* (under proc_lock) */
{
    p->hnext = pid_hash[p->pid % NPID];
    pid_hash[p->pid % NPID] = p;
    p->aprev = all_tail;
    if (all_tail) all_tail->anext = p; else all_procs = p;
    all_tail = p;
    nprocs++;
}

static void proc_free(proc_t *p)         /* (under proc_lock) */
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
    spin_lock(&proc_lock);
    for (proc_t *p = all_procs; p; p = p->anext)
        for (task_t *t = p->threads; t; t = t->tnext) stack_note(t);
    spin_unlock(&proc_lock);
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
    t->bg = p->bg;
    t->cpu = this_cpu()->id;             /* (its first CPU: place_on starts there) */
    t->tid = __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);
    spin_lock(&p->lock);
    t->tnext = p->threads;
    p->threads = t;
    p->nthreads++;
    spin_unlock(&p->lock);
    __atomic_fetch_add(&nthreads, 1, __ATOMIC_RELAXED);
    spin_lock(&tid_hash[t->tid % NTID].lock);
    t->hnext = tid_hash[t->tid % NTID].head;
    tid_hash[t->tid % NTID].head = t;
    spin_unlock(&tid_hash[t->tid % NTID].lock);
    return t;
}

/* A new thread: its first switch_to enters user mode at entry, with the
 * stack and arg as its first argument (arch_thread_start); then it may run. */
long thread_create(uint64_t entry, uint64_t stack, uint64_t arg)
{
    if (entry >= USER_TOP || stack >= USER_TOP || stack < PAGE) return -EINVAL;
    task_t *t = thread_new(cur->proc);
    if (!t) return -ENOMEM;
    int tid = t->tid;
    arch_thread_start(t, entry, (stack & ~15UL) - 8, arg);   /* -8: as if called (the ABI's alignment) */
    ready(t);
    return tid;
}

/* Wake thread t from whatever it waits in, its call returning ret: its
 * process is ending (-EPIPE: it then ends itself on its way out of the
 * kernel), or a signal came for it (-EINTR). Under its process's lock
 * (TS_WAIT changes only under it); each other kind of wait is checked
 * again under its own lock: whoever takes a thread off a queue wakes it,
 * never two. A caller a server has just taken off its queue (TS_SEND)
 * may be waiting for the reply by now: tried too (if the server has not
 * got so far, it sees why itself: ipc.c). */
__attribute__((cold)) static void stop_thread(task_t *t, long ret)
{
    int s = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE), got = 0;
    if (s == TS_SLEEP) {
        spin_lock(&sleep_lock);
        if ((got = t->state == TS_SLEEP)) { sleep_remove(t); t->state = TS_READY; }
        spin_unlock(&sleep_lock);
    } else if (s == TS_WAIT) got = 1;
    else if (s == TS_SEND || s == TS_RECV || s == TS_LOOKUP) got = ipc_unblock(t, s);
    if (!got && (s == TS_REPLY || s == TS_SEND)) got = ipc_unreply(t);
    if (got) { t->ret = ret; ready(t); }
}

/* The last thread of p has ended (on this CPU): close its ports, free its
 * memory; it stays as a "zombie" (its exit status) until its parent
 * collects it with SYS_WAIT, unless there is no parent to do so. */
static void proc_end(proc_t *p)
{
    p->exiting = 1;
    ipc_exit(p);                         /* its ports, and whoever waits on it */
    cpu_t *c = this_cpu();
    arch_as_load(c->cur_as = kernel_as); /* leave its address space before freeing it */
    vm_free(p->as);
    p->as = 0;
    p->pages = 0;
    if (p == super) panic("init, the supervisor, ended (status %d)\n", p->status);
    /* Its children are adopted by the supervisor, which then learns when
     * they end (a session's shell whose login program died, for example). */
    int adopted = 0;
    spin_lock(&proc_lock);
    for (proc_t *q = all_procs, *n; q; q = n) {
        n = q->anext;
        if (q->ppid != p->pid) continue;
        if (super && !super->exiting) { q->ppid = super->pid; adopted |= q->zombie; }
        else if (q->zombie) proc_free(q);
        else q->ppid = 0;
    }
    if (adopted && super->child_port) port_notify(super->child_port, super, NOTE_CHILD);
    proc_t *parent = p->ppid ? proc_find(p->ppid) : 0;
    if (!parent || parent->exiting) { proc_free(p); spin_unlock(&proc_lock); return; }
    p->zombie = 1;
    spin_lock(&parent->lock);
    for (task_t *w = parent->threads; w; w = w->tnext)
        if (w->state == TS_WAIT && (w->wpid == -1 || w->wpid == p->pid)) { w->ret = 0; ready(w); }
    spin_unlock(&parent->lock);
    if (parent->child_port) port_notify(parent->child_port, parent, NOTE_CHILD);
    spin_unlock(&proc_lock);
}

void thread_exit(void)
{
    task_t *t = cur;
    proc_t *p = t->proc;
    spin_lock(&tid_hash[t->tid % NTID].lock);
    for (task_t **h = &tid_hash[t->tid % NTID].head; *h; h = &(*h)->hnext)
        if (*h == t) { *h = t->hnext; break; }
    spin_unlock(&tid_hash[t->tid % NTID].lock);
    spin_lock(&p->lock);
    for (task_t **q = &p->threads; *q; q = &(*q)->tnext)
        if (*q == t) { *q = t->tnext; break; }
    int last = !--p->nthreads;
    spin_unlock(&p->lock);
    __atomic_fetch_sub(&nthreads, 1, __ATOMIC_RELAXED);
    if (t->sigctx) { fpu_free(t); t->fpu = t->sigctx->fpu; kfree(t->sigctx); }   /* (in a signal's function) */
    fpu_free(t);                         /* (its registers are not saved: it is gone) */
#ifdef STACKCHECK
    stack_note(t);
#endif
    if (last) proc_end(p);
    t->state = TS_DEAD;                  /* its stack goes once this CPU is off it (finish_switch) */
    schedule();
    __builtin_unreachable();
}

/* Make process p end with status code: all its threads. Those waiting are
 * woken; each then ends itself in kernel_exit (the last one frees the
 * process).
 * A thread of p may be going to sleep right now, on another CPU, and
 * check `exiting` (blocked()) just as we set it. So before looking at the
 * threads we wait until every other CPU has answered a shootdown (it does
 * so only from user mode, idle or a spin loop, then leaves through
 * kernel_exit): a thread that went to sleep before its CPU answered is
 * seen asleep below; one that goes to sleep after it sees `exiting`; one
 * that runs or is ready reads `exiting` in its next kernel_exit. The cost
 * is on the killer, not on every message. (Not needed if p has no other
 * thread.) */
static void proc_stop(proc_t *p, int code)
{
    spin_lock(&p->lock);
    if (p->exiting) { spin_unlock(&p->lock); return; }
    p->status = code;
    p->exiting = 1;
    int others = p->nthreads > (cur && cur->proc == p);
    spin_unlock(&p->lock);
    int sig = EXIT_SIGNO(code);           /* (Ctrl-C and hang-ups are no news) */
    if (code && sig != SIGINT && sig != SIGHUP)
        kprintf("mk: %s (pid %d) %s %d\n", p->name, p->pid, sig ? "ended by signal" : "exited with", sig ? sig : code);
    if (!others) return;
    tlb_shootdown(p);
    spin_lock(&p->lock);
    for (task_t *t = p->threads; t; t = t->tnext)
        if (t != cur) stop_thread(t, -EPIPE);
    spin_unlock(&p->lock);
}

void proc_exit(int code)
{
    proc_stop(cur->proc, code);
    thread_exit();
}

/* ---- Signals. One the process neither catches nor ignores ends it at
 * once, from the sender (proc_stop: SIGKILL always). An ignored one is
 * dropped. A caught one waits (sigpend) for the thread that set the
 * function, which is woken if it waits (its call returns -EINTR), and runs
 * the function on its way out of the kernel: sig_deliver. Its registers
 * stay in the kernel meanwhile (a sigctx_t: the program cannot change
 * them), its floating-point registers too (the function starts with fresh
 * ones); SYS_SIGRETURN puts all back. One function at a time per thread:
 * signals that come meanwhile wait for its end. */

__attribute__((cold)) long stop_why(task_t *t) { return t->proc->exiting ? -EPIPE : sig_due(t) ? -EINTR : 0; }
__attribute__((cold)) static int unblocked(task_t *t)   /* (blocked: it must not wait after all) */
{
    int e = (int)stop_why(t);
    if (e) t->state = TS_RUN;
    return e;
}

__attribute__((cold)) static void leaving(cpu_t *c, task_t *t)   /* (kernel_exit: its process ends, or a signal) */
{
    if (t->proc->exiting) thread_exit();
    while (sig_due(t)) sig_deliver(t);
    timer_update(c);
}

/* Send sig to p (under proc_lock: p cannot be freed meanwhile). -> 1 if
 * it is the caller's own process, which must end (after unlocking). As
 * in proc_stop, a shootdown first: a thread of p about to wait on another
 * CPU then sees sigpend; one running in user mode comes into the kernel,
 * and leaves through kernel_exit. */
__attribute__((cold)) static int sig_post(proc_t *p, int sig)
{
    uint32_t bit = 1u << sig;
    if (sig == SIGKILL || !(p->sigcatch & bit)) {
        if (sig != SIGKILL && (p->sigign & bit)) return 0;
        if (p == cur->proc) return 1;
        proc_stop(p, EXIT_SIGNAL(sig));
        return 0;
    }
    spin_lock(&p->lock);
    task_t *t = p->threads;              /* its signal thread; if that one ended, any */
    for (task_t *x = p->threads; x; x = x->tnext) if (x->tid == p->sigtid) t = x;
    if (t) __atomic_or_fetch(&t->sigpend, bit, __ATOMIC_SEQ_CST);
    spin_unlock(&p->lock);
    if (!t || t == cur) return 0;        /* (the caller: on its way out of this call) */
    tlb_shootdown(p);
    spin_lock(&p->lock);
    for (task_t *x = p->threads; x; x = x->tnext)
        if (x == t && sig_due(t)) stop_thread(t, -EINTR);
    spin_unlock(&p->lock);
    return 0;
}

/* SYS_KILL: to process pid, or (pid < 0) to each of group -pid. Root may
 * signal any process but the supervisor; others only their own. sig 0:
 * nothing is sent (is there one it could go to?). */
__attribute__((cold)) long proc_kill(long pid, int sig)
{
    proc_t *me = cur->proc;
    long r = -ESRCH;
    int self = 0;
    if (sig < 0 || sig >= NSIG || !pid) return -EINVAL;
    spin_lock(&proc_lock);
    for (proc_t *p = pid > 0 ? proc_find((int)pid) : all_procs; p; p = pid > 0 ? 0 : p->anext) {
        if ((pid < 0 && p->pgid != -pid) || p->zombie || p->exiting) continue;
        if (p == super || (me->uid && me->uid != p->uid)) { if (r) r = -EPERM; continue; }
        r = 0;
        if (sig) self |= sig_post(p, sig);
    }
    spin_unlock(&proc_lock);
    if (self) proc_exit(EXIT_SIGNAL(sig));
    return r;
}

/* SYS_SIGNAL: what sig does to this process (SIG_DFL, SIG_IGN, or entry:
 * a function, run by the calling thread). */
__attribute__((cold)) long sig_set(int sig, uint64_t entry)
{
    proc_t *p = cur->proc;
    uint32_t bit = 1u << sig;
    if (sig <= 0 || sig >= NSIG || sig == SIGKILL || entry >= USER_TOP) return -EINVAL;
    spin_lock(&p->lock);
    uint64_t ign = (uint64_t)SIG_IGN;
    p->sigign = entry == ign ? p->sigign | bit : p->sigign & ~bit;
    p->sigcatch = entry > ign ? p->sigcatch | bit : p->sigcatch & ~bit;
    if (entry > ign) { p->sigentry = entry; p->sigtid = cur->tid; }
    spin_unlock(&p->lock);
    return 0;
}

/* On t's way out of the kernel (kernel_exit): its first waiting signal.
 * Caught (still), it now runs the function; else it ends or is dropped
 * (SYS_SIGNAL changed it since). */
__attribute__((cold)) void sig_deliver(task_t *t)
{
    proc_t *p = t->proc;
    int sig = __builtin_ctz(t->sigpend);
    uint32_t bit = 1u << sig;
    __atomic_and_fetch(&t->sigpend, ~bit, __ATOMIC_RELAXED);
    if (!(p->sigcatch & bit)) { if (p->sigign & bit) return; proc_exit(EXIT_SIGNAL(sig)); }
    sigctx_t *x = kzalloc(sizeof *x);
    if (!x) proc_exit(EXIT_SIGNAL(sig));
    regs_t *r = (regs_t *)(t->kstack + KSTACK) - 1;     /* (its registers in user mode) */
    x->r = *r;
    x->fpu = fpu_detach(this_cpu(), t);
    t->sigctx = x;
    arch_sig_enter(r, p->sigentry, sig);
}

/* SYS_SIGRETURN: the function is done; r: the registers to leave with. */
__attribute__((cold)) long sig_return(regs_t *r)
{
    task_t *t = cur;
    sigctx_t *x = t->sigctx;
    if (!x) return -EINVAL;
    *r = x->r;
    fpu_attach(this_cpu(), t, x->fpu);
    t->sigctx = 0;
    kfree(x);
    arch_sig_return(r);
    return SC_RET(r);                    /* (syscall() stores it there again) */
}

/* SYS_WAIT: collect an ended child (pid, or any if -1). A child that ends
 * does so under proc_lock: we either find it a zombie, or wait (TS_WAIT)
 * before it ends, and it wakes us. */
long proc_wait(int pid, int *ustatus, int flags)
{
    task_t *t = cur;
    proc_t *me = t->proc;
    for (;;) {
        int found = 0;
        spin_lock(&proc_lock);
        for (proc_t *q = all_procs; q; q = q->anext) {
            if (q->ppid != me->pid || (pid > 0 && q->pid != pid)) continue;
            found = 1;
            if (!q->zombie) continue;
            int st = q->status, id = q->pid;
            proc_free(q);
            spin_unlock(&proc_lock);
            if (ustatus && vm_copy(me->as, ustatus, 0, &st, sizeof st)) return -EFAULT;
            return id;
        }
        if (!found || (flags & WAIT_NOHANG)) { spin_unlock(&proc_lock); return found ? 0 : -ECHILD; }
        spin_lock(&me->lock);
        t->wpid = pid;
        int e = blocked(t, TS_WAIT);
        spin_unlock(&me->lock);
        spin_unlock(&proc_lock);
        if (e) return e;
        schedule();
        if (t->ret < 0) return t->ret;
    }
}

/* ---- Starting a program. An ELF executable lists "segments": pieces of
 * the file to place at given addresses, with their rights (read, write,
 * execute). We copy each into fresh pages of a new address space and add a
 * stack. args ("name arg1 arg2") is put on its stack, and its address given
 * to the program as its first argument. The file is read from address space src_as (0:
 * the kernel's, for the boot modules), through vm_copy: a bad file or
 * pointer from a program gives an error, never a crash. The new process
 * is nobody's yet: proc_start makes it known and runs it. */
typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version;
                 uint64_t entry, phoff, shoff; uint32_t flags;
                 uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } elf_hdr;
typedef struct { uint32_t type, flags; uint64_t off, vaddr, paddr, filesz, memsz, align; } elf_ph;
#define STACK_PAGES 8                    /* 32 KiB */

proc_t *proc_new(uint64_t src_as, const uint8_t *img, uint64_t len, const char *args, int uid, int gid,
                 const mk_groups_t *groups, int driver, uint64_t quota, long *err)
{
    elf_hdr h;
    *err = -EFAULT;
    if (len < sizeof h || vm_copy(0, &h, src_as, img, sizeof h)) return 0;
    *err = -EINVAL;
    if (memcmp(h.ident, "\177ELF\2", 5) || h.machine != ELF_MACHINE || h.type != 2 || h.phnum > 32 ||
        h.phoff + h.phnum * sizeof(elf_ph) > len)
        return 0;                        /* not a 64-bit executable for this processor */
    uint64_t n = strlen(args) + 1;
    if (n > 256) return 0;
    quarantine_release();                /* dead drivers' DMA buffers, once safe */
    *err = -ENOMEM;
    proc_t *p = kzalloc(sizeof *p);
    if (!p) return 0;
    p->quota = quota;
    if (!(p->as = vm_new())) goto fail;

    uint64_t end = 0;
    for (int i = 0; i < h.phnum; i++) {
        elf_ph ph;
        if (vm_copy(0, &ph, src_as, img + h.phoff + i * sizeof ph, sizeof ph)) { *err = -EFAULT; goto fail; }
        if (ph.type != 1) continue;      /* PT_LOAD */
        if (ph.off + ph.filesz > len || ph.filesz > ph.memsz || ph.vaddr + ph.memsz > MMIO_BASE) { *err = -EINVAL; goto fail; }
        uint64_t fl = VM_U | (ph.flags & 2 ? VM_W : 0) | (ph.flags & 1 ? 0 : VM_NX);
        for (uint64_t va = ph.vaddr & ~(PAGE - 1); va < ph.vaddr + ph.memsz; va += PAGE) {
            uint64_t pa = (!quota || p->pages < quota) ? page_alloc(1) : 0;
            if (!pa) goto fail;
            if (vm_map(p->as, va, pa, fl)) { page_free(pa, 1); *err = -EINVAL; goto fail; }
            p->pages++;
        }
        /* the file's bytes; the rest stays zero (.bss) */
        if (vm_load(p->as, (void *)ph.vaddr, src_as, img + ph.off, ph.filesz)) { *err = -EFAULT; goto fail; }
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

    p->uid = uid; p->gid = gid; p->driver = driver;
    if (groups) p->groups = *groups;
    p->brk_base = p->brk = (end + PAGE - 1) & ~(PAGE - 1);
    p->mmio = MMIO_BASE;
    for (int i = 0; i < 15 && args[i] && args[i] != ' '; i++) p->name[i] = args[i];
    task_t *t = thread_new(p);
    if (!t) goto fail;
    p->pid = t->tid;
    arch_thread_start(t, h.entry, sp, sp);
    return p;
fail:
    if (p->as) vm_free(p->as);
    kfree(p);
    return 0;
}

/* Make new process p known, as a child of ppid (or, if that one has ended
 * meanwhile, of the supervisor), and run its first thread. -> its pid. */
int proc_start(proc_t *p, int ppid)
{
    task_t *t = p->threads;
    int pid = p->pid;
    spin_lock(&proc_lock);
    proc_t *parent = ppid ? proc_find(ppid) : 0;
    if (ppid && (!parent || parent->zombie)) ppid = super && !super->exiting ? super->pid : 0;
    p->ppid = ppid;
    list_add(p);
    spin_unlock(&proc_lock);
    ready(t);
    return pid;
}
