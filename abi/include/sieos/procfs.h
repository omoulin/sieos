/*
 * sieos/procfs.h - binary records of the /proc file system (ABI v2).
 *
 * Solaris-style: each process has a directory /proc/<pid> with binary
 * files read with pread():
 *     psinfo    sieos_psinfo_t   (readable by anyone)
 *     status    sieos_pstatus_t  (owner or root)
 *     cred      sieos_prcred_t   (owner or root)
 *     usage     sieos_prusage_t
 *     lwp/<id>/lwpsinfo       sieos_lwpsinfo_t
 *     cwd, root, fd/<n>       symbolic links
 *     (as and ctl are reserved for debuggers)
 * Field names follow Solaris; the exact layout is SIEOS's own.
 */
#ifndef SIEOS_ABI_PROCFS_H
#define SIEOS_ABI_PROCFS_H

#include "types.h"
#include "signal.h"

#define SIEOS_PRFNSZ   16
#define SIEOS_PRARGSZ  80
#define SIEOS_PRCLSZ   8

/* pr_flag */
#define SIEOS_PR_ISSYS   0x0001
#define SIEOS_PR_STOPPED 0x0002
#define SIEOS_PR_ZOMBIE  0x0008

/* pr_state */
#define SIEOS_SSLEEP  1
#define SIEOS_SRUN    2
#define SIEOS_SZOMB   3
#define SIEOS_SSTOP   4
#define SIEOS_SIDL    5
#define SIEOS_SONPROC 6

typedef struct {
    int pr_flag;
    sieos_lwpid_t pr_lwpid;
    sieos_uintptr_t pr_addr;
    sieos_uintptr_t pr_wchan;          /* sleep channel, 0 if runnable */
    char pr_stype;
    char pr_state;                  /* SIEOS_S* */
    char pr_sname;                  /* 'S' 'R' 'Z' 'T' 'I' 'O' */
    char pr_nice;
    int pr_pri;                     /* higher = better */
    char pr_clname[SIEOS_PRCLSZ];      /* scheduling class, e.g. "TS" */
    struct sieos_timespec pr_start;    /* start time since the epoch */
    struct sieos_timespec pr_time;     /* CPU time used by this LWP */
    int pr_onpro;                   /* CPU last run on */
    int pr_bindpro;                 /* bound CPU or SIEOS_PBIND_NONE */
    char pr_name[32];               /* lwp_name() */
} sieos_lwpsinfo_t;

typedef struct {
    int pr_flag;
    int pr_nlwp;
    sieos_pid_t pr_pid;
    sieos_pid_t pr_ppid;
    sieos_pid_t pr_pgid;
    sieos_pid_t pr_sid;
    sieos_uid_t pr_uid;
    sieos_uid_t pr_euid;
    sieos_gid_t pr_gid;
    sieos_gid_t pr_egid;
    sieos_uintptr_t pr_addr;
    sieos_size_t pr_size;              /* address space size, KiB */
    sieos_size_t pr_rssize;            /* resident set, KiB */
    sieos_dev_t pr_ttydev;             /* controlling terminal or ~0 */
    unsigned short pr_pctcpu;       /* % CPU, fraction of 0x8000 */
    unsigned short pr_pctmem;
    int __pad1;
    struct sieos_timespec pr_start;
    struct sieos_timespec pr_time;     /* CPU time of the process */
    struct sieos_timespec pr_ctime;    /* CPU time of reaped children */
    char pr_fname[SIEOS_PRFNSZ];       /* last component of exec'd path */
    char pr_psargs[SIEOS_PRARGSZ];     /* initial argument string */
    int pr_wstat;                   /* wait status if zombie */
    int pr_argc;
    sieos_uintptr_t pr_argv;
    sieos_uintptr_t pr_envp;
    char pr_dmodel;                 /* 2 = LP64 */
    char __pad2[3];
    int pr_nzomb;
    sieos_lwpsinfo_t pr_lwp;           /* representative LWP */
} sieos_psinfo_t;

typedef struct {
    int pr_flags;
    int pr_nlwp;
    sieos_pid_t pr_pid, pr_ppid, pr_pgid, pr_sid;
    sieos_lwpid_t pr_agentid;
    int __pad;
    sieos_sigset_t pr_sigpend;         /* process-directed pending signals */
    sieos_uintptr_t pr_brkbase;
    sieos_size_t pr_brksize;
    sieos_uintptr_t pr_stkbase;
    sieos_size_t pr_stksize;
    struct sieos_timespec pr_utime, pr_stime, pr_cutime, pr_cstime;
    sieos_sigset_t pr_sigtrace;
    char pr_clname[SIEOS_PRCLSZ];
} sieos_pstatus_t;

typedef struct {
    sieos_uid_t pr_euid, pr_ruid, pr_suid;
    sieos_gid_t pr_egid, pr_rgid, pr_sgid;
    int pr_ngroups;
    sieos_gid_t pr_groups[16];
} sieos_prcred_t;

typedef struct {
    sieos_lwpid_t pr_lwpid;
    int pr_count;                   /* number of LWPs covered */
    struct sieos_timespec pr_tstamp, pr_create, pr_term, pr_rtime;
    struct sieos_timespec pr_utime, pr_stime;
    unsigned long pr_minf, pr_majf, pr_nswap, pr_inblk, pr_oublk;
    unsigned long pr_msnd, pr_mrcv, pr_sigs, pr_vctx, pr_ictx, pr_sysc, pr_ioch;
} sieos_prusage_t;

/*
 * Core files: a process killed by a signal whose default action is to dump
 * core, with a non-zero RLIMIT_CORE and no set-id credentials, leaves "core"
 * in its working directory: an ELF64 ET_CORE file with one PT_NOTE and a
 * PT_LOAD per run of mapped pages, cut at RLIMIT_CORE.  The notes (name
 * "CORE") are the process's pstatus and psinfo, and the registers of the LWP
 * that took the signal (name "SIEOS").
 */
#define SIEOS_NT_PSTATUS 10         /* sieos_pstatus_t */
#define SIEOS_NT_PSINFO  13         /* sieos_psinfo_t */
#define SIEOS_NT_LWPREGS 0x5301     /* struct sieos_core_regs */
struct sieos_core_regs {
    int cr_lwpid;
    int cr_sig;
    sieos_gregset_t cr_gregs;
};

SIEOS_STATIC_ASSERT(sizeof(struct sieos_core_regs) == 232, "core_regs size");
SIEOS_STATIC_ASSERT(sizeof(sieos_lwpsinfo_t) == 112, "lwpsinfo size");
SIEOS_STATIC_ASSERT(sizeof(sieos_psinfo_t) == 368, "psinfo size");
SIEOS_STATIC_ASSERT(sizeof(sieos_pstatus_t) == 168, "pstatus size");
SIEOS_STATIC_ASSERT(sizeof(sieos_prcred_t) == 92, "prcred size");
SIEOS_STATIC_ASSERT(sizeof(sieos_prusage_t) == 200, "prusage size");

#endif
