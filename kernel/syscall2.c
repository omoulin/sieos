/*
 * syscall2.c - ABI v2: the syscall instruction, Solaris-inspired numbers,
 * carry flag + positive errno on failure (see abi/include/sieos/syscall.h
 * and docs/abi-v2.md).
 *
 * Processes, LWPs, signals, credentials, time and memory are dispatched
 * here; the file calls are in sysfile2.c.  Where v1 and v2 constants differ
 * (signals, errno), the kernel translates at this boundary.
 */
#include "proc.h"
#include "mm.h"
#include "fs.h"
#include "abi2.h"
#include "vm.h"
#include "sieos/syscall.h"
#include "sieos/errno.h"
#include "sieos/fcntl.h"
#include "sieos/stat.h"
#include "sieos/signal.h"
#include "sieos/time.h"
#include "sieos/sysinfo.h"
#include "sieos/wait.h"
#include "sieos/lwp.h"

long syscall_dispatch(struct trapframe *tf);

/* ---------------- translations ---------------- */

long sieos_errno(long e)
{
    switch (e) {
    case EPERM: return SIEOS_EPERM;
    case ENOENT: return SIEOS_ENOENT;
    case ESRCH: return SIEOS_ESRCH;
    case EINTR: return SIEOS_EINTR;
    case EIO: return SIEOS_EIO;
    case ENXIO: return SIEOS_ENXIO;
    case E2BIG: return SIEOS_E2BIG;
    case ENOEXEC: return SIEOS_ENOEXEC;
    case EBADF: return SIEOS_EBADF;
    case ECHILD: return SIEOS_ECHILD;
    case EAGAIN: return SIEOS_EAGAIN;
    case ENOMEM: return SIEOS_ENOMEM;
    case EACCES: return SIEOS_EACCES;
    case EFAULT: return SIEOS_EFAULT;
    case EBUSY: return SIEOS_EBUSY;
    case EEXIST: return SIEOS_EEXIST;
    case EXDEV: return SIEOS_EXDEV;
    case ENODEV: return SIEOS_ENODEV;
    case ENOTDIR: return SIEOS_ENOTDIR;
    case EISDIR: return SIEOS_EISDIR;
    case EINVAL: return SIEOS_EINVAL;
    case ENFILE: return SIEOS_ENFILE;
    case EMFILE: return SIEOS_EMFILE;
    case ENOTTY: return SIEOS_ENOTTY;
    case EFBIG: return SIEOS_EFBIG;
    case ENOSPC: return SIEOS_ENOSPC;
    case ESPIPE: return SIEOS_ESPIPE;
    case EROFS: return SIEOS_EROFS;
    case EPIPE: return SIEOS_EPIPE;
    case ERANGE: return SIEOS_ERANGE;
    case ENAMETOOLONG: return SIEOS_ENAMETOOLONG;
    case ENOSYS: return SIEOS_ENOSYS;
    case ENOTEMPTY: return SIEOS_ENOTEMPTY;
    case ELOOP: return SIEOS_ELOOP;
    case ENOTSOCK: return SIEOS_ENOTSOCK;
    case EDESTADDRREQ: return SIEOS_EDESTADDRREQ;
    case EMSGSIZE: return SIEOS_EMSGSIZE;
    case EPROTONOSUPPORT: return SIEOS_EPROTONOSUPPORT;
    case EOPNOTSUPP: return SIEOS_EOPNOTSUPP;
    case EAFNOSUPPORT: return SIEOS_EAFNOSUPPORT;
    case EADDRINUSE: return SIEOS_EADDRINUSE;
    case EADDRNOTAVAIL: return SIEOS_EADDRNOTAVAIL;
    case ENETDOWN: return SIEOS_ENETDOWN;
    case ENETUNREACH: return SIEOS_ENETUNREACH;
    case ECONNABORTED: return SIEOS_ECONNABORTED;
    case ECONNRESET: return SIEOS_ECONNRESET;
    case ENOBUFS: return SIEOS_ENOBUFS;
    case EISCONN: return SIEOS_EISCONN;
    case ENOTCONN: return SIEOS_ENOTCONN;
    case ETIMEDOUT: return SIEOS_ETIMEDOUT;
    case ECONNREFUSED: return SIEOS_ECONNREFUSED;
    case EHOSTUNREACH: return SIEOS_EHOSTUNREACH;
    case EALREADY: return SIEOS_EALREADY;
    case EINPROGRESS: return SIEOS_EINPROGRESS;
    case EDEADLK: return SIEOS_EDEADLK;
    case ENOLCK: return SIEOS_ENOLCK;
    case ETIME: return SIEOS_ETIME;
    case EOVERFLOW: return SIEOS_EOVERFLOW;
    case EMLINK: return SIEOS_EMLINK;
    case ETXTBSY: return SIEOS_ETXTBSY;
    case EDOM: return SIEOS_EDOM;
    case ECANCELED: return SIEOS_ECANCELED;
    case ENOTSUP_K: return SIEOS_ENOTSUP;
    case ENOPROTOOPT_K: return SIEOS_ENOPROTOOPT;
    case ENOMSG_K: return SIEOS_ENOMSG;
    case EIDRM_K: return SIEOS_EIDRM;
    case ELIBBAD_K: return SIEOS_ELIBBAD;
    case EPROTOTYPE_K: return SIEOS_EPROTOTYPE;
    default: return SIEOS_EIO;
    }
}


/* ---------------- helpers ---------------- */

/* Call the v1 implementation with a copy of the frame. */
static long v1(struct trapframe *tf, long nr, uint64_t a1, uint64_t a2, uint64_t a3)
{
    struct trapframe t = *tf;
    t.rax = nr;
    t.rdi = a1;
    t.rsi = a2;
    t.rdx = a3;
    t.r10 = t.r8 = t.r9 = 0;
    return syscall_dispatch(&t);
}

static uint64_t ns_per_tick(void)
{
    return 1000000000UL / TIMER_HZ;
}

static long do_nanosleep(const struct sieos_timespec *ureq, struct sieos_timespec *urem)
{
    if (!user_ok(ureq, sizeof(*ureq), false) || (urem && !user_ok(urem, sizeof(*urem), true)))
        return -EFAULT;
    struct sieos_timespec req = *ureq;
    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000L)
        return -EINVAL;
    uint64_t ns = (uint64_t)req.tv_sec * 1000000000UL + req.tv_nsec;
    uint64_t when = hrtime() + ns;
    long r;
    if (!ns)
        r = 0;
    else if (hr_timers())
        r = proc_sleep_until_ns(when);               /* the local timer fires at the deadline */
    else                                             /* the first tick at or after it */
        r = proc_sleep_until((when + ns_per_tick() - 1) / ns_per_tick());
    if (r == -EINTR && urem) {
        uint64_t now = hrtime();
        uint64_t left = when > now ? when - now : 0;
        struct sieos_timespec rem = { (long)(left / 1000000000UL), (long)(left % 1000000000UL) };
        memcpy(urem, &rem, sizeof(rem));
    }
    return r;
}

static long do_sysinfo(long cmd, char *ubuf, long count)
{
    char tmp[160];
    const char *v;
    if (cmd == SIEOS_SI_SET_HOSTNAME) {
        if (current->euid != 0)
            return -EPERM;
        char name[65];
        int r = user_fetch_str(ubuf, name, sizeof(name));
        if (r < 0)
            return r == -ENAMETOOLONG ? -EINVAL : r;
        strlcpy(sys_hostname, name, sizeof(sys_hostname));
        return strlen(sys_hostname) + 1;
    }
    switch (cmd) {
    case SIEOS_SI_SYSNAME:          v = OS_NAME; break;
    case SIEOS_SI_HOSTNAME:         v = sys_hostname; break;
    case SIEOS_SI_RELEASE:          v = OS_RELEASE; break;
    case SIEOS_SI_VERSION:          v = OS_LONGNAME; break;
    case SIEOS_SI_MACHINE:          v = "i86pc"; break;
    case SIEOS_SI_ARCHITECTURE:     v = "i386"; break;
    case SIEOS_SI_ARCHITECTURE_64:
    case SIEOS_SI_ARCHITECTURE_K:
    case SIEOS_SI_ARCHITECTURE_NATIVE: v = "amd64"; break;
    case SIEOS_SI_PLATFORM:         v = "i86pc"; break;
    case SIEOS_SI_ISALIST:          v = "amd64 pentium_pro+mmx pentium_pro pentium+mmx pentium i486 i386 i86"; break;
    case SIEOS_SI_HW_PROVIDER:      v = OS_NAME; break;
    case SIEOS_SI_HW_SERIAL:        v = "0"; break;
    case SIEOS_SI_SRPC_DOMAIN:      v = ""; break;
    default:                        return -EINVAL;
    }
    snprintf(tmp, sizeof(tmp), "%s", v);
    long len = strlen(tmp) + 1;
    if (count < 0 || (count && !user_ok(ubuf, count, true)))
        return -EFAULT;
    if (count) {
        long n = MIN(len, count);
        memcpy(ubuf, tmp, n);
        ubuf[n - 1] = 0;
    }
    return len;                                     /* the size needed, as on Solaris */
}

static long do_waitid(int idtype, long id, sieos_siginfo_t *uinfo, int options)
{
    if (uinfo && !user_ok(uinfo, sizeof(*uinfo), true))
        return -EFAULT;
    struct ksiginfo info;
    int word = 0;
    long r = proc_waitid(idtype, id, options, &info, &word);
    if (r < 0)
        return r;
    if (uinfo) {
        sieos_siginfo_t si;
        memset(&si, 0, sizeof(si));
        if (r > 0) {
            si.si_signo = SIGCHLD;
            si.si_code = info.code;
            si.__data.proc.pid = info.pid;
            si.__data.proc.uid = info.uid;
            si.__data.proc.status = info.status;
        }
        memcpy(uinfo, &si, sizeof(si));
    }
    return 0;
}

static long do_pgrpsys(int op, int pid, int pgid)
{
    struct proc *p;
    switch (op) {
    case SIEOS_PGRP_GETPGRP:
        return current->pgid;
    case SIEOS_PGRP_SETPGRP:
        proc_setsid();                          /* Solaris setpgrp: leader of a new session if possible */
        return current->pgid;
    case SIEOS_PGRP_GETSID:
    case SIEOS_PGRP_GETPGID:
        p = pid ? proc_find(pid) : current;
        if (!p || p->state != PSTATE_RUNNING)
            return -ESRCH;
        return op == SIEOS_PGRP_GETSID ? p->sid : p->pgid;
    case SIEOS_PGRP_SETSID:
        return proc_setsid();
    case SIEOS_PGRP_SETPGID:
        return proc_setpgid(pid, pgid);
    }
    return -EINVAL;
}

/* ---------------- dispatch ---------------- */

long syscall_dispatch_v2(struct trapframe *tf)
{
    uint64_t a1 = tf->rdi, a2 = tf->rsi, a3 = tf->rdx, a4 = tf->r10;
    struct proc *p = current;
    bool handled;
    long fr = syscall_file_v2(tf, &handled);
    if (handled)
        return fr;
    fr = syscall_misc_v2(tf, &handled);
    if (handled)
        return fr;
    fr = syscall_sock_v2(tf, &handled);
    if (handled)
        return fr;
    switch (tf->rax) {
    /* processes */
    case SIEOS_SYS_exit:      return v1(tf, SYS_exit, a1, 0, 0);
    case SIEOS_SYS_forkx:
    case SIEOS_SYS_vforkx:
        if (a1 & ~(uint64_t)(SIEOS_FORK_NOSIGCHLD | SIEOS_FORK_WAITPID))
            return -EINVAL;
        return proc_fork((int)a1);
    case SIEOS_SYS_waitid:    return do_waitid((int)a1, (long)a2, (sieos_siginfo_t *)a3, (int)a4);
    case SIEOS_SYS_pgrpsys:   return do_pgrpsys((int)a1, (int)a2, (int)a3);
    case SIEOS_SYS_execve:    return v1(tf, SYS_exec, a1, a2, a3);
    case SIEOS_SYS_getpid:
        tf->rdx = p->parent ? p->parent->pid : 0;     /* rv2: parent pid */
        return p->pid;
    case SIEOS_SYS_yield:     return 0;
    case SIEOS_SYS_umask:     return v1(tf, SYS_umask, a1, 0, 0);
    case SIEOS_SYS_brk: {
        if (!a1)
            return p->brk;
        if (a1 < p->heap_start)
            return -EINVAL;
        long incr = (long)(a1 - p->brk);
        long old = v1(tf, SYS_sbrk, incr, 0, 0);
        return old < 0 && old >= -4095 ? old : (long)p->brk;
    }
    case SIEOS_SYS_sysinfo:   return do_sysinfo(a1, (char *)a2, (long)a3);
    /* signals */
    case SIEOS_SYS_kill:        return kill_pids((int)a1, (int)a2, NULL);
    case SIEOS_SYS_sigaction:   return sys2_sigaction((int)a1, (const struct sieos_sigaction *)a2, (struct sieos_sigaction *)a3);
    case SIEOS_SYS_sigpending:  return sys2_sigpending((int)a1, (sieos_sigset_t *)a2);
    case SIEOS_SYS_sigsuspend:  return sys2_sigsuspend((const sieos_sigset_t *)a1);
    case SIEOS_SYS_sigaltstack: return sys2_sigaltstack((const sieos_stack_t *)a1, (sieos_stack_t *)a2);
    case SIEOS_SYS_sigqueue:    return sys2_sigqueue((int)a1, (int)a2, a3);
    case SIEOS_SYS_sigtimedwait:
        return sys2_sigtimedwait((const sieos_sigset_t *)a1, (sieos_siginfo_t *)a2, (const struct sieos_timespec *)a3);
    case SIEOS_SYS_context:     return sys2_context((int)a1, (sieos_ucontext_t *)a2, tf);
    /* lightweight processes */
    case SIEOS_SYS_lwp_create:  return sys2_lwp_create((const sieos_ucontext_t *)a1, (int)a2, (sieos_lwpid_t *)a3);
    case SIEOS_SYS_lwp_exit:    lwp_exit_self();
    case SIEOS_SYS_lwp_self:    return curlwp->lwpid;
    case SIEOS_SYS_lwp_kill:    return sys2_lwp_kill((int)a1, (int)a2);
    case SIEOS_SYS_lwp_wait:    return sys2_lwp_wait((int)a1, (sieos_lwpid_t *)a2);
    case SIEOS_SYS_lwp_suspend: return sys2_lwp_suspend((int)a1);
    case SIEOS_SYS_lwp_continue: return sys2_lwp_continue((int)a1);
    case SIEOS_SYS_lwp_park:    return sys2_lwp_park((const struct sieos_timespec *)a1, (int)a2);
    case SIEOS_SYS_lwp_unpark:  return sys2_lwp_unpark((int)a1);
    case SIEOS_SYS_lwp_unpark_all: return sys2_lwp_unpark_all((const sieos_lwpid_t *)a1, (int)a2);
    case SIEOS_SYS_lwp_private: return sys2_lwp_private((int)a1, (int)a2, a3);
    case SIEOS_SYS_lwp_umtx_wait:
        return sys2_lwp_umtx_wait(a1, (int)a2, (const struct sieos_timespec *)a3, (int)a4);
    case SIEOS_SYS_lwp_umtx_wake: return sys2_lwp_umtx_wake(a1, (int)a2, (int)a3);
    case SIEOS_SYS_lwp_sigmask: return sys2_sigmask((int)a1, (const sieos_sigset_t *)a2, (sieos_sigset_t *)a3);
    case SIEOS_SYS_lwp_name:    return sys2_lwp_name((int)a1, (int)a2, (char *)a3, a4);
    /* credentials */
    case SIEOS_SYS_getuid:
        tf->rdx = p->euid;                            /* rv2: effective uid */
        return p->uid;
    case SIEOS_SYS_geteuid:   return p->euid;
    case SIEOS_SYS_getgid:
        tf->rdx = p->egid;                            /* rv2: effective gid */
        return p->gid;
    case SIEOS_SYS_getegid:   return p->egid;
    case SIEOS_SYS_setuid:    return v1(tf, SYS_setuid, a1, 0, 0);
    case SIEOS_SYS_setgid:    return v1(tf, SYS_setgid, a1, 0, 0);
    case SIEOS_SYS_seteuid:   return v1(tf, SYS_seteuid, a1, 0, 0);
    case SIEOS_SYS_setegid:   return v1(tf, SYS_setegid, a1, 0, 0);
    case SIEOS_SYS_getgroups: return v1(tf, SYS_getgroups, a1, a2, 0);
    case SIEOS_SYS_setgroups: return v1(tf, SYS_setgroups, a1, a2, 0);
    /* time */
    case SIEOS_SYS_nanosleep: return do_nanosleep((const struct sieos_timespec *)a1, (struct sieos_timespec *)a2);
    /* memory */
    case SIEOS_SYS_mmap:      return vm_mmap(a1, a2, (int)a3, (int)a4, (int)tf->r8, tf->r9);
    case SIEOS_SYS_munmap:    return vm_munmap(a1, a2);
    case SIEOS_SYS_mprotect:  return vm_mprotect(a1, a2, (int)a3);
    case SIEOS_SYS_mincore:   return vm_mincore(a1, a2, (char *)a3);
    case SIEOS_SYS_memcntl:   return vm_memcntl(a1, a2, (int)a3, a4);
    /* SIEOS extensions */
    case SIEOS_SYS_fbmap:     return v1(tf, SYS_fbmap, a1, 0, 0);
    case SIEOS_SYS_cpuinfo:   return v1(tf, SYS_cpuinfo, a1, a2, 0);
    case SIEOS_SYS_meminfo:   return v1(tf, SYS_meminfo, a1, 0, 0);
    case SIEOS_SYS_procinfo:  return v1(tf, SYS_procinfo, a1, a2, 0);
    }
    return -ENOSYS;
}
