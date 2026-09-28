/*
 * sieos/lwp.h - lightweight processes (ABI v2).
 *
 * A process owns one or more LWPs (kernel-scheduled threads).  They share
 * the address space, file descriptors, current directory, credentials and
 * signal dispositions; each LWP has its own registers, FPU state, signal
 * mask, pending signals and TLS base (%fs).
 *
 * lwp_create(ucp, flags, &id) starts an LWP with the registers in
 * ucp->uc_mcontext.gregs (SIEOS_REG_RIP, SIEOS_REG_RSP, SIEOS_REG_RDI...,
 * SIEOS_REG_FSBASE) and the signal mask in ucp->uc_sigmask.
 *
 * lwp_umtx_wait/wake are address-based sleep/wake primitives for user
 * synchronisation objects (the kernel side of mutexes and condvars):
 * wait sleeps if *addr == expected, wake wakes up to count sleepers.
 */
#ifndef SIEOS_ABI_LWP_H
#define SIEOS_ABI_LWP_H

/* lwp_create() flags */
#define SIEOS_LWP_DAEMON    0x00000020
#define SIEOS_LWP_DETACHED  0x00000040     /* not joinable with lwp_wait */
#define SIEOS_LWP_SUSPENDED 0x00000080

/* lwp_private() */
#define SIEOS_LWP_SETPRIVATE 0
#define SIEOS_LWP_GETPRIVATE 1
#define SIEOS_LWP_FSBASE     0              /* %fs: TLS pointer (variant II, TCB self-pointer) */
#define SIEOS_LWP_GSBASE     1
#define SIEOS_LWP_EXITWORD   2              /* int *: set to 0 and lwp_umtx_wake'd (all) when the LWP exits */
#define SIEOS_LWP_ROBUSTLIST 3              /* robust mutex list head, walked when the LWP exits */

/* Robust mutexes: the list registered with lwp_private(SETPRIVATE, ROBUSTLIST).
 * When the LWP exits, each listed lock word whose owner bits equal its
 * lwpid becomes (word & WAITERS) | OWNER_DIED and one waiter is woken. */
struct sieos_robust_list {
    struct sieos_robust_list *next;
};
struct sieos_robust_list_head {
    struct sieos_robust_list list;         /* circular; &list when empty */
    long futex_offset;                     /* lock word = entry + futex_offset */
    struct sieos_robust_list *list_op_pending;
};
#define SIEOS_ROBUST_WAITERS    0x80000000U
#define SIEOS_ROBUST_OWNER_DIED 0x40000000U
#define SIEOS_ROBUST_TID_MASK   0x3fffffffU

/* lwp_umtx_*() flags */
#define SIEOS_UMTX_PRIVATE   0x1            /* waiters are in this process only */
#define SIEOS_UMTX_ABSTIME   0x2            /* timeout is CLOCK_REALTIME absolute */

/* lwp_name() */
#define SIEOS_LWP_NAME_GET   0
#define SIEOS_LWP_NAME_SET   1
#define SIEOS_LWP_NAME_MAX   32

#endif
