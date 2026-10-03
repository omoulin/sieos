#!/usr/bin/env python3
"""
sieos-port.py SRC_TARBALL DEST - prepare the musl source tree for SIEOS.

1. extract musl (pristine release tarball) into DEST
2. copy the port overlay (libc/port) over it: arch/sieos64, src/*/sieos64
   (musl's per-arch replacement mechanism) and src/sieos (new sources);
   then libc/backports: fixes from later musl releases (see its README)
3. copy the kernel ABI headers (abi/include/sieos) to arch/sieos64/sieos for
   the port's own sources
4. expand @DEFINES regex@ markers in overlay headers with the matching
   SIEOS_ constants of the ABI headers, prefixes stripped
5. generate arch/sieos64/bits/syscall.h.in (Linux names -> SIEOS numbers,
   libc emulation numbers, or ENOSYS)
6. apply the text patches below; each must match exactly once
7. generate the Solaris extension headers from the ABI headers
"""
import os, re, shutil, sys, tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
ABI = os.path.join(HERE, '..', 'abi', 'include', 'sieos')

# ---------------------------------------------------------------- ABI values

FILE_OF = {}

def abi_defines():
    d = {}
    order = []
    for fn in sorted(os.listdir(ABI)):
        text = open(os.path.join(ABI, fn)).read()
        text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
        for m in re.finditer(r'^#define\s+SIEOS_(\w+)[ \t]+(.+?)\s*$', text, re.M):
            name, val = m.group(1), m.group(2).strip()
            val = re.sub(r'\bSIEOS_', '', val)
            val = re.sub(r'\bsieos_', '', val)
            if name not in d:
                order.append(name)
                FILE_OF[name] = fn
            d[name] = val
    return d, order

DEFS, ORDER = abi_defines()

def expand_markers(path):
    text = open(path).read()
    def rep(m):
        spec = m.group(1).strip()
        only = None
        if re.match(r'^[\w.]+\.h:', spec):
            only, spec = spec.split(':', 1)
        rx = re.compile(r'^(?:%s)$' % spec)
        out = ['#define %s %s' % (n, DEFS[n]) for n in ORDER
               if rx.match(n) and (only is None or FILE_OF[n] == only)]
        if not out:
            sys.exit('%s: no ABI constant matches %s' % (path, m.group(1)))
        return '\n'.join(out)
    new = re.sub(r'^@DEFINES\s+(.+?)@\s*$', rep, text, flags=re.M)
    if new != text:
        open(path, 'w').write(new)

# ---------------------------------------------------------------- syscalls

EMU_BASE, NOSYS_BASE = 1000, 2000

def sieos_sys():
    return {n[4:]: int(v) for n, v in DEFS.items() if n.startswith('SYS_') and v.isdigit()}

SIEOS_SYS = sieos_sys()

# Linux name -> SIEOS name (same arguments and semantics)
NATIVE = {
    'read': 'read', 'write': 'write', 'close': 'close', 'lseek': 'lseek', 'readv': 'readv',
    'writev': 'writev', 'pread64': 'pread', 'pwrite64': 'pwrite', 'ioctl': 'ioctl', 'openat': 'openat',
    'fcntl': 'fcntl', 'pipe2': 'pipe2', 'newfstatat': 'fstatat', 'faccessat': 'faccessat',
    'fchmodat': 'fchmodat', 'fchownat': 'fchownat', 'mkdirat': 'mkdirat', 'mknodat': 'mknodat',
    'unlinkat': 'unlinkat', 'renameat': 'renameat', 'linkat': 'linkat', 'symlinkat': 'symlinkat',
    'readlinkat': 'readlinkat', 'utimensat': 'utimensat', 'fchmodat2': 'fchmodat', 'faccessat2': 'faccessat', 'ftruncate': 'ftruncate', 'chdir': 'chdir',
    'fchdir': 'fchdir', 'getcwd': 'getcwd', 'chroot': 'chroot', 'sync': 'sync', 'mmap': 'mmap',
    'munmap': 'munmap', 'mprotect': 'mprotect', 'mincore': 'mincore', 'brk': 'brk', 'execve': 'execve',
    'getpid': 'getpid', 'getuid': 'getuid', 'geteuid': 'geteuid', 'getgid': 'getgid', 'getegid': 'getegid',
    'setuid': 'setuid', 'setgid': 'setgid', 'setreuid': 'setreuid', 'setregid': 'setregid',
    'getgroups': 'getgroups', 'setgroups': 'setgroups', 'umask': 'umask', 'kill': 'kill',
    'sigaltstack': 'sigaltstack', 'rt_sigsuspend': 'sigsuspend', 'rt_sigprocmask': 'lwp_sigmask',
    'nanosleep': 'nanosleep', 'clock_gettime': 'clock_gettime', 'clock_settime': 'clock_settime',
    'clock_getres': 'clock_getres', 'getitimer': 'getitimer', 'setitimer': 'setitimer', 'times': 'times',
    'getrlimit': 'getrlimit', 'setrlimit': 'setrlimit', 'getrusage': 'getrusage', 'sched_yield': 'yield',
    'socket': 'so_socket', 'socketpair': 'so_socketpair', 'bind': 'bind', 'listen': 'listen',
    'accept': 'accept', 'accept4': 'accept', 'connect': 'connect', 'getsockname': 'getsockname',
    'getpeername': 'getpeername', 'sendto': 'sendto', 'recvfrom': 'recvfrom', 'setsockopt': 'setsockopt',
    'getsockopt': 'getsockopt', 'shutdown': 'shutdown', 'sendmsg': 'sendmsg', 'recvmsg': 'recvmsg',
    # Linux's descriptors and transfers (sieos/fdext.h)
    'epoll_create1': 'epoll_create1', 'epoll_ctl': 'epoll_ctl', 'eventfd2': 'eventfd2',
    'timerfd_create': 'timerfd_create', 'timerfd_settime': 'timerfd_settime',
    'timerfd_gettime': 'timerfd_gettime', 'memfd_create': 'memfd_create', 'pidfd_open': 'pidfd_open',
    'pidfd_send_signal': 'pidfd_send_signal', 'splice': 'splice', 'copy_file_range': 'copy_file_range',
    'preadv': 'preadv', 'pwritev': 'pwritev', 'mremap': 'mremap',
}

# emulated in libc (src/sieos/emu.c); the order gives the numbers
EMU = [
    'futex', 'set_tid_address', 'exit', 'exit_group', 'fork', 'getppid', 'gettid', 'tkill', 'tgkill',
    'rt_sigpending', 'rt_sigtimedwait', 'rt_sigqueueinfo', 'wait4', 'waitid', 'getdents64', 'dup', 'dup3',
    'fsync', 'fdatasync', 'syncfs', 'truncate', 'fallocate', 'fadvise64', 'readahead', 'sendfile', 'flock',
    'madvise', 'msync', 'mlock', 'munlock', 'mlockall', 'munlockall', 'uname', 'sethostname', 'sysinfo',
    'reboot', 'setpgid', 'getpgid', 'getsid', 'setsid', 'setresuid', 'setresgid', 'getresuid', 'getresgid',
    'getrandom', 'ppoll', 'pselect6', 'clock_nanosleep', 'gettimeofday', 'settimeofday', 'msgget',
    'msgctl', 'msgrcv', 'msgsnd', 'semget', 'semctl', 'semop', 'semtimedop', 'shmget', 'shmctl', 'shmat',
    'shmdt', 'sched_getaffinity', 'sched_setaffinity', 'sched_get_priority_max', 'sched_get_priority_min',
    'sched_getscheduler', 'sched_getparam', 'rt_sigaction', 'fchown', 'fchmod', 'set_robust_list', 'get_robust_list',
    'mount', 'umount2', 'getpriority', 'setpriority', 'sched_setscheduler', 'sched_setparam',
    'sched_rr_get_interval', 'prlimit64', 'epoll_pwait',
]

def aarch64_names(tree):
    text = open(os.path.join(tree, 'arch', 'aarch64', 'bits', 'syscall.h.in')).read()
    return re.findall(r'__NR_(\w+)', text)

def gen_syscalls(tree):
    names = aarch64_names(tree)
    extra = ['fork']
    lines = ['/* generated by libc/sieos-port.py: Linux names -> SIEOS ABI v2 */']
    emu = {n: EMU_BASE + i for i, n in enumerate(EMU)}
    nosys = NOSYS_BASE
    for n in names + extra:
        if n in NATIVE:
            v = SIEOS_SYS[NATIVE[n]]
        elif n in emu:
            v = emu[n]
        else:
            v = nosys
            nosys += 1
        lines.append('#define __NR_%-24s %d' % (n, v))
    missing = [n for n in list(NATIVE) + EMU if n not in names + extra]
    if missing:
        sys.exit('syscall names not in the table: %s' % missing)
    path = os.path.join(tree, 'arch', 'sieos64', 'bits', 'syscall.h.in')
    open(path, 'w').write('\n'.join(lines) + '\n')
    # numbers for the emulation layer
    hdr = ['/* generated: libc emulation numbers (src/sieos/emu.c) */',
           '#define __SIEOS_EMU_BASE %d' % EMU_BASE, '#define __SIEOS_NOSYS_BASE %d' % NOSYS_BASE]
    hdr += ['#define __EMU_%s %d' % (n, EMU_BASE + i) for i, n in enumerate(EMU)]
    open(os.path.join(tree, 'arch', 'sieos64', 'sieos_emu.h'), 'w').write('\n'.join(hdr) + '\n')

# ---------------------------------------------------------------- patches

def D(name):
    return DEFS[name]

def patches():
    P = []
    add = lambda f, old, new: P.append((f, old, new))
    # configure: the target triplet x86_64-*-sieos* selects arch sieos64
    add('configure', 'x86_64-x32*|x32*|x86_64*x32) ARCH=x32 ;;',
        'x86_64*sieos*) ARCH=sieos64 ;;\nx86_64-x32*|x32*|x86_64*x32) ARCH=x32 ;;')
    # empty libraries: -lsocket -lnsl link as on Solaris
    add('Makefile', 'EMPTY_LIB_NAMES = m rt pthread crypt util xnet resolv dl',
        'EMPTY_LIB_NAMES = m rt pthread crypt util xnet resolv dl socket nsl')
    # sigset_t is 128 bits, like the kernel's (and Solaris's)
    add('include/alltypes.h.in', 'TYPEDEF struct __sigset_t { unsigned long __bits[128/sizeof(long)]; } sigset_t;',
        'TYPEDEF struct __sigset_t { unsigned long __bits[2]; } sigset_t;')
    add('include/alltypes.h.in', 'TYPEDEF unsigned _Reg nlink_t;', 'TYPEDEF unsigned nlink_t;')
    add('include/alltypes.h.in', 'TYPEDEF long blksize_t;', 'TYPEDEF int blksize_t;')
    # fcntl.h: Solaris struct flock, lock types, *at constants
    add('include/fcntl.h', '''struct flock {
	short l_type;
	short l_whence;
	off_t l_start;
	off_t l_len;
	pid_t l_pid;
};''', '''struct flock {
	short l_type;
	short l_whence;
	int __l_pad;
	off_t l_start;
	off_t l_len;
	int l_sysid;
	pid_t l_pid;
	long __l_pad2[4];
};''')
    # EOPNOTSUPP is not ENOTSUP on SIEOS (Solaris's numbers): its own message
    add('src/errno/__strerror.h', 'E(ENOTSUP,      "Not supported")',
        'E(ENOTSUP,      "Not supported")\nE(EOPNOTSUPP,   "Operation not supported")')
    # no pipe size control: a pipe holds 64 KiB
    add('include/fcntl.h', '#define F_SETPIPE_SZ\t1031\n#define F_GETPIPE_SZ\t1032\n', '')
    add('include/fcntl.h', '#define F_DUPFD_CLOEXEC 1030', '#define F_DUPFD_CLOEXEC %s\n#define F_DUP2FD %s\n#define F_DUP2FD_CLOEXEC %s\n#define F_FREESP %s'
        % (D('F_DUPFD_CLOEXEC'), D('F_DUP2FD'), D('F_DUP2FD_CLOEXEC'), D('F_FREESP')))
    add('include/fcntl.h', '#define F_RDLCK 0\n#define F_WRLCK 1\n#define F_UNLCK 2',
        '#define F_RDLCK %s\n#define F_WRLCK %s\n#define F_UNLCK %s' % (D('F_RDLCK'), D('F_WRLCK'), D('F_UNLCK')))
    add('include/fcntl.h', '''#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define AT_SYMLINK_FOLLOW 0x400
#define AT_EACCESS 0x200''', '''#define AT_FDCWD (-3041965)
#define AT_SYMLINK_NOFOLLOW %s
#define AT_REMOVEDIR %s
#define AT_SYMLINK_FOLLOW %s
#define AT_EACCESS %s''' % (D('AT_SYMLINK_NOFOLLOW'), D('AT_REMOVEDIR'), D('AT_SYMLINK_FOLLOW'), D('AT_EACCESS')))
    # AT_EMPTY_PATH is handled by libc (fstatat with an empty path = fstat)
    add('include/fcntl.h', '#define AT_EMPTY_PATH 0x1000', '#define AT_EMPTY_PATH 0x40000000')
    # SIEOS has no O_ASYNC (nor has Solaris): FASYNC only where it exists
    add('include/fcntl.h', '#define FASYNC O_ASYNC', '#ifdef O_ASYNC\n#define FASYNC O_ASYNC\n#endif')
    # sys/wait.h: Solaris idtypes and options
    add('include/sys/wait.h', '''	P_ALL = 0,
	P_PID = 1,
	P_PGID = 2,
	P_PIDFD = 3''', '''	P_PID = 0,
	P_PPID = 1,
	P_PGID = 2,
	P_SID = 3,
	P_CID = 4,
	P_UID = 5,
	P_GID = 6,
	P_ALL = 7,
	P_LWPID = 8''')
    add('include/sys/wait.h', '''#define WNOHANG    1
#define WUNTRACED  2''', '''#define WNOHANG    0x40
#define WUNTRACED  0x04''')
    add('include/sys/wait.h', '''#define WSTOPPED   2
#define WEXITED    4
#define WCONTINUED 8
#define WNOWAIT    0x1000000''', '''#define WSTOPPED   0x04
#define WEXITED    0x01
#define WCONTINUED 0x08
#define WNOWAIT    0x80
#define WTRAPPED   0x02''')
    # signal.h: sigprocmask "how"
    add('include/signal.h', '''#define SIG_BLOCK     0
#define SIG_UNBLOCK   1
#define SIG_SETMASK   2''', '''#define SIG_BLOCK     1
#define SIG_UNBLOCK   2
#define SIG_SETMASK   3''')
    # time.h: Solaris clock ids
    add('include/time.h', '''#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7
#define CLOCK_REALTIME_ALARM     8
#define CLOCK_BOOTTIME_ALARM     9
#define CLOCK_SGI_CYCLE         10
#define CLOCK_TAI               11''', '''#define CLOCK_VIRTUAL            1
#define CLOCK_THREAD_CPUTIME_ID  2
#define CLOCK_REALTIME           3
#define CLOCK_MONOTONIC          4
#define CLOCK_PROCESS_CPUTIME_ID 5
#define CLOCK_HIGHRES            CLOCK_MONOTONIC
#define CLOCK_PROF               CLOCK_THREAD_CPUTIME_ID
#define CLOCK_MONOTONIC_RAW      CLOCK_MONOTONIC
#define CLOCK_REALTIME_COARSE    CLOCK_REALTIME
#define CLOCK_MONOTONIC_COARSE   CLOCK_MONOTONIC
#define CLOCK_BOOTTIME           CLOCK_MONOTONIC''')
    # sys/resource.h: Solaris resource numbers and RLIM_ values
    add('include/sys/resource.h', '''#define RLIM_INFINITY (~0ULL)
#define RLIM_SAVED_CUR RLIM_INFINITY
#define RLIM_SAVED_MAX RLIM_INFINITY''', '''#define RLIM_INFINITY ((rlim_t)-3)
#define RLIM_SAVED_MAX ((rlim_t)-2)
#define RLIM_SAVED_CUR ((rlim_t)-1)''')
    add('include/sys/resource.h', '''#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#ifndef RLIMIT_RSS
#define RLIMIT_RSS     5
#define RLIMIT_NPROC   6
#define RLIMIT_NOFILE  7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS      9
#endif
#define RLIMIT_LOCKS   10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE 12
#define RLIMIT_NICE    13
#define RLIMIT_RTPRIO  14
#define RLIMIT_RTTIME  15
#define RLIMIT_NLIMITS 16''', '''#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_NOFILE  5
#define RLIMIT_VMEM    6
#define RLIMIT_AS      RLIMIT_VMEM
#define RLIMIT_NPROC   7
#define RLIMIT_NLIMITS 8''')
    # sys/stat.h: UTIME_NOW / UTIME_OMIT
    add('include/sys/stat.h', '#define UTIME_NOW  0x3fffffff\n#define UTIME_OMIT 0x3ffffffe',
        '#define UTIME_NOW  (-1L)\n#define UTIME_OMIT (-2L)')
    # sys/mman.h: Solaris MAP_ANON / MAP_NORESERVE, and MAP_ALIGN
    add('include/sys/mman.h', '#define MAP_ANON       0x20', '#define MAP_ANON       %s\n#define MAP_ALIGN      %s' % (D('MAP_ANON'), D('MAP_ALIGN')))
    add('include/sys/mman.h', '#define MAP_NORESERVE  0x4000', '#define MAP_NORESERVE  %s' % D('MAP_NORESERVE'))
    # sys/socket.h: Solaris msghdr and MSG_ flags
    add('include/sys/socket.h', '''struct msghdr {
	void *msg_name;
	socklen_t msg_namelen;
	struct iovec *msg_iov;
#if __LONG_MAX > 0x7fffffff && __BYTE_ORDER == __BIG_ENDIAN
	int __pad1;
#endif
	int msg_iovlen;
#if __LONG_MAX > 0x7fffffff && __BYTE_ORDER == __LITTLE_ENDIAN
	int __pad1;
#endif
	void *msg_control;
#if __LONG_MAX > 0x7fffffff && __BYTE_ORDER == __BIG_ENDIAN
	int __pad2;
#endif
	socklen_t msg_controllen;
#if __LONG_MAX > 0x7fffffff && __BYTE_ORDER == __LITTLE_ENDIAN
	int __pad2;
#endif
	int msg_flags;
};''', '''struct msghdr {
	void *msg_name;
	socklen_t msg_namelen;
	int __pad1;
	struct iovec *msg_iov;
	int msg_iovlen;
	int __pad2;
	void *msg_control;
	socklen_t msg_controllen;
	int msg_flags;
};''')
    add('include/sys/socket.h', '#define SOCK_RAW       3\n#define SOCK_RDM       4\n#define SOCK_SEQPACKET 5',
        '#ifndef SOCK_RAW\n#define SOCK_RAW       3\n#define SOCK_RDM       4\n#define SOCK_SEQPACKET 5\n#endif')
    add('include/sys/socket.h', '#define PF_INET6        10', '#define PF_INET6        26')
    # sockaddr_in6 is Solaris's (32 bytes, with __sin6_src_id) and so are the IPPROTO_IPV6 options
    add('include/netinet/in.h', '\tuint32_t        sin6_scope_id;\n};',
        '\tuint32_t        sin6_scope_id;\n\tuint32_t        __sin6_src_id;\n};')
    for name, val in [('IPV6_UNICAST_HOPS', 'UNICAST_HOPS'), ('IPV6_MULTICAST_IF', 'MULTICAST_IF'),
                      ('IPV6_MULTICAST_HOPS', 'MULTICAST_HOPS'), ('IPV6_MULTICAST_LOOP', 'MULTICAST_LOOP'),
                      ('IPV6_JOIN_GROUP', 'JOIN_GROUP'), ('IPV6_LEAVE_GROUP', 'LEAVE_GROUP'),
                      ('IPV6_V6ONLY', 'V6ONLY')]:
        old = {'IPV6_UNICAST_HOPS': 16, 'IPV6_MULTICAST_IF': 17, 'IPV6_MULTICAST_HOPS': 18,
               'IPV6_MULTICAST_LOOP': 19, 'IPV6_JOIN_GROUP': 20, 'IPV6_LEAVE_GROUP': 21, 'IPV6_V6ONLY': 26}[name]
        add('include/netinet/in.h', '#define %-23s %d\n' % (name, old), '#define %-23s %s\n' % (name, D('IPV6_' + val)))
    add('include/sys/socket.h', '#define SCM_RIGHTS      0x01', '#define SCM_RIGHTS      %s' % D('SCM_RIGHTS'))
    # process-level sched_*: SIEOS has them (priocntl); the emulation takes the pid negated
    # (the pthread functions pass LWP ids)
    for fn, sig, call in [
            ('sched_setscheduler', 'pid_t pid, int sched, const struct sched_param *param',
             'syscall(SYS_sched_setscheduler, -(pid ? pid : getpid()), sched, param)'),
            ('sched_getscheduler', 'pid_t pid', 'syscall(SYS_sched_getscheduler, -(pid ? pid : getpid()))'),
            ('sched_setparam', 'pid_t pid, const struct sched_param *param',
             'syscall(SYS_sched_setparam, -(pid ? pid : getpid()), param)'),
            ('sched_getparam', 'pid_t pid, struct sched_param *param',
             'syscall(SYS_sched_getparam, -(pid ? pid : getpid()), param)')]:
        add('src/sched/%s.c' % fn, 'int %s(%s)\n{\n\treturn __syscall_ret(-ENOSYS);\n}' % (fn, sig),
            '#include <unistd.h>\nint %s(%s)\n{\n\tif (pid < 0) return __syscall_ret(-EINVAL);\n\treturn %s;\n}' % (fn, sig, call))
    # posix_spawn's scheduling attributes (src/process/sieos64/posix_spawnattr_sched.c): the
    # child sets its policy and priority (the emulation takes the pid negated)
    add('src/process/posix_spawn.c', """	if (attr->__flags & POSIX_SPAWN_SETPGROUP)
		if ((ret=__syscall(SYS_setpgid, 0, attr->__pgrp)))
			goto fail;
""", """	if (attr->__flags & POSIX_SPAWN_SETPGROUP)
		if ((ret=__syscall(SYS_setpgid, 0, attr->__pgrp)))
			goto fail;

	if (attr->__flags & (POSIX_SPAWN_SETSCHEDULER | POSIX_SPAWN_SETSCHEDPARAM)) {
		struct sched_param sp = { .sched_priority = attr->__prio };
		long self = -__syscall(SYS_getpid);
		if (attr->__flags & POSIX_SPAWN_SETSCHEDULER)
			ret = __syscall(SYS_sched_setscheduler, self, attr->__pol, &sp);
		else
			ret = __syscall(SYS_sched_setparam, self, &sp);
		if (ret < 0)
			goto fail;
	}
""")
    # strftime's %Z: the zone name the program's struct tm gives (tm_zone), as glibc; musl
    # kept only names from its own tables (Python's time.strftime builds its tm_zone itself)
    add('src/time/__tz.c', """	if (p != __utc && p != __tzname[0] && p != __tzname[1] &&
	    (!zi || (uintptr_t)p-(uintptr_t)abbrevs >= abbrevs_end - abbrevs))
		p = "";""", """	if (!p)
		p = __tzname[tm->tm_isdst > 0];""")
    # strftime's E and O modifiers: eras and alternative digits (src/time/sieos64/__strftime_mod.c)
    # (and a conversion's buffer of 400 bytes, not 100: a UTF-8 %c, Shan's or Burmese's, is longer)
    add('src/time/time_impl.h', 'hidden const char *__strftime_fmt_1(char (*)[100], size_t *, int, const struct tm *, locale_t, int);',
        'hidden const char *__strftime_fmt_1(char (*)[400], size_t *, int, const struct tm *, locale_t, int);\n'
        'hidden const char *__strftime_fmt_mod(char (*)[400], size_t *, int, int, const struct tm *, locale_t, int);')
    add('src/time/strftime.c', 'const char *__strftime_fmt_1(char (*s)[100], size_t *l, int f, const struct tm *tm, locale_t loc, int pad)',
        'const char *__strftime_fmt_1(char (*s)[400], size_t *l, int f, const struct tm *tm, locale_t loc, int pad)')
    add('src/time/strftime.c', '\tchar buf[100];\n', '\tchar buf[400];\n')
    add('src/time/wcsftime.c', '\tchar buf[100];\n\twchar_t wbuf[100];\n', '\tchar buf[400];\n\twchar_t wbuf[400];\n')
    add('src/time/strftime.c', """		if (*f == 'E' || *f == 'O') f++;
		t = __strftime_fmt_1(&buf, &k, *f, tm, loc, pad);""", """		int mod = 0;
		if (*f == 'E' || *f == 'O') mod = *f++;
		t = __strftime_fmt_mod(&buf, &k, mod, *f, tm, loc, pad);""")
    add('src/time/wcsftime.c', """		if (*f == 'E' || *f == 'O') f++;
		t_mb = __strftime_fmt_1(&buf, &k, *f, tm, loc, pad);""", """		int mod = 0;
		if (*f == 'E' || *f == 'O') mod = *f++;
		t_mb = __strftime_fmt_mod(&buf, &k, mod, *f, tm, loc, pad);""")
    # sendmsg and recvmsg with several buffers on datagram sockets: one datagram, gathered and
    # scattered by the C library (src/network/sieos64/__sieos_dgram.c)
    add('src/network/sendmsg.c', """ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
""", """hidden int __sieos_is_dgram(int);
hidden char *__sieos_dgram_buf(const struct msghdr *, size_t, size_t *);
hidden void __sieos_dgram_copy(const struct msghdr *, char *, size_t, int);
#include <stdlib.h>

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
	if (msg && msg->msg_iovlen > 1 && __sieos_is_dgram(fd)) {
		size_t n;
		char *b = __sieos_dgram_buf(msg, (size_t)-1, &n);
		if (!b) return -1;
		__sieos_dgram_copy(msg, b, n, 1);
		struct iovec v = { b, n };
		struct msghdr m = *msg;
		m.msg_iov = &v;
		m.msg_iovlen = 1;
		ssize_t r = sendmsg(fd, &m, flags);
		free(b);
		return r;
	}
""")
    add('src/network/recvmsg.c', """ssize_t recvmsg(int fd, struct msghdr *msg, int flags)
{
""", """hidden int __sieos_is_dgram(int);
hidden char *__sieos_dgram_buf(const struct msghdr *, size_t, size_t *);
hidden void __sieos_dgram_copy(const struct msghdr *, char *, size_t, int);
#include <stdlib.h>

ssize_t recvmsg(int fd, struct msghdr *msg, int flags)
{
	if (msg && msg->msg_iovlen > 1 && __sieos_is_dgram(fd)) {
		size_t n;
		char *b = __sieos_dgram_buf(msg, 65536, &n);
		if (!b) return -1;
		struct iovec v = { b, n }, *iov = msg->msg_iov;
		int iovlen = msg->msg_iovlen;
		msg->msg_iov = &v;
		msg->msg_iovlen = 1;
		ssize_t r = recvmsg(fd, msg, flags);
		msg->msg_iov = iov;
		msg->msg_iovlen = iovlen;
		if (r > 0) __sieos_dgram_copy(msg, b, (size_t)r < n ? (size_t)r : n, 0);
		free(b);
		return r;
	}
""")
    # confstr(_CS_PATH): the POSIX utilities are the GNU ones in /usr/gnu/bin (as /usr/xpg4/bin on
    # Solaris); /bin holds the smaller SIEOS programs
    add('src/conf/confstr.c', 's = "/bin:/usr/bin";', 's = "/usr/gnu/bin:/bin:/usr/bin";')
    # fchmodat and faccessat take a flags argument on SIEOS (Linux's take none): pass 0
    add('src/stat/chmod.c', 'syscall(SYS_fchmodat, AT_FDCWD, path, mode);', 'syscall(SYS_fchmodat, AT_FDCWD, path, mode, 0);')
    add('src/stat/fchmod.c', 'syscall(SYS_fchmodat, AT_FDCWD, buf, mode);', 'syscall(SYS_fchmodat, AT_FDCWD, buf, mode, 0);')
    add('src/stat/fchmodat.c', 'if (!flag) return syscall(SYS_fchmodat, fd, path, mode);',
        'if (!flag) return syscall(SYS_fchmodat, fd, path, mode, 0);')
    add('src/stat/fchmodat.c', 'else ret = syscall(SYS_fchmodat, AT_FDCWD, proc, mode);',
        'else ret = syscall(SYS_fchmodat, AT_FDCWD, proc, mode, 0);')
    add('src/unistd/faccessat.c', 'return syscall(SYS_faccessat, fd, filename, amode);',
        'return syscall(SYS_faccessat, fd, filename, amode, 0);')
    # ldso: $ORIGIN of the main program from AT_SUN_EXECNAME (no /proc/self/exe)
    add('ldso/dynlink.c', '''		l = readlink("/proc/self/exe", buf, buf_size);
		if (l == -1) switch (errno) {
		case ENOENT:
		case ENOTDIR:
		case EACCES:
			return 0;
		default:
			return -1;
		}
		if (l >= buf_size)
			return 0;
		buf[l] = 0;
		origin = buf;''', '''		{
			const char *en = 0;
			for (size_t *a = libc.auxv; *a; a += 2)
				if (a[0] == %s) en = (const char *)a[1];
			if (!en) return 0;
			l = strlen(en);
			if (l >= buf_size)
				return 0;
			memcpy(buf, en, l + 1);
		}
		origin = buf;''' % D('AT_SUN_EXECNAME'))
    add('include/stdlib.h', '#define WNOHANG    1\n#define WUNTRACED  2', '#define WNOHANG    0x40\n#define WUNTRACED  0x04')
    flags = ['OOB', 'PEEK', 'DONTROUTE', 'CTRUNC', 'TRUNC', 'DONTWAIT', 'EOR', 'WAITALL', 'NOSIGNAL']
    old_msg = '''#define MSG_OOB       0x0001
#define MSG_PEEK      0x0002
#define MSG_DONTROUTE 0x0004
#define MSG_CTRUNC    0x0008
#define MSG_PROXY     0x0010
#define MSG_TRUNC     0x0020
#define MSG_DONTWAIT  0x0040
#define MSG_EOR       0x0080
#define MSG_WAITALL   0x0100
#define MSG_FIN       0x0200
#define MSG_SYN       0x0400
#define MSG_CONFIRM   0x0800
#define MSG_RST       0x1000
#define MSG_ERRQUEUE  0x2000
#define MSG_NOSIGNAL  0x4000
#define MSG_MORE      0x8000'''
    add('include/sys/socket.h', old_msg, '\n'.join('#define MSG_%-9s %s' % (f, D('MSG_' + f)) for f in flags) +
        '\n#define MSG_MORE      0')
    # sys/ipc.h, sys/sem.h, sys/shm.h: Solaris commands
    add('include/sys/ipc.h', '#define IPC_RMID 0\n#define IPC_SET  1', '#define IPC_RMID %s\n#define IPC_SET  %s' % (D('IPC_RMID'), D('IPC_SET')))
    add('include/sys/sem.h', '''#define GETPID		11
#define GETVAL		12
#define GETALL		13
#define GETNCNT		14
#define GETZCNT		15
#define SETVAL		16
#define SETALL		17''', '\n'.join('#define %s\t\t%s' % (n, D(n)) for n in
                                     ['GETPID', 'GETVAL', 'GETALL', 'GETNCNT', 'GETZCNT', 'SETVAL', 'SETALL']))
    add('include/sys/shm.h', '#define SHM_LOCK 11\n#define SHM_UNLOCK 12', '#define SHM_LOCK %s\n#define SHM_UNLOCK %s' % (D('SHM_LOCK'), D('SHM_UNLOCK')))
    # termios.h: Solaris NCCS
    add('include/termios.h', '#define NCCS 32', '#define NCCS 19')
    # sys/sysmacros.h: dev_t = major << 32 | minor
    add('include/sys/sysmacros.h', '''#define major(x) \\
	((unsigned)( (((x)>>31>>1) & 0xfffff000) | (((x)>>8) & 0x00000fff) ))
#define minor(x) \\
	((unsigned)( (((x)>>12) & 0xffffff00) | ((x) & 0x000000ff) ))

#define makedev(x,y) ( \\
        (((x)&0xfffff000ULL) << 32) | \\
	(((x)&0x00000fffULL) << 8) | \\
        (((y)&0xffffff00ULL) << 12) | \\
	(((y)&0x000000ffULL)) )''', '''#define major(x) ((unsigned)((unsigned long long)(x) >> 32))
#define minor(x) ((unsigned)((x) & 0xffffffffULL))
#define makedev(x,y) ((((unsigned long long)(x)) << 32) | ((y) & 0xffffffffULL))''')
    # internal signals: SIGCANCEL is Solaris's (36); SIGTIMER, SIGSYNCCALL keep 32 and 33
    add('src/internal/pthread_impl.h', '#define SIGTIMER 32\n#define SIGCANCEL 33\n#define SIGSYNCCALL 34',
        '#define SIGTIMER 32\n#define SIGCANCEL 36\n#define SIGSYNCCALL 33')
    # signals reserved for libc: 32 (SIGTIMER), 33 (SIGSYNCCALL), 36 (SIGCANCEL)
    for f in ['src/signal/sigaddset.c', 'src/signal/sigdelset.c']:
        add(f, 'sig-32U < 3', '(sig-32U < 2 || sig == 36)')
    add('src/signal/sigaction.c', 'if (sig-32U < 3 || sig-1U >= _NSIG-1)', 'if (sig-32U < 2 || sig == 36 || sig-1U >= _NSIG-1)')
    add('src/process/posix_spawn.c', 'if (i-32<3U) {', 'if (i-32<2U || i==36) {')
    add('src/signal/sigfillset.c', 'set->__bits[0] = 0xfffffffc7ffffffful;', 'set->__bits[0] = 0xfffffff67ffffffful;')
    add('src/signal/block.c', '0xfffffffc7fffffff, -1UL', '0xfffffff67fffffff, -1UL')
    # the thread's name (src/thread/sieos64/pthread_[gs]etname_np.c): no prctl, no /proc/self/task
    add('src/internal/pthread_impl.h', '\tvoid *stdio_locks;\n', '\tvoid *stdio_locks;\n\tchar name[16];\n')
    add('src/internal/pthread_impl.h', '[sizeof(long)==4] = 3UL<<(32*(sizeof(long)>4)) })',
        '[0] = (1UL<<32) | (1UL<<35) })')
    add('src/signal/sigrtmin.c', 'return 35;', 'return %s;' % D('SIGRTMIN'))
    add('src/signal/sigrtmax.c', 'return _NSIG-1;', 'return %s;' % D('SIGRTMAX'))
    # sysconf: no RLIMIT_NPROC; the kernel's process table has 256 slots
    add('src/conf/sysconf.c', '[_SC_CHILD_MAX] = RLIM(NPROC),', '[_SC_CHILD_MAX] = 255,')
    # condition variables: clock ids are stored XOR CLOCK_REALTIME, so that the
    # all-zero default (PTHREAD_COND_INITIALIZER, default attributes) means CLOCK_REALTIME
    add('src/thread/pthread_condattr_setclock.c', 'if (clk < 0 || clk-2U < 2) return EINVAL;',
        'if (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC) return EINVAL;')
    add('src/thread/pthread_condattr_setclock.c', 'a->__attr |= clk;', 'a->__attr |= clk ^ CLOCK_REALTIME;')
    add('src/thread/pthread_attr_get.c', '*clk = a->__attr & 0x7fffffff;', '*clk = (a->__attr & 0x7fffffff) ^ CLOCK_REALTIME;')
    add('src/thread/pthread_cond_timedwait.c', 'clock = c->_c_clock,', 'clock = c->_c_clock ^ CLOCK_REALTIME,')
    # Solaris extensions in standard headers
    add('include/sys/time.h', '''int gettimeofday (struct timeval *__restrict, void *__restrict);''', '''int gettimeofday (struct timeval *__restrict, void *__restrict);

#if defined(_GNU_SOURCE) || defined(_BSD_SOURCE)
typedef long long hrtime_t;
hrtime_t gethrtime(void);
hrtime_t gethrvtime(void);
#endif''')
    add('include/stdlib.h', '''int getloadavg(double *, int);''', '''int getloadavg(double *, int);
void closefrom(int);
int fdwalk(int (*)(void *, int), void *);
const char *getexecname(void);''')
    add('include/signal.h', '''void psiginfo(const siginfo_t *, const char *);''', '''void psiginfo(const siginfo_t *, const char *);
#if defined(_GNU_SOURCE) || defined(_BSD_SOURCE)
#define SIG2STR_MAX 32
int sig2str(int, char *);
int str2sig(const char *, int *);
#endif''')
    # sigevent: Solaris's notification values; SIGEV_PORT (event ports), no SIGEV_THREAD_ID
    add('include/signal.h', '''#define SIGEV_SIGNAL 0
#define SIGEV_NONE 1
#define SIGEV_THREAD 2
#define SIGEV_THREAD_ID 4''', '''#define SIGEV_NONE %s
#define SIGEV_SIGNAL %s
#define SIGEV_THREAD %s
#define SIGEV_PORT %s''' % (D('SIGEV_NONE'), D('SIGEV_SIGNAL'), D('SIGEV_THREAD'), D('SIGEV_PORT')))
    add('include/ucontext.h', '''int  swapcontext(struct __ucontext *, const struct __ucontext *);''',
        '''int  swapcontext(struct __ucontext *, const struct __ucontext *);

#if defined(_GNU_SOURCE) || defined(_BSD_SOURCE)
int walkcontext(const struct __ucontext *, int (*)(unsigned long, int, void *), void *);
int printstack(int);
int addrtosymstr(void *, char *, int);
#endif''')
    add('include/stdlib.h', '''int getloadavg(double *, int);''', '''int getloadavg(double *, int);
unsigned arc4random(void);
void arc4random_buf(void *, size_t);
unsigned arc4random_uniform(unsigned);''')
    # POSIX.1-2024's waits with a clock (src/sieos/clockwait.c)
    add('include/pthread.h', 'int pthread_mutex_timedlock(pthread_mutex_t *__restrict, const struct timespec *__restrict);',
        'int pthread_mutex_timedlock(pthread_mutex_t *__restrict, const struct timespec *__restrict);\n'
        'int pthread_mutex_clocklock(pthread_mutex_t *__restrict, clockid_t, const struct timespec *__restrict);')
    add('include/pthread.h', 'int pthread_cond_timedwait(pthread_cond_t *__restrict, pthread_mutex_t *__restrict, const struct timespec *__restrict);',
        'int pthread_cond_timedwait(pthread_cond_t *__restrict, pthread_mutex_t *__restrict, const struct timespec *__restrict);\n'
        'int pthread_cond_clockwait(pthread_cond_t *__restrict, pthread_mutex_t *__restrict, clockid_t, const struct timespec *__restrict);')
    add('include/pthread.h', 'int pthread_rwlock_timedrdlock(pthread_rwlock_t *__restrict, const struct timespec *__restrict);',
        'int pthread_rwlock_timedrdlock(pthread_rwlock_t *__restrict, const struct timespec *__restrict);\n'
        'int pthread_rwlock_clockrdlock(pthread_rwlock_t *__restrict, clockid_t, const struct timespec *__restrict);')
    add('include/pthread.h', 'int pthread_rwlock_timedwrlock(pthread_rwlock_t *__restrict, const struct timespec *__restrict);',
        'int pthread_rwlock_timedwrlock(pthread_rwlock_t *__restrict, const struct timespec *__restrict);\n'
        'int pthread_rwlock_clockwrlock(pthread_rwlock_t *__restrict, clockid_t, const struct timespec *__restrict);')
    add('include/semaphore.h', '#define __NEED_time_t\n', '#define __NEED_time_t\n#define __NEED_clockid_t\n')
    add('include/semaphore.h', 'int    sem_timedwait(sem_t *__restrict, const struct timespec *__restrict);',
        'int    sem_timedwait(sem_t *__restrict, const struct timespec *__restrict);\n'
        'int    sem_clockwait(sem_t *__restrict, clockid_t, const struct timespec *__restrict);')
    # backtrace(3C) unwinds through its own frames: they have unwind tables
    add('Makefile', '$(CRT_OBJS): CFLAGS_ALL += -DCRT',
        '$(CRT_OBJS): CFLAGS_ALL += -DCRT\n\nobj/src/sieos/backtrace.o obj/src/sieos/backtrace.lo: '
        'CFLAGS_ALL += -funwind-tables -fasynchronous-unwind-tables')
    # posix_spawn: the child is a fork (no CLONE_VM|CLONE_VFORK processes on SIEOS)
    add('src/process/posix_spawn.c', '''	pid = __clone(child, stack+sizeof stack,
		CLONE_VM|CLONE_VFORK|SIGCHLD, &args);''', '''	(void)stack;
	pid = __syscall(SYS_fork);
	if (pid == 0) child(&args);''')
    # POSIX.1-2024 names musl 1.2.6 does not have yet: timestamps are in ns on
    # every SIEOS file system; there is no UUCP
    add('include/unistd.h', '#define _PC_2_SYMLINKS	20', '#define _PC_2_SYMLINKS	20\n#define _PC_TIMESTAMP_RESOLUTION	21')
    add('include/unistd.h', '#define _SC_SIGSTKSZ	250', '#define _SC_SIGSTKSZ	250\n#define _SC_XOPEN_UUCP	251')
    add('src/conf/fpathconf.c', '[_PC_2_SYMLINKS] = 1', '[_PC_2_SYMLINKS] = 1,\n\t\t[_PC_TIMESTAMP_RESOLUTION] = 1')
    add('src/conf/sysconf.c', '[_SC_SIGSTKSZ] = JT_SIGSTKSZ,', '[_SC_SIGSTKSZ] = JT_SIGSTKSZ,\n\t\t[_SC_XOPEN_UUCP] = -1,')
    # backports/src/time/strptime.c (musl 1.2.6) parses %Z with __tzname_to_isdst
    add('src/time/time_impl.h', 'hidden const char *__tm_to_tzname(const struct tm *);',
        'hidden const char *__tm_to_tzname(const struct tm *);\nhidden int __tzname_to_isdst(const char *restrict *);')
    add('src/time/__tz.c', '''const char *__tm_to_tzname(const struct tm *tm)''', '''int __tzname_to_isdst(const char *restrict *s)
{
	size_t len;
	int isdst = -1;
	LOCK(lock);
	if (tzname[0] && !strncmp(*s, tzname[0], len = strlen(tzname[0]))) {
		isdst = 0;
		*s += len;
	} else if (tzname[1] && !strncmp(*s, tzname[1], len=strlen(tzname[1]))) {
		isdst = 1;
		*s += len;
	} else {
		while (isalpha(**s)) ++*s;
	}
	UNLOCK(lock);
	return isdst;
}

const char *__tm_to_tzname(const struct tm *tm)''')
    return P

def apply_patches(tree):
    for f, old, new in patches():
        path = os.path.join(tree, f)
        text = open(path).read()
        n = text.count(old)
        if n != 1:
            sys.exit('patch %s: expected 1 match, found %d:\n%s' % (f, n, old[:200]))
        open(path, 'w').write(text.replace(old, new))

# ---------------------------------------------------------------- Solaris headers

def strip_prefixes(text):
    text = re.sub(r'\bSIEOS_ABI_(\w+)_H\b', r'_SIEOS_SYS_\1_H', text)
    text = re.sub(r'\bSIEOS_', '', text)
    text = re.sub(r'\bsieos_', '', text)
    return text

def gen_solaris_headers(tree):
    """Public Solaris-extension headers generated from the ABI headers."""
    out = {
        'include/sys/procfs.h': ('procfs.h', ['sys/types.h', 'signal.h', 'stdint.h', 'time.h']),
        'include/sys/__lwp_abi.h': ('lwp.h', ['sys/types.h']),
        'include/sys/port.h': ('port.h', ['sys/types.h', 'time.h']),
    }
    for dst, (src, incs) in out.items():
        text = open(os.path.join(ABI, src)).read()
        body = re.sub(r'#include "[^"]+"\n', '', text)
        body = strip_prefixes(body)
        # the ABI's own typedef names are the libc ones
        body = re.sub(r'STATIC_ASSERT\([^;]*\);\n', '', body)
        pre = ''.join('#include <%s>\n' % i for i in incs)
        if src == 'procfs.h':
            pre += ('#define __NEED_sigset_t\n#define __NEED_struct_timespec\n#include <bits/alltypes.h>\n'
                    '#include <sys/lwp.h>\n')
        if src == 'port.h':
            body = body.replace('\n#endif', '\ntypedef struct file_obj file_obj_t;\n#endif')
        body = body.replace('#define _SIEOS_SYS_', pre + '#define _SIEOS_SYS_', 1) if pre else body
        open(os.path.join(tree, dst), 'w').write(body)

# ---------------------------------------------------------------- main

def gen_collate(tree):
    """src/internal/sieos_collate.h: for letters with accents (Latin, Greek, Cyrillic), the
    base letter they sort as, their accent and case (src/locale/sieos64/__sieos_coll.c),
    from the build host's Unicode database."""
    import unicodedata
    special = {0xdf: ('ss', 0, 0), 0x1e9e: ('ss', 0, 1), 0xe6: ('ae', 0, 0), 0xc6: ('ae', 0, 1),
               0x153: ('oe', 0, 0), 0x152: ('oe', 0, 1), 0x133: ('ij', 0, 0), 0x132: ('ij', 0, 1),
               0xf8: ('o', 0x80, 0), 0xd8: ('o', 0x80, 1), 0x111: ('d', 0x81, 0), 0x110: ('d', 0x81, 1),
               0x142: ('l', 0x82, 0), 0x141: ('l', 0x82, 1), 0x131: ('i', 0x83, 0), 0xf0: ('d', 0x84, 0),
               0xd0: ('d', 0x84, 1), 0xfe: ('th', 0, 0), 0xde: ('th', 0, 1), 0x127: ('h', 0x81, 0),
               0x126: ('h', 0x81, 1), 0x167: ('t', 0x81, 0), 0x166: ('t', 0x81, 1)}
    rows = []
    for lo, hi in [(0xc0, 0x250), (0x370, 0x400), (0x400, 0x530), (0x1e00, 0x1f00)]:
        for cp in range(lo, hi):
            ch = chr(cp)
            cat = unicodedata.category(ch)
            if not cat.startswith('L'):
                continue
            upper = 1 if cat in ('Lu', 'Lt') else 0
            if cp in special:
                base, acc, upper = special[cp]
            else:
                d = unicodedata.normalize('NFD', ch)
                marks = [m for m in d[1:] if unicodedata.combining(m)]
                if not marks:
                    continue
                base = unicodedata.normalize('NFD', d[0].lower())[0]
                m = ord(marks[0])
                acc = m - 0x2ff if 0x300 <= m < 0x370 else 0x7f
            p = [ord(c) for c in base] + [0]
            rows.append((cp, p[0], p[1], acc, upper))
    lines = ['/* generated by libc/sieos-port.py from Unicode %s: letter, its base letter(s), accent, '
             'case */' % unicodedata.unidata_version,
             'static const struct { unsigned short cp, p1, p2; unsigned char acc, upper; } __coll_tab[] = {']
    lines += ['\t{ 0x%04x, 0x%04x, 0x%04x, %d, %d },' % r for r in rows]
    lines.append('};')
    open(os.path.join(tree, 'src', 'internal', 'sieos_collate.h'), 'w').write('\n'.join(lines) + '\n')

def main():
    tarball, dest = sys.argv[1], sys.argv[2]
    if os.path.exists(dest):
        shutil.rmtree(dest)
    tmp = dest + '.tmp'
    if os.path.exists(tmp):
        shutil.rmtree(tmp)
    with tarfile.open(tarball) as t:
        t.extractall(tmp)
    top = os.path.join(tmp, os.listdir(tmp)[0])
    shutil.move(top, dest)
    shutil.rmtree(tmp)
    overlay = os.path.join(HERE, 'port')
    for root, dirs, files in os.walk(overlay):
        rel = os.path.relpath(root, overlay)
        os.makedirs(os.path.join(dest, rel), exist_ok=True)
        for f in files:
            shutil.copy(os.path.join(root, f), os.path.join(dest, rel, f))
    backports = os.path.join(HERE, 'backports')
    for root, dirs, files in os.walk(backports):
        rel = os.path.relpath(root, backports)
        for f in files:
            if f != 'README.md':
                shutil.copy(os.path.join(root, f), os.path.join(dest, rel, f))
    shutil.copytree(ABI, os.path.join(dest, 'arch', 'sieos64', 'sieos'))
    for root, dirs, files in os.walk(os.path.join(dest, 'arch', 'sieos64')):
        for f in files:
            if f.endswith('.h') or f.endswith('.in'):
                expand_markers(os.path.join(root, f))
    for root, dirs, files in os.walk(os.path.join(overlay, 'include')):
        for f in files:
            expand_markers(os.path.join(dest, 'include', os.path.relpath(os.path.join(root, f), os.path.join(overlay, 'include'))))
    gen_syscalls(dest)
    apply_patches(dest)
    gen_solaris_headers(dest)
    gen_collate(dest)

if __name__ == '__main__':
    main()
