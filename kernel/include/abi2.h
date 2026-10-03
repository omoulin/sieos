/*
 * abi2.h - Kernel side of ABI v2 (the syscall instruction).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_ABI2_H
#define SIEOS_ABI2_H

#include "kernel.h"

#include "ksig.h"
#include "sieos/procfs.h"

struct trapframe;
struct file;

/* kernel-internal return: the handler already set the user registers (context) */
#define EJUSTRETURN 514

/* errors ABI v1 has no number for (values from the Linux table, translated by sieos_errno) */
#define EDEADLK    35
#define ENOLCK     37
#define ETIME      62
#define EOVERFLOW  75
#define EMLINK     31
#define ETXTBSY    26
#define EDOM       33
#define ECANCELED 125
#define ENOTSUP_K 252          /* SIEOS_ENOTSUP (v1 folds it into EOPNOTSUPP) */
#define ENOPROTOOPT_K 92
#define ENOMSG_K   42
#define EIDRM_K    43
#define ELIBBAD_K  80           /* a bad dynamic linker (SIEOS_ELIBBAD) */
#define EPROTOTYPE_K 91
#define ENOTBLK_K  15           /* SIEOS_ENOTBLK */

long syscall_dispatch_v2(struct trapframe *tf);
long syscall_file_v2(struct trapframe *tf, bool *handled);    /* sysfile2.c */
long syscall_fdext_v2(struct trapframe *tf, bool *handled);   /* fdext.c */
struct sieos_ucred;
long sys2_ucredsys(long op, long id, struct sieos_ucred *ubuf);  /* unix.c */
long syscall_misc_v2(struct trapframe *tf, bool *handled);    /* sysmisc2.c */
long syscall_sock_v2(struct trapframe *tf, bool *handled);    /* sock2.c */
extern char sys_hostname[65];

/* ipc.c */
struct proc;
long sys2_msgsys(long op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
long sys2_semsys(long op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
long sys2_shmsys(long op, uint64_t a1, uint64_t a2, uint64_t a3);
void ipc_proc_exit(struct proc *p);
long sieos_errno(long v1_errno);                 /* ABI v1 (Linux-like) errno -> Solaris value */

/* signal.c */
void      sig_gregs_to_tf(const sieos_greg_t *g, struct trapframe *tf);
void      sig_tf_to_gregs(const struct trapframe *tf, sieos_greg_t *g, uint64_t fsbase);
bool      core_dump(int sig, const struct trapframe *tf);   /* core.c: true if "core" was written */

/* procfs.c: the /proc records (sieos/procfs.h), also written into core files */
struct proc;
void procfs_psinfo(struct proc *p, sieos_psinfo_t *ps);
void procfs_pstatus(struct proc *p, sieos_pstatus_t *st);
ksigset_t sig_set_from_v2(const sieos_sigset_t *s);
void sig_set_to_v2(ksigset_t k, sieos_sigset_t *s);
long sys2_sigaction(int sig, const struct sieos_sigaction *act, struct sieos_sigaction *old);
long sys2_sigmask(int how, const sieos_sigset_t *set, sieos_sigset_t *old);
long sys2_sigpending(int op, sieos_sigset_t *set);
long sys2_sigsuspend(const sieos_sigset_t *set);
long sys2_sigaltstack(const sieos_stack_t *ss, sieos_stack_t *old);
long sys2_sigqueue(int pid, int sig, uint64_t value);
long sys2_sigtimedwait(const sieos_sigset_t *set, sieos_siginfo_t *info, const struct sieos_timespec *timeout);
long sys2_context(int op, sieos_ucontext_t *ucp, struct trapframe *tf);
long sys2_lwp_kill(int lwpid, int sig);

/* lwp.c */
long sys2_lwp_create(const sieos_ucontext_t *ucp, int flags, sieos_lwpid_t *idp);
long sys2_lwp_wait(int id, sieos_lwpid_t *departed);
long sys2_lwp_suspend(int id);
long sys2_lwp_continue(int id);
long sys2_lwp_park(const struct sieos_timespec *timeout, int unpark_first);
long sys2_lwp_unpark(int id);
long sys2_lwp_unpark_all(const sieos_lwpid_t *ids, int n);
long sys2_lwp_private(int op, int which, uint64_t base);
long sys2_lwp_umtx_wait(uint64_t addr, int expected, const struct sieos_timespec *timeout, int flags);
long sys2_lwp_umtx_wake(uint64_t addr, int count, int flags);
long sys2_lwp_name(int op, int id, char *buf, size_t len);

/* helpers exported by syscall.c */
bool user_ok(const void *p, size_t n, bool write);
int  user_fetch_str(const char *u, char *k, size_t max);
struct file *fd_file(int fd);

#endif
