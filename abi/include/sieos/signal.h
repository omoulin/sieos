/*
 * sieos/signal.h - Signals, siginfo, sigaction and machine contexts (ABI v2).
 *
 * Signal numbers, siginfo codes and the amd64 ucontext/mcontext layout
 * follow Solaris.
 *
 * Delivery: the kernel builds, on the user stack (or the alternate
 * stack with SIEOS_SA_ONSTACK), an sieos_ucontext_t describing the interrupted
 * state and an sieos_siginfo_t, then calls handler(sig, siginfo *, ucontext *)
 * with a zero return address.  The handler must not return: libc wraps
 * user handlers and finishes with context(SIEOS_SETCONTEXT, ucp).
 */
#ifndef SIEOS_ABI_SIGNAL_H
#define SIEOS_ABI_SIGNAL_H

#include "types.h"

#define SIEOS_SIGHUP      1
#define SIEOS_SIGINT      2
#define SIEOS_SIGQUIT     3
#define SIEOS_SIGILL      4
#define SIEOS_SIGTRAP     5
#define SIEOS_SIGABRT     6
#define SIEOS_SIGIOT      SIEOS_SIGABRT
#define SIEOS_SIGEMT      7
#define SIEOS_SIGFPE      8
#define SIEOS_SIGKILL     9
#define SIEOS_SIGBUS     10
#define SIEOS_SIGSEGV    11
#define SIEOS_SIGSYS     12
#define SIEOS_SIGPIPE    13
#define SIEOS_SIGALRM    14
#define SIEOS_SIGTERM    15
#define SIEOS_SIGUSR1    16
#define SIEOS_SIGUSR2    17
#define SIEOS_SIGCHLD    18
#define SIEOS_SIGCLD     SIEOS_SIGCHLD
#define SIEOS_SIGPWR     19
#define SIEOS_SIGWINCH   20
#define SIEOS_SIGURG     21
#define SIEOS_SIGPOLL    22
#define SIEOS_SIGIO      SIEOS_SIGPOLL
#define SIEOS_SIGSTOP    23
#define SIEOS_SIGTSTP    24
#define SIEOS_SIGCONT    25
#define SIEOS_SIGTTIN    26
#define SIEOS_SIGTTOU    27
#define SIEOS_SIGVTALRM  28
#define SIEOS_SIGPROF    29
#define SIEOS_SIGXCPU    30
#define SIEOS_SIGXFSZ    31
#define SIEOS_SIGWAITING 32
#define SIEOS_SIGLWP     33
#define SIEOS_SIGFREEZE  34
#define SIEOS_SIGTHAW    35
#define SIEOS_SIGCANCEL  36    /* reserved for libc thread cancellation */
#define SIEOS_SIGLOST    37
#define SIEOS_SIGXRES    38
#define SIEOS_SIGJVM1    39
#define SIEOS_SIGJVM2    40
#define SIEOS_SIGINFO    41
#define SIEOS_SIGRTMIN   42
#define SIEOS_SIGRTMAX   73
#define SIEOS_NSIG       74    /* valid signals are 1 .. SIEOS_NSIG - 1 */

/* 128-bit signal set: bit (sig - 1) of word (sig - 1) / 32 */
typedef struct {
    unsigned int __sigbits[4];
} sieos_sigset_t;

#define SIEOS_SIG_BLOCK   1
#define SIEOS_SIG_UNBLOCK 2
#define SIEOS_SIG_SETMASK 3

#define SIEOS_SIG_DFL ((void (*)(int))0)
#define SIEOS_SIG_IGN ((void (*)(int))1)
#define SIEOS_SIG_HOLD ((void (*)(int))2)
#define SIEOS_SIG_ERR ((void (*)(int))-1)

/* sa_flags */
#define SIEOS_SA_ONSTACK   0x00000001
#define SIEOS_SA_RESETHAND 0x00000002
#define SIEOS_SA_RESTART   0x00000004
#define SIEOS_SA_SIGINFO   0x00000008
#define SIEOS_SA_NODEFER   0x00000010
#define SIEOS_SA_NOCLDWAIT 0x00010000
#define SIEOS_SA_NOCLDSTOP 0x00020000

union sieos_sigval {
    int   sival_int;
    void *sival_ptr;
};

/* siginfo_t: 256 bytes, Solaris field order (signo, code, errno). */
#define SIEOS_SI_MAXSZ 256
#define SIEOS_SI_PAD   ((SIEOS_SI_MAXSZ / sizeof(int)) - 4)

typedef struct {
    int si_signo;
    int si_code;
    int si_errno;
    int si_pad;
    union {
        int __pad[SIEOS_SI_PAD];
        struct {                        /* kill, sigqueue, SIGCHLD */
            sieos_pid_t pid;
            sieos_uid_t uid;
            int status;                 /* exit status or signal (CLD_*) */
            int __resv;
            union sieos_sigval value;
            sieos_clock_t utime, stime;
        } proc;
        struct {                        /* SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP */
            void *addr;
            int trapno;
            int __resv;
            void *pc;
        } fault;
        struct {                        /* SIGPOLL */
            int fd;
            long band;
        } file;
        struct {                        /* SI_TIMER: a POSIX timer's (value where proc's is) */
            int timerid;
            int overrun;                /* expirations since, while it was pending */
            int __resv[2];
            union sieos_sigval value;
        } timer;
    } __data;
} sieos_siginfo_t;

#define sieos_si_pid    __data.proc.pid
#define sieos_si_uid    __data.proc.uid
#define sieos_si_status __data.proc.status
#define sieos_si_value  __data.proc.value
#define sieos_si_addr   __data.fault.addr

/* sigevent: how a POSIX timer notifies (timer_create, system call 126) */
#define SIEOS_SIGEV_NONE   1
#define SIEOS_SIGEV_SIGNAL 2
#define SIEOS_SIGEV_THREAD 3            /* the C library's (a thread waiting on an event port) */
#define SIEOS_SIGEV_PORT   4            /* an event to a port: sigev_value.sival_ptr is a sieos_port_notify_t * (port.h) */

struct sieos_sigevent {
    int sigev_notify;
    int sigev_signo;
    union sieos_sigval sigev_value;
    void (*sigev_function)(union sieos_sigval);          /* (SIGEV_THREAD: the C library's) */
    void *sigev_attributes;
    int __sigev_pad2;
};

/* si_code values */
#define SIEOS_SI_USER     0
#define SIEOS_SI_LWP      (-1)
#define SIEOS_SI_QUEUE    (-2)
#define SIEOS_SI_TIMER    (-3)
#define SIEOS_SI_ASYNCIO  (-4)
#define SIEOS_SI_MESGQ    (-5)
#define SIEOS_SI_NOINFO   32767

#define SIEOS_ILL_ILLOPC  1
#define SIEOS_ILL_PRVOPC  5
#define SIEOS_FPE_INTDIV  1
#define SIEOS_FPE_FLTDIV  3
#define SIEOS_SEGV_MAPERR 1
#define SIEOS_SEGV_ACCERR 2
#define SIEOS_BUS_ADRALN  1
#define SIEOS_BUS_ADRERR  2
#define SIEOS_TRAP_BRKPT  1
#define SIEOS_CLD_EXITED    1
#define SIEOS_CLD_KILLED    2
#define SIEOS_CLD_DUMPED    3
#define SIEOS_CLD_TRAPPED   4
#define SIEOS_CLD_STOPPED   5
#define SIEOS_CLD_CONTINUED 6

/* Solaris order: flags first. */
struct sieos_sigaction {
    int sa_flags;
    int __pad;
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, sieos_siginfo_t *, void *);
    } __sa_u;
    sieos_sigset_t sa_mask;
    int sa_resv[2];
};

typedef struct {
    void *ss_sp;
    sieos_size_t ss_size;
    int ss_flags;
    int __pad;
} sieos_stack_t;

#define SIEOS_SS_ONSTACK 0x00000001
#define SIEOS_SS_DISABLE 0x00000002
#define SIEOS_MINSIGSTKSZ 2048
#define SIEOS_SIGSTKSZ    8192

/* ---- amd64 machine context (Solaris layout) ---- */
#define SIEOS_REG_R15    0
#define SIEOS_REG_R14    1
#define SIEOS_REG_R13    2
#define SIEOS_REG_R12    3
#define SIEOS_REG_R11    4
#define SIEOS_REG_R10    5
#define SIEOS_REG_R9     6
#define SIEOS_REG_R8     7
#define SIEOS_REG_RDI    8
#define SIEOS_REG_RSI    9
#define SIEOS_REG_RBP   10
#define SIEOS_REG_RBX   11
#define SIEOS_REG_RDX   12
#define SIEOS_REG_RCX   13
#define SIEOS_REG_RAX   14
#define SIEOS_REG_TRAPNO 15
#define SIEOS_REG_ERR   16
#define SIEOS_REG_RIP   17
#define SIEOS_REG_CS    18
#define SIEOS_REG_RFL   19
#define SIEOS_REG_RSP   20
#define SIEOS_REG_SS    21
#define SIEOS_REG_FS    22
#define SIEOS_REG_GS    23
#define SIEOS_REG_ES    24
#define SIEOS_REG_DS    25
#define SIEOS_REG_FSBASE 26
#define SIEOS_REG_GSBASE 27
#define SIEOS_NGREG     28

typedef long sieos_greg_t;
typedef sieos_greg_t sieos_gregset_t[SIEOS_NGREG];

/* FXSAVE image (512 bytes) plus status words, 16-byte aligned. */
typedef struct __attribute__((aligned(16))) {
    struct {
        sieos_uint16_t cw;
        sieos_uint16_t sw;
        sieos_uint8_t  fctw;
        sieos_uint8_t  __fx_rsvd;
        sieos_uint16_t fop;
        sieos_uint64_t rip;
        sieos_uint64_t rdp;
        sieos_uint32_t mxcsr;
        sieos_uint32_t mxcsr_mask;
        sieos_uint8_t  st[8][16];
        sieos_uint8_t  xmm[16][16];
        sieos_uint8_t  __fx_ign2[6][16];
        sieos_uint32_t status;             /* sw at the time of the exception */
        sieos_uint32_t xstatus;            /* mxcsr at the time of the exception */
    } fpchip_state;
} sieos_fpregset_t;

typedef struct {
    sieos_gregset_t gregs;
    sieos_fpregset_t fpregs;
} sieos_mcontext_t;

/* uc_flags */
#define SIEOS_UC_SIGMASK 0x01
#define SIEOS_UC_STACK   0x02
#define SIEOS_UC_CPU     0x04
#define SIEOS_UC_FPU     0x08
#define SIEOS_UC_ALL     (SIEOS_UC_SIGMASK | SIEOS_UC_STACK | SIEOS_UC_CPU | SIEOS_UC_FPU)
/* The whole extended state (AVX, AVX-512) of a signal's frame: an xsave image
 * (standard form) on the stack, uc_filler[0] the magic, [1] its address,
 * [2] its size; setcontext restores it too.  (fpregs stays the x87/SSE part.) */
#define SIEOS_UC_XSAVE   0x10
#define SIEOS_UC_XSAVE_MAGIC 0x5853415645L  /* "XSAVE" */

typedef struct __attribute__((aligned(16))) sieos_ucontext {
    unsigned long uc_flags;
    struct sieos_ucontext *uc_link;
    sieos_sigset_t uc_sigmask;
    sieos_stack_t uc_stack;
    sieos_mcontext_t uc_mcontext;
    long uc_filler[5];
} sieos_ucontext_t;

SIEOS_STATIC_ASSERT(sizeof(sieos_sigset_t) == 16, "sigset size");
SIEOS_STATIC_ASSERT(sizeof(sieos_siginfo_t) == 256, "siginfo size");
SIEOS_STATIC_ASSERT(SIEOS_OFFSETOF(sieos_siginfo_t, si_code) == 4, "si_code offset");
SIEOS_STATIC_ASSERT(sizeof(sieos_fpregset_t) == 528, "fpregset size");
SIEOS_STATIC_ASSERT(SIEOS_OFFSETOF(sieos_mcontext_t, fpregs) == 224, "mcontext fpregs offset");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_sigaction) == 40, "sigaction size");
SIEOS_STATIC_ASSERT(sizeof(sieos_stack_t) == 24, "stack_t size");
SIEOS_STATIC_ASSERT(sizeof(sieos_mcontext_t) == 752, "mcontext size");
SIEOS_STATIC_ASSERT(sizeof(sieos_ucontext_t) == 864, "ucontext size");
SIEOS_STATIC_ASSERT(SIEOS_OFFSETOF(sieos_ucontext_t, uc_mcontext) == 64, "uc_mcontext offset");

#endif
