/*
 * sync.c - Sleep queues, mutexes, condition variables and reader/writer
 * locks (see sync.h).
 *
 * A sleep queue is a FIFO of LWPs blocked on one address (a channel),
 * hashed into SQ_HASH buckets with a spin lock each.  Blocking takes two
 * steps: the LWP joins the queue under the bucket's lock, then (its
 * interlock released) takes the lock again and goes to sleep only if
 * nobody has dequeued it meanwhile; a waker dequeues under the same lock.
 * No wake-up is lost between the caller's test of its condition and its
 * sleep, and no two bucket locks are ever held together.
 *
 * Lock order: a bucket's lock, then the dispatcher's (disp_enter).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sync.h"
#include "proc.h"
#include "smp.h"

#define SQ_HASH 512

struct sleepq {
    struct spinlock lock;
    struct lwp *head, *tail;
} __attribute__((aligned(64)));

static struct sleepq sleepq[SQ_HASH];

static struct sleepq *sq_of(const void *chan)
{
    uint64_t a = (uint64_t)chan;
    return &sleepq[((a >> 3) ^ (a >> 12) ^ (a >> 21)) % SQ_HASH];
}

static void sq_insert(struct sleepq *sq, struct lwp *l, const void *chan, bool sig)
{
    l->wchan = chan;
    l->sleep_sig = sig;
    l->sq_next = NULL;
    l->sq_prev = sq->tail;
    if (sq->tail)
        sq->tail->sq_next = l;
    else
        sq->head = l;
    sq->tail = l;
}

static void sq_remove(struct sleepq *sq, struct lwp *l)
{
    if (l->sq_prev)
        l->sq_prev->sq_next = l->sq_next;
    else
        sq->head = l->sq_next;
    if (l->sq_next)
        l->sq_next->sq_prev = l->sq_prev;
    else
        sq->tail = l->sq_prev;
    l->sq_next = l->sq_prev = NULL;
    l->wchan = NULL;
}

/* Wake up to n LWPs blocked on chan (n < 0: all), the bucket's lock held. */
static int sq_wake_locked(struct sleepq *sq, const void *chan, int n)
{
    int woken = 0;
    struct lwp *l = sq->head;
    while (l && (n < 0 || woken < n)) {
        struct lwp *next = l->sq_next;
        if (l->wchan == chan) {
            sq_remove(sq, l);
            disp_enter();
            setrun_locked(l);
            disp_exit();
            woken++;
        }
        l = next;
    }
    return woken;
}

/*
 * The second step of blocking: l is on sq's queue for chan, the bucket's
 * lock held.  Releases the lock and the interlock; sleeps unless it was
 * woken meanwhile; leaves the queue.
 */
static void sq_block_locked(struct sleepq *sq, const void *chan, kmutex_t *interlock)
{
    struct lwp *l = curlwp;
    if (interlock) {
        spin_unlock(&sq->lock);
        mutex_exit(interlock);
        spin_lock(&sq->lock);
    }
    if (l->wchan == chan) {
        disp_enter();
        if (sleep_due(l)) {
            disp_exit();
            sq_remove(sq, l);
            spin_unlock(&sq->lock);
        } else {
            l->state = LWP_SLEEPING;
            spin_unlock(&sq->lock);
            swtch();                             /* (returns with the dispatcher's lock released) */
            if (l->wchan) {                      /* (a stray wake-up: still queued) */
                spin_lock(&sq->lock);
                if (l->wchan == chan)
                    sq_remove(sq, l);
                spin_unlock(&sq->lock);
            }
        }
    } else {
        spin_unlock(&sq->lock);
    }
}

void sleepq_block(const void *chan, kmutex_t *interlock, bool sig)
{
    struct sleepq *sq = sq_of(chan);
    spin_lock(&sq->lock);
    sq_insert(sq, curlwp, chan, sig);
    sq_block_locked(sq, chan, interlock);
}

/*
 * Wake l from whatever queue it is on, asleep or about to be (sig: only
 * from an interruptible sleep); due(l), if given, is checked under the
 * queue's lock.  True if it was woken.
 */
bool sleepq_unsleep(struct lwp *l, bool sig, bool (*due)(struct lwp *))
{
    for (;;) {
        const void *w = l->wchan;
        if (!w)
            return false;
        struct sleepq *sq = sq_of(w);
        spin_lock(&sq->lock);
        if (l->wchan != w) {
            spin_unlock(&sq->lock);
            continue;
        }
        bool ok = (!sig || l->sleep_sig) && (!due || due(l));
        if (ok) {
            sq_remove(sq, l);
            disp_enter();
            setrun_locked(l);
            disp_exit();
        }
        spin_unlock(&sq->lock);
        return ok;
    }
}

int sleepq_wakeup(const void *chan, int n)
{
    struct sleepq *sq = sq_of(chan);
    spin_lock(&sq->lock);
    int r = sq->head ? sq_wake_locked(sq, chan, n) : 0;
    spin_unlock(&sq->lock);
    return r;
}

/* ------------------------------------------------------------------ */
/* Mutexes                                                             */
/* ------------------------------------------------------------------ */

#define M_WAITERS 1UL
#define M_OWNER(o) ((struct lwp *)((o) & ~M_WAITERS))

void mutex_init(kmutex_t *m, const char *name, int type, void *arg)
{
    (void)name;
    (void)arg;
    m->m_owner = 0;
    m->m_spin = type == MUTEX_SPIN;
}

void mutex_destroy(kmutex_t *m)
{
    if (m->m_owner)
        panic("mutex_destroy: %p held", m);
}

static inline bool owner_running(struct lwp *o)
{
    return o->oncpu && o->state == LWP_RUNNING;
}

static void mutex_vector_enter(kmutex_t *m)
{
    struct lwp *me = curlwp;
    if (m->m_spin) {
        for (;;) {
            uintptr_t o = 0;
            if (__atomic_compare_exchange_n(&m->m_owner, &o, (uintptr_t)me, false, __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED))
                return;
            if (M_OWNER(o) == me)
                panic("mutex_enter: spin mutex %p already held", m);
            while (m->m_owner)
                cpu_relax();
        }
    }
    for (;;) {
        uintptr_t o = m->m_owner;
        if (!o) {
            if (__atomic_compare_exchange_n(&m->m_owner, &o, (uintptr_t)me, false, __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED))
                return;
            continue;
        }
        struct lwp *owner = M_OWNER(o);
        if (owner == me)
            panic("mutex_enter: %p already held by this LWP", m);
        /* The owner runs: it will release it soon (kernel code is not preempted). */
        if (owner_running(owner)) {
            cpu_relax();
            continue;
        }
        struct sleepq *sq = sq_of(m);
        spin_lock(&sq->lock);
        o = m->m_owner;
        if (!o || (!(o & M_WAITERS) && !__atomic_compare_exchange_n(&m->m_owner, &o, o | M_WAITERS, false,
                                                                   __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))) {
            spin_unlock(&sq->lock);
            continue;
        }
        if (owner_running(M_OWNER(o))) {
            spin_unlock(&sq->lock);                  /* (it runs again: spin) */
            continue;
        }
        sq_insert(sq, me, m, false);
        sq_block_locked(sq, m, NULL);
    }
}

void mutex_enter(kmutex_t *m)
{
    uintptr_t o = 0;
    if (!m->m_spin &&
        __atomic_compare_exchange_n(&m->m_owner, &o, (uintptr_t)curlwp, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;
    mutex_vector_enter(m);
}

int mutex_tryenter(kmutex_t *m)
{
    uintptr_t o = 0;
    return __atomic_compare_exchange_n(&m->m_owner, &o, (uintptr_t)curlwp, false, __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
}

void mutex_exit(kmutex_t *m)
{
    uintptr_t o = (uintptr_t)curlwp;
    if (__atomic_compare_exchange_n(&m->m_owner, &o, 0, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        return;
    if (M_OWNER(o) != curlwp)
        panic("mutex_exit: %p not held by this LWP (owner %p)", m, (void *)o);
    /* LWPs wait: release it and wake them all (they contend again) */
    struct sleepq *sq = sq_of(m);
    spin_lock(&sq->lock);
    __atomic_store_n(&m->m_owner, 0, __ATOMIC_RELEASE);
    sq_wake_locked(sq, m, -1);
    spin_unlock(&sq->lock);
}

int mutex_owned(const kmutex_t *m)
{
    return M_OWNER(m->m_owner) == curlwp;
}

/* ------------------------------------------------------------------ */
/* Condition variables                                                 */
/* ------------------------------------------------------------------ */

void cv_init(kcondvar_t *cv, const char *name, int type, void *arg)
{
    (void)name;
    (void)type;
    (void)arg;
    cv->cv_waiters = 0;
}

void cv_destroy(kcondvar_t *cv)
{
    (void)cv;
}

static void cv_block(kcondvar_t *cv, kmutex_t *m, bool sig)
{
    struct sleepq *sq = sq_of(cv);
    spin_lock(&sq->lock);
    cv->cv_waiters++;
    sq_insert(sq, curlwp, cv, sig);
    sq_block_locked(sq, cv, m);
    __atomic_fetch_sub(&cv->cv_waiters, 1, __ATOMIC_RELAXED);
    mutex_enter(m);
}

void cv_wait(kcondvar_t *cv, kmutex_t *m)
{
    cv_block(cv, m, false);
}

int cv_wait_sig(kcondvar_t *cv, kmutex_t *m)
{
    cv_block(cv, m, true);
    return !lwp_sig_pending(curlwp);
}

long cv_timedwait(kcondvar_t *cv, kmutex_t *m, uint64_t deadline)
{
    if (ticks >= deadline)
        return -1;
    struct lwp *l = curlwp;
    uint64_t saved = l->wake_tick;
    l->wake_tick = deadline;
    cv_block(cv, m, false);
    l->wake_tick = saved;
    return ticks >= deadline ? -1 : 1;
}

long cv_timedwait_sig(kcondvar_t *cv, kmutex_t *m, uint64_t deadline)
{
    struct lwp *l = curlwp;
    if (deadline && ticks >= deadline)
        return -1;
    uint64_t saved = l->wake_tick;
    l->wake_tick = deadline;
    cv_block(cv, m, true);
    l->wake_tick = saved;
    if (lwp_sig_pending(l))
        return 0;
    return deadline && ticks >= deadline ? -1 : 1;
}

long cv_timedwait_sig_hires(kcondvar_t *cv, kmutex_t *m, uint64_t deadline_ns)
{
    struct lwp *l = curlwp;
    if (deadline_ns && hrtime() >= deadline_ns)
        return -1;
    uint64_t saved = l->wake_ns;
    l->wake_ns = deadline_ns;
    if (deadline_ns)
        lapic_timer_hint(deadline_ns);
    cv_block(cv, m, true);
    l->wake_ns = saved;
    if (lwp_sig_pending(l))
        return 0;
    return deadline_ns && hrtime() >= deadline_ns ? -1 : 1;
}

void cv_signal(kcondvar_t *cv)
{
    if (__atomic_load_n(&cv->cv_waiters, __ATOMIC_ACQUIRE))
        sleepq_wakeup(cv, 1);
}

void cv_broadcast(kcondvar_t *cv)
{
    if (__atomic_load_n(&cv->cv_waiters, __ATOMIC_ACQUIRE))
        sleepq_wakeup(cv, -1);
}

/* ------------------------------------------------------------------ */
/* Reader/writer locks                                                 */
/* ------------------------------------------------------------------ */

void rw_init(krwlock_t *rw, const char *name, int type, void *arg)
{
    (void)type;
    (void)arg;
    memset(rw, 0, sizeof(*rw));
    mutex_init(&rw->rw_mx, name, MUTEX_SPIN, NULL);
}

void rw_destroy(krwlock_t *rw)
{
    if (rw->rw_readers)
        panic("rw_destroy: %p held", rw);
}

static inline void rw_mx_init(krwlock_t *rw)
{
    rw->rw_mx.m_spin = 1;                        /* (a zeroed krwlock_t is a valid one) */
}

void rw_enter(krwlock_t *rw, krw_t how)
{
    rw_mx_init(rw);
    mutex_enter(&rw->rw_mx);
    if (how == RW_WRITER) {
        if (rw->rw_owner == curlwp)
            panic("rw_enter: %p already write-held by this LWP", rw);
        rw->rw_wwait++;
        while (rw->rw_readers)
            cv_wait(&rw->rw_cv, &rw->rw_mx);
        rw->rw_wwait--;
        rw->rw_readers = -1;
        rw->rw_owner = curlwp;
    } else {
        while (rw->rw_readers < 0 || rw->rw_wwait)
            cv_wait(&rw->rw_cv, &rw->rw_mx);
        rw->rw_readers++;
    }
    mutex_exit(&rw->rw_mx);
}

int rw_tryenter(krwlock_t *rw, krw_t how)
{
    int ok = 0;
    rw_mx_init(rw);
    mutex_enter(&rw->rw_mx);
    if (how == RW_WRITER && !rw->rw_readers) {
        rw->rw_readers = -1;
        rw->rw_owner = curlwp;
        ok = 1;
    } else if (how == RW_READER && rw->rw_readers >= 0 && !rw->rw_wwait) {
        rw->rw_readers++;
        ok = 1;
    }
    mutex_exit(&rw->rw_mx);
    return ok;
}

void rw_exit(krwlock_t *rw)
{
    mutex_enter(&rw->rw_mx);
    if (rw->rw_readers < 0) {
        rw->rw_readers = 0;
        rw->rw_owner = NULL;
    } else if (rw->rw_readers > 0) {
        rw->rw_readers--;
    } else {
        panic("rw_exit: %p not held", rw);
    }
    if (!rw->rw_readers)
        cv_broadcast(&rw->rw_cv);
    mutex_exit(&rw->rw_mx);
}

void rw_downgrade(krwlock_t *rw)
{
    mutex_enter(&rw->rw_mx);
    if (rw->rw_owner != curlwp)
        panic("rw_downgrade: %p not write-held by this LWP", rw);
    rw->rw_readers = 1;
    rw->rw_owner = NULL;
    cv_broadcast(&rw->rw_cv);
    mutex_exit(&rw->rw_mx);
}

int rw_tryupgrade(krwlock_t *rw)
{
    int ok = 0;
    mutex_enter(&rw->rw_mx);
    if (rw->rw_readers == 1 && !rw->rw_wwait) {
        rw->rw_readers = -1;
        rw->rw_owner = curlwp;
        ok = 1;
    }
    mutex_exit(&rw->rw_mx);
    return ok;
}

int rw_lock_held(krwlock_t *rw, int which)
{
    if (which & RW_WRITE_HELD && rw->rw_owner == curlwp)
        return 1;
    return (which & RW_READ_HELD) && rw->rw_readers > 0;
}

/* ------------------------------------------------------------------ */
/* Recursive mutexes                                                   */
/* ------------------------------------------------------------------ */

void rmutex_enter(krmutex_t *m)
{
    struct lwp *me = curlwp;
    if (m->rm_owner == me) {
        m->rm_depth++;
        return;
    }
    mutex_enter(&m->rm_mx);
    m->rm_owner = me;
    m->rm_depth = 1;
}

int rmutex_tryenter(krmutex_t *m)
{
    struct lwp *me = curlwp;
    if (m->rm_owner == me) {
        m->rm_depth++;
        return 1;
    }
    if (!mutex_tryenter(&m->rm_mx))
        return 0;
    m->rm_owner = me;
    m->rm_depth = 1;
    return 1;
}

void rmutex_exit(krmutex_t *m)
{
    if (m->rm_owner != curlwp)
        panic("rmutex_exit: %p not held by this LWP", m);
    if (--m->rm_depth)
        return;
    m->rm_owner = NULL;
    mutex_exit(&m->rm_mx);
}

int rmutex_owned(const krmutex_t *m)
{
    return m->rm_owner == curlwp;
}

int rmutex_depth(const krmutex_t *m)
{
    return m->rm_owner == curlwp ? m->rm_depth : 0;
}

int rmutex_release_all(krmutex_t *m)
{
    if (m->rm_owner != curlwp)
        return 0;
    int d = m->rm_depth;
    m->rm_depth = 0;
    m->rm_owner = NULL;
    mutex_exit(&m->rm_mx);
    return d;
}

void rmutex_reenter(krmutex_t *m, int depth)
{
    if (!depth)
        return;
    rmutex_enter(m);
    m->rm_depth = depth;
}
