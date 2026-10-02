/*
 * sieos/syscall.h - SIEOS ABI v2 system call numbers and calling convention.
 *
 * Entry:   the "syscall" instruction.  rax = number; arguments in
 *          rdi, rsi, rdx, r10, r8, r9.  rcx and r11 are clobbered.
 * Return:  carry flag clear -> success, rax = value (rdx = second value
 *          for the calls marked "rv2");
 *          carry flag set   -> failure, rax = positive SIEOS_E* errno.
 *          (The Solaris convention; libc turns it into errno / -1.)
 *
 * Names and semantics follow Solaris where Solaris has the call; the
 * numbering is SIEOS's own.  Numbers are stable once released; gaps are
 * reserved for their group.  Calls listed as "reserved" are defined by the
 * ABI but may return SIEOS_ENOSYS until the milestone that implements them.
 */
#ifndef SIEOS_ABI_SYSCALL_H
#define SIEOS_ABI_SYSCALL_H

/* ---- processes (1-19) ------------------------------------------------ */
#define SIEOS_SYS_exit             1   /* exit(int status)                    - does not return */
#define SIEOS_SYS_forkx            2   /* forkx(int flags)                    - child: 0, parent: pid */
#define SIEOS_SYS_vforkx           3   /* vforkx(int flags)                   - may behave like forkx */
#define SIEOS_SYS_execve           4   /* execve(path, argv, envp) */
#define SIEOS_SYS_waitid           5   /* waitid(idtype, id, siginfo *, options) */
#define SIEOS_SYS_getpid           6   /* getpid()               rv2: rdx = parent pid */
#define SIEOS_SYS_pgrpsys          7   /* pgrpsys(op, pid, pgid) SIEOS_PGRP_* ops */
#define SIEOS_SYS_uadmin           8   /* uadmin(cmd, fcn, mdep) */
#define SIEOS_SYS_sysinfo          9   /* sysinfo(cmd, buf, count) */
#define SIEOS_SYS_sysconfig       10   /* sysconfig(name) */
#define SIEOS_SYS_priocntl        11   /* priocntl(idtype, id, cmd, arg), sieos/priocntl.h */
#define SIEOS_SYS_yield           12   /* yield() */
#define SIEOS_SYS_getrlimit       13   /* getrlimit(resource, struct sieos_rlimit *) */
#define SIEOS_SYS_setrlimit       14   /* setrlimit(resource, const struct sieos_rlimit *) */
#define SIEOS_SYS_brk             15   /* brk(void *end)                      - returns new break */
#define SIEOS_SYS_umask           16   /* umask(mask)                         - returns old mask */
#define SIEOS_SYS_chroot          17   /* chroot(path) */
#define SIEOS_SYS_getrusage       18   /* getrusage(who, struct sieos_rusage *)  who: SIEOS_RUSAGE_* */

/* ---- lightweight processes (20-39) ----------------------------------- */
#define SIEOS_SYS_lwp_create      20   /* lwp_create(ucontext *, flags, lwpid *) */
#define SIEOS_SYS_lwp_exit        21   /* lwp_exit()                          - does not return */
#define SIEOS_SYS_lwp_self        22   /* lwp_self() */
#define SIEOS_SYS_lwp_kill        23   /* lwp_kill(lwpid, sig) */
#define SIEOS_SYS_lwp_wait        24   /* lwp_wait(lwpid or 0 = any, lwpid *departed) */
#define SIEOS_SYS_lwp_suspend     25   /* lwp_suspend(lwpid) */
#define SIEOS_SYS_lwp_continue    26   /* lwp_continue(lwpid) */
#define SIEOS_SYS_lwp_park        27   /* lwp_park(const timespec *rel, lwpid unpark_first) */
#define SIEOS_SYS_lwp_unpark      28   /* lwp_unpark(lwpid) */
#define SIEOS_SYS_lwp_unpark_all  29   /* lwp_unpark_all(const lwpid *, int n) */
#define SIEOS_SYS_lwp_private     30   /* lwp_private(op, which, base)        SIEOS_LWP_*PRIVATE */
#define SIEOS_SYS_lwp_umtx_wait   31   /* lwp_umtx_wait(int *addr, int expected, const timespec *rel, flags) */
#define SIEOS_SYS_lwp_umtx_wake   32   /* lwp_umtx_wake(int *addr, int count, flags) - returns woken */
#define SIEOS_SYS_lwp_sigmask     33   /* lwp_sigmask(how, const sigset *, sigset *old) */
#define SIEOS_SYS_lwp_name        34   /* lwp_name(op, lwpid, buf, len)       SIEOS_LWP_NAME_* */

/* ---- signals and contexts (40-49) ------------------------------------ */
#define SIEOS_SYS_kill            40   /* kill(pid, sig) */
#define SIEOS_SYS_sigaction       41   /* sigaction(sig, const sieos_sigaction *, sieos_sigaction *) */
#define SIEOS_SYS_sigpending      42   /* sigpending(op, sigset *)            SIEOS_SIGPENDING / SIEOS_SIGFILLSET */
#define SIEOS_SYS_sigsuspend      43   /* sigsuspend(const sigset *) */
#define SIEOS_SYS_sigaltstack     44   /* sigaltstack(const stack *, stack *old) */
#define SIEOS_SYS_sigqueue        45   /* sigqueue(pid, sig, value, flags) */
#define SIEOS_SYS_sigtimedwait    46   /* sigtimedwait(const sigset *, siginfo *, const timespec *) */
#define SIEOS_SYS_context         47   /* context(op, ucontext *)             SIEOS_GETCONTEXT / SIEOS_SETCONTEXT */

/* ---- file descriptors and I/O (50-89) -------------------------------- */
#define SIEOS_SYS_read            50
#define SIEOS_SYS_write           51
#define SIEOS_SYS_openat          52   /* openat(dirfd, path, oflag, mode) */
#define SIEOS_SYS_close           53
#define SIEOS_SYS_pread           54   /* pread(fd, buf, n, off) */
#define SIEOS_SYS_pwrite          55
#define SIEOS_SYS_readv           56   /* readv(fd, const iovec *, iovcnt) */
#define SIEOS_SYS_writev          57
#define SIEOS_SYS_lseek           58   /* lseek(fd, off, whence)              - returns new offset */
#define SIEOS_SYS_fcntl           59   /* fcntl(fd, cmd, arg)                 dup/cloexec/locks/F_FREESP */
#define SIEOS_SYS_ioctl           60
#define SIEOS_SYS_pipe2           61   /* pipe2(int fds[2], flags) */
#define SIEOS_SYS_pollsys         62   /* pollsys(pollfd *, nfds, const timespec *, const sigset *) */
#define SIEOS_SYS_getdents        63   /* getdents(fd, sieos_dirent *, size) */
#define SIEOS_SYS_fstatat         64   /* fstatat(fd, path or NULL, sieos_stat *, flag) - NULL path = fstat */
#define SIEOS_SYS_fchmodat        65   /* fchmodat(fd, path or NULL, mode, flag) */
#define SIEOS_SYS_fchownat        66   /* fchownat(fd, path or NULL, uid, gid, flag) */
#define SIEOS_SYS_faccessat       67   /* faccessat(fd, path, amode, flag) */
#define SIEOS_SYS_mkdirat         68
#define SIEOS_SYS_mknodat         69   /* mknodat(fd, path, mode, dev) */
#define SIEOS_SYS_unlinkat        70   /* unlinkat(fd, path, flag)            SIEOS_AT_REMOVEDIR */
#define SIEOS_SYS_renameat        71   /* renameat(fromfd, from, tofd, to) */
#define SIEOS_SYS_linkat          72   /* linkat(fd1, p1, fd2, p2, flag) */
#define SIEOS_SYS_symlinkat       73   /* symlinkat(target, fd, path) */
#define SIEOS_SYS_readlinkat      74   /* readlinkat(fd, path, buf, size) */
#define SIEOS_SYS_utimensat       75   /* utimensat(fd, path or NULL, const timespec[2], flag) */
#define SIEOS_SYS_ftruncate       76   /* ftruncate(fd, length) */
#define SIEOS_SYS_fdsync          77   /* fdsync(fd, flag)                    SIEOS_FSYNC / SIEOS_FDSYNC */
#define SIEOS_SYS_chdir           78
#define SIEOS_SYS_fchdir          79
#define SIEOS_SYS_getcwd          80   /* getcwd(buf, size) */
#define SIEOS_SYS_mount           81   /* mount(spec, dir, mflag, fstype, data, datalen) */
#define SIEOS_SYS_umount2         82   /* umount2(dir, flags) */
#define SIEOS_SYS_statvfs         83   /* statvfs(path, sieos_statvfs *) */
#define SIEOS_SYS_fstatvfs        84   /* fstatvfs(fd, sieos_statvfs *) */
#define SIEOS_SYS_sync            85

/* ---- memory (90-99) -------------------------------------------------- */
#define SIEOS_SYS_mmap            90   /* mmap(addr, len, prot, flags, fd, off) */
#define SIEOS_SYS_munmap          91
#define SIEOS_SYS_mprotect        92
#define SIEOS_SYS_memcntl         93   /* memcntl(addr, len, cmd, arg, attr, mask)  msync/madvise/mlock */
#define SIEOS_SYS_mincore         94   /* mincore(addr, len, char *vec) */

/* ---- credentials (100-114) ------------------------------------------- */
#define SIEOS_SYS_getuid         100   /* getuid()               rv2: rdx = euid */
#define SIEOS_SYS_geteuid        101
#define SIEOS_SYS_getgid         102   /* getgid()               rv2: rdx = egid */
#define SIEOS_SYS_getegid        103
#define SIEOS_SYS_setuid         104
#define SIEOS_SYS_setgid         105
#define SIEOS_SYS_seteuid        106
#define SIEOS_SYS_setegid        107
#define SIEOS_SYS_setreuid       108
#define SIEOS_SYS_setregid       109
#define SIEOS_SYS_getgroups      110   /* getgroups(n, gid *) */
#define SIEOS_SYS_setgroups      111

/* ---- time (115-129) -------------------------------------------------- */
#define SIEOS_SYS_clock_gettime  115
#define SIEOS_SYS_clock_settime  116
#define SIEOS_SYS_clock_getres   117
#define SIEOS_SYS_nanosleep      118   /* nanosleep(const timespec *, timespec *rem) */
#define SIEOS_SYS_setitimer      119
#define SIEOS_SYS_getitimer      120
#define SIEOS_SYS_times          121   /* times(sieos_tms *) - returns elapsed ticks */
#define SIEOS_SYS_gethrtime      122   /* gethrtime()        - ns since boot, never fails */
#define SIEOS_SYS_gethrvtime     123   /* gethrvtime()       - LWP CPU time in ns */
#define SIEOS_SYS_adjtime        124
#define SIEOS_SYS_stime          125   /* stime(time_t) - set the clock (root) */
#define SIEOS_SYS_timer_create   126   /* timer_create(clock, sigevent *, timer_t *)   (reserved) */
#define SIEOS_SYS_timer_delete   127   /* (reserved) */
#define SIEOS_SYS_timer_settime  128   /* (reserved) */
#define SIEOS_SYS_timer_gettime  129   /* (reserved) */

/* ---- sockets (130-149) ----------------------------------------------- */
#define SIEOS_SYS_so_socket      130   /* so_socket(domain, type, protocol) - type may carry SIEOS_SOCK_CLOEXEC/NONBLOCK */
#define SIEOS_SYS_bind           131
#define SIEOS_SYS_listen         132
#define SIEOS_SYS_accept         133   /* accept(s, addr, socklen *, flags) */
#define SIEOS_SYS_connect        134
#define SIEOS_SYS_shutdown       135
#define SIEOS_SYS_recvfrom       136   /* recvfrom(s, buf, n, flags, addr, socklen *) */
#define SIEOS_SYS_sendto         137   /* sendto(s, buf, n, flags, addr, socklen) */
#define SIEOS_SYS_recvmsg        138
#define SIEOS_SYS_sendmsg        139
#define SIEOS_SYS_getsockname    140
#define SIEOS_SYS_getpeername    141
#define SIEOS_SYS_getsockopt     142   /* getsockopt(s, level, name, val, socklen *) */
#define SIEOS_SYS_setsockopt     143
#define SIEOS_SYS_so_socketpair  144   /* so_socketpair(domain, type, proto, int sv[2]) (reserved: AF_UNIX) */

/* ---- System V IPC, doors, event ports (150-159) - reserved ----------- */
#define SIEOS_SYS_msgsys         150
#define SIEOS_SYS_semsys         151
#define SIEOS_SYS_shmsys         152
#define SIEOS_SYS_door           153
#define SIEOS_SYS_portfs         154

/* ---- processors (160-169) -------------------------------------------- */
#define SIEOS_SYS_processor_info 160   /* processor_info(id, sieos_processor_info *) */
#define SIEOS_SYS_p_online       161   /* p_online(id, flag)                 SIEOS_P_* */
#define SIEOS_SYS_processor_bind 162   /* processor_bind(idtype, id, cpu, processorid *obind) */
#define SIEOS_SYS_pset           163   /* (reserved) */
#define SIEOS_SYS_getloadavg     164   /* getloadavg(long *avg[3] in 1/1000ths, n) */
#define SIEOS_SYS_lwp_affinity   165   /* lwp_affinity(idtype, id, op, uint64_t *mask)  SIEOS_AFF_*: the CPUs it may run on */

/* ---- SIEOS extensions (200-) ------------------------------------------ */
#define SIEOS_SYS_fbmap          200   /* fbmap(fd) - map /dev/fb0, returns address */
#define SIEOS_SYS_netinfo        201   /* netinfo(sieos_netinfo *, int index): ENODEV past the last */
#define SIEOS_SYS_netstat        202   /* netstat(sieos_sockinfo *, max) */
#define SIEOS_SYS_cpuinfo        203   /* cpuinfo(sieos_cpuinfo *, max) - processors filled */
#define SIEOS_SYS_meminfo        204   /* meminfo(sieos_meminfo *) */
#define SIEOS_SYS_procinfo       205   /* procinfo(sieos_procinfo *, max) - processes filled */
#define SIEOS_SYS_netinfo6       206   /* netinfo6(sieos_netinfo6 *, int index) */
#define SIEOS_SYS_netstat6       207   /* netstat6(sieos_sockinfo6 *, max) - IPv4 and IPv6 sockets */
#define SIEOS_SYS_devinfo        208   /* devinfo(sieos_devinfo *, int index): ENODEV past the last */
#define SIEOS_SYS_netconfig      209   /* netconfig(const sieos_netconfig *): an interface's IPv4 settings (root) */
#define SIEOS_SYS_wifi           210   /* wifi(int op, void *buf, long n): the Wi-Fi device (SIEOS_WIFI_OP_*) */
#define SIEOS_SYS_modinfo        211   /* modinfo(sieos_modinfo *, int index): the drivers known; ENOENT past the last */
#define SIEOS_SYS_modload        212   /* modload(const char *path): load a driver (root) */

/* ---- Linux's descriptors and transfers (213-227), their arguments and flags (SIEOS's
 * O_CLOEXEC, O_NONBLOCK, clocks and signals); see sieos/fdext.h ---- */
#define SIEOS_SYS_epoll_create1     213   /* epoll_create1(flags)              EPOLL_CLOEXEC */
#define SIEOS_SYS_epoll_ctl         214   /* epoll_ctl(epfd, op, fd, sieos_epoll_event *) */
#define SIEOS_SYS_epoll_wait        215   /* epoll_wait(epfd, sieos_epoll_event *, max, timeout ms) */
#define SIEOS_SYS_eventfd2          216   /* eventfd2(initval, flags)          EFD_* */
#define SIEOS_SYS_timerfd_create    217   /* timerfd_create(clock, flags) */
#define SIEOS_SYS_timerfd_settime   218   /* timerfd_settime(fd, flags, new, old)  sieos_itimerspec */
#define SIEOS_SYS_timerfd_gettime   219   /* timerfd_gettime(fd, sieos_itimerspec *) */
#define SIEOS_SYS_memfd_create      220   /* memfd_create(name, flags)         MFD_*: an unnamed tmpfs file */
#define SIEOS_SYS_pidfd_open        221   /* pidfd_open(pid, flags): readable when the process ends */
#define SIEOS_SYS_pidfd_send_signal 222   /* pidfd_send_signal(pidfd, sig, NULL, 0) */
#define SIEOS_SYS_splice            223   /* splice(fd_in, int64 *off_in, fd_out, int64 *off_out, len, flags) */
#define SIEOS_SYS_copy_file_range   224   /* copy_file_range(fd_in, *off_in, fd_out, *off_out, len, 0) */
#define SIEOS_SYS_preadv            225   /* preadv(fd, iov, cnt, off) */
#define SIEOS_SYS_pwritev           226   /* pwritev(fd, iov, cnt, off) */
#define SIEOS_SYS_mremap            227   /* mremap(addr, oldlen, newlen, flags, newaddr)  MREMAP_* */

#define SIEOS_NSYSCALLS          256

/* pgrpsys() operations */
#define SIEOS_PGRP_GETPGRP 0
#define SIEOS_PGRP_SETPGRP 1
#define SIEOS_PGRP_GETSID  2
#define SIEOS_PGRP_SETSID  3
#define SIEOS_PGRP_GETPGID 4
#define SIEOS_PGRP_SETPGID 5

/* forkx()/vforkx() flags */
#define SIEOS_FORK_NOSIGCHLD 0x0001
#define SIEOS_FORK_WAITPID   0x0002

/* context() operations */
#define SIEOS_GETCONTEXT 0
#define SIEOS_SETCONTEXT 1

/* sigpending() operations */
#define SIEOS_SIGPENDING 1
#define SIEOS_SIGFILLSET 2

/* fdsync() flags */
#define SIEOS_FSYNC  0x10
#define SIEOS_FDSYNC 0x40

#endif
