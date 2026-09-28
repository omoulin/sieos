/*
 * lwp.c - ABI v2 lightweight-process calls: create/exit/wait, suspend,
 * park/unpark, the TLS pointer, user-mutex wait/wake and names.
 */
#include "proc.h"
#include "mm.h"
#include "abi2.h"
#include "sieos/lwp.h"
#include "sieos/errno.h"
#include "sieos/time.h"

static int umtx_chan;

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
    static sieos_ucontext_t uc;                      /* under the big kernel lock */
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
    if (flags & SIEOS_LWP_SUSPENDED)
        l->state = LWP_SUSPENDED;
    else
        make_runnable(l);
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
    for (;;) {
        bool found = false;
        for (int i = 0; i < NLWP; i++) {
            struct lwp *l = &lwp_table[i];
            if (l->state == LWP_UNUSED || l->proc != p || l == me || (id && l->lwpid != id))
                continue;
            if (l->detached) {
                if (id)
                    return -EINVAL;
                continue;
            }
            found = true;
            if (l->state == LWP_ZOMBIE) {
                int who = l->lwpid;
                pmm_free_contig(V2P(l->kstack), KSTACK_SIZE / PAGE_SIZE);
                memset(l, 0, sizeof(*l));
                if (departed)
                    *departed = who;
                return 0;
            }
        }
        if (!found)
            return -ESRCH;
        if (signal_pending(p))
            return -EINTR;
        sleep_on(&p->nlwp);
    }
}

long sys2_lwp_suspend(int id)
{
    struct lwp *t = lwp_find(current, id);
    if (!t || t->state == LWP_ZOMBIE)
        return -ESRCH;
    t->suspend_req = true;                           /* stops at its next return to user mode */
    return 0;
}

long sys2_lwp_continue(int id)
{
    struct lwp *t = lwp_find(current, id);
    if (!t || t->state == LWP_ZOMBIE)
        return -ESRCH;
    t->suspend_req = false;
    if (t->state == LWP_SUSPENDED)
        make_runnable(t);
    return 0;
}

static long unpark(int id)
{
    struct lwp *t = lwp_find(current, id);
    if (!t || t->state == LWP_ZOMBIE)
        return -ESRCH;
    t->park_token = true;
    wakeup(&t->park_token);
    return 0;
}

long sys2_lwp_park(const struct sieos_timespec *timeout, int unpark_first)
{
    struct lwp *l = curlwp;
    if (timeout && !user_ok(timeout, sizeof(*timeout), false))
        return -EFAULT;
    if (unpark_first)
        unpark(unpark_first);
    uint64_t deadline = timeout ? ticks + ts_to_ticks(timeout) : 0;
    for (;;) {
        if (l->park_token) {
            l->park_token = false;
            return 0;
        }
        if (signal_pending(current))
            return -EINTR;
        if (timeout && ticks >= deadline)
            return -ETIME;
        l->wake_tick = deadline;
        sleep_on(&l->park_token);
        l->wake_tick = 0;
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

/* The key of a user-mutex word: its physical address, or (pid, address) for private ones. */
static long umtx_key(uint64_t addr, int flags, uint64_t *key)
{
    if (addr & 3)
        return -EINVAL;
    if (!user_ok((void *)addr, 4, false))
        return -EFAULT;
    if (flags & SIEOS_UMTX_PRIVATE) {
        *key = ((uint64_t)current->pid << 48) | addr | 1;    /* odd: never a physical address */
        return 0;
    }
    uint64_t pa = vmm_translate(current->pml4, addr, NULL);
    if (!pa)
        return -EFAULT;
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
    if (*(volatile int *)addr != expected)
        return -EAGAIN;
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
    l->umtx_key = key;
    for (;;) {
        l->wake_tick = deadline;
        sleep_on(&umtx_chan);
        l->wake_tick = 0;
        if (!l->umtx_key)
            return 0;                                /* woken by lwp_umtx_wake */
        if (signal_pending(current)) {
            l->umtx_key = 0;
            return -EINTR;
        }
        if (timeout && ticks >= deadline) {
            l->umtx_key = 0;
            return -ETIMEDOUT;
        }
    }
}

long sys2_lwp_umtx_wake(uint64_t addr, int count, int flags)
{
    uint64_t key;
    long r = umtx_key(addr, flags, &key);
    if (r < 0)
        return r;
    int n = 0;
    for (int i = 0; i < NLWP && n < count; i++) {
        struct lwp *l = &lwp_table[i];
        if (l->state == LWP_SLEEPING && l->umtx_key == key) {
            l->umtx_key = 0;
            make_runnable(l);
            n++;
        }
    }
    return n;
}

long sys2_lwp_name(int op, int id, char *buf, size_t len)
{
    struct lwp *t = id ? lwp_find(current, id) : curlwp;
    if (!t || t->state == LWP_ZOMBIE)
        return -ESRCH;
    if (op == SIEOS_LWP_NAME_GET) {
        if (!user_ok(buf, len, true) || !len)
            return -EFAULT;
        size_t n = strlen(t->name);
        if (n + 1 > len)
            return -ERANGE;
        memcpy(buf, t->name, n + 1);
        return 0;
    }
    if (op == SIEOS_LWP_NAME_SET) {
        char name[SIEOS_LWP_NAME_MAX];
        int r = user_fetch_str(buf, name, sizeof(name));
        if (r < 0)
            return r;
        strlcpy(t->name, name, sizeof(t->name));
        return 0;
    }
    return -EINVAL;
}

/* Wake up to n waiters on a user word, whether they wait on its shared or private key. */
static void umtx_wake_both(struct proc *p, uint64_t addr, int n)
{
    uint64_t pa = vmm_translate(p->pml4, addr, NULL);
    uint64_t shared = pa & ~1UL, priv = ((uint64_t)p->pid << 48) | addr | 1;
    for (int i = 0; i < NLWP && n > 0; i++) {
        struct lwp *w = &lwp_table[i];
        if (w->state == LWP_SLEEPING && w->umtx_key && (w->umtx_key == shared || w->umtx_key == priv)) {
            w->umtx_key = 0;
            make_runnable(w);
            n--;
        }
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
