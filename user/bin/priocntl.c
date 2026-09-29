/*
 * priocntl - scheduling classes and parameters, as Solaris priocntl(1)
 *   priocntl -l                                       the classes
 *   priocntl -d [-i pid|all] [id...]                  display (default: this shell)
 *   priocntl -s -c class [-p pri] [-m uprilim] [-t ms] [-i pid|all] id...
 *   priocntl -e -c class [-p pri] [-m uprilim] [-t ms] command [arg...]
 * Classes: TS (time sharing, -p/-m -60..60), FX (fixed, 0..60), RT (real
 * time, -p 0..59, root only; -t the quantum in ms, 0 for none).
 */
#include "sieos.h"
#include "sieos/syscall.h"
#include "sieos/priocntl.h"
#include "sieos/wait.h"
#include <sys/syscall.h>

static long pc(long idtype, long id, long cmd, void *arg)
{
    return syscall(SIEOS_SYS_priocntl, idtype, id, cmd, arg);
}

static int class_id(const char *name)
{
    sieos_pcinfo_t pi;
    memset(&pi, 0, sizeof(pi));
    snprintf(pi.pc_clname, sizeof(pi.pc_clname), "%s", name);
    if (pc(0, 0, SIEOS_PC_GETCID, &pi) < 0)
        return -1;
    return pi.pc_cid;
}

static const char *class_name(int cid)
{
    static sieos_pcinfo_t pi;
    memset(&pi, 0, sizeof(pi));
    pi.pc_cid = cid;
    return pc(0, 0, SIEOS_PC_GETCLINFO, &pi) < 0 ? "?" : pi.pc_clname;
}

static int display(long idtype, long id)
{
    sieos_pcparms_t pp;
    pp.pc_cid = SIEOS_PC_CLNULL;
    if (pc(idtype, id, SIEOS_PC_GETPARMS, &pp) < 0) {
        dprintf(STDERR_FILENO, "priocntl: %ld: %s\n", id, strerror(errno));
        return 1;
    }
    printf("%-8ld %-4s ", id == SIEOS_P_MYID ? (long)getpid() : id, class_name(pp.pc_cid));
    switch (pp.pc_cid) {
    case SIEOS_CID_TS: {
        sieos_tsparms_t *ts = (void *)pp.pc_clparms;
        printf("uprilim %d upri %d\n", ts->ts_uprilim, ts->ts_upri);
        break;
    }
    case SIEOS_CID_FX: {
        sieos_fxparms_t *fx = (void *)pp.pc_clparms;
        printf("uprilim %d upri %d quantum %s\n", fx->fx_uprilim, fx->fx_upri,
               fx->fx_tqnsecs == SIEOS_FX_TQINF ? "none" : "set");
        break;
    }
    case SIEOS_CID_RT: {
        sieos_rtparms_t *rt = (void *)pp.pc_clparms;
        if (rt->rt_tqnsecs == SIEOS_RT_TQINF)
            printf("pri %d quantum none\n", rt->rt_pri);
        else
            printf("pri %d quantum %u ms\n", rt->rt_pri, rt->rt_tqsecs * 1000 + rt->rt_tqnsecs / 1000000);
        break;
    }
    default:
        printf("\n");
    }
    return 0;
}

static int make_parms(const char *cls, const char *p, const char *m, const char *t, sieos_pcparms_t *pp)
{
    int cid = class_id(cls);
    if (cid < 0) {
        dprintf(STDERR_FILENO, "priocntl: %s: unknown class\n", cls);
        return -1;
    }
    memset(pp, 0, sizeof(*pp));
    pp->pc_cid = cid;
    long ms = t ? atol(t) : -1;
    switch (cid) {
    case SIEOS_CID_TS: {
        sieos_tsparms_t *ts = (void *)pp->pc_clparms;
        ts->ts_uprilim = m ? atoi(m) : SIEOS_TS_NOCHANGE;
        ts->ts_upri = p ? atoi(p) : SIEOS_TS_NOCHANGE;
        return 0;
    }
    case SIEOS_CID_FX: {
        sieos_fxparms_t *fx = (void *)pp->pc_clparms;
        fx->fx_uprilim = m ? atoi(m) : SIEOS_FX_NOCHANGE;
        fx->fx_upri = p ? atoi(p) : SIEOS_FX_NOCHANGE;
        fx->fx_tqsecs = ms > 0 ? ms / 1000 : 0;
        fx->fx_tqnsecs = ms < 0 ? SIEOS_FX_TQDEF : ms == 0 ? SIEOS_FX_TQINF : (ms % 1000) * 1000000;
        return 0;
    }
    case SIEOS_CID_RT: {
        sieos_rtparms_t *rt = (void *)pp->pc_clparms;
        rt->rt_pri = p ? atoi(p) : 0;
        rt->rt_tqsecs = ms > 0 ? ms / 1000 : 0;
        rt->rt_tqnsecs = ms < 0 ? SIEOS_RT_TQDEF : ms == 0 ? SIEOS_RT_TQINF : (ms % 1000) * 1000000;
        return 0;
    }
    }
    dprintf(STDERR_FILENO, "priocntl: %s: cannot be set\n", cls);
    return -1;
}

static int usage(void)
{
    dprintf(STDERR_FILENO, "usage: priocntl -l | -d [-i pid|all] [id...] |\n"
                           "       -s -c class [-p pri] [-m uprilim] [-t ms] [-i pid|all] id... |\n"
                           "       -e -c class [-p pri] [-m uprilim] [-t ms] command [arg...]\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();
    const char *mode = argv[1], *cls = NULL, *p = NULL, *m = NULL, *t = NULL;
    long idtype = SIEOS_P_PID;
    int i = 2;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] && strcmp(mode, "-l"); i++) {
        const char *o = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v)
            return usage();
        if (!strcmp(o, "-c"))
            cls = v;
        else if (!strcmp(o, "-p"))
            p = v;
        else if (!strcmp(o, "-m"))
            m = v;
        else if (!strcmp(o, "-t"))
            t = v;
        else if (!strcmp(o, "-i"))
            idtype = !strcmp(v, "all") ? SIEOS_P_ALL : !strcmp(v, "pid") ? SIEOS_P_PID : -1;
        else
            return usage();
        i++;
        if (idtype < 0)
            return usage();
    }
    if (!strcmp(mode, "-l")) {
        printf("CONFIGURED CLASSES\n==================\n\nSYS (System Class)\n\n"
               "TS (Time Sharing)\n\tConfigured TS User Priority Range: -60 through 60\n\n"
               "FX (Fixed priority)\n\tConfigured FX User Priority Range: 0 through 60\n\n"
               "RT (Real Time)\n\tConfigured RT User Priority Range: 0 through 59\n");
        return 0;
    }
    if (!strcmp(mode, "-d")) {
        int rc = 0;
        printf("%-8s %-4s %s\n", "PID", "CLS", "PARAMETERS");
        if (idtype == SIEOS_P_ALL || i == argc)
            return display(SIEOS_P_PID, idtype == SIEOS_P_ALL ? 1 : SIEOS_P_MYID) && idtype != SIEOS_P_ALL;
        for (; i < argc; i++)
            rc |= display(SIEOS_P_PID, atol(argv[i]));
        return rc;
    }
    sieos_pcparms_t pp;
    if (!cls || make_parms(cls, p, m, t, &pp) < 0)
        return usage();
    if (!strcmp(mode, "-s")) {
        int rc = 0;
        if (i == argc)
            return usage();
        for (; i < argc; i++)
            if (pc(idtype, atol(argv[i]), SIEOS_PC_SETPARMS, &pp) < 0) {
                dprintf(STDERR_FILENO, "priocntl: %s: %s\n", argv[i], strerror(errno));
                rc = 1;
            }
        return rc;
    }
    if (!strcmp(mode, "-e")) {
        if (i == argc)
            return usage();
        if (pc(SIEOS_P_PID, SIEOS_P_MYID, SIEOS_PC_SETPARMS, &pp) < 0) {
            dprintf(STDERR_FILENO, "priocntl: %s\n", strerror(errno));
            return 1;
        }
        execvp(argv[i], argv + i);
        dprintf(STDERR_FILENO, "priocntl: %s: %s\n", argv[i], strerror(errno));
        return 127;
    }
    return usage();
}
