/*
 * emu.c - Linux system calls musl uses, in terms of the SIEOS ABI v2.
 *
 * musl calls the kernel through __syscallN(SYS_x, ...).  SYS_ names whose
 * SIEOS call has the same arguments and meaning are the SIEOS numbers; the
 * others get numbers from __SIEOS_EMU_BASE up and arrive here
 * (arch/sieos64/syscall_arch.h), or from __SIEOS_NOSYS_BASE up and fail
 * with ENOSYS.  Return values follow musl's convention: -errno on error.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/select.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sched.h>
#include <limits.h>
#include <stdbool.h>
#include "syscall.h"
#include "atomic.h"
#include "ksigaction.h"
#include <dirent.h>
/* musl names sigaction fields with macros; the ABI structure has the plain names */
#undef sa_handler
#undef sa_sigaction
#include "sieos/abi.h"

#define S(n) SIEOS_SYS_##n

/* ---------------- helpers ---------------- */

/* getpid, getuid and getgid return a second value in rdx. */
static long sys_rv2(long n, long *second)
{
	unsigned long ret, rdx;
	unsigned char cf;
	__asm__ __volatile__ ("syscall; setc %2" : "=a"(ret), "=d"(rdx), "=r"(cf) : "a"(n)
			      : "rcx", "r11", "memory", "cc");
	if (second) *second = rdx;
	return cf ? -(long)ret : (long)ret;
}

static long open_ro(const char *path)
{
	return __syscall(S(openat), SIEOS_AT_FDCWD, path, SIEOS_O_RDONLY | SIEOS_O_CLOEXEC, 0);
}

/* ---------------- signals ---------------- */

static int si_code_from_k(int code)
{
	switch (code) {
	case SIEOS_SI_LWP:     return SI_TKILL;
	case SIEOS_SI_QUEUE:   return SI_QUEUE;
	case SIEOS_SI_TIMER:   return SI_TIMER;
	case SIEOS_SI_ASYNCIO: return SI_ASYNCIO;
	case SIEOS_SI_MESGQ:   return SI_MESGQ;
	case SIEOS_SI_NOINFO:  return SI_KERNEL;
	}
	return code;
}

hidden void __sieos_siginfo(siginfo_t *si, const sieos_siginfo_t *k)
{
	memset(si, 0, sizeof *si);
	si->si_signo = k->si_signo;
	si->si_code = si_code_from_k(k->si_code);
	si->si_errno = k->si_errno;
	int sig = k->si_signo;
	if (k->si_code > 0 && (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGTRAP)) {
		si->si_addr = k->__data.fault.addr;
	} else {
		si->si_pid = k->__data.proc.pid;
		si->si_uid = k->__data.proc.uid;
		if (sig == SIGCHLD && k->si_code > 0) {
			si->si_status = k->__data.proc.status;
			si->si_utime = k->__data.proc.utime;
			si->si_stime = k->__data.proc.stime;
		} else {
			si->si_value.sival_ptr = k->__data.proc.value.sival_ptr;
		}
	}
}

/* The kernel calls this for every caught signal; it runs the user's
 * handler and resumes the interrupted context (handlers never return to
 * the kernel: context(SETCONTEXT) does). */
static void (*volatile handlers[_NSIG])();
static volatile int handler_flags[_NSIG];

static void sig_wrapper(int sig, sieos_siginfo_t *ksi, void *uc)
{
	void (*h)() = handlers[sig];
	if (handler_flags[sig] & SA_SIGINFO) {
		siginfo_t si;
		if (ksi)
			__sieos_siginfo(&si, ksi);
		else
			memset(&si, 0, sizeof si), si.si_signo = sig;
		((void (*)(int, siginfo_t *, void *))h)(sig, &si, uc);
	} else {
		((void (*)(int))h)(sig);
	}
	__syscall(S(context), SIEOS_SETCONTEXT, uc);
	for (;;) __syscall(S(exit), 127);            /* not reached */
}

static long emu_rt_sigaction(int sig, const struct k_sigaction *ksa, struct k_sigaction *kold)
{
	struct sieos_sigaction n, o;
	if (sig < 1 || sig >= _NSIG)
		return -EINVAL;
	void (*oldh)() = handlers[sig];
	int oldf = handler_flags[sig];
	if (ksa) {
		memset(&n, 0, sizeof n);
		n.sa_flags = ksa->flags & (SA_ONSTACK | SA_RESETHAND | SA_RESTART | SA_SIGINFO | SA_NODEFER |
		                           SA_NOCLDWAIT | SA_NOCLDSTOP);
		memcpy(&n.sa_mask, ksa->mask, sizeof n.sa_mask);
		if ((unsigned long)ksa->handler > 1) {
			handlers[sig] = ksa->handler;
			handler_flags[sig] = ksa->flags;
			n.sa_flags |= SA_SIGINFO;
			n.__sa_u.sa_sigaction = (void (*)(int, sieos_siginfo_t *, void *))sig_wrapper;
		} else {
			n.__sa_u.sa_handler = ksa->handler;
		}
	}
	long r = __syscall(S(sigaction), sig, ksa ? &n : 0, &o);
	if (r < 0) {
		if (ksa) {
			handlers[sig] = oldh;
			handler_flags[sig] = oldf;
		}
		return r;
	}
	if (kold) {
		memset(kold, 0, sizeof *kold);
		if (o.__sa_u.sa_sigaction == (void *)sig_wrapper) {
			kold->handler = (void (*)(int))oldh;
			kold->flags = (o.sa_flags & ~SA_SIGINFO) | (oldf & SA_SIGINFO);
		} else {
			kold->handler = o.__sa_u.sa_handler;
			kold->flags = o.sa_flags;
		}
		memcpy(kold->mask, &o.sa_mask, sizeof o.sa_mask);
	}
	return 0;
}

static long emu_sigtimedwait(const sigset_t *set, siginfo_t *info, const struct timespec *ts)
{
	sieos_siginfo_t k;
	long r = __syscall(S(sigtimedwait), set, &k, ts);
	if (r > 0 && info)
		__sieos_siginfo(info, &k);
	return r;
}

/* ---------------- processes ---------------- */

static int status_word(const sieos_siginfo_t *k)
{
	int st = k->__data.proc.status;
	switch (k->si_code) {
	case SIEOS_CLD_EXITED:    return (st & 0xff) << 8;
	case SIEOS_CLD_KILLED:    return st & 0x7f;
	case SIEOS_CLD_DUMPED:    return (st & 0x7f) | 0x80;
	case SIEOS_CLD_STOPPED:
	case SIEOS_CLD_TRAPPED:   return ((st & 0xff) << 8) | 0x7f;
	case SIEOS_CLD_CONTINUED: return 0xffff;
	}
	return 0;
}

static long emu_wait4(pid_t pid, int *status, int options, struct rusage *ru)
{
	int idtype;
	long id;
	if (pid < -1) {
		idtype = P_PGID, id = -pid;
	} else if (pid == -1) {
		idtype = P_ALL, id = 0;
	} else if (pid == 0) {
		idtype = P_PGID, id = __syscall(S(pgrpsys), SIEOS_PGRP_GETPGRP, 0, 0);
	} else {
		idtype = P_PID, id = pid;
	}
	if (options & ~(WNOHANG | WUNTRACED | WCONTINUED | WNOWAIT))
		return -EINVAL;
	sieos_siginfo_t k;
	memset(&k, 0, sizeof k);
	long r = __syscall(S(waitid), idtype, id, &k, options | WEXITED);
	if (r < 0)
		return r;
	if (!k.__data.proc.pid)
		return 0;                                /* WNOHANG, nothing yet */
	if (status)
		*status = status_word(&k);
	if (ru) {
		memset(ru, 0, sizeof *ru);
		__syscall(S(getrusage), RUSAGE_CHILDREN, ru);
	}
	return k.__data.proc.pid;
}

static long emu_waitid(idtype_t type, id_t id, siginfo_t *info, int options)
{
	sieos_siginfo_t k;
	memset(&k, 0, sizeof k);
	long r = __syscall(S(waitid), type, id, &k, options);
	if (r == 0 && info) {
		if (k.__data.proc.pid)
			__sieos_siginfo(info, &k);
		else
			memset(info, 0, sizeof *info);
	}
	return r;
}

/* The saved IDs are in /proc/self/cred. */
static long read_cred(sieos_prcred_t *c)
{
	long fd = open_ro("/proc/self/cred");
	if (fd < 0)
		return fd;
	long n = __syscall(S(read), fd, c, sizeof *c);
	__syscall(S(close), fd);
	return n == sizeof *c ? 0 : -EIO;
}

/* setresuid/setresgid on setuid/seteuid/setreuid (the saved ID follows the kernel's rules). */
static long emu_setresid(long r, long e, long s, bool gid)
{
	long set = gid ? S(setgid) : S(setuid), sete = gid ? S(setegid) : S(seteuid);
	long setre = gid ? S(setregid) : S(setreuid);
	(void)s;
	if (r == -1 && s == -1)
		return e == -1 ? 0 : __syscall(sete, e);
	if (r == e && e == s)
		return __syscall(set, r);
	return __syscall(setre, r, e);
}

static long emu_getresid(unsigned *r, unsigned *e, unsigned *s, bool gid)
{
	sieos_prcred_t c;
	long ret = read_cred(&c);
	if (ret < 0) {
		long second;
		long real = sys_rv2(gid ? S(getgid) : S(getuid), &second);
		c.pr_ruid = c.pr_rgid = real;
		c.pr_euid = c.pr_egid = c.pr_suid = c.pr_sgid = second;
	}
	*r = gid ? c.pr_rgid : c.pr_ruid;
	*e = gid ? c.pr_egid : c.pr_euid;
	*s = gid ? c.pr_sgid : c.pr_suid;
	return 0;
}

static long emu_uname(struct utsname *u)
{
	static const int cmd[] = { SIEOS_SI_SYSNAME, SIEOS_SI_HOSTNAME, SIEOS_SI_RELEASE, SIEOS_SI_VERSION,
	                           SIEOS_SI_MACHINE, SIEOS_SI_SRPC_DOMAIN };
	char *field[] = { u->sysname, u->nodename, u->release, u->version, u->machine, u->domainname };
	for (int i = 0; i < 6; i++) {
		long r = __syscall(S(sysinfo), cmd[i], field[i], sizeof u->sysname);
		if (r < 0)
			field[i][0] = 0;
	}
	return 0;
}

static long emu_sysinfo(struct sysinfo *si)
{
	memset(si, 0, sizeof *si);
	si->uptime = __syscall(S(gethrtime)) / 1000000000L;
	long avg[3] = { 0 };
	__syscall(S(getloadavg), avg, 3);
	for (int i = 0; i < 3; i++)
		si->loads[i] = (unsigned long)avg[i] * 65536 / 1000;
	si->totalram = __syscall(S(sysconfig), SIEOS_CONFIG_PHYS_PAGES);
	si->freeram = __syscall(S(sysconfig), SIEOS_CONFIG_AVPHYS_PAGES);
	si->procs = 1;
	si->mem_unit = 4096;
	return 0;
}

static long emu_reboot(int m1, int m2, int cmd)
{
	(void)m1, (void)m2;
	switch ((unsigned)cmd) {
	case 0x01234567:                         /* RB_AUTOBOOT */
		return __syscall(S(uadmin), SIEOS_A_REBOOT, SIEOS_AD_BOOT, 0);
	case 0xcdef0123:                         /* RB_HALT_SYSTEM */
		return __syscall(S(uadmin), SIEOS_A_SHUTDOWN, SIEOS_AD_HALT, 0);
	case 0x4321fedc:                         /* RB_POWER_OFF */
		return __syscall(S(uadmin), SIEOS_A_SHUTDOWN, SIEOS_AD_POWEROFF, 0);
	}
	return -EINVAL;
}

/* ---------------- files ---------------- */

struct linux_dirent64 {
	unsigned long d_ino;
	long d_off;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[];
};

/* SIEOS getdents (no d_type) -> the struct dirent musl's readdir expects.
 * Converted records are larger; entries that do not fit are left for the
 * next call by seeking back to their cookie. */
static long emu_getdents64(int fd, char *buf, size_t len)
{
	char tmp[4096] __attribute__((aligned(8)));
	long n = __syscall(S(getdents), fd, tmp, len < sizeof tmp ? len : sizeof tmp);
	if (n <= 0)
		return n;
	size_t out = 0;
	long off = 0, cookie = 0;
	while (off < n) {
		struct sieos_dirent *e = (struct sieos_dirent *)(tmp + off);
		size_t nl = strlen(e->d_name);
		size_t hdr = __builtin_offsetof(struct linux_dirent64, d_name);
		size_t reclen = (hdr + nl + 1 + 7) & ~7UL;
		if (out + reclen > len)
			break;
		struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + out);
		memset(d, 0, reclen);
		d->d_ino = e->d_ino;
		d->d_off = e->d_off;
		d->d_reclen = reclen;
		d->d_type = DT_UNKNOWN;
		memcpy(d->d_name, e->d_name, nl);
		out += reclen;
		cookie = e->d_off;
		off += e->d_reclen;
	}
	if (off < n) {
		if (!out)
			return -EINVAL;
		__syscall(S(lseek), fd, cookie, SEEK_SET);
	}
	return out;
}

static long emu_truncate(const char *path, off_t len)
{
	long fd = __syscall(S(openat), SIEOS_AT_FDCWD, path, SIEOS_O_WRONLY | SIEOS_O_CLOEXEC, 0);
	if (fd < 0)
		return fd;
	long r = __syscall(S(ftruncate), fd, len);
	__syscall(S(close), fd);
	return r;
}

static long emu_fallocate(int fd, int mode, off_t off, off_t len)
{
	if (mode)
		return -EOPNOTSUPP;
	if (off < 0 || len <= 0)
		return -EINVAL;
	struct stat st;
	long r = __syscall(S(fstatat), fd, 0, &st, 0);
	if (r < 0)
		return r;
	if (!S_ISREG(st.st_mode))
		return S_ISDIR(st.st_mode) ? -EISDIR : -ENODEV;
	if (off + len > st.st_size)
		return __syscall(S(ftruncate), fd, off + len);
	return 0;
}

static long emu_sendfile(int out, int in, off_t *offp, size_t count)
{
	char buf[4096];
	size_t done = 0;
	off_t off = offp ? *offp : 0;
	while (done < count) {
		size_t chunk = count - done < sizeof buf ? count - done : sizeof buf;
		long n = offp ? __syscall(S(pread), in, buf, chunk, off) : __syscall(S(read), in, buf, chunk);
		if (n <= 0) {
			if (n < 0 && !done)
				return n;
			break;
		}
		long w = __syscall(S(write), out, buf, n);
		if (w <= 0) {
			if (w < 0 && !done)
				return w;
			break;
		}
		done += w;
		off += w;
		if (w < n) {
			if (!offp)
				__syscall(S(lseek), in, w - n, SEEK_CUR);
			break;
		}
	}
	if (offp)
		*offp = off;
	return done;
}

/* BSD flock() as a whole-file POSIX lock */
static long emu_flock(int fd, int op)
{
	struct flock l = { 0 };
	int cmd = (op & LOCK_NB) ? F_SETLK : F_SETLKW;
	switch (op & ~LOCK_NB) {
	case LOCK_SH: l.l_type = F_RDLCK; break;
	case LOCK_EX: l.l_type = F_WRLCK; break;
	case LOCK_UN: l.l_type = F_UNLCK; break;
	default: return -EINVAL;
	}
	l.l_whence = SEEK_SET;
	long r = __syscall(S(fcntl), fd, cmd, &l);
	if (r == -EACCES)
		r = -EWOULDBLOCK;
	if (r == -EBADF && l.l_type == F_WRLCK) {
		/* a read-only descriptor cannot hold a POSIX write lock: take a read lock */
		l.l_type = F_RDLCK;
		r = __syscall(S(fcntl), fd, cmd, &l);
	}
	return r;
}

static long emu_getrandom(void *buf, size_t len)
{
	long fd = open_ro("/dev/urandom");
	if (fd < 0)
		return fd;
	size_t done = 0;
	while (done < len) {
		long n = __syscall(S(read), fd, (char *)buf + done, len - done);
		if (n <= 0) {
			if (!done)
				done = n < 0 ? (size_t)n : 0;
			break;
		}
		done += n;
	}
	__syscall(S(close), fd);
	return done;
}

/* ---------------- memory ---------------- */

static long emu_madvise(void *addr, size_t len, int advice)
{
	long a;
	switch (advice) {
	case MADV_NORMAL:     a = SIEOS_MADV_NORMAL; break;
	case MADV_RANDOM:     a = SIEOS_MADV_RANDOM; break;
	case MADV_SEQUENTIAL: a = SIEOS_MADV_SEQUENTIAL; break;
	case MADV_WILLNEED:   a = SIEOS_MADV_WILLNEED; break;
	case MADV_DONTNEED:   a = SIEOS_MADV_DONTNEED; break;
	case MADV_FREE:       a = SIEOS_MADV_FREE; break;
	default:              return 0;          /* other advice is accepted and ignored */
	}
	return __syscall(S(memcntl), addr, len, SIEOS_MC_ADVISE, a, 0, 0);
}

/* ---------------- time ---------------- */

static long emu_clock_nanosleep(clockid_t clk, int flags, const struct timespec *req, struct timespec *rem)
{
	if (clk == CLOCK_THREAD_CPUTIME_ID || clk == CLOCK_PROCESS_CPUTIME_ID || clk == CLOCK_VIRTUAL)
		return -EINVAL;
	if (!(flags & TIMER_ABSTIME))
		return __syscall(S(nanosleep), req, rem);
	struct timespec now, rel;
	long r = __syscall(S(clock_gettime), clk, &now);
	if (r < 0)
		return r;
	rel.tv_sec = req->tv_sec - now.tv_sec;
	rel.tv_nsec = req->tv_nsec - now.tv_nsec;
	if (rel.tv_nsec < 0) {
		rel.tv_nsec += 1000000000;
		rel.tv_sec--;
	}
	if (rel.tv_sec < 0)
		return 0;
	return __syscall(S(nanosleep), &rel, 0);
}

/* ---------------- polling ---------------- */

static long emu_pselect6(int n, fd_set *r, fd_set *w, fd_set *e, const struct timespec *ts, const long *data)
{
	const sigset_t *mask = data ? (const sigset_t *)data[0] : 0;
	if (n < 0 || n > FD_SETSIZE)
		return -EINVAL;
	struct pollfd pfd[FD_SETSIZE];
	int np = 0;
	for (int fd = 0; fd < n; fd++) {
		short ev = 0;
		if (r && FD_ISSET(fd, r)) ev |= POLLIN;
		if (w && FD_ISSET(fd, w)) ev |= POLLOUT;
		if (e && FD_ISSET(fd, e)) ev |= POLLPRI;
		if (ev) {
			pfd[np].fd = fd;
			pfd[np].events = ev;
			pfd[np].revents = 0;
			np++;
		}
	}
	long ret = __syscall(S(pollsys), pfd, np, ts, mask);
	if (ret < 0)
		return ret;
	if (r) FD_ZERO(r);
	if (w) FD_ZERO(w);
	if (e) FD_ZERO(e);
	long count = 0;
	for (int i = 0; i < np; i++) {
		short rv = pfd[i].revents;
		if (rv & POLLNVAL)
			return -EBADF;
		if ((pfd[i].events & POLLIN) && (rv & (POLLIN | POLLHUP | POLLERR))) FD_SET(pfd[i].fd, r), count++;
		if ((pfd[i].events & POLLOUT) && (rv & (POLLOUT | POLLERR))) FD_SET(pfd[i].fd, w), count++;
		if ((pfd[i].events & POLLPRI) && (rv & POLLPRI)) FD_SET(pfd[i].fd, e), count++;
	}
	return count;
}

/* ---------------- threads ---------------- */

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_LOCK_PI 6
#define FUTEX_UNLOCK_PI 7
#define FUTEX_TRYLOCK_PI 8
#define FUTEX_PRIVATE 128

/*
 * Priority-inheritance futexes, on lwp_umtx_wait/wake.  The lock word holds
 * the owner's LWP id, FUTEX_WAITERS while someone sleeps on it, and
 * FUTEX_OWNER_DIED once the kernel's robust-list walk found its owner dead.
 * The scheduler has a single class, so there is no priority to lend: what
 * remains is the ownership protocol musl's PTHREAD_PRIO_INHERIT mutexes use.
 * An unlock frees the word and, if FUTEX_WAITERS was set, wakes every
 * waiter to contend again (a free word is 0, as musl's trylock expects).
 */
#define PI_WAITERS 0x80000000u
#define PI_OWNER_DIED 0x40000000u
#define PI_TID 0x3fffffffu

static long pi_lock(volatile int *addr, const struct timespec *at, long flags, bool try)
{
	unsigned tid = __syscall(S(lwp_self));
	for (;;) {
		unsigned v = *addr;
		if (!(v & PI_TID)) {                   /* free, perhaps after its owner died */
			if (a_cas(addr, v, tid | (v & (PI_WAITERS | PI_OWNER_DIED))) == (int)v)
				return 0;
			continue;
		}
		if ((v & PI_TID) == tid)
			return -EDEADLK;
		if (try)
			return -EAGAIN;
		if (!(v & PI_WAITERS)) {
			if (a_cas(addr, v, v | PI_WAITERS) != (int)v)
				continue;
			v |= PI_WAITERS;
		}
		struct timespec rel, *to = 0;
		if (at) {                              /* the timeout is absolute, CLOCK_REALTIME */
			struct timespec now;
			__syscall(S(clock_gettime), CLOCK_REALTIME, &now);
			rel.tv_sec = at->tv_sec - now.tv_sec;
			rel.tv_nsec = at->tv_nsec - now.tv_nsec;
			if (rel.tv_nsec < 0)
				rel.tv_sec--, rel.tv_nsec += 1000000000;
			if (rel.tv_sec < 0)
				return -ETIMEDOUT;
			to = &rel;
		}
		long r = __syscall(S(lwp_umtx_wait), addr, (int)v, to, flags);
		if (r == -ETIMEDOUT || r == -EINTR)
			return r;
	}
}

static long pi_unlock(volatile int *addr, long flags)
{
	unsigned tid = __syscall(S(lwp_self));
	for (;;) {
		unsigned v = *addr;
		if ((v & PI_TID) != tid)
			return -EPERM;
		if (a_cas(addr, v, 0) != (int)v)
			continue;
		if (v & PI_WAITERS)
			__syscall(S(lwp_umtx_wake), addr, INT_MAX, flags);
		return 0;
	}
}

static long emu_futex(volatile int *addr, int op, int val, const struct timespec *to, volatile int *addr2, int val3)
{
	long flags = (op & FUTEX_PRIVATE) ? SIEOS_UMTX_PRIVATE : 0;
	switch (op & 127) {
	case FUTEX_WAIT:
		return __syscall(S(lwp_umtx_wait), addr, val, to, flags);
	case FUTEX_WAKE:
		return __syscall(S(lwp_umtx_wake), addr, val, flags);
	case FUTEX_CMP_REQUEUE:
		if (*addr != val3)
			return -EAGAIN;
		/* fall through */
	case FUTEX_LOCK_PI:
		return pi_lock(addr, to, flags, false);
	case FUTEX_TRYLOCK_PI:
		return pi_lock(addr, 0, flags, true);
	case FUTEX_UNLOCK_PI:
		return pi_unlock(addr, flags);
	case FUTEX_REQUEUE: {
		/* no requeueing: wake the requeued waiters too (they re-check and wait again) */
		long a = __syscall(S(lwp_umtx_wake), addr, INT_MAX, flags);
		long b = __syscall(S(lwp_umtx_wake), addr2, INT_MAX, flags);
		return (a > 0 ? a : 0) + (b > 0 ? b : 0);
	}
	}
	return -ENOSYS;
}

/* ---------------- scheduling (priocntl, sieos/priocntl.h) ---------------- */

/* sched_* take a thread's LWP id (0: the caller), or a pid negated (process-level calls). */
static void sched_target(long id, long *idtype, long *pid)
{
	if (id < 0) {
		*idtype = SIEOS_P_PID;
		*pid = -id;
	} else {
		*idtype = SIEOS_P_LWPID;
		*pid = id ? id : SIEOS_P_MYID;
	}
}

static long emu_getparms(long id, sieos_pcparms_t *pp)
{
	long idt, t;
	sched_target(id, &idt, &t);
	pp->pc_cid = SIEOS_PC_CLNULL;
	return __syscall(S(priocntl), idt, t, SIEOS_PC_GETPARMS, pp);
}

static long emu_setscheduler(long id, int policy, const struct sched_param *param)
{
	sieos_pcparms_t pp;
	long idt, t;
	sched_target(id, &idt, &t);
	memset(&pp, 0, sizeof(pp));
	int prio = param ? param->sched_priority : 0;
	switch (policy & ~0x40000000) {                   /* SCHED_RESET_ON_FORK is ignored */
	case SCHED_OTHER:
	case SCHED_BATCH:
	case SCHED_IDLE: {
		if (prio)
			return -EINVAL;
		sieos_tsparms_t *ts = (void *)pp.pc_clparms;
		pp.pc_cid = SIEOS_CID_TS;
		ts->ts_uprilim = ts->ts_upri = SIEOS_TS_NOCHANGE;
		break;
	}
	case SCHED_FIFO:
	case SCHED_RR: {
		if (prio < 0 || prio > SIEOS_RT_MAXPRI)
			return -EINVAL;
		sieos_rtparms_t *rt = (void *)pp.pc_clparms;
		pp.pc_cid = SIEOS_CID_RT;
		rt->rt_pri = prio;
		rt->rt_tqnsecs = (policy & ~0x40000000) == SCHED_FIFO ? SIEOS_RT_TQINF : SIEOS_RT_TQDEF;
		break;
	}
	default:
		return -EINVAL;
	}
	return __syscall(S(priocntl), idt, t, SIEOS_PC_SETPARMS, &pp);
}

static long emu_getscheduler(long id)
{
	sieos_pcparms_t pp;
	long r = emu_getparms(id, &pp);
	if (r < 0)
		return r;
	if (pp.pc_cid != SIEOS_CID_RT)
		return SCHED_OTHER;
	return ((sieos_rtparms_t *)pp.pc_clparms)->rt_tqnsecs == SIEOS_RT_TQINF ? SCHED_FIFO : SCHED_RR;
}

static long emu_getparam(long id, struct sched_param *param)
{
	sieos_pcparms_t pp;
	long r = emu_getparms(id, &pp);
	if (r < 0)
		return r;
	memset(param, 0, sizeof(*param));
	if (pp.pc_cid == SIEOS_CID_RT)
		param->sched_priority = ((sieos_rtparms_t *)pp.pc_clparms)->rt_pri;
	return 0;
}

static long emu_nice(int which, long who, int op, int val)
{
	if (which != PRIO_PROCESS)
		return -EINVAL;                               /* process groups and users are not supported */
	sieos_pcnice_t n = { val, op };
	long r = __syscall(S(priocntl), SIEOS_P_PID, who ? who : SIEOS_P_MYID, SIEOS_PC_DONICE, &n);
	if (r < 0)
		return r;
	return op == SIEOS_PC_GETNICE ? 20 - n.pc_val : 0;   /* the kernel convention musl expects */
}

/* The calling LWP's processors (lwp_affinity: a 64-bit mask, a bit per processor) */
static long emu_sched_getaffinity(size_t size, unsigned char *mask)
{
	uint64_t m;
	if (size < 8)
		return -EINVAL;
	long r = __syscall(S(lwp_affinity), SIEOS_P_LWPID, SIEOS_P_MYID, SIEOS_AFF_GET, &m);
	if (r < 0)
		return r;
	memset(mask, 0, size);
	for (int i = 0; i < 64; i++)
		if (m >> i & 1)
			mask[i / 8] |= 1 << (i % 8);
	return 8;
}

static long emu_sched_setaffinity(size_t size, const unsigned char *mask)
{
	uint64_t m = 0;
	for (size_t i = 0; i < size && i < 8; i++)
		m |= (uint64_t)mask[i] << (8 * i);
	return __syscall(S(lwp_affinity), SIEOS_P_LWPID, SIEOS_P_MYID, SIEOS_AFF_SET, &m);
}

/* prlimit: this process's limits (getrlimit, setrlimit); another's are not reachable */
static long emu_prlimit(pid_t pid, int res, const struct rlimit *nl, struct rlimit *ol)
{
	if (pid < 0)
		return -ESRCH;
	if (pid && pid != sys_rv2(S(getpid), 0)) {
		long r = __syscall(S(kill), pid, 0);
		return r == -ESRCH ? -ESRCH : -EPERM;
	}
	long r = 0;
	if (ol && (r = __syscall(S(getrlimit), res, ol)) < 0)
		return r;
	if (nl)
		r = __syscall(S(setrlimit), res, nl);
	return r;
}

/* epoll_pwait: epoll_wait with sigs as the signal mask meanwhile */
static long emu_epoll_pwait(int fd, void *ev, int cnt, int to, const sigset_t *sigs)
{
	sigset_t old;
	if (sigs && __syscall(SYS_rt_sigprocmask, SIG_SETMASK, sigs, &old, _NSIG/8) < 0)
		return -EINVAL;
	long r = __syscall(S(epoll_wait), fd, ev, cnt, to);
	if (sigs)
		__syscall(SYS_rt_sigprocmask, SIG_SETMASK, &old, 0, _NSIG/8);
	return r;
}

/* ---------------- dispatch ---------------- */

hidden long __sieos_emu(long n, long a, long b, long c, long d, long e, long f)
{
	long second;
	switch (n) {
	case __EMU_futex:           return emu_futex((volatile int *)a, b, c, (const struct timespec *)d, (volatile int *)e, f);
	case __EMU_set_tid_address:
		__syscall(S(lwp_private), SIEOS_LWP_SETPRIVATE, SIEOS_LWP_EXITWORD, a);
		return __syscall(S(lwp_self));
	case __EMU_exit:            for (;;) __syscall(S(lwp_exit));
	case __EMU_exit_group:      for (;;) __syscall(S(exit), a);
	case __EMU_fork:            return __syscall(S(forkx), 0);
	case __EMU_getppid:         sys_rv2(S(getpid), &second); return second;
	case __EMU_gettid:          return __syscall(S(lwp_self));
	case __EMU_tkill:           return __syscall(S(lwp_kill), a, b);
	case __EMU_tgkill:
		if (a != sys_rv2(S(getpid), 0))
			return -ESRCH;
		return __syscall(S(lwp_kill), b, c);
	case __EMU_rt_sigpending:   return __syscall(S(sigpending), SIEOS_SIGPENDING, a);
	case __EMU_rt_sigtimedwait: return emu_sigtimedwait((const sigset_t *)a, (siginfo_t *)b, (const struct timespec *)c);
	case __EMU_rt_sigqueueinfo: return __syscall(S(sigqueue), a, b, ((siginfo_t *)c)->si_value.sival_ptr, 0);
	case __EMU_rt_sigaction:    return emu_rt_sigaction(a, (const struct k_sigaction *)b, (struct k_sigaction *)c);
	case __EMU_wait4:           return emu_wait4(a, (int *)b, c, (struct rusage *)d);
	case __EMU_waitid:          return emu_waitid(a, b, (siginfo_t *)c, d);
	case __EMU_getdents64:      return emu_getdents64(a, (char *)b, c);
	case __EMU_dup:             return __syscall(S(fcntl), a, SIEOS_F_DUPFD, 0);
	case __EMU_dup3:
		if (a == b || (c & ~O_CLOEXEC))
			return -EINVAL;
		return __syscall(S(fcntl), a, (c & O_CLOEXEC) ? SIEOS_F_DUP2FD_CLOEXEC : SIEOS_F_DUP2FD, b);
	case __EMU_fsync:           return __syscall(S(fdsync), a, SIEOS_FSYNC);
	case __EMU_fdatasync:       return __syscall(S(fdsync), a, SIEOS_FDSYNC);
	case __EMU_syncfs:          return __syscall(S(sync));
	case __EMU_truncate:        return emu_truncate((const char *)a, b);
	case __EMU_fallocate:       return emu_fallocate(a, b, c, d);
	case __EMU_fadvise64:       return 0;
	case __EMU_readahead:       return 0;
	case __EMU_sendfile:        return emu_sendfile(a, b, (off_t *)c, d);
	case __EMU_flock:           return emu_flock(a, b);
	case __EMU_madvise:         return emu_madvise((void *)a, b, c);
	case __EMU_msync:           return __syscall(S(memcntl), a, b, SIEOS_MC_SYNC, c, 0, 0);
	case __EMU_mlock:           return __syscall(S(memcntl), a, b, SIEOS_MC_LOCK, 0, 0, 0);
	case __EMU_munlock:         return __syscall(S(memcntl), a, b, SIEOS_MC_UNLOCK, 0, 0, 0);
	case __EMU_mlockall:        return __syscall(S(memcntl), 0, 0, SIEOS_MC_LOCKAS, a, 0, 0);
	case __EMU_munlockall:      return __syscall(S(memcntl), 0, 0, SIEOS_MC_UNLOCKAS, 0, 0, 0);
	case __EMU_uname:           return emu_uname((struct utsname *)a);
	case __EMU_sethostname: {
		char name[65];
		if (b < 0 || b > 64)
			return -EINVAL;
		memcpy(name, (const char *)a, b);
		name[b] = 0;
		long r = __syscall(S(sysinfo), SIEOS_SI_SET_HOSTNAME, name, 0);
		return r < 0 ? r : 0;
	}
	case __EMU_sysinfo:         return emu_sysinfo((struct sysinfo *)a);
	case __EMU_reboot:          return emu_reboot(a, b, c);
	case __EMU_setpgid:         return __syscall(S(pgrpsys), SIEOS_PGRP_SETPGID, a, b);
	case __EMU_getpgid:         return __syscall(S(pgrpsys), SIEOS_PGRP_GETPGID, a, 0);
	case __EMU_getsid:          return __syscall(S(pgrpsys), SIEOS_PGRP_GETSID, a, 0);
	case __EMU_setsid:          return __syscall(S(pgrpsys), SIEOS_PGRP_SETSID, 0, 0);
	case __EMU_setresuid:       return emu_setresid(a, b, c, false);
	case __EMU_setresgid:       return emu_setresid(a, b, c, true);
	case __EMU_getresuid:       return emu_getresid((unsigned *)a, (unsigned *)b, (unsigned *)c, false);
	case __EMU_getresgid:       return emu_getresid((unsigned *)a, (unsigned *)b, (unsigned *)c, true);
	case __EMU_getrandom:       return emu_getrandom((void *)a, b);
	case __EMU_ppoll:           return __syscall(S(pollsys), a, b, c, d);
	case __EMU_pselect6:        return emu_pselect6(a, (fd_set *)b, (fd_set *)c, (fd_set *)d, (const struct timespec *)e, (const long *)f);
	case __EMU_clock_nanosleep: return emu_clock_nanosleep(a, b, (const struct timespec *)c, (struct timespec *)d);
	case __EMU_gettimeofday: {
		struct timespec ts;
		long r = __syscall(S(clock_gettime), CLOCK_REALTIME, &ts);
		if (r == 0 && a) {
			((struct timeval *)a)->tv_sec = ts.tv_sec;
			((struct timeval *)a)->tv_usec = ts.tv_nsec / 1000;
		}
		return r;
	}
	case __EMU_settimeofday: {
		if (!a)
			return 0;
		struct timespec ts = { ((struct timeval *)a)->tv_sec, ((struct timeval *)a)->tv_usec * 1000 };
		return __syscall(S(clock_settime), CLOCK_REALTIME, &ts);
	}
	case __EMU_msgget:          return __syscall(S(msgsys), SIEOS_MSGGET, a, b);
	case __EMU_msgctl:          return __syscall(S(msgsys), SIEOS_MSGCTL, a, b, c);
	case __EMU_msgrcv:          return __syscall(S(msgsys), SIEOS_MSGRCV, a, b, c, d, e);
	case __EMU_msgsnd:          return __syscall(S(msgsys), SIEOS_MSGSND, a, b, c, d);
	case __EMU_semget:          return __syscall(S(semsys), SIEOS_SEMGET, a, b, c);
	case __EMU_semctl:          return __syscall(S(semsys), SIEOS_SEMCTL, a, b, c, d);
	case __EMU_semop:           return __syscall(S(semsys), SIEOS_SEMOP, a, b, c);
	case __EMU_semtimedop:      return __syscall(S(semsys), SIEOS_SEMTIMEDOP, a, b, c, d);
	case __EMU_shmget:          return __syscall(S(shmsys), SIEOS_SHMGET, a, b, c);
	case __EMU_shmctl:          return __syscall(S(shmsys), SIEOS_SHMCTL, a, b, c);
	case __EMU_shmat:           return __syscall(S(shmsys), SIEOS_SHMAT, a, b, c);
	case __EMU_shmdt:           return __syscall(S(shmsys), SIEOS_SHMDT, a);
	case __EMU_sched_getaffinity:
		if (a && a != sys_rv2(S(getpid), 0) && a != __syscall(S(lwp_self)))
			return -ESRCH;
		return emu_sched_getaffinity(b, (unsigned char *)c);
	case __EMU_sched_setaffinity:
		if (a && a != sys_rv2(S(getpid), 0) && a != __syscall(S(lwp_self)))
			return -ESRCH;
		return emu_sched_setaffinity(b, (const unsigned char *)c);
	case __EMU_sched_get_priority_max:
	case __EMU_sched_get_priority_min:
		if (a == SCHED_FIFO || a == SCHED_RR)
			return n == __EMU_sched_get_priority_max ? SIEOS_RT_MAXPRI : 0;
		return a == SCHED_OTHER || a == SCHED_BATCH || a == SCHED_IDLE ? 0 : -EINVAL;
	case __EMU_sched_getscheduler:
		return emu_getscheduler(a);
	case __EMU_sched_setscheduler:
		return emu_setscheduler(a, b, (const struct sched_param *)c);
	case __EMU_sched_setparam: {
		long pol = emu_getscheduler(a);
		return pol < 0 ? pol : emu_setscheduler(a, pol, (const struct sched_param *)b);
	}
	case __EMU_epoll_pwait:     return emu_epoll_pwait(a, (void *)b, c, d, (const sigset_t *)e);
	case __EMU_prlimit64:       return emu_prlimit(a, b, (const struct rlimit *)c, (struct rlimit *)d);
	case __EMU_sched_rr_get_interval: {
		sieos_pcparms_t pp;
		long r = emu_getparms(a, &pp);
		if (r < 0)
			return r;
		struct timespec *ts = (void *)b;
		ts->tv_sec = 0;
		ts->tv_nsec = 100000000;                      /* RT's default quantum, and a TS middle one */
		if (pp.pc_cid == SIEOS_CID_RT) {
			sieos_rtparms_t *rt = (void *)pp.pc_clparms;
			ts->tv_sec = rt->rt_tqnsecs == SIEOS_RT_TQINF ? 0 : rt->rt_tqsecs;
			ts->tv_nsec = rt->rt_tqnsecs == SIEOS_RT_TQINF ? 0 : rt->rt_tqnsecs;
		}
		return 0;
	}
	case __EMU_getpriority:
		return emu_nice(a, b, SIEOS_PC_GETNICE, 0);
	case __EMU_setpriority:
		return emu_nice(a, b, SIEOS_PC_SETNICE, c);
	case __EMU_fchown:          return __syscall(S(fchownat), a, 0, b, c, 0);
	case __EMU_fchmod:          return __syscall(S(fchmodat), a, 0, b, 0);
	case __EMU_set_robust_list:
		if (b != sizeof(struct sieos_robust_list_head))
			return -EINVAL;
		return __syscall(S(lwp_private), SIEOS_LWP_SETPRIVATE, SIEOS_LWP_ROBUSTLIST, a);
	case __EMU_get_robust_list:
		if (a && a != __syscall(S(lwp_self)))
			return -EPERM;
		*(long *)b = __syscall(S(lwp_private), SIEOS_LWP_GETPRIVATE, SIEOS_LWP_ROBUSTLIST, 0);
		*(size_t *)c = sizeof(struct sieos_robust_list_head);
		return 0;
	case __EMU_sched_getparam:
		return emu_getparam(a, (struct sched_param *)b);
	case __EMU_mount: {
		/* Linux order and flags -> Solaris mount(spec, dir, mflag, fstype, data, len);
		 * like Linux, a mount may cover a non-empty directory */
		unsigned long lf = d;
		long mf = SIEOS_MS_OVERLAY;
		if (lf & ~(unsigned long)(1 | 2 | 32 | 0xc0ed0000UL))       /* MS_RDONLY, MS_NOSUID, MS_REMOUNT, MS_MGC_VAL */
			return -EINVAL;
		if (lf & 1) mf |= SIEOS_MS_RDONLY;
		if (lf & 2) mf |= SIEOS_MS_NOSUID;
		if (lf & 32) mf |= SIEOS_MS_REMOUNT;
		return __syscall(S(mount), a, b, mf, c, 0, 0);
	}
	case __EMU_umount2:
		if (b & ~1L)                                   /* MNT_FORCE only */
			return -EINVAL;
		return __syscall(S(umount2), a, (b & 1) ? SIEOS_MS_FORCE : 0);
	}
	return -ENOSYS;
}
