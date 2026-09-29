/*
 * proc.h - Processes, credentials, signals and scheduling.
 */
#ifndef SIEOS_PROC_H
#define SIEOS_PROC_H

#include "kernel.h"
#include "arch.h"
#include "ksig.h"
#include "smp.h"
#include "cpu.h"

#define NPROC       256
#define NOFILE      64
#define KSTACK_SIZE (16 * 1024)
#define MAXARGS     (256 * 1024)
#define MAXENV      (256 * 1024)
#define MAXARGSTR   2096640              /* ARG_MAX (as Solaris): argument and environment strings and pointers */
#define MAXPATH     1024

struct file;
struct inode;

struct ksigaction {
    uint64_t handler;           /* SIG_DFL (0), SIG_IGN (1) or user address */
    uint64_t flags;             /* v1 SA_* or v2 SIEOS_SA_* (see v2) */
    uint64_t restorer;          /* v1 only */
    ksigset_t mask;
};

/*
 * A process owns the address space, descriptors, directories, credentials,
 * signal dispositions and its LWPs (lightweight processes, the kernel's
 * threads).  The scheduler runs LWPs.
 */
enum { LWP_UNUSED, LWP_EMBRYO, LWP_RUNNABLE, LWP_RUNNING, LWP_SLEEPING, LWP_STOPPED, LWP_SUSPENDED, LWP_ZOMBIE };

struct lwp {
    int state;                  /* LWP_* */
    int lwpid;                  /* 1, 2, ... within the process */
    struct proc *proc;
    bool is_idle;
    int cpu;                    /* CPU it last ran on */
    char name[32];

    uint64_t kstack;            /* base of kernel stack */
    uint64_t ctx_rsp;           /* saved kernel rsp when switched out */
    struct trapframe *tf;       /* user trap frame at top of kstack */
    struct fpu_state fpu;       /* x87/SSE registers while switched out */
    uint64_t fsbase;            /* TLS pointer */

    void *chan;                 /* sleep channel */
    uint64_t wake_tick;         /* for timed sleeps */
    uint64_t wake_ns;           /* high-resolution sleeps: the hrtime to wake at */
    uint64_t ticks;             /* CPU time */
    uint64_t sticks;            /* ... of which in the kernel */
    uint64_t nvcsw, nivcsw;     /* voluntary / involuntary context switches */
    uint64_t minflt;            /* page faults resolved */
    int bound;                  /* processor_bind: CPU + 1, 0 = unbound */
    int cid;                    /* scheduling class (sieos/priocntl.h), see sched.c */
    short upri, uprilim;        /* TS/FX user priority and its limit */
    short cpupri;               /* TS: the dispatcher priority */
    short rtpri;                /* RT: the priority */
    uint32_t quantum;           /* RT/FX: ticks, 0 = none */
    uint64_t last_run;          /* tick it last ran at */

    ksigset_t sig_blocked;
    ksigset_t sig_pending;      /* LWP-directed signals */
    struct ksiginfo siginfo[KNSIG];
    bool restart_syscall;       /* last syscall returned -ERESTART */
    uint64_t orig_rax;          /* syscall number for restarts */
    bool saved_mask_valid;      /* sigsuspend: restore this mask after delivery */
    ksigset_t saved_mask;
    uint64_t altstack_sp, altstack_size;
    int altstack_flags;

    bool detached;
    bool suspend_req;           /* lwp_suspend: stop at the next return to user mode */
    bool park_token;            /* lwp_unpark before lwp_park */
    uint64_t umtx_key;          /* physical address waited on (lwp_umtx_wait) */
    bool must_exit;             /* the process is exiting or exec'ing */
    uint64_t exit_word;         /* lwp_private(EXITWORD): cleared and woken at exit */
    uint64_t robust_list;       /* lwp_private(ROBUSTLIST): robust mutexes held */
};

struct proc {
    int pid;
    int state;                  /* PSTATE_UNUSED, PSTATE_EMBRYO, PSTATE_RUNNING (alive), PSTATE_ZOMBIE */
    struct proc *parent;
    char name[32];
    char psargs[80];            /* initial arguments, for /proc psinfo */
    int argc;
    uint64_t argv_addr, envp_addr;

    uint64_t pml4;              /* physical address of page tables */
    uint64_t heap_start;
    struct spinlock vmlock;     /* the address space: areas and page tables (vm.c) */
    uint64_t brk;
    struct vm_area *areas;      /* mmap regions (vm.c) */

    int exit_status;            /* wait status word, kernel signal numbers */
    uint64_t ticks;             /* CPU time of the LWPs that have exited */
    uint64_t child_ticks;       /* CPU time of waited-for children */
    struct kusage { uint64_t sticks, nvcsw, nivcsw, minflt; } ru, cru;   /* exited LWPs; children */
    uint64_t maxrss_kb;
    int cpu_limit_sent;         /* RLIMIT_CPU: seconds already signalled */
    uint64_t cpu_total;         /* CPU ticks of every LWP (for RLIMIT_CPU) */
    uint64_t start_tick;

    /* credentials */
    int uid, euid, suid;
    int gid, egid, sgid;
    int ngroups;
    int groups[NGROUPS_MAX];
    int umask;

    /* job control */
    int pgid;
    int sid;

    /* signals: dispositions and process-directed pending signals */
    ksigset_t sig_pending;
    struct ksiginfo siginfo[KNSIG];
    struct ksigaction sigact[KNSIG];
    bool stopped;               /* job-control stop: every LWP stops */
    int stop_sig;               /* signal that stopped us */
    bool stop_reported;         /* stop already reported to wait() */
    bool cont_pending;          /* continued, not yet reported */
    bool exiting;               /* an LWP called exit or took a fatal signal */
    bool nosigchld, waitpid_only;   /* forkx flags */

    struct file *ofile[NOFILE];
    uint8_t fdflags[NOFILE];    /* FD_CLOEXEC */
    struct inode *cwd;
    struct inode *root;

    int nlwp;                   /* LWPs not yet exited */
    int next_lwpid;

    /* interval timers (ITIMER_REAL, VIRTUAL, PROF): ticks left and reload */
    uint64_t itimer_value[3], itimer_interval[3];
    /* resource limits (SIEOS_RLIMIT_*): current and maximum */
    uint64_t rlim_cur[16], rlim_max[16];
};

#define curlwp (mycpu()->lwp)
#define current (curlwp->proc)
extern struct proc proc_table[NPROC];
#define NLWP 512
extern struct lwp lwp_table[NLWP];

void proc_init_cpu(struct cpu *c);
void cpu_idle(void) __attribute__((noreturn));
void schedule(void);
void sleep_on(void *chan);
void wakeup(void *chan);
void make_runnable(struct lwp *l);
void sched_tick(struct trapframe *tf);   /* per-CPU timer: accounting + preemption */
void clock_tick(uint64_t n);             /* global clock: n ticks passed; timed sleepers */
int  proc_state(struct proc *p);         /* PSTATE_* summary of a process and its LWPs */
extern unsigned long loadavg[3];         /* fixed point, 1.0 = 2048 */

int  proc_spawn_init(const char *path, const char *cmdline);
long proc_fork(int flags);
struct fs;
bool proc_table_uses(struct fs *fs);     /* a working or root directory, or a mapping, is on fs */
long proc_exec(const char *path, char *const argv[], char *const envp[]);
void proc_exit(int status) __attribute__((noreturn));
void lwp_exit_self(void) __attribute__((noreturn));
long proc_waitid(int idtype, long id, int options, struct ksiginfo *info, int *status_word);
long proc_sleep_until(uint64_t wake_tick);
long proc_sleep_until_ns(uint64_t when_ns);   /* nanosleep with hr_timers() */
uint64_t hr_wake(uint64_t now_ns);         /* wake due high-resolution sleepers; the next deadline */
struct proc *proc_find(int pid);
struct lwp *lwp_find(struct proc *p, int lwpid);
bool pgrp_exists_in_session(int pgid, int sid);
long proc_setpgid(int pid, int pgid);
long proc_setsid(void);
struct lwp *lwp_alloc(struct proc *p);

/* sched.c: scheduling classes */
void sched_init_lwp(struct lwp *l, struct lwp *from);
int  sched_gpri(const struct lwp *l);
uint32_t sched_quantum(const struct lwp *l);
void sched_expired(struct lwp *l);
void sched_woke(struct lwp *l);
void sched_second(void);
const char *sched_class_name(int cid);
long sys2_priocntl(long idtype, long id, long cmd, void *arg);
void lwp_exit_word(struct lwp *l);        /* lwp.c */

/* signal.c */
void signal_send(struct proc *p, int sig);
void signal_send_info(struct proc *p, int sig, const struct ksiginfo *info);
void signal_purge(struct proc *p, struct lwp *l, int sig);   /* queued real-time signals */
void signal_lwp(struct lwp *l, int sig, const struct ksiginfo *info);
int  signal_pgrp(int pgid, int sig);
bool signal_pending(struct proc *p);
bool signal_ignored_or_blocked(struct proc *p, int sig);
void signal_deliver(struct trapframe *tf);
void signal_exec_reset(struct proc *p);
void signal_fault(int sig, int code, uint64_t addr);    /* from the trap handler */
long kill_pids(int pid, int sig, const struct ksiginfo *info);    /* sig in kernel numbering */

/* cred helpers (perm.c) */
bool cred_in_group(struct proc *p, int gid);

#endif
