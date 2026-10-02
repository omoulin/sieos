/*
 * sysmisc2.c - ABI v2 time, resource and system calls: clocks and
 * high-resolution time, interval timers, times, stime/adjtime, resource
 * limits and usage, pollsys, sysconfig, uadmin, and the processor calls
 * (processor_info, p_online, processor_bind, getloadavg).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "proc.h"
#include "jbd2.h"
#include "net.h"
#include "mm.h"
#include "fs.h"
#include "poll.h"
#include "abi2.h"
#include "sieos/syscall.h"
#include "sieos/errno.h"
#include "sieos/time.h"
#include "sieos/sysinfo.h"
#include "sieos/signal.h"
#include "sieos/poll.h"
#include "sieos/wait.h"

#define NS_PER_TICK (1000000000UL / TIMER_HZ)

static bool is_root(void)
{
    return current->euid == 0;
}

static void ns_to_ts(uint64_t ns, struct sieos_timespec *ts)
{
    ts->tv_sec = ns / 1000000000UL;
    ts->tv_nsec = ns % 1000000000UL;
}

static void ticks_to_tv(uint64_t t, struct sieos_timeval *tv)
{
    tv->tv_sec = t / TIMER_HZ;
    tv->tv_usec = (t % TIMER_HZ) * (1000000 / TIMER_HZ);
}

static uint64_t tv_to_ticks(const struct sieos_timeval *tv)
{
    uint64_t us = (uint64_t)tv->tv_sec * 1000000UL + tv->tv_usec;
    uint64_t t = (us * TIMER_HZ + 999999) / 1000000;
    return us && !t ? 1 : t;
}

/* CPU ticks of the calling process: exited LWPs plus the live ones. */
/* (the caller's process: its p_lock is taken here) */
static void proc_times(struct proc *p, uint64_t *total, uint64_t *sys)
{
    mutex_enter(&p->p_lock);
    uint64_t t = p->ticks, s = p->ru.sticks;
    for (int i = 0; i < NLWP; i++) {
        struct lwp *l = &lwp_table[i];
        if (l->proc == p && l->state != LWP_UNUSED && l->state != LWP_ZOMBIE) {
            t += l->ticks;
            s += l->sticks;
        }
    }
    mutex_exit(&p->p_lock);
    *total = t;
    *sys = s;
}

/* ---------------- clocks ---------------- */

/*
 * A process's or thread's CPU clock by id, as the C library makes them
 * (Linux's encoding, pthread_getcpuclockid, clock_getcpuclockid): -(id + 1) * 8
 * plus 2 for a process, 6 for a thread.  This process and its own threads.
 * Its CPU time in *ns; false: no such clock.
 */
static bool cpu_clock(long clk, uint64_t *ns)
{
    if (clk >= 0 || ((clk & 7) != 2 && (clk & 7) != 6))
        return false;
    long id = ~(clk >> 3);
    if ((clk & 7) == 2) {
        if (id != 0 && id != current->pid)
            return false;
        uint64_t t, s;
        proc_times(current, &t, &s);
        *ns = t * NS_PER_TICK;
        return true;
    }
    for (int i = 0; i < NLWP; i++) {
        struct lwp *l = &lwp_table[i];
        if (l->state != LWP_UNUSED && l->state != LWP_ZOMBIE && l->proc == current && l->lwpid == id) {
            *ns = l->ticks * NS_PER_TICK;
            return true;
        }
    }
    return false;
}

static long do_clock_gettime(long clk, struct sieos_timespec *uts)
{
    if (!user_ok(uts, sizeof(*uts), true))
        return -EFAULT;
    struct sieos_timespec ts;
    uint64_t t, s;
    switch (clk) {
    case SIEOS_CLOCK_REALTIME: {
        int64_t ns = realtime_ns();
        ts.tv_sec = ns / 1000000000L;
        ts.tv_nsec = ns % 1000000000L;
        break;
    }
    case SIEOS_CLOCK_MONOTONIC:
        ns_to_ts(hrtime(), &ts);
        break;
    case SIEOS_CLOCK_PROCESS_CPUTIME_ID:
        proc_times(current, &t, &s);
        ns_to_ts(t * NS_PER_TICK, &ts);
        break;
    case SIEOS_CLOCK_THREAD_CPUTIME_ID:
        ns_to_ts(curlwp->ticks * NS_PER_TICK, &ts);
        break;
    case SIEOS_CLOCK_VIRTUAL:                    /* the LWP's user time */
        ns_to_ts((curlwp->ticks - curlwp->sticks) * NS_PER_TICK, &ts);
        break;
    default:
        if (!cpu_clock(clk, &t))
            return -EINVAL;
        ns_to_ts(t, &ts);
        break;
    }
    memcpy(uts, &ts, sizeof(ts));
    return 0;
}

static long do_clock_settime(long clk, const struct sieos_timespec *uts)
{
    if (!user_ok(uts, sizeof(*uts), false))
        return -EFAULT;
    if (clk != SIEOS_CLOCK_REALTIME)
        return -EINVAL;
    if (!is_root())
        return -EPERM;
    if (uts->tv_nsec < 0 || uts->tv_nsec >= 1000000000L || uts->tv_sec < 0)
        return -EINVAL;
    realtime_set(uts->tv_sec * 1000000000L + uts->tv_nsec);
    return 0;
}

static long do_clock_getres(long clk, struct sieos_timespec *u)
{
    long res;
    switch (clk) {
    case SIEOS_CLOCK_REALTIME:
    case SIEOS_CLOCK_MONOTONIC:
        res = tsc_hz ? 1 : (long)NS_PER_TICK;
        break;
    case SIEOS_CLOCK_PROCESS_CPUTIME_ID:
    case SIEOS_CLOCK_THREAD_CPUTIME_ID:
    case SIEOS_CLOCK_VIRTUAL:
        res = NS_PER_TICK;
        break;
    default: {
        uint64_t ns;
        if (!cpu_clock(clk, &ns))
            return -EINVAL;
        res = NS_PER_TICK;
        break;
    }
    }
    if (u) {
        if (!user_ok(u, sizeof(*u), true))
            return -EFAULT;
        struct sieos_timespec r = { 0, res };
        memcpy(u, &r, sizeof(r));
    }
    return 0;
}

/* ---------------- interval timers ---------------- */

static int itimer_index(long which)
{
    switch (which) {
    case SIEOS_ITIMER_REAL:     return 0;
    case SIEOS_ITIMER_VIRTUAL:  return 1;
    case SIEOS_ITIMER_PROF:
    case SIEOS_ITIMER_REALPROF: return 2;
    }
    return -1;
}

static void itimer_get(struct proc *p, int i, struct sieos_itimerval *v)
{
    ticks_to_tv(p->itimer_value[i], &v->it_value);
    ticks_to_tv(p->itimer_interval[i], &v->it_interval);
}

static long do_getitimer(long which, struct sieos_itimerval *u)
{
    int i = itimer_index(which);
    if (i < 0)
        return -EINVAL;
    if (!user_ok(u, sizeof(*u), true))
        return -EFAULT;
    struct sieos_itimerval v;
    mutex_enter(&current->p_lock);
    itimer_get(current, i, &v);
    mutex_exit(&current->p_lock);
    memcpy(u, &v, sizeof(v));
    return 0;
}

static long do_setitimer(long which, const struct sieos_itimerval *un, struct sieos_itimerval *uo)
{
    int i = itimer_index(which);
    if (i < 0)
        return -EINVAL;
    if ((un && !user_ok(un, sizeof(*un), false)) || (uo && !user_ok(uo, sizeof(*uo), true)))
        return -EFAULT;
    struct proc *p = current;
    struct sieos_itimerval old, n = { { 0, 0 }, { 0, 0 } };
    if (un) {
        n = *un;
        if (n.it_value.tv_sec < 0 || n.it_value.tv_usec < 0 || n.it_value.tv_usec >= 1000000 ||
            n.it_interval.tv_sec < 0 || n.it_interval.tv_usec < 0 || n.it_interval.tv_usec >= 1000000)
            return -EINVAL;
    }
    mutex_enter(&p->p_lock);
    itimer_get(p, i, &old);
    if (un) {
        uint64_t v = tv_to_ticks(&n.it_value);
        p->itimer_interval[i] = v ? tv_to_ticks(&n.it_interval) : 0;
        __atomic_store_n(&p->itimer_value[i], v, __ATOMIC_RELAXED);   /* (the clock counts it down) */
    }
    mutex_exit(&p->p_lock);
    if (uo)
        memcpy(uo, &old, sizeof(old));
    return 0;
}

static long do_times(struct sieos_tms *u)
{
    if (u) {
        if (!user_ok(u, sizeof(*u), true))
            return -EFAULT;
        uint64_t t, s;
        proc_times(current, &t, &s);
        struct sieos_tms v;
        v.tms_utime = t - s;
        v.tms_stime = s;
        mutex_enter(&current->p_lock);
        v.tms_cutime = current->child_ticks - current->cru.sticks;
        v.tms_cstime = current->cru.sticks;
        mutex_exit(&current->p_lock);
        memcpy(u, &v, sizeof(v));
    }
    return ticks;                                /* elapsed time since boot, in CLK_TCK units */
}

static long do_adjtime(const struct sieos_timeval *ud, struct sieos_timeval *uo)
{
    if ((ud && !user_ok(ud, sizeof(*ud), false)) || (uo && !user_ok(uo, sizeof(*uo), true)))
        return -EFAULT;
    if (ud && !is_root())
        return -EPERM;
    int64_t delta = ud ? (int64_t)ud->tv_sec * 1000000000L + (int64_t)ud->tv_usec * 1000 : 0;
    int64_t old = realtime_adjust(delta, ud != NULL);
    if (uo) {
        struct sieos_timeval o = { old / 1000000000L, (old % 1000000000L) / 1000 };
        memcpy(uo, &o, sizeof(o));
    }
    return 0;
}

/* ---------------- resources ---------------- */

static uint64_t rl(uint64_t v)
{
    return v == SIEOS_RLIM_INFINITY ? UINT64_MAX : v;
}

static long do_getrlimit(long res, struct sieos_rlimit *u)
{
    if (res < 0 || res >= SIEOS_RLIM_NLIMITS)
        return -EINVAL;
    if (!user_ok(u, sizeof(*u), true))
        return -EFAULT;
    mutex_enter(&current->p_lock);
    struct sieos_rlimit r = { current->rlim_cur[res], current->rlim_max[res] };
    mutex_exit(&current->p_lock);
    memcpy(u, &r, sizeof(r));
    return 0;
}

static long setrlimit_locked(struct proc *p, long res, struct sieos_rlimit r);

static long do_setrlimit(long res, const struct sieos_rlimit *u)
{
    if (res < 0 || res >= SIEOS_RLIM_NLIMITS)
        return -EINVAL;
    if (!user_ok(u, sizeof(*u), false))
        return -EFAULT;
    struct sieos_rlimit r = *u;
    struct proc *p = current;
    mutex_enter(&p->p_lock);
    long e = setrlimit_locked(p, res, r);
    mutex_exit(&p->p_lock);
    return e;
}

static long setrlimit_locked(struct proc *p, long res, struct sieos_rlimit r)
{
    if (r.rlim_cur == SIEOS_RLIM_SAVED_CUR)
        r.rlim_cur = p->rlim_cur[res];
    else if (r.rlim_cur == SIEOS_RLIM_SAVED_MAX)
        r.rlim_cur = p->rlim_max[res];
    if (r.rlim_max == SIEOS_RLIM_SAVED_CUR)
        r.rlim_max = p->rlim_cur[res];
    else if (r.rlim_max == SIEOS_RLIM_SAVED_MAX)
        r.rlim_max = p->rlim_max[res];
    /* RLIM_INFINITY is the largest value */
    uint64_t cur = r.rlim_cur, max = r.rlim_max;
    if (rl(cur) > rl(max))
        return -EINVAL;
    if (rl(max) > rl(p->rlim_max[res]) && !is_root())
        return -EPERM;
    if (res == SIEOS_RLIMIT_STACK && cur != SIEOS_RLIM_INFINITY && cur < 64 * 1024)
        return -EINVAL;
    p->rlim_cur[res] = cur;
    p->rlim_max[res] = max;
    if (res == SIEOS_RLIMIT_CPU)
        p->cpu_limit_sent = 0;
    return 0;
}

static long do_getrusage(long who, struct sieos_rusage *u)
{
    if (!user_ok(u, sizeof(*u), true))
        return -EFAULT;
    struct proc *p = current;
    struct sieos_rusage r;
    memset(&r, 0, sizeof(r));
    uint64_t t, s;
    long rss = p->pml4 && p->pml4 != kernel_pml4_phys ? (long)vmm_user_pages(p->pml4) * 4 : 0;
    if ((uint64_t)rss > p->maxrss_kb)
        p->maxrss_kb = rss;
    switch (who) {
    case SIEOS_RUSAGE_SELF:
        proc_times(p, &t, &s);
        mutex_enter(&p->p_lock);
        r.ru_nvcsw = p->ru.nvcsw;
        r.ru_nivcsw = p->ru.nivcsw;
        r.ru_minflt = p->ru.minflt;
        for (int i = 0; i < NLWP; i++) {
            struct lwp *l = &lwp_table[i];
            if (l->proc == p && l->state != LWP_UNUSED && l->state != LWP_ZOMBIE) {
                r.ru_nvcsw += l->nvcsw;
                r.ru_nivcsw += l->nivcsw;
                r.ru_minflt += l->minflt;
            }
        }
        r.ru_maxrss = p->maxrss_kb;
        mutex_exit(&p->p_lock);
        break;
    case SIEOS_RUSAGE_CHILDREN:
        mutex_enter(&p->p_lock);
        t = p->child_ticks;
        s = p->cru.sticks;
        r.ru_nvcsw = p->cru.nvcsw;
        r.ru_nivcsw = p->cru.nivcsw;
        r.ru_minflt = p->cru.minflt;
        mutex_exit(&p->p_lock);
        break;
    case SIEOS_RUSAGE_LWP: {
        struct lwp *l = curlwp;
        t = l->ticks;
        s = l->sticks;
        r.ru_nvcsw = l->nvcsw;
        r.ru_nivcsw = l->nivcsw;
        r.ru_minflt = l->minflt;
        r.ru_maxrss = p->maxrss_kb;
        break;
    }
    default:
        return -EINVAL;
    }
    ticks_to_tv(t - s, &r.ru_utime);
    ticks_to_tv(s, &r.ru_stime);
    memcpy(u, &r, sizeof(r));
    return 0;
}

/* ---------------- pollsys ---------------- */

static long do_pollsys(struct pollfd *fds, long nfds, const struct sieos_timespec *uts, const sieos_sigset_t *umask)
{
    if ((uts && !user_ok(uts, sizeof(*uts), false)) || (umask && !user_ok(umask, sizeof(*umask), false)))
        return -EFAULT;
    int ms = -1;
    if (uts) {
        if (uts->tv_sec < 0 || uts->tv_nsec < 0 || uts->tv_nsec >= 1000000000L)
            return -EINVAL;
        uint64_t m = (uint64_t)uts->tv_sec * 1000 + (uts->tv_nsec + 999999) / 1000000;
        ms = m > 0x7FFFFFFF ? 0x7FFFFFFF : (int)m;
    }
    struct lwp *l = curlwp;
    ksigset_t old = l->sig_blocked;
    if (umask)
        l->sig_blocked = sig_set_from_v2(umask) & ~(KSIGBIT(SIGKILL) | KSIGBIT(SIGSTOP));
    long r = sys_poll(fds, nfds, ms);
    if (umask) {
        if (r == -EINTR) {                       /* restore after the handler runs (as sigsuspend) */
            l->saved_mask = old;
            l->saved_mask_valid = true;
        } else {
            l->sig_blocked = old;
        }
    }
    return r;
}

/* ---------------- system ---------------- */

static long do_sysconfig(long name)
{
    int online = 0;
    for (int i = 0; i < ncpu; i++)
        if (cpus[i].online && !cpus[i].offline)
            online++;
    switch (name) {
    case SIEOS_CONFIG_NGROUPS:     return NGROUPS_MAX;
    case SIEOS_CONFIG_CHILD_MAX:   return NPROC - 1;
    case SIEOS_CONFIG_OPEN_FILES: {
        uint64_t c = current->rlim_cur[SIEOS_RLIMIT_NOFILE];
        return c == SIEOS_RLIM_INFINITY || c > NOFILE ? NOFILE : (long)c;
    }
    case SIEOS_CONFIG_POSIX_VER:   return 200112L;
    case SIEOS_CONFIG_PAGESIZE:    return PAGE_SIZE;
    case SIEOS_CONFIG_CLK_TCK:     return TIMER_HZ;
    case SIEOS_CONFIG_XOPEN_VER:   return 600;
    case SIEOS_CONFIG_NPROC_CONF:  return ncpu;
    case SIEOS_CONFIG_NPROC_ONLN:  return online;
    case SIEOS_CONFIG_NPROC_MAX:   return ncpu;
    case SIEOS_CONFIG_ARG_MAX:     return MAXARGSTR;
    case SIEOS_CONFIG_PHYS_PAGES:  return pmm_total_pages();
    case SIEOS_CONFIG_AVPHYS_PAGES: return pmm_free_pages();
    case SIEOS_CONFIG_STACK_PROT:  return 3;     /* PROT_READ | PROT_WRITE: the stack is not executable */
    }
    return -EINVAL;
}

long system_halt(bool restart);                  /* syscall.c */

static long do_uadmin(long cmd, long fcn)
{
    if (!is_root())
        return -EPERM;
    switch (cmd) {
    case SIEOS_A_REBOOT:
        return system_halt(true);
    case SIEOS_A_SHUTDOWN:
        if (fcn == SIEOS_AD_BOOT || fcn == SIEOS_AD_IBOOT)
            return system_halt(true);
        if (fcn == SIEOS_AD_HALT || fcn == SIEOS_AD_POWEROFF)
            return system_halt(false);
        return -EINVAL;
    case SIEOS_A_REMOUNT:
        vfs_sync();
        return 0;
    case SIEOS_A_JTEST:
        jbd_crash_test = true;
        return 0;
    case SIEOS_A_NETTEST:
        net_test_drop = fcn & 0xFFFF;
        net_test_reorder = (fcn >> 16) & 0xFFFF;
        return 0;
    }
    return -EINVAL;
}

/* ---------------- processors ---------------- */

static struct cpu *cpu_by_id(long id)
{
    for (int i = 0; i < ncpu; i++)
        if (cpus[i].id == id && cpus[i].online)
            return &cpus[i];
    return NULL;
}

static int cpu_status(struct cpu *c)
{
    return c->offline ? SIEOS_P_OFFLINE : SIEOS_P_ONLINE;
}

static long do_processor_info(long id, sieos_processor_info_t *u)
{
    struct cpu *c = cpu_by_id(id);
    if (!c)
        return -EINVAL;
    if (!user_ok(u, sizeof(*u), true))
        return -EFAULT;
    sieos_processor_info_t pi;
    memset(&pi, 0, sizeof(pi));
    pi.pi_state = cpu_status(c);
    strlcpy(pi.pi_processor_type, "i386", sizeof(pi.pi_processor_type));
    strlcpy(pi.pi_fputypes, "i387 compatible", sizeof(pi.pi_fputypes));
    pi.pi_clock = tsc_hz / 1000000;
    memcpy(u, &pi, sizeof(pi));
    return 0;
}

static long do_p_online(long id, long flag)
{
    struct cpu *c = cpu_by_id(id);
    if (!c)
        return -EINVAL;
    int old = cpu_status(c);
    switch (flag) {
    case SIEOS_P_STATUS:
        return old;
    case SIEOS_P_ONLINE:
    case SIEOS_P_NOINTR:
        if (!is_root())
            return -EPERM;
        c->offline = false;
        return old;
    case SIEOS_P_OFFLINE: {
        if (!is_root())
            return -EPERM;
        int online = 0;
        for (int i = 0; i < ncpu; i++)
            if (cpus[i].online && !cpus[i].offline)
                online++;
        if (!c->offline && online <= 1)
            return -EBUSY;                       /* the last processor stays online */
        uint64_t others = 0;                     /* the processors left online */
        for (int i = 0; i < ncpu; i++)
            if (cpus[i].online && !cpus[i].offline && &cpus[i] != c)
                others |= 1ULL << cpus[i].id;
        disp_enter();
        for (int i = 0; i < NLWP; i++) {
            struct lwp *l = &lwp_table[i];
            if (l->state != LWP_UNUSED && (l->bound == c->id + 1 || (l->affinity && !(l->affinity & others)))) {
                disp_exit();
                return -EBUSY;                   /* LWPs are bound to it, or may run on it only */
            }
        }
        c->offline = true;
        c->need_resched = true;
        disp_exit();
        if (c != mycpu())
            smp_resched(c);                      /* (it leaves its LWP at its next reschedule) */
        return old;
    }
    }
    return -EINVAL;
}

/*
 * lwp_affinity(idtype, id, op, uint64_t *mask): the processors an LWP (P_LWPID,
 * of this process) or every LWP of a process (P_PID) may run on, a bit per
 * processor id.  SIEOS_AFF_GET reads the first one's (all online processors
 * when it has no set), SIEOS_AFF_SET sets them: online processors only, at
 * least one; all of them clears the set.
 */
static long do_lwp_affinity(long idtype, long id, long op, uint64_t *umask)
{
    id = (int)id;
    if (!user_ok(umask, sizeof(*umask), op == SIEOS_AFF_GET))
        return -EFAULT;
    if (op != SIEOS_AFF_GET && op != SIEOS_AFF_SET)
        return -EINVAL;
    if (idtype != SIEOS_P_LWPID && idtype != SIEOS_P_PID)
        return -EINVAL;
    uint64_t online = 0, set = 0;
    for (int i = 0; i < ncpu; i++)
        if (cpus[i].online && !cpus[i].offline)
            online |= 1ULL << cpus[i].id;
    if (op == SIEOS_AFF_SET) {
        uint64_t m = *umask;
        if (!(m & online))
            return -EINVAL;
        set = (m & online) == online ? 0 : (m & online);
    }
    mutex_enter(&pidlock);
    struct proc *p = idtype == SIEOS_P_LWPID || id == SIEOS_P_MYID ? current : proc_find(id);
    long r = 0;
    if (!p || p->state != PSTATE_RUNNING)
        r = -ESRCH;
    else if (op == SIEOS_AFF_SET && p != current && !is_root() && current->euid != p->uid && current->euid != p->euid)
        r = -EPERM;
    if (r) {
        mutex_exit(&pidlock);
        return r;
    }
    bool found = false;
    uint64_t got = 0;
    mutex_enter(&p->p_lock);
    disp_enter();                                    /* (the dispatcher reads the affinities) */
    for (int i = 0; i < NLWP; i++) {
        struct lwp *l = &lwp_table[i];
        if (l->proc != p || l->state == LWP_UNUSED || l->state == LWP_ZOMBIE)
            continue;
        if (idtype == SIEOS_P_LWPID && l->lwpid != (id == SIEOS_P_MYID ? curlwp->lwpid : id))
            continue;
        found = true;
        if (op == SIEOS_AFF_GET) {
            got = l->affinity ? (l->affinity & online) : online;
            break;
        }
        l->affinity = set;
        if (l != curlwp && l->state == LWP_RUNNING)
            for (int c = 0; c < ncpu; c++)
                if (cpus[c].lwp == l && set && !(set & (1ULL << cpus[c].id)))
                    cpus[c].need_resched = true; /* (it moves at its next reschedule) */
    }
    disp_exit();
    mutex_exit(&p->p_lock);
    mutex_exit(&pidlock);
    if (!found)
        return -ESRCH;
    if (op == SIEOS_AFF_GET) {
        *umask = got;
        return 0;
    }
    if (curlwp->affinity && !(curlwp->affinity & (1ULL << mycpu()->id)))
        preempt();                              /* move to an allowed processor now */
    return 0;
}

static long do_processor_bind(long idtype, long id, long cpu, int *uobind)
{
    id = (int)id;                                /* id_t is 32 bits: P_MYID arrives as 0xffffffff */
    if (uobind && !user_ok(uobind, sizeof(int), true))
        return -EFAULT;
    if (cpu != SIEOS_PBIND_NONE && cpu != SIEOS_PBIND_QUERY) {
        struct cpu *c = cpu_by_id(cpu);
        if (!c || c->offline)
            return -EINVAL;
    }
    if (idtype != SIEOS_P_LWPID && idtype != SIEOS_P_PID)
        return -EINVAL;
    mutex_enter(&pidlock);
    struct proc *p = idtype == SIEOS_P_LWPID || id == SIEOS_P_MYID ? current : proc_find(id);
    long r = 0;
    if (!p || p->state != PSTATE_RUNNING)
        r = -ESRCH;
    else if (p != current && !is_root() && current->euid != p->uid && current->euid != p->euid)
        r = -EPERM;
    if (r) {
        mutex_exit(&pidlock);
        return r;
    }
    int old = -2;
    mutex_enter(&p->p_lock);
    disp_enter();
    for (int i = 0; i < NLWP; i++) {
        struct lwp *l = &lwp_table[i];
        if (l->proc != p || l->state == LWP_UNUSED || l->state == LWP_ZOMBIE)
            continue;
        if (idtype == SIEOS_P_LWPID && l->lwpid != (id == SIEOS_P_MYID ? curlwp->lwpid : id))
            continue;
        if (old == -2)
            old = l->bound ? l->bound - 1 : SIEOS_PBIND_NONE;
        if (cpu != SIEOS_PBIND_QUERY)
            l->bound = cpu == SIEOS_PBIND_NONE ? 0 : (int)cpu + 1;
    }
    disp_exit();
    mutex_exit(&p->p_lock);
    mutex_exit(&pidlock);
    if (old == -2)
        return -ESRCH;
    if (uobind)
        *uobind = old;
    if (cpu >= 0 && curlwp->bound && curlwp->bound != mycpu()->id + 1)
        preempt();                              /* move to the new processor now */
    return 0;
}

static long do_getloadavg(long *u, long n)
{
    if (n < 0)
        return -EINVAL;
    n = MIN(n, 3);
    if (n && !user_ok(u, n * sizeof(long), true))
        return -EFAULT;
    for (long i = 0; i < n; i++)
        u[i] = (long)(loadavg[i] * 1000 / 2048);
    return n;
}

/* ---------------- dispatch ---------------- */

long syscall_misc_v2(struct trapframe *tf, bool *handled)
{
    uint64_t a1 = tf->rdi, a2 = tf->rsi, a3 = tf->rdx, a4 = tf->r10;
    *handled = true;
    switch (tf->rax) {
    case SIEOS_SYS_clock_gettime: return do_clock_gettime(a1, (struct sieos_timespec *)a2);
    case SIEOS_SYS_clock_settime: return do_clock_settime(a1, (const struct sieos_timespec *)a2);
    case SIEOS_SYS_clock_getres:  return do_clock_getres(a1, (struct sieos_timespec *)a2);
    case SIEOS_SYS_gethrtime:     return (long)hrtime();
    case SIEOS_SYS_gethrvtime:    return (long)(curlwp->ticks * NS_PER_TICK);
    case SIEOS_SYS_setitimer:     return do_setitimer(a1, (const struct sieos_itimerval *)a2, (struct sieos_itimerval *)a3);
    case SIEOS_SYS_getitimer:     return do_getitimer(a1, (struct sieos_itimerval *)a2);
    case SIEOS_SYS_times:         return do_times((struct sieos_tms *)a1);
    case SIEOS_SYS_adjtime:       return do_adjtime((const struct sieos_timeval *)a1, (struct sieos_timeval *)a2);
    case SIEOS_SYS_stime:
        if (!is_root())
            return -EPERM;
        if ((long)a1 < 0)
            return -EINVAL;
        realtime_set((int64_t)a1 * 1000000000L);
        return 0;
    case SIEOS_SYS_getrlimit:     return do_getrlimit(a1, (struct sieos_rlimit *)a2);
    case SIEOS_SYS_setrlimit:     return do_setrlimit(a1, (const struct sieos_rlimit *)a2);
    case SIEOS_SYS_getrusage:     return do_getrusage((long)a1, (struct sieos_rusage *)a2);
    case SIEOS_SYS_pollsys:
        return do_pollsys((struct pollfd *)a1, a2, (const struct sieos_timespec *)a3, (const sieos_sigset_t *)a4);
    case SIEOS_SYS_sysconfig:     return do_sysconfig(a1);
    case SIEOS_SYS_uadmin:        return do_uadmin(a1, a2);
    case SIEOS_SYS_processor_info: return do_processor_info(a1, (sieos_processor_info_t *)a2);
    case SIEOS_SYS_p_online:      return do_p_online(a1, a2);
    case SIEOS_SYS_processor_bind: return do_processor_bind(a1, a2, a3, (int *)a4);
    case SIEOS_SYS_lwp_affinity:   return do_lwp_affinity(a1, a2, a3, (uint64_t *)a4);
    case SIEOS_SYS_priocntl:     return sys2_priocntl(a1, a2, a3, (void *)a4);
    case SIEOS_SYS_getloadavg:    return do_getloadavg((long *)a1, a2);
    case SIEOS_SYS_msgsys:        return sys2_msgsys(a1, a2, a3, a4, tf->r8, tf->r9);
    case SIEOS_SYS_semsys:        return sys2_semsys(a1, a2, a3, a4, tf->r8);
    case SIEOS_SYS_shmsys:        return sys2_shmsys(a1, a2, a3, a4);
    }
    *handled = false;
    return -ENOSYS;
}
