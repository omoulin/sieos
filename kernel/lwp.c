/*
 * lwp.c - ABI v2 lightweight-process calls: create/exit/wait, suspend,
 * park/unpark, the TLS pointer, user-mutex wait/wake and names.
 *
 * The process's LWPs are under its p_lock (proc.c).  User-mutex waiters
 * wait on a hash of their keys: a bucket's mutex covers the test of the
 * user word and the sleep, and the wakes for its keys.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "proc.h"
#include "mm.h"
#include "abi2.h"
#include "sieos/lwp.h"
#include "sieos/errno.h"
#include "sieos/time.h"

#define UMTX_HASH 64
static struct umtxq {
    kmutex_t lock;
    kcondvar_t cv;
} umtxq[UMTX_HASH];

static struct umtxq *umtxq_of(uint64_t key)
{
    return &umtxq[(key ^ (key >> 13) ^ (key >> 29)) % UMTX_HASH];
}

static uint64_t ts_to_ticks(const struct sieos_timespec *ts)
{
    uint64_t ns = (uint64_t)ts->tv_sec * 1000000000UL + ts->tv_nsec;
    return (ns * TIMER_HZ + 999999999UL) / 1000000000UL;
}

long sys2_lwp_create(const sieos_ucontext_t *ucp, int flags, sieos_lwpid_t *idp)
{
    if (!user_ok(ucp, sizeof(*ucp), false) || (idp && !user_ok(idp, sizeof(*idp), true)))
        return -EFAULT;
    if (flags & ~(SIEOS_LWP_DAEMON | SIEOS_LWP_DETACHED | SIEOS_LWP_SUSPENDED))
        return -EINVAL;
    sieos_ucontext_t uc;
    memcpy(&uc, ucp, sizeof(uc));
    uint64_t rip = uc.uc_mcontext.gregs[SIEOS_REG_RIP], rsp = uc.uc_mcontext.gregs[SIEOS_REG_RSP];
    if (rip >= USER_LIMIT || rsp >= USER_LIMIT)
        return -EINVAL;
    struct lwp *l = lwp_alloc(current);
    if (!l)
        return -EAGAIN;
    sig_gregs_to_tf(uc.uc_mcontext.gregs, l->tf);
    l->fsbase = uc.uc_mcontext.gregs[SIEOS_REG_FSBASE];
    l->sig_blocked = sig_set_from_v2(&uc.uc_sigmask);
    l->detached = flags & SIEOS_LWP_DETACHED;
    if (idp)
        *idp = l->lwpid;
    if (flags & SIEOS_LWP_SUSPENDED) {
        disp_enter();
        l->state = LWP_SUSPENDED;
        disp_exit();
    } else {
        make_runnable(l);
    }
    return 0;
}

long sys2_lwp_wait(int id, sieos_lwpid_t *departed)
{
    struct lwp *me = curlwp;
    struct proc *p = current;
    if (departed && !user_ok(departed, sizeof(*departed), true))
        return -EFAULT;
    if (id == me->lwpid)
        return -EDEADLK;
    mutex_enter(&p->p_lock);
    for (;;) {
        bool found = false;
        for (int i = 0; i < NLWP; i++) {
            struct lwp *l = &lwp_table[i];
            if (l->state == LWP_UNUSED || l->proc != p || l == me || (id && l->lwpid != id))
                continue;
            if (l->detached) {
                if (id) {
                    mutex_exit(&p->p_lock);
                    return -EINVAL;
                }
                continue;
            }
            found = true;
            if (l->state == LWP_ZOMBIE) {
                int who = l->lwpid;
                lwp_free(l);                     /* (waits until it has switched away) */
                mutex_exit(&p->p_lock);
                if (departed)
                    *departed = who;
                return 0;
            }
        }
        if (!found) {
            mutex_exit(&p->p_lock);
            return -ESRCH;
        }
        if (!cv_wait_sig(&p->p_lwpcv, &p->p_lock)) {
            mutex_exit(&p->p_lock);
            return -EINTR;
        }
    }
}

long sys2_lwp_suspend(int id)
{
    struct proc *p = current;
    mutex_enter(&p->p_lock);
    struct lwp *t = lwp_find(p, id);
    long r = !t || t->state == LWP_ZOMBIE ? -ESRCH : 0;
    if (!r)
        t->suspend_req = true;                       /* stops at its next return to user mode */
    mutex_exit(&p->p_lock);
    return r;
}

long sys2_lwp_continue(int id)
{
    struct proc *p = current;
    mutex_enter(&p->p_lock);
    struct lwp *t = lwp_find(p, id);
    long r = !t || t->state == LWP_ZOMBIE ? -ESRCH : 0;
    if (!r) {
        t->suspend_req = false;
        if (t->state == LWP_SUSPENDED)
            make_runnable(t);
    }
    mutex_exit(&p->p_lock);
    return r;
}

static long unpark(int id)
{
    struct proc *p = current;
    mutex_enter(&p->p_lock);
    struct lwp *t = lwp_find(p, id);
    long r = !t || t->state == LWP_ZOMBIE ? -ESRCH : 0;
    if (!r) {
        t->park_token = true;
        sleepq_wakeup(&t->park_token, -1);
    }
    mutex_exit(&p->p_lock);
    return r;
}

long sys2_lwp_park(const struct sieos_timespec *timeout, int unpark_first)
{
    struct lwp *l = curlwp;
    if (timeout && !user_ok(timeout, sizeof(*timeout), false))
        return -EFAULT;
    if (unpark_first)
        unpark(unpark_first);
    uint64_t deadline = timeout ? ticks + ts_to_ticks(timeout) : 0;
    struct proc *p = l->proc;
    mutex_enter(&p->p_lock);
    for (;;) {
        long r = 1;
        if (l->park_token) {
            l->park_token = false;
            r = 0;
        } else if (signal_pending(p)) {
            r = -EINTR;
        } else if (timeout && ticks >= deadline) {
            r = -ETIME;
        }
        if (r <= 0) {
            mutex_exit(&p->p_lock);
            return r;
        }
        l->wake_tick = deadline;
        sleepq_block(&l->park_token, &p->p_lock, true);
        l->wake_tick = 0;
        mutex_enter(&p->p_lock);
    }
}

long sys2_lwp_unpark(int id)
{
    return unpark(id);
}

long sys2_lwp_unpark_all(const sieos_lwpid_t *ids, int n)
{
    if (n < 0 || n > 1024 || (n && !user_ok(ids, n * sizeof(*ids), false)))
        return n < 0 || n > 1024 ? -EINVAL : -EFAULT;
    for (int i = 0; i < n; i++)
        unpark(ids[i]);
    return 0;
}

long sys2_lwp_private(int op, int which, uint64_t base)
{
    struct lwp *l = curlwp;
    if (which == SIEOS_LWP_EXITWORD || which == SIEOS_LWP_ROBUSTLIST) {
        uint64_t *slot = which == SIEOS_LWP_EXITWORD ? &l->exit_word : &l->robust_list;
        if (op == SIEOS_LWP_GETPRIVATE)
            return (long)*slot;
        if (op != SIEOS_LWP_SETPRIVATE || (base & (which == SIEOS_LWP_EXITWORD ? 3 : 7)) || base >= USER_LIMIT)
            return -EINVAL;
        *slot = base;
        return 0;
    }
    if (which != SIEOS_LWP_FSBASE)
        return -EINVAL;                              /* %gs belongs to the kernel */
    if (op == SIEOS_LWP_GETPRIVATE)
        return (long)l->fsbase;
    if (op != SIEOS_LWP_SETPRIVATE || base >= USER_LIMIT)
        return -EINVAL;
    l->fsbase = base;
    wrmsr(MSR_FS_BASE, base);
    return 0;
}

/*
 * The key of a user-mutex word: (pid, address) for a private one, or one in
 * the process's own memory; its physical address only in memory shared with
 * other processes (MAP_SHARED, shm).  (As Linux: private memory turns
 * copy-on-write at fork, and a store to the word between a wait and its wake
 * moved it to another page, the wake missing the waiter.)
 */
static long umtx_key(uint64_t addr, int flags, uint64_t *key)
{
    if (addr & 3)
        return -EINVAL;
    if (!user_ok((void *)addr, 4, false))
        return -EFAULT;
    uint64_t fl = 0, pa = (flags & SIEOS_UMTX_PRIVATE) ? 0 : vmm_translate(current->pml4, addr, &fl);
    if (!pa || !(fl & PTE_SHARED)) {
        *key = ((uint64_t)current->pid << 48) | addr | 1;    /* odd: never a physical address */
        return 0;
    }
    *key = pa & ~1UL;
    return 0;
}

long sys2_lwp_umtx_wait(uint64_t addr, int expected, const struct sieos_timespec *timeout, int flags)
{
    uint64_t key;
    long r = umtx_key(addr, flags, &key);
    if (r < 0)
        return r;
    if (timeout && !user_ok(timeout, sizeof(*timeout), false))
        return -EFAULT;
    uint64_t deadline = 0;
    if (timeout) {
        if (flags & SIEOS_UMTX_ABSTIME) {
            long now = kernel_time();
            struct sieos_timespec rel = { timeout->tv_sec - now, timeout->tv_nsec };
            if (rel.tv_sec < 0)
                return -ETIMEDOUT;
            deadline = ticks + ts_to_ticks(&rel);
        } else {
            deadline = ticks + ts_to_ticks(timeout);
        }
    }
    struct lwp *l = curlwp;
    struct umtxq *q = umtxq_of(key);
    mutex_enter(&q->lock);
    if (*(volatile int *)addr != expected) {         /* (tested under the bucket's lock: a wake cannot pass between) */
        mutex_exit(&q->lock);
        return -EAGAIN;
    }
    l->umtx_key = key;
    r = 0;
    for (;;) {
        l->wake_tick = deadline;
        sleepq_block(&l->umtx_key, &q->lock, true);
        l->wake_tick = 0;
        mutex_enter(&q->lock);
        if (!l->umtx_key) {
            r = 0;                                   /* woken by lwp_umtx_wake */
            break;
        }
        if (signal_pending(current)) {
            r = -EINTR;
            break;
        }
        if (timeout && ticks >= deadline) {
            r = -ETIMEDOUT;
            break;
        }
    }
    l->umtx_key = 0;
    mutex_exit(&q->lock);
    return r;
}

/* q's lock held: wake up to n waiters on key; how many. */
static int umtx_wake_key(uint64_t key, int n)
{
    int woken = 0;
    for (int i = 0; i < NLWP && woken < n; i++) {
        struct lwp *l = &lwp_table[i];
        /* (one inside the wait, asleep or not: a signal may have made it runnable,
         * and skipping it then lost this wake when it went back to sleep) */
        if (l->umtx_key == key && l->state != LWP_UNUSED && l->state != LWP_ZOMBIE) {
            l->umtx_key = 0;
            sleepq_wakeup(&l->umtx_key, -1);
            woken++;
        }
    }
    return woken;
}

long sys2_lwp_umtx_wake(uint64_t addr, int count, int flags)
{
    uint64_t key;
    long r = umtx_key(addr, flags, &key);
    if (r < 0)
        return r;
    struct umtxq *q = umtxq_of(key);
    mutex_enter(&q->lock);
    int n = umtx_wake_key(key, count);
    mutex_exit(&q->lock);
    return n;
}

long sys2_lwp_name(int op, int id, char *buf, size_t len)
{
    struct proc *p = current;
    char name[SIEOS_LWP_NAME_MAX];
    if (op == SIEOS_LWP_NAME_GET) {
        if (!user_ok(buf, len, true) || !len)
            return -EFAULT;
    } else if (op == SIEOS_LWP_NAME_SET) {
        int r = user_fetch_str(buf, name, sizeof(name));
        if (r < 0)
            return r;
    } else {
        return -EINVAL;
    }
    char cur[32];
    mutex_enter(&p->p_lock);
    struct lwp *t = id ? lwp_find(p, id) : curlwp;
    if (!t || t->state == LWP_ZOMBIE) {
        mutex_exit(&p->p_lock);
        return -ESRCH;
    }
    if (op == SIEOS_LWP_NAME_SET)
        strlcpy(t->name, name, sizeof(t->name));
    else
        strlcpy(cur, t->name, sizeof(cur));
    mutex_exit(&p->p_lock);
    if (op == SIEOS_LWP_NAME_GET) {
        size_t n = strlen(cur);
        if (n + 1 > len)
            return -ERANGE;
        memcpy(buf, cur, n + 1);
    }
    return 0;
}

/* Wake up to n waiters on a user word, whether they wait on its shared or private key. */
static void umtx_wake_both(struct proc *p, uint64_t addr, int n)
{
    uint64_t pa = vmm_translate(p->pml4, addr, NULL);
    uint64_t shared = pa & ~1UL, priv = ((uint64_t)p->pid << 48) | addr | 1;
    struct umtxq *q = umtxq_of(priv);
    mutex_enter(&q->lock);
    n -= umtx_wake_key(priv, n);
    mutex_exit(&q->lock);
    if (n > 0 && pa) {
        q = umtxq_of(shared);
        mutex_enter(&q->lock);
        umtx_wake_key(shared, n);
        mutex_exit(&q->lock);
    }
}

/* A robust lock word owned by the dying LWP: mark it OWNER_DIED, wake one waiter. */
static void robust_death(struct lwp *l, uint64_t word)
{
    struct proc *p = l->proc;
    if ((word & 3) || !user_range_ok(p->pml4, word, 4, true))
        return;
    volatile uint32_t *w = (volatile uint32_t *)word;
    uint32_t old = *w;
    for (;;) {
        if ((old & SIEOS_ROBUST_TID_MASK) != (uint32_t)l->lwpid)
            return;
        uint32_t nw = (old & SIEOS_ROBUST_WAITERS) | SIEOS_ROBUST_OWNER_DIED;
        if (__atomic_compare_exchange_n(w, &old, nw, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            break;
    }
    if (old & SIEOS_ROBUST_WAITERS)
        umtx_wake_both(p, word, 1);
}

static void lwp_exit_robust(struct lwp *l)
{
    struct proc *p = l->proc;
    uint64_t head = l->robust_list;
    l->robust_list = 0;
    if (!head || !user_range_ok(p->pml4, head, sizeof(struct sieos_robust_list_head), false))
        return;
    struct sieos_robust_list_head *h = (struct sieos_robust_list_head *)head;
    long off = h->futex_offset;
    uint64_t pending = (uint64_t)h->list_op_pending;
    uint64_t e = (uint64_t)h->list.next;
    for (int limit = 0; e && e != head && limit < 2048; limit++) {
        if (!user_range_ok(p->pml4, e, 8, false))
            break;
        uint64_t next = (uint64_t)((struct sieos_robust_list *)e)->next;
        if (e != pending)
            robust_death(l, e + off);
        e = next;
    }
    if (pending)
        robust_death(l, pending + off);
}

/* An exiting LWP: its robust mutexes, then its exit word (store 0, wake every waiter). */
void lwp_exit_word(struct lwp *l)
{
    lwp_exit_robust(l);
    uint64_t addr = l->exit_word;
    struct proc *p = l->proc;
    l->exit_word = 0;
    if (!addr || !user_range_ok(p->pml4, addr, 4, true))
        return;
    *(volatile int *)addr = 0;
    umtx_wake_both(p, addr, NLWP);
}
