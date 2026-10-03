/*
 * ptimer.c - POSIX timers (timer_create(3C) ...: system calls 126-129, 112).
 *
 * A process has up to NPTIMER timers on CLOCK_REALTIME or CLOCK_MONOTONIC
 * (CLOCK_HIGHRES).  An expiration notifies as its sigevent says:
 *   SIEOS_SIGEV_SIGNAL  the signal, with si_code SI_TIMER, the timer's id,
 *                       the expirations missed (si_overrun) and its value;
 *   SIEOS_SIGEV_PORT    an event to an event port (port.c: PORT_SOURCE_TIMER,
 *                       the object the timer's id, the events the number
 *                       of expirations);
 *   SIEOS_SIGEV_NONE    nothing (timer_gettime shows it).
 * SIEOS_SIGEV_THREAD is the C library's, over SIGEV_PORT, as Solaris's.
 * The clock thread looks at the armed timers every tick (ptimer_tick): the
 * resolution is a tick.  Timers go at exec and at exit; fork does not copy
 * them.
 *
 * Locking: ptimer_lock covers every timer and the armed list; an expiring
 * timer signals its process under it (its p_lock after: the process cannot
 * go meanwhile, its exit deleting its timers first), or posts to its port.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "proc.h"
#include "mm.h"
#include "abi2.h"
#include "port.h"
#include "sieos/port.h"
#include "sieos/signal.h"
#include "sieos/time.h"
#include "sieos/syscall.h"

struct ptimer {
    struct ptimer *next;                         /* the armed ones */
    struct proc *p;
    int id, clock;
    int notify, signo;
    uint64_t value;                              /* sigev_value */
    struct port *port;                           /* SIGEV_PORT: held */
    void *port_user;
    bool armed;
    uint64_t when, interval;                     /* ns on its clock */
    int overrun;                                 /* expirations missed at the last notification */
};

static kmutex_t ptimer_lock;
static struct ptimer *armed;

static bool clock_ok(int clock)
{
    return clock == SIEOS_CLOCK_REALTIME || clock == SIEOS_CLOCK_MONOTONIC;
}

static uint64_t clock_ns(int clock)
{
    return clock == SIEOS_CLOCK_REALTIME ? (uint64_t)realtime_ns() : hrtime();
}

static uint64_t ts_ns(const struct sieos_timespec *t)
{
    return (uint64_t)t->tv_sec * 1000000000UL + t->tv_nsec;
}

static void ns_ts(uint64_t ns, struct sieos_timespec *t)
{
    t->tv_sec = ns / 1000000000UL;
    t->tv_nsec = ns % 1000000000UL;
}

static bool ts_ok(const struct sieos_timespec *t)
{
    return t->tv_sec >= 0 && t->tv_nsec >= 0 && t->tv_nsec < 1000000000L;
}

/* ptimer_lock held */
static void unarm(struct ptimer *t)
{
    if (!t->armed)
        return;
    for (struct ptimer **pp = &armed; *pp; pp = &(*pp)->next)
        if (*pp == t) {
            *pp = t->next;
            break;
        }
    t->next = NULL;
    t->armed = false;
}

/* ptimer_lock held: the caller's timer id. */
static struct ptimer *timer_of(long id)
{
    struct proc *p = current;
    return id >= 0 && id < NPTIMER ? p->timers[id] : NULL;
}

static long t_create(int clock, const struct sieos_sigevent *uev, int *uid)
{
    if (!clock_ok(clock))
        return -EINVAL;
    if ((uev && !user_ok(uev, sizeof(*uev), false)) || !user_ok(uid, sizeof(*uid), true))
        return -EFAULT;
    struct ptimer *t = kzalloc(sizeof(*t));
    if (!t)
        return -EAGAIN;
    t->clock = clock;
    t->notify = SIEOS_SIGEV_SIGNAL;              /* (no sigevent: SIGALRM, the timer's id as value) */
    t->signo = SIGALRM;
    if (uev) {
        struct sieos_sigevent ev = *uev;
        t->notify = ev.sigev_notify;
        t->signo = ev.sigev_signo;
        t->value = (uint64_t)ev.sigev_value.sival_ptr;
        long r = 0;
        if (t->notify == SIEOS_SIGEV_SIGNAL) {
            if (t->signo <= 0 || t->signo >= KNSIG)
                r = -EINVAL;
        } else if (t->notify == SIEOS_SIGEV_PORT) {
            const sieos_port_notify_t *pn = ev.sigev_value.sival_ptr;
            if (!user_ok(pn, sizeof(*pn), false)) {
                r = -EFAULT;
            } else {
                t->port_user = pn->portnfy_user;
                t->port = port_of_fd(pn->portnfy_port);
                if (!t->port)
                    r = -EBADF;
            }
        } else if (t->notify != SIEOS_SIGEV_NONE) {
            r = -EINVAL;                         /* (SIGEV_THREAD: the C library's) */
        }
        if (r < 0) {
            kfree(t);
            return r;
        }
    }
    struct proc *p = current;
    mutex_enter(&ptimer_lock);
    int id = -1;
    for (int i = 0; i < NPTIMER && id < 0; i++)
        if (!p->timers[i])
            id = i;
    if (id >= 0) {
        t->id = id;
        t->p = p;
        if (!uev)
            t->value = id;
        p->timers[id] = t;
    }
    mutex_exit(&ptimer_lock);
    if (id < 0) {
        if (t->port)
            port_rele(t->port);
        kfree(t);
        return -EAGAIN;
    }
    *uid = id;
    return 0;
}

static long t_delete(long id)
{
    mutex_enter(&ptimer_lock);
    struct ptimer *t = timer_of(id);
    if (t) {
        unarm(t);
        current->timers[id] = NULL;
    }
    mutex_exit(&ptimer_lock);
    if (!t)
        return -EINVAL;
    if (t->port)
        port_rele(t->port);
    kfree(t);
    return 0;
}

/* ptimer_lock held */
static void get_locked(struct ptimer *t, struct sieos_itimerspec *cur)
{
    uint64_t now = clock_ns(t->clock);
    ns_ts(t->interval, &cur->it_interval);
    ns_ts(t->armed && t->when > now ? t->when - now : (t->armed ? 1 : 0), &cur->it_value);
}

static long t_settime(long id, long flags, const struct sieos_itimerspec *unew, struct sieos_itimerspec *uold)
{
    if (!user_ok(unew, sizeof(*unew), false) || (uold && !user_ok(uold, sizeof(*uold), true)))
        return -EFAULT;
    struct sieos_itimerspec nv = *unew, ov;
    if (!ts_ok(&nv.it_value) || !ts_ok(&nv.it_interval))
        return -EINVAL;
    mutex_enter(&ptimer_lock);
    struct ptimer *t = timer_of(id);
    if (!t) {
        mutex_exit(&ptimer_lock);
        return -EINVAL;
    }
    get_locked(t, &ov);
    unarm(t);
    uint64_t v = ts_ns(&nv.it_value);
    t->interval = ts_ns(&nv.it_interval);
    t->overrun = 0;
    if (v) {
        t->when = (flags & SIEOS_TIMER_ABSTIME) ? v : clock_ns(t->clock) + v;
        t->armed = true;
        t->next = armed;
        armed = t;
    }
    mutex_exit(&ptimer_lock);
    if (uold)
        *uold = ov;
    return 0;
}

static long t_gettime(long id, struct sieos_itimerspec *ucur)
{
    if (!user_ok(ucur, sizeof(*ucur), true))
        return -EFAULT;
    struct sieos_itimerspec cur;
    mutex_enter(&ptimer_lock);
    struct ptimer *t = timer_of(id);
    if (t)
        get_locked(t, &cur);
    mutex_exit(&ptimer_lock);
    if (!t)
        return -EINVAL;
    *ucur = cur;
    return 0;
}

static long t_getoverrun(long id)
{
    mutex_enter(&ptimer_lock);
    struct ptimer *t = timer_of(id);
    long r = t ? t->overrun : -EINVAL;
    mutex_exit(&ptimer_lock);
    return r;
}

/* The clock thread, every tick: the timers that expired notify. */
void ptimer_tick(void)
{
    if (!__atomic_load_n(&armed, __ATOMIC_RELAXED))
        return;
    mutex_enter(&ptimer_lock);
    for (struct ptimer **pp = &armed; *pp;) {
        struct ptimer *t = *pp;
        uint64_t now = clock_ns(t->clock);
        if (now < t->when) {
            pp = &t->next;
            continue;
        }
        uint64_t k = 1;
        if (t->interval) {
            k += (now - t->when) / t->interval;
            t->when += k * t->interval;
            pp = &t->next;
        } else {
            *pp = t->next;                       /* a one-shot: disarmed */
            t->next = NULL;
            t->armed = false;
        }
        t->overrun = k > 1 ? (int)MIN(k - 1, (uint64_t)INT32_MAX) : 0;
        if (t->notify == SIEOS_SIGEV_SIGNAL) {
            struct ksiginfo info = { .code = SIEOS_SI_TIMER, .pid = t->id, .uid = t->overrun, .value = t->value };
            signal_send_info(t->p, t->signo, &info);   /* (si_timerid, si_overrun: siginfo's timer fields) */
        } else if (t->notify == SIEOS_SIGEV_PORT) {
            port_post_timer(t->port, (int)MIN(k, (uint64_t)INT32_MAX), t->id, t->port_user);
        }
    }
    mutex_exit(&ptimer_lock);
}

/* Exit and exec: the process's timers go. */
void ptimer_proc_exit(struct proc *p)
{
    struct ptimer *gone[NPTIMER];
    int n = 0;
    mutex_enter(&ptimer_lock);
    for (int i = 0; i < NPTIMER; i++)
        if (p->timers[i]) {
            unarm(p->timers[i]);
            gone[n++] = p->timers[i];
            p->timers[i] = NULL;
        }
    mutex_exit(&ptimer_lock);
    for (int i = 0; i < n; i++) {
        if (gone[i]->port)
            port_rele(gone[i]->port);
        kfree(gone[i]);
    }
}

long sys2_timer(struct trapframe *tf, bool *handled)
{
    uint64_t a1 = tf->rdi, a2 = tf->rsi, a3 = tf->rdx, a4 = tf->r10;
    *handled = true;
    switch (tf->rax) {
    case SIEOS_SYS_timer_create:     return t_create((int)a1, (const struct sieos_sigevent *)a2, (int *)a3);
    case SIEOS_SYS_timer_delete:     return t_delete((long)a1);
    case SIEOS_SYS_timer_settime:
        return t_settime((long)a1, (long)a2, (const struct sieos_itimerspec *)a3, (struct sieos_itimerspec *)a4);
    case SIEOS_SYS_timer_gettime:    return t_gettime((long)a1, (struct sieos_itimerspec *)a2);
    case SIEOS_SYS_timer_getoverrun: return t_getoverrun((long)a1);
    case SIEOS_SYS_portfs:           return sys2_portfs(tf);
    }
    *handled = false;
    return 0;
}
