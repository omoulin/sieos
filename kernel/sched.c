/*
 * sched.c - Scheduling classes and priocntl (sieos/priocntl.h), as Solaris:
 * the dispatcher (proc.c: schedule) runs the runnable LWP of highest global
 * priority, round robin among equals.
 *
 *   TS  time sharing, 0-59: cpupri falls by 10 when a quantum is used up,
 *       rises to at least 50 after a sleep, and to 50 after a second without
 *       running; the user priority (upri, -60..60, nice) shifts it.  The
 *       quantum is 200 ms at priority 0 down to 20 ms at 59.
 *   FX  fixed, 0-60 (the user priority), 200 ms quantum by default.
 *   RT  real time, 100-159 (rt_pri 0-59 + 100), 100 ms quantum by default, or
 *       none; only root may enter it.
 * A woken LWP that outranks the LWP running on a CPU preempts it.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "proc.h"
#include "smp.h"
#include "abi2.h"
#include "sieos/priocntl.h"
#include "sieos/wait.h"
#include "sieos/errno.h"

#define TS_MAXPRI  59
#define TS_SLPRET  50
#define TS_TQEXP   10
#define TS_LWAIT   50
#define RT_BASE    100
#define RT_TQDEF_TICKS 10
#define FX_TQDEF_TICKS 20

static const char *const class_names[] = { "SYS", "TS", "FX", "RT" };

void sched_init_lwp(struct lwp *l, struct lwp *from)
{
    l->affinity = from && !from->is_idle ? from->affinity : 0;   /* (threads keep their creator's) */
    if (from && !from->is_idle && from->cid) {
        l->cid = from->cid;
        l->upri = from->upri;
        l->uprilim = from->uprilim;
        l->cpupri = from->cpupri;
        l->rtpri = from->rtpri;
        l->quantum = from->quantum;
    } else {
        l->cid = SIEOS_CID_TS;
        l->cpupri = 29;
        l->quantum = 0;
    }
}

int sched_gpri(const struct lwp *l)
{
    if (l->kthread)
        return l->kpri;
    switch (l->cid) {
    case SIEOS_CID_RT:
        return RT_BASE + l->rtpri;
    case SIEOS_CID_FX:
        return l->upri;
    default: {
        int p = l->cpupri + l->upri;
        return p < 0 ? 0 : p > TS_MAXPRI ? TS_MAXPRI : p;
    }
    }
}

/* The LWP's quantum in ticks; 0 = unlimited (RT with RT_TQINF). */
uint32_t sched_quantum(const struct lwp *l)
{
    switch (l->cid) {
    case SIEOS_CID_RT:
    case SIEOS_CID_FX:
        return l->quantum;
    default:
        return 20 - sched_gpri(l) * 18 / TS_MAXPRI;      /* 200 ms .. 20 ms */
    }
}

void sched_expired(struct lwp *l)
{
    if (l->cid == SIEOS_CID_TS)
        l->cpupri = l->cpupri > TS_TQEXP ? l->cpupri - TS_TQEXP : 0;
}

/* Preempt the CPU running the lowest-priority LWP below l's priority. */
static void preempt_for(struct lwp *l)
{
    int pri = sched_gpri(l), best = -1, low = pri;
    for (int i = 0; i < ncpu; i++) {
        struct cpu *c = &cpus[i];
        if (!c->online || c->offline || (l->bound && l->bound != c->id + 1) ||
            (l->affinity && !(l->affinity & (1ULL << c->id))))
            continue;
        if (!c->lwp || c->lwp->is_idle)
            return;                                /* an idle CPU takes it (smp_kick_idle) */
        int cp = sched_gpri(c->lwp);
        if (cp < low) {
            low = cp;
            best = i;
        }
    }
    if (best < 0 || cpus[best].lwp->kthread)
        return;                                    /* (kernel threads run until they block) */
    cpus[best].need_resched = true;
    if (&cpus[best] != mycpu())
        smp_resched(&cpus[best]);
}

/* l became runnable after sleeping. */
void sched_woke(struct lwp *l)
{
    if (l->cid == SIEOS_CID_TS && l->cpupri < TS_SLPRET)
        l->cpupri = TS_SLPRET;
    preempt_for(l);
}

/* Every second: TS LWPs that waited a second without running are raised. */
void sched_second(void)
{
    disp_enter();
    for (int i = 0; i < NLWP; i++) {
        struct lwp *l = &lwp_table[i];
        if (l->state == LWP_RUNNABLE && l->cid == SIEOS_CID_TS && ticks - l->last_run >= TIMER_HZ &&
            l->cpupri < TS_LWAIT)
            l->cpupri = TS_LWAIT;
    }
    disp_exit();
}

const char *sched_class_name(int cid)
{
    return cid >= 0 && cid < 4 ? class_names[cid] : "??";
}

/* ---------------- priocntl ---------------- */

static long ticks_of(unsigned int secs, int nsecs, long def)
{
    if (nsecs == SIEOS_RT_TQDEF)
        return def;
    if (nsecs == SIEOS_RT_TQINF)
        return 0;
    if (nsecs < 0 || nsecs >= 1000000000)
        return -1;
    uint64_t t = (uint64_t)secs * TIMER_HZ + ((uint64_t)nsecs * TIMER_HZ + 999999999) / 1000000000;
    return t ? (long)t : 1;
}

static bool may_change(struct lwp *l)
{
    return current->euid == 0 || current->euid == l->proc->uid || current->euid == l->proc->euid;
}

/* Apply pcparms to one LWP; -errno. */
static long set_parms(struct lwp *l, const sieos_pcparms_t *pp)
{
    bool root = current->euid == 0;
    if (!may_change(l))
        return -EPERM;
    switch (pp->pc_cid) {
    case SIEOS_CID_TS: {
        const sieos_tsparms_t *ts = (const void *)pp->pc_clparms;
        int lim = ts->ts_uprilim == SIEOS_TS_NOCHANGE ? (l->cid == SIEOS_CID_TS ? l->uprilim : 0) : ts->ts_uprilim;
        int up = ts->ts_upri == SIEOS_TS_NOCHANGE ? (l->cid == SIEOS_CID_TS ? l->upri : 0) : ts->ts_upri;
        if (lim < -SIEOS_TS_MAXUPRI || lim > SIEOS_TS_MAXUPRI || up < -SIEOS_TS_MAXUPRI || up > SIEOS_TS_MAXUPRI)
            return -EINVAL;
        if (!root && (l->cid == SIEOS_CID_RT || (l->cid == SIEOS_CID_TS && lim > l->uprilim) ||
                      (l->cid != SIEOS_CID_TS && lim > 0)))
            return -EPERM;                           /* only root raises a limit */
        if (up > lim)
            up = lim;
        if (l->cid != SIEOS_CID_TS)
            l->cpupri = 29;
        l->cid = SIEOS_CID_TS;
        l->uprilim = lim;
        l->upri = up;
        return 0;
    }
    case SIEOS_CID_FX: {
        const sieos_fxparms_t *fx = (const void *)pp->pc_clparms;
        int lim = fx->fx_uprilim == SIEOS_FX_NOCHANGE ? (l->cid == SIEOS_CID_FX ? l->uprilim : 0) : fx->fx_uprilim;
        int up = fx->fx_upri == SIEOS_FX_NOCHANGE ? (l->cid == SIEOS_CID_FX ? l->upri : 0) : fx->fx_upri;
        long q = ticks_of(fx->fx_tqsecs, fx->fx_tqnsecs, FX_TQDEF_TICKS);
        if (lim < 0 || lim > SIEOS_FX_MAXUPRI || up < 0 || up > SIEOS_FX_MAXUPRI || q < 0)
            return -EINVAL;
        if (!root && (l->cid == SIEOS_CID_RT || lim > (l->cid == SIEOS_CID_FX ? l->uprilim : 0)))
            return -EPERM;
        if (up > lim)
            up = lim;
        l->cid = SIEOS_CID_FX;
        l->uprilim = lim;
        l->upri = up;
        l->quantum = fx->fx_tqnsecs == SIEOS_RT_NOCHANGE && l->quantum ? l->quantum : (uint32_t)q;
        return 0;
    }
    case SIEOS_CID_RT: {
        const sieos_rtparms_t *rt = (const void *)pp->pc_clparms;
        if (!root)
            return -EPERM;
        int pri = rt->rt_pri == SIEOS_RT_NOCHANGE ? (l->cid == SIEOS_CID_RT ? l->rtpri : 0) : rt->rt_pri;
        long q = rt->rt_tqnsecs == SIEOS_RT_NOCHANGE ? (l->cid == SIEOS_CID_RT ? (long)l->quantum : RT_TQDEF_TICKS)
                                                     : ticks_of(rt->rt_tqsecs, rt->rt_tqnsecs, RT_TQDEF_TICKS);
        if (pri < 0 || pri > SIEOS_RT_MAXPRI || q < 0)
            return -EINVAL;
        l->cid = SIEOS_CID_RT;
        l->rtpri = pri;
        l->quantum = q;
        return 0;
    }
    }
    return -EINVAL;
}

static void get_parms(const struct lwp *l, sieos_pcparms_t *pp)
{
    memset(pp->pc_clparms, 0, sizeof(pp->pc_clparms));
    pp->pc_cid = l->cid;
    switch (l->cid) {
    case SIEOS_CID_TS: {
        sieos_tsparms_t *ts = (void *)pp->pc_clparms;
        ts->ts_uprilim = l->uprilim;
        ts->ts_upri = l->upri;
        break;
    }
    case SIEOS_CID_FX: {
        sieos_fxparms_t *fx = (void *)pp->pc_clparms;
        fx->fx_uprilim = l->uprilim;
        fx->fx_upri = l->upri;
        fx->fx_tqsecs = l->quantum / TIMER_HZ;
        fx->fx_tqnsecs = l->quantum ? (int)((l->quantum % TIMER_HZ) * (1000000000 / TIMER_HZ)) : SIEOS_FX_TQINF;
        break;
    }
    case SIEOS_CID_RT: {
        sieos_rtparms_t *rt = (void *)pp->pc_clparms;
        rt->rt_pri = l->rtpri;
        rt->rt_tqsecs = l->quantum / TIMER_HZ;
        rt->rt_tqnsecs = l->quantum ? (int)((l->quantum % TIMER_HZ) * (1000000000 / TIMER_HZ)) : SIEOS_RT_TQINF;
        break;
    }
    }
}

/* The LWPs an (idtype, id) names; the callback returns <0 to stop.  It runs
 * under pidlock and the dispatcher's lock (it changes what swtch reads). */
static long for_each_locked(int idtype, long id, long (*fn)(struct lwp *, void *), void *arg);

static long for_each(int idtype, long id, long (*fn)(struct lwp *, void *), void *arg)
{
    mutex_enter(&pidlock);
    disp_enter();
    long r = for_each_locked(idtype, id, fn, arg);
    disp_exit();
    mutex_exit(&pidlock);
    return r;
}

static long for_each_locked(int idtype, long id, long (*fn)(struct lwp *, void *), void *arg)
{
    long r = -ESRCH, any = 0;
    for (int i = 0; i < NLWP; i++) {
        struct lwp *l = &lwp_table[i];
        if (l->state == LWP_UNUSED || l->state == LWP_ZOMBIE || l->is_idle || !l->proc)
            continue;
        bool match;
        switch (idtype) {
        case SIEOS_P_PID:
            match = l->proc->pid == (id == SIEOS_P_MYID ? current->pid : id);
            break;
        case SIEOS_P_LWPID:
            match = l->proc == current && l->lwpid == (id == SIEOS_P_MYID ? curlwp->lwpid : id);
            break;
        case SIEOS_P_ALL:
            match = l->proc->pid > 1;
            break;
        default:
            return -EINVAL;
        }
        if (!match)
            continue;
        any = 1;
        long e = fn(l, arg);
        if (e < 0)
            return e;
        r = 0;
    }
    return any ? r : -ESRCH;
}

static long do_set(struct lwp *l, void *arg)
{
    return set_parms(l, arg);
}

static long do_get(struct lwp *l, void *arg)
{
    get_parms(l, arg);
    return -1000;                                    /* the first one only */
}

struct nice_arg { sieos_pcnice_t n; bool got; };

static long do_nice(struct lwp *l, void *arg)
{
    struct nice_arg *a = arg;
    if (a->n.pc_op == SIEOS_PC_GETNICE) {
        if (l->cid == SIEOS_CID_TS || l->cid == SIEOS_CID_FX) {
            a->n.pc_val = -l->upri / 3;
            a->got = true;
            return -1000;
        }
        return 0;
    }
    if (l->cid != SIEOS_CID_TS && l->cid != SIEOS_CID_FX)
        return 0;
    int nice = a->n.pc_val < -20 ? -20 : a->n.pc_val > 19 ? 19 : a->n.pc_val;
    int up = -nice * 3;
    if (!may_change(l))
        return -EPERM;
    if (up > l->upri && current->euid != 0)
        return -EACCES;                              /* only root raises a priority (lowers nice) */
    if (up > l->uprilim)
        l->uprilim = up;
    l->upri = up;
    return 0;
}

long sys2_priocntl(long idtype, long id, long cmd, void *arg)
{
    switch (cmd) {
    case SIEOS_PC_GETCID:
    case SIEOS_PC_GETCLINFO: {
        sieos_pcinfo_t pi;
        if (!user_ok(arg, sizeof(pi), true))
            return -EFAULT;
        memcpy(&pi, arg, sizeof(pi));
        int cid = -1;
        if (cmd == SIEOS_PC_GETCID) {
            pi.pc_clname[SIEOS_PC_CLNMSZ - 1] = 0;
            for (int c = 0; c < 4; c++)
                if (!strcmp(pi.pc_clname, class_names[c]))
                    cid = c;
        } else if (pi.pc_cid < 4) {
            cid = pi.pc_cid;
        }
        if (cid < 0)
            return -EINVAL;
        memset(&pi, 0, sizeof(pi));
        pi.pc_cid = cid;
        strlcpy(pi.pc_clname, class_names[cid], sizeof(pi.pc_clname));
        sieos_pri_t *max = (void *)pi.pc_clinfo;
        *max = cid == SIEOS_CID_TS ? SIEOS_TS_MAXUPRI : cid == SIEOS_CID_RT ? SIEOS_RT_MAXPRI
             : cid == SIEOS_CID_FX ? SIEOS_FX_MAXUPRI : 0;
        memcpy(arg, &pi, sizeof(pi));
        return 4;                                    /* the number of classes */
    }
    case SIEOS_PC_GETPRIRANGE: {
        sieos_pcpri_t pr;
        if (!user_ok(arg, sizeof(pr), true))
            return -EFAULT;
        memcpy(&pr, arg, sizeof(pr));
        switch (pr.pc_cid) {
        case SIEOS_CID_TS: pr.pc_clpmin = -SIEOS_TS_MAXUPRI; pr.pc_clpmax = SIEOS_TS_MAXUPRI; break;
        case SIEOS_CID_FX: pr.pc_clpmin = 0; pr.pc_clpmax = SIEOS_FX_MAXUPRI; break;
        case SIEOS_CID_RT: pr.pc_clpmin = 0; pr.pc_clpmax = SIEOS_RT_MAXPRI; break;
        default: return -EINVAL;
        }
        memcpy(arg, &pr, sizeof(pr));
        return 0;
    }
    case SIEOS_PC_SETPARMS: {
        sieos_pcparms_t pp;
        if (!user_ok(arg, sizeof(pp), false))
            return -EFAULT;
        memcpy(&pp, arg, sizeof(pp));
        long r = for_each(idtype, id, do_set, &pp);
        for (int i = 0; i < ncpu; i++)               /* new priorities take effect at once */
            cpus[i].need_resched = true;
        return r;
    }
    case SIEOS_PC_GETPARMS: {
        sieos_pcparms_t pp;
        if (!user_ok(arg, sizeof(pp), true))
            return -EFAULT;
        int want = (int)((sieos_pcparms_t *)arg)->pc_cid;         /* pc_cid is unsigned: PC_CLNULL is -1 */
        long r = for_each(idtype, id, do_get, &pp);
        if (r != -1000)
            return r;
        if (want != SIEOS_PC_CLNULL && want != (int)pp.pc_cid)
            return -ESRCH;                           /* not in the class asked for */
        memcpy(arg, &pp, sizeof(pp));
        return 0;
    }
    case SIEOS_PC_DONICE: {
        struct nice_arg a = { .got = false };
        if (!user_ok(arg, sizeof(a.n), true))
            return -EFAULT;
        memcpy(&a.n, arg, sizeof(a.n));
        if (a.n.pc_op != SIEOS_PC_GETNICE && a.n.pc_op != SIEOS_PC_SETNICE)
            return -EINVAL;
        long r = for_each(idtype, id, do_nice, &a);
        if (a.n.pc_op == SIEOS_PC_GETNICE) {
            if (!a.got)
                return r < 0 && r != -1000 ? r : -EINVAL;
            memcpy(arg, &a.n, sizeof(a.n));
            return 0;
        }
        return r == -1000 ? 0 : r;
    }
    }
    return -EINVAL;
}
