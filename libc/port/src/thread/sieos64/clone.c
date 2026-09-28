/*
 * __clone for threads: an LWP created from a ucontext.  The new LWP starts
 * in lwp_start(fn, arg, ctid) on the given stack with %fs = tls, with the
 * caller's signal mask.  CLONE_CHILD_CLEARTID becomes the LWP's exit word
 * (cleared and woken by the kernel when the LWP ends).  Process creation
 * with shared memory (posix_spawn's CLONE_VM|CLONE_VFORK) is not
 * supported: posix_spawn uses fork on SIEOS.
 */
#define _GNU_SOURCE
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include "pthread_impl.h"
#include "syscall.h"
#undef sa_handler
#undef sa_sigaction
#include "sieos/syscall.h"
#include "sieos/lwp.h"
#include "sieos/signal.h"

static _Noreturn void lwp_start(int (*fn)(void *), void *arg, volatile int *ctid)
{
	if (ctid)
		__syscall(SIEOS_SYS_lwp_private, SIEOS_LWP_SETPRIVATE, SIEOS_LWP_EXITWORD, ctid);
	int r = fn(arg);
	(void)r;
	for (;;) __syscall(SIEOS_SYS_lwp_exit);
}

int __clone(int (*fn)(void *), void *stack, int flags, void *arg, ...)
{
	va_list ap;
	va_start(ap, arg);
	pid_t *ptid = va_arg(ap, pid_t *);
	void *tls = va_arg(ap, void *);
	volatile int *ctid = va_arg(ap, volatile int *);
	va_end(ap);

	if ((flags & (CLONE_VM | CLONE_THREAD)) != (CLONE_VM | CLONE_THREAD))
		return -ENOSYS;
	sieos_ucontext_t uc;
	memset(&uc, 0, sizeof uc);
	unsigned long sp = ((unsigned long)stack & -16UL) - 8;    /* as if lwp_start had been called */
	uc.uc_mcontext.gregs[SIEOS_REG_RIP] = (long)lwp_start;
	uc.uc_mcontext.gregs[SIEOS_REG_RSP] = sp;
	uc.uc_mcontext.gregs[SIEOS_REG_RDI] = (long)fn;
	uc.uc_mcontext.gregs[SIEOS_REG_RSI] = (long)arg;
	uc.uc_mcontext.gregs[SIEOS_REG_RDX] = (flags & CLONE_CHILD_CLEARTID) ? (long)ctid : 0;
	uc.uc_mcontext.gregs[SIEOS_REG_RFL] = 0x202;
	uc.uc_mcontext.gregs[SIEOS_REG_FSBASE] = (flags & CLONE_SETTLS) ? (long)tls
		: __syscall(SIEOS_SYS_lwp_private, SIEOS_LWP_GETPRIVATE, SIEOS_LWP_FSBASE, 0);
	uc.uc_mcontext.fpregs.fpchip_state.cw = 0x37f;
	uc.uc_mcontext.fpregs.fpchip_state.mxcsr = 0x1f80;
	__syscall(SIEOS_SYS_lwp_sigmask, SIEOS_SIG_BLOCK, 0, &uc.uc_sigmask);
	uc.uc_flags = SIEOS_UC_ALL;
	unsigned id = 0;
	long r = __syscall(SIEOS_SYS_lwp_create, &uc, SIEOS_LWP_DETACHED, &id);
	if (r < 0)
		return r;
	if ((flags & CLONE_PARENT_SETTID) && ptid)
		*ptid = id;
	return id;
}
