/*
 * sieos/time.h - clocks, interval timers, times(), rlimits and rusage (ABI v2).
 * Clock ids and rlimit resources follow Solaris.
 */
#ifndef SIEOS_ABI_TIME_H
#define SIEOS_ABI_TIME_H

#include "types.h"

#define SIEOS_CLOCK_VIRTUAL            1
#define SIEOS_CLOCK_THREAD_CPUTIME_ID  2
#define SIEOS_CLOCK_REALTIME           3
#define SIEOS_CLOCK_MONOTONIC          4
#define SIEOS_CLOCK_PROCESS_CPUTIME_ID 5
#define SIEOS_CLOCK_HIGHRES            SIEOS_CLOCK_MONOTONIC
#define SIEOS_CLOCK_PROF               SIEOS_CLOCK_THREAD_CPUTIME_ID

#define SIEOS_TIMER_RELTIME 0x0
#define SIEOS_TIMER_ABSTIME 0x1

#define SIEOS_ITIMER_REAL     0
#define SIEOS_ITIMER_VIRTUAL  1
#define SIEOS_ITIMER_PROF     2
#define SIEOS_ITIMER_REALPROF 3

struct sieos_itimerval {
    struct sieos_timeval it_interval;
    struct sieos_timeval it_value;
};

struct sieos_itimerspec {
    struct sieos_timespec it_interval;
    struct sieos_timespec it_value;
};

/* times(): clock ticks of SIEOS_CLK_TCK per second */
#define SIEOS_CLK_TCK 100
struct sieos_tms {
    sieos_clock_t tms_utime;
    sieos_clock_t tms_stime;
    sieos_clock_t tms_cutime;
    sieos_clock_t tms_cstime;
};

/* getrlimit()/setrlimit() */
#define SIEOS_RLIMIT_CPU    0
#define SIEOS_RLIMIT_FSIZE  1
#define SIEOS_RLIMIT_DATA   2
#define SIEOS_RLIMIT_STACK  3
#define SIEOS_RLIMIT_CORE   4
#define SIEOS_RLIMIT_NOFILE 5
#define SIEOS_RLIMIT_VMEM   6
#define SIEOS_RLIMIT_AS     SIEOS_RLIMIT_VMEM
#define SIEOS_RLIMIT_NPROC  7    /* SIEOS extension: processes of the real user (fork fails with EAGAIN) */
#define SIEOS_RLIM_NLIMITS  8

#define SIEOS_RLIM_INFINITY  ((sieos_rlim_t)-3)
#define SIEOS_RLIM_SAVED_MAX ((sieos_rlim_t)-2)
#define SIEOS_RLIM_SAVED_CUR ((sieos_rlim_t)-1)

struct sieos_rlimit {
    sieos_rlim_t rlim_cur;
    sieos_rlim_t rlim_max;
};

/* getrusage() */
#define SIEOS_RUSAGE_SELF      0
#define SIEOS_RUSAGE_CHILDREN  (-1)
#define SIEOS_RUSAGE_LWP       1

struct sieos_rusage {
    struct sieos_timeval ru_utime;
    struct sieos_timeval ru_stime;
    long ru_maxrss;                 /* KiB */
    long ru_ixrss, ru_idrss, ru_isrss;
    long ru_minflt, ru_majflt, ru_nswap;
    long ru_inblock, ru_oublock;
    long ru_msgsnd, ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw, ru_nivcsw;
};

SIEOS_STATIC_ASSERT(sizeof(struct sieos_rusage) == 144, "rusage size");

#endif
