/*
 * solaris.c - Solaris interfaces of the SIEOS C library: high-resolution
 * time, sysinfo(SI_*), processors, uadmin, LWPs, Solaris threads,
 * closefrom/fdwalk, getexecname, sig2str/str2sig and sigsend.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/time.h>
#include <sys/processor.h>
#include <sys/procset.h>
#include <sys/uadmin.h>
#include <sys/lwp.h>
#include <sys/procfs.h>
#include <thread.h>
#include "syscall.h"
#include "sieos/syscall.h"

/* ---------------- time ---------------- */

hrtime_t gethrtime(void)
{
	return __syscall(SIEOS_SYS_gethrtime);
}

hrtime_t gethrvtime(void)
{
	return __syscall(SIEOS_SYS_gethrvtime);
}

/* ---------------- system ---------------- */

long __sysinfo_si(int cmd, char *buf, long count)
{
	return syscall(SIEOS_SYS_sysinfo, cmd, buf, count);
}

int uadmin(int cmd, int fcn, uintptr_t mdep)
{
	return syscall(SIEOS_SYS_uadmin, cmd, fcn, mdep);
}

/* ---------------- processors ---------------- */

int processor_info(processorid_t id, processor_info_t *info)
{
	return syscall(SIEOS_SYS_processor_info, id, info);
}

int p_online(processorid_t id, int flag)
{
	return syscall(SIEOS_SYS_p_online, id, flag);
}

int processor_bind(idtype_t type, id_t id, processorid_t cpu, processorid_t *obind)
{
	return syscall(SIEOS_SYS_processor_bind, type, (long)(int)id, cpu, obind);   /* P_MYID is -1 */
}

/* The processor the calling LWP last ran on (its lwpsinfo). */
processorid_t getcpuid(void)
{
	char path[64];
	lwpsinfo_t li;
	snprintf(path, sizeof path, "/proc/self/lwp/%u/lwpsinfo", _lwp_self());
	FILE *f = fopen(path, "re");
	if (!f)
		return -1;
	size_t n = fread(&li, sizeof li, 1, f);
	fclose(f);
	return n == 1 ? li.pr_onpro : -1;
}

/* ---------------- LWPs ---------------- */

lwpid_t _lwp_self(void)
{
	return __syscall(SIEOS_SYS_lwp_self);
}

int _lwp_kill(lwpid_t id, int sig)
{
	return -__syscall(SIEOS_SYS_lwp_kill, id, sig);
}

int _lwp_suspend(lwpid_t id)
{
	return -__syscall(SIEOS_SYS_lwp_suspend, id);
}

int _lwp_continue(lwpid_t id)
{
	return -__syscall(SIEOS_SYS_lwp_continue, id);
}

int _lwp_info(void *p)
{
	(void)p;
	return ENOSYS;
}

int sigsend(idtype_t type, id_t id, int sig)
{
	pid_t me = getpid();
	switch (type) {
	case P_PID:
		return kill(id == (id_t)P_MYID ? me : (pid_t)id, sig);
	case P_PGID:
		return kill(-(id == (id_t)P_MYID ? getpgrp() : (pid_t)id), sig);
	case P_SID:
		if (id == (id_t)P_MYID || (pid_t)id == getsid(0))
			return kill(0, sig);
		break;
	case P_ALL:
		return kill(-1, sig);
	case P_LWPID: {
		int e = _lwp_kill(id == (id_t)P_MYID ? _lwp_self() : id, sig);
		if (e) {
			errno = e;
			return -1;
		}
		return 0;
	}
	default:
		break;
	}
	errno = EINVAL;
	return -1;
}

/* ---------------- Solaris threads ---------------- */

int thr_create(void *stack, size_t size, void *(*fn)(void *), void *arg, long flags, thread_t *out)
{
	pthread_attr_t a;
	pthread_t t;
	int r;
	if (flags & THR_SUSPENDED)
		return ENOTSUP;
	if ((r = pthread_attr_init(&a)))
		return r;
	if (stack && size)
		r = pthread_attr_setstack(&a, stack, size);
	else if (size)
		r = pthread_attr_setstacksize(&a, size);
	if (!r && (flags & (THR_DETACHED | THR_DAEMON)))
		r = pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
	if (!r)
		r = pthread_create(&t, &a, fn, arg);
	pthread_attr_destroy(&a);
	if (!r && out)
		*out = t;
	return r;
}

int thr_join(thread_t t, thread_t *departed, void **status)
{
	if (!t)
		return ENOTSUP;                      /* joining "any thread" has no POSIX equivalent */
	int r = pthread_join(t, status);
	if (!r && departed)
		*departed = t;
	return r;
}

void thr_exit(void *status)
{
	pthread_exit(status);
}

thread_t thr_self(void)
{
	return pthread_self();
}

int thr_main(void)
{
	return _lwp_self() == 1;
}

int thr_kill(thread_t t, int sig)
{
	return pthread_kill(t, sig);
}

int thr_sigsetmask(int how, const sigset_t *set, sigset_t *old)
{
	return pthread_sigmask(how, set, old);
}

void thr_yield(void)
{
	sched_yield();
}

size_t thr_min_stack(void)
{
	return PTHREAD_STACK_MIN;
}

int thr_keycreate(thread_key_t *k, void (*dtor)(void *))
{
	return pthread_key_create(k, dtor);
}

int thr_setspecific(thread_key_t k, void *v)
{
	return pthread_setspecific(k, v);
}

int thr_getspecific(thread_key_t k, void **v)
{
	*v = pthread_getspecific(k);
	return 0;
}

static int concurrency;

int thr_getconcurrency(void)
{
	return concurrency;
}

int thr_setconcurrency(int n)
{
	if (n < 0)
		return EINVAL;
	concurrency = n;
	return 0;
}

/* ---------------- descriptors and the program ---------------- */

void closefrom(int low)
{
	long max = sysconf(_SC_OPEN_MAX);
	for (long fd = low < 0 ? 0 : low; fd < max; fd++)
		__syscall(SYS_close, fd);
}

/* Call f(cd, fd) for every open descriptor, in order, until it returns non-zero. */
int fdwalk(int (*f)(void *, int), void *cd)
{
	long max = sysconf(_SC_OPEN_MAX);
	for (long fd = 0; fd < max; fd++) {
		if (__syscall(SYS_fcntl, fd, F_GETFD) < 0)
			continue;
		int r = f(cd, fd);
		if (r)
			return r;
	}
	return 0;
}

const char *getexecname(void)
{
	return (const char *)getauxval(2014);           /* AT_SUN_EXECNAME */
}

/* ---------------- signal names ---------------- */

static const struct { const char *name; int sig; } signames[] = {
	{ "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "ILL", SIGILL }, { "TRAP", SIGTRAP },
	{ "ABRT", SIGABRT }, { "IOT", SIGABRT }, { "EMT", SIGEMT }, { "FPE", SIGFPE }, { "KILL", SIGKILL },
	{ "BUS", SIGBUS }, { "SEGV", SIGSEGV }, { "SYS", SIGSYS }, { "PIPE", SIGPIPE }, { "ALRM", SIGALRM },
	{ "TERM", SIGTERM }, { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 }, { "CLD", SIGCHLD }, { "CHLD", SIGCHLD },
	{ "PWR", SIGPWR }, { "WINCH", SIGWINCH }, { "URG", SIGURG }, { "POLL", SIGPOLL }, { "IO", SIGPOLL },
	{ "STOP", SIGSTOP }, { "TSTP", SIGTSTP }, { "CONT", SIGCONT }, { "TTIN", SIGTTIN }, { "TTOU", SIGTTOU },
	{ "VTALRM", SIGVTALRM }, { "PROF", SIGPROF }, { "XCPU", SIGXCPU }, { "XFSZ", SIGXFSZ },
	{ "WAITING", SIGWAITING }, { "LWP", SIGLWP }, { "FREEZE", SIGFREEZE }, { "THAW", SIGTHAW },
	{ "CANCEL", 36 }, { "LOST", SIGLOST }, { "XRES", SIGXRES }, { "JVM1", SIGJVM1 }, { "JVM2", SIGJVM2 },
	{ "INFO", SIGINFO },
};

int sig2str(int sig, char *buf)
{
	int rtmin = SIGRTMIN, rtmax = SIGRTMAX;
	for (size_t i = 0; i < sizeof signames / sizeof *signames; i++)
		if (signames[i].sig == sig) {
			strcpy(buf, signames[i].name);
			return 0;
		}
	if (sig == rtmin) { strcpy(buf, "RTMIN"); return 0; }
	if (sig == rtmax) { strcpy(buf, "RTMAX"); return 0; }
	if (sig > rtmin && sig < rtmax) {
		if (sig - rtmin <= (rtmax - rtmin) / 2)
			sprintf(buf, "RTMIN+%d", sig - rtmin);
		else
			sprintf(buf, "RTMAX-%d", rtmax - sig);
		return 0;
	}
	return -1;
}

int str2sig(const char *s, int *sig)
{
	int rtmin = SIGRTMIN, rtmax = SIGRTMAX;
	char *end;
	if (*s >= '0' && *s <= '9') {
		long n = strtol(s, &end, 10);
		if (*end || n <= 0 || n > rtmax)
			return -1;
		*sig = n;
		return 0;
	}
	for (size_t i = 0; i < sizeof signames / sizeof *signames; i++)
		if (!strcmp(s, signames[i].name)) {
			*sig = signames[i].sig;
			return 0;
		}
	if (!strncmp(s, "RTMIN", 5) || !strncmp(s, "RTMAX", 5)) {
		int base = s[4] == 'N' ? rtmin : rtmax;
		long off = 0;
		if (s[5]) {
			if ((s[4] == 'N' && s[5] != '+') || (s[4] == 'X' && s[5] != '-'))
				return -1;
			off = strtol(s + 6, &end, 10);
			if (*end || off < 0)
				return -1;
			if (s[5] == '-')
				off = -off;
		}
		if (base + off < rtmin || base + off > rtmax)
			return -1;
		*sig = base + off;
		return 0;
	}
	return -1;
}
