/*
 * lxtest.c - liblxcompat's tests, run on SIEOS: the /proc and /sys files,
 * prctl, statfs, SO_PEERCRED, memfd seals, sendmmsg/recvmmsg, sched_getcpu,
 * the futex bitset operations, inotify; and FIONREAD's byte counts.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <linux/magic.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static int fails;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #c, errno, strerror(errno)); fails++; } } while (0)

static char **g_argv;
static int g_argc;

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static ssize_t slurp(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t n = 0;
    for (ssize_t r; n < size - 1 && (r = read(fd, buf + n, size - 1 - n)) > 0;)
        n += r;
    close(fd);
    buf[n] = 0;
    return n;
}

static void *idle_thread(void *a)
{
    (void)a;
    pause();
    return 0;
}

static void procfiles(void)
{
    static char buf[1 << 16];
    T(slurp("/proc/cpuinfo", buf, sizeof buf) > 0);
    int n = 0;
    for (char *p = buf; (p = strstr(p, "processor\t:")); p++)
        n++;
    T(n == sysconf(_SC_NPROCESSORS_ONLN));
    T(strstr(buf, "flags\t\t:") && strstr(buf, " sse2") && strstr(buf, "model name\t:"));

    T(slurp("/proc/meminfo", buf, sizeof buf) > 0);
    unsigned long total = 0, avail = 0;
    char *m = strstr(buf, "MemTotal:"), *a = strstr(buf, "MemAvailable:");
    T(m && a && sscanf(m, "MemTotal: %lu kB", &total) == 1 && sscanf(a, "MemAvailable: %lu kB", &avail) == 1);
    T(total > 100000 && avail > 0 && avail <= total);

    pthread_t th[2];
    pthread_create(&th[0], 0, idle_thread, 0);
    pthread_create(&th[1], 0, idle_thread, 0);
    usleep(20000);
    T(slurp("/proc/self/task/1/status", buf, sizeof buf) > 0 || slurp("/proc/self/stat", buf, sizeof buf) > 0);
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/task/%d/status", getpid(), (int)gettid());
    T(slurp(path, buf, sizeof buf) > 0);
    int threads = 0;
    char *t = strstr(buf, "Threads:");
    T(t && sscanf(t, "Threads: %d", &threads) == 1 && threads == 3);
    T(strstr(buf, "VmRSS:") && strstr(buf, "Name:\tlxtest"));

    T(slurp("/proc/self/stat", buf, sizeof buf) > 0);
    int pid = 0, ppid = 0, nthr = 0;
    char comm[32], st;
    T(sscanf(buf, "%d (%31[^)]) %c %d", &pid, comm, &st, &ppid) == 4 && pid == getpid() && ppid == getppid());
    char *rp = strrchr(buf, ')');
    unsigned long f[20];
    T(rp && sscanf(rp + 2, "%c %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %d", &st, &f[0], &f[1],
                   &f[2], &f[3], &f[4], &f[5], &f[6], &f[7], &f[8], &f[9], &f[10], &f[11], &f[12], &f[13], &f[14],
                   &f[15], &nthr) == 18 && nthr == 3);
    T(slurp("/proc/self/statm", buf, sizeof buf) > 0);
    unsigned long size, res;
    T(sscanf(buf, "%lu %lu", &size, &res) == 2 && size > 0 && res > 0 && res <= size);

    ssize_t cl = slurp("/proc/self/cmdline", buf, sizeof buf);
    size_t want = 0;
    for (int i = 0; i < g_argc; i++)
        want += strlen(g_argv[i]) + 1;
    T(cl == (ssize_t)want && !strcmp(buf, g_argv[0]));
    setenv("LXTEST_LATE", "1", 1);
    ssize_t el = slurp("/proc/self/environ", buf, sizeof buf);
    bool path_seen = false;
    for (char *e = buf; e < buf + el; e += strlen(e) + 1)
        path_seen |= !strncmp(e, "LXTEST_VAR=", 11);
    T(el > 0 && path_seen);

    int fd = open("/proc/self/auxv", O_RDONLY);
    T(fd >= 0);
    Elf64_auxv_t av;
    long pagesz = 0;
    while (read(fd, &av, sizeof av) == sizeof av && av.a_type != AT_NULL)
        if (av.a_type == AT_PAGESZ)
            pagesz = av.a_un.a_val;
    close(fd);
    T(pagesz == 4096);

    T(slurp("/proc/self/maps", buf, sizeof buf) > 0);
    T(strstr(buf, "libc.so") || strstr(buf, "ld-musl"));
    T(strstr(buf, "[stack]") && strstr(buf, "r-xp"));

    char exe[PATH_MAX] = { 0 }, real[PATH_MAX];
    T(readlink("/proc/self/exe", exe, sizeof exe - 1) > 0 && realpath(g_argv[0], real) && !strcmp(exe, real));
    struct stat sb;
    T(lstat("/proc/self/exe", &sb) == 0 && S_ISLNK(sb.st_mode));
    T(stat("/proc/self/exe", &sb) == 0 && S_ISREG(sb.st_mode));

    DIR *d = opendir("/proc/self/task");
    T(d != NULL);
    int seen = 0, me = 0;
    for (struct dirent *e; d && (e = readdir(d));) {
        seen++;
        me |= atoi(e->d_name) == gettid();
    }
    if (d)
        closedir(d);
    T(seen == 3 && me);

    T(slurp("/sys/devices/system/cpu/online", buf, sizeof buf) > 0 && buf[0] == '0');
    T(slurp("/sys/devices/system/cpu/possible", buf, sizeof buf) > 0);
    d = opendir("/sys/devices/system/cpu");
    int cpus = 0;
    for (struct dirent *e; d && (e = readdir(d));)
        cpus += !strncmp(e->d_name, "cpu", 3) && e->d_name[3] >= '0' && e->d_name[3] <= '9';
    if (d)
        closedir(d);
    T(cpus == sysconf(_SC_NPROCESSORS_CONF));

    FILE *fp = fopen("/proc/cpuinfo", "re");
    T(fp && fgets(buf, sizeof buf, fp) && !strncmp(buf, "processor", 9));
    if (fp)
        fclose(fp);
    T(chdir("/proc") == 0);
    T(slurp("self/comm", buf, sizeof buf) > 0 && !strcmp(buf, "lxtest\n"));
    T(chdir("/") == 0);
    T(slurp("/proc/./self/../self/comm", buf, sizeof buf) > 0 && !strcmp(buf, "lxtest\n"));
    T(stat("/proc/cpuinfo", &sb) == 0 && S_ISREG(sb.st_mode));
    T(access("/proc/meminfo", R_OK) == 0);
    errno = 0;
    T(access("/proc/meminfo", W_OK) < 0 && errno == EACCES);
    errno = 0;
    T(open("/proc/cpuinfo", O_WRONLY) < 0 && errno == EACCES);
    /* SIEOS's own /proc still works */
    T(access("/proc/self/psinfo", R_OK) == 0);
    snprintf(path, sizeof path, "/proc/%d/fd/0", getpid());
    T(readlink(path, buf, 100) > 0);

    char b1[64], b2[64], u1[64], u2[64];
    T(slurp("/proc/sys/kernel/random/boot_id", b1, sizeof b1) == 37 &&
      slurp("/proc/sys/kernel/random/boot_id", b2, sizeof b2) == 37 && !strcmp(b1, b2));
    T(slurp("/proc/sys/kernel/random/uuid", u1, sizeof u1) == 37 &&
      slurp("/proc/sys/kernel/random/uuid", u2, sizeof u2) == 37 && strcmp(u1, u2));
    T(slurp("/proc/uptime", buf, sizeof buf) > 0 && atof(buf) > 0);
    T(slurp("/proc/stat", buf, sizeof buf) > 0 && !strncmp(buf, "cpu ", 4) && strstr(buf, "btime "));
    errno = 0;
    T(open("/proc/self/nonexistent", O_RDONLY) < 0 && errno == ENOENT);
}

static void prctls(void)
{
    char name[16] = { 0 };
    T(prctl(PR_SET_NAME, "lx-renamed", 0, 0, 0) == 0);
    T(prctl(PR_GET_NAME, name, 0, 0, 0) == 0 && !strcmp(name, "lx-renamed"));
    T(prctl(PR_SET_NAME, "lxtest", 0, 0, 0) == 0);
    T(prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == 0 && prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 0);
    T(prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == 0);
    T(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 && prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1);
    T(prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, 0, 0, 0) == 0);
    errno = 0;
    T(prctl(12345, 0, 0, 0, 0) < 0 && errno == EINVAL);

    /* PR_SET_PDEATHSIG: a grandchild dies with its parent */
    int p[2];
    T(pipe(p) == 0);
    pid_t mid = fork();
    if (mid == 0) {
        pid_t gc = fork();
        if (gc == 0) {
            close(p[0]);
            prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
            for (;;)
                pause();
        }
        usleep(100000);
        _exit(0);
    }
    close(p[1]);
    waitpid(mid, 0, 0);
    struct pollfd pf = { p[0], POLLIN, 0 };
    long t0 = now_ms();
    T(poll(&pf, 1, 3000) == 1 && now_ms() - t0 < 3000);
    char c;
    T(read(p[0], &c, 1) == 0);                   /* EOF: the grandchild is gone */
    close(p[0]);
}

static void statfss(void)
{
    struct statfs s;
    T(statfs("/", &s) == 0 && s.f_type == EXT4_SUPER_MAGIC && s.f_bsize > 0 && s.f_blocks > 0);
    T(statfs("/tmp", &s) == 0 && s.f_type == TMPFS_MAGIC);
    T(statfs("/proc", &s) == 0 && s.f_type == PROC_SUPER_MAGIC);
    int fd = open("/", O_RDONLY);
    T(fstatfs(fd, &s) == 0 && s.f_type == EXT4_SUPER_MAGIC);
    close(fd);
    errno = 0;
    T(statfs("/nonexistent", &s) < 0 && errno == ENOENT);
}

static void sockets(void)
{
    int sv[2];
    T(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct ucred c;
    socklen_t len = sizeof c;
    T(getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &c, &len) == 0 && len == sizeof c && c.pid == getpid() &&
      c.uid == geteuid() && c.gid == getegid());
    int one = 1;
    T(setsockopt(sv[0], SOL_SOCKET, SO_PASSCRED, &one, sizeof one) == 0);
    /* FIONREAD: the bytes waiting */
    T(write(sv[1], "hello world", 11) == 11);
    usleep(10000);
    int n = 0;
    T(ioctl(sv[0], FIONREAD, &n) == 0 && n == 11);
    close(sv[0]);
    close(sv[1]);
    int pp[2];
    T(pipe(pp) == 0 && write(pp[1], "abcdefg", 7) == 7);
    T(ioctl(pp[0], FIONREAD, &n) == 0 && n == 7);
    close(pp[0]);
    close(pp[1]);

    T(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
    struct mmsghdr out[3], in[3];
    struct iovec oi[3], ii[3];
    char obuf[3][8] = { "one", "two", "three" }, ibuf[3][16];
    memset(out, 0, sizeof out);
    memset(in, 0, sizeof in);
    for (int i = 0; i < 3; i++) {
        oi[i] = (struct iovec){ obuf[i], strlen(obuf[i]) };
        out[i].msg_hdr.msg_iov = &oi[i];
        out[i].msg_hdr.msg_iovlen = 1;
        ii[i] = (struct iovec){ ibuf[i], sizeof ibuf[i] };
        in[i].msg_hdr.msg_iov = &ii[i];
        in[i].msg_hdr.msg_iovlen = 1;
    }
    T(sendmmsg(sv[0], out, 3, 0) == 3 && out[2].msg_len == 5);
    T(ioctl(sv[1], FIONREAD, &n) == 0 && n == 3);           /* the next datagram's */
    T(recvmmsg(sv[1], in, 3, 0, NULL) == 3 && in[1].msg_len == 3 && !memcmp(ibuf[2], "three", 5));
    T(sendmmsg(sv[0], out, 1, 0) == 1);
    T(recvmmsg(sv[1], in, 3, MSG_WAITFORONE, NULL) == 1);
    close(sv[0]);
    close(sv[1]);
}

static void memfds(void)
{
    int fd = memfd_create("lx", MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL);
    T(fd >= 0);
    T(ftruncate(fd, 4096) == 0);
    T(fcntl(fd, F_GET_SEALS) == 0);
    T(fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) == 0);
    T(fcntl(fd, F_GET_SEALS) == (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE));
    T(fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL) == 0);
    errno = 0;
    T(fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) < 0 && errno == EPERM);
    T(fcntl(fd, F_GETFD) == FD_CLOEXEC);
    close(fd);
    int fd2 = memfd_create("lx2", MFD_ALLOW_SEALING);
    T(fd2 >= 0 && fcntl(fd2, F_GET_SEALS) == 0);
    close(fd2);
}

static int fword;
static void *futex_waker(void *a)
{
    (void)a;
    usleep(50000);
    __atomic_store_n(&fword, 1, __ATOMIC_SEQ_CST);
    syscall(SYS_futex, &fword, FUTEX_WAKE_BITSET_PRIVATE, 1, NULL, NULL, FUTEX_BITSET_MATCH_ANY);
    return 0;
}

static void futexes(void)
{
    T(sched_getcpu() >= 0 && sched_getcpu() < sysconf(_SC_NPROCESSORS_CONF));
    struct timespec at;
    clock_gettime(CLOCK_MONOTONIC, &at);
    at.tv_nsec += 50000000;
    if (at.tv_nsec >= 1000000000) {
        at.tv_sec++;
        at.tv_nsec -= 1000000000;
    }
    long t0 = now_ms();
    errno = 0;
    T(syscall(SYS_futex, &fword, FUTEX_WAIT_BITSET_PRIVATE, 0, &at, NULL, FUTEX_BITSET_MATCH_ANY) < 0 &&
      errno == ETIMEDOUT);
    T(now_ms() - t0 >= 40);
    clock_gettime(CLOCK_REALTIME, &at);
    at.tv_sec += 5;
    pthread_t th;
    pthread_create(&th, 0, futex_waker, 0);
    t0 = now_ms();
    while (!__atomic_load_n(&fword, __ATOMIC_SEQ_CST))
        syscall(SYS_futex, &fword, FUTEX_WAIT_BITSET_PRIVATE | FUTEX_CLOCK_REALTIME, 0, &at, NULL,
                FUTEX_BITSET_MATCH_ANY);
    T(now_ms() - t0 < 4000);
    pthread_join(th, 0);
    errno = 0;
    T(syscall(SYS_futex, &fword, FUTEX_WAIT_BITSET_PRIVATE, 1, &at, NULL, 1) < 0 && errno == EINVAL);
    T(syscall(SYS_getpid) == getpid());
}

/* inotify: the next event, waited for at most ms */
static int next_event(int fd, struct inotify_event *ev, char *name, int ms)
{
    struct pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, ms) != 1)
        return -1;
    int avail = 0;
    if (ioctl(fd, FIONREAD, &avail) < 0 || avail < (int)sizeof *ev)
        return -1;
    if (read(fd, ev, sizeof *ev) != sizeof *ev)
        return -1;
    name[0] = 0;
    if (ev->len && read(fd, name, ev->len) != (ssize_t)ev->len)
        return -1;
    return 0;
}

/* Wait for an event of mask (with name, if given) on wd: the others are skipped. */
static int expect(int fd, int wd, uint32_t mask, const char *name, uint32_t *cookie)
{
    struct inotify_event ev;
    char nm[NAME_MAX + 16];
    long t0 = now_ms();
    while (now_ms() - t0 < 3000) {
        if (next_event(fd, &ev, nm, 3000) < 0)
            return 0;
        if (ev.wd == wd && (ev.mask & mask) && (!name || !strcmp(nm, name))) {
            if (cookie)
                *cookie = ev.cookie;
            return 1;
        }
    }
    return 0;
}

static void inotifies(void)
{
    const char *dir = "/tmp/lxino";
    mkdir(dir, 0755);
    int fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    T(fd >= 0);
    T(fcntl(fd, F_GETFD) == FD_CLOEXEC && (fcntl(fd, F_GETFL) & O_NONBLOCK));
    char c;
    errno = 0;
    T(read(fd, &c, 1) < 0 && errno == EAGAIN);
    int wd = inotify_add_watch(fd, dir, IN_CREATE | IN_DELETE | IN_MOVE | IN_MODIFY | IN_ATTRIB | IN_ONLYDIR);
    T(wd > 0);
    T(inotify_add_watch(fd, dir, IN_CREATE | IN_MASK_ADD) == wd);
    errno = 0;
    T(inotify_add_watch(fd, "/tmp/lxino-none", IN_CREATE) < 0 && errno == ENOENT);

    int f = open("/tmp/lxino/a", O_CREAT | O_WRONLY, 0644);
    T(f >= 0);
    T(expect(fd, wd, IN_CREATE, "a", 0));
    T(write(f, "data", 4) == 4);
    T(expect(fd, wd, IN_MODIFY, "a", 0));
    close(f);
    T(chmod("/tmp/lxino/a", 0600) == 0);
    T(expect(fd, wd, IN_ATTRIB, "a", 0));
    T(rename("/tmp/lxino/a", "/tmp/lxino/b") == 0);
    uint32_t c1 = 0, c2 = 1;
    T(expect(fd, wd, IN_MOVED_FROM, "a", &c1));
    T(expect(fd, wd, IN_MOVED_TO, "b", &c2));
    T(c1 == c2 && c1 != 0);
    T(mkdir("/tmp/lxino/sub", 0755) == 0);
    struct inotify_event ev;
    char nm[NAME_MAX + 16];
    T(next_event(fd, &ev, nm, 3000) == 0 && ev.wd == wd && (ev.mask & IN_CREATE) && (ev.mask & IN_ISDIR) &&
      !strcmp(nm, "sub"));
    T(unlink("/tmp/lxino/b") == 0);
    T(expect(fd, wd, IN_DELETE, "b", 0));

    /* a file's own watch */
    f = open("/tmp/lxino/self", O_CREAT | O_WRONLY, 0644);
    int fw = inotify_add_watch(fd, "/tmp/lxino/self", IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF);
    T(fw > 0 && fw != wd);
    errno = 0;
    T(inotify_add_watch(fd, "/tmp/lxino/self", IN_MODIFY | IN_ONLYDIR) < 0 && errno == ENOTDIR);
    T(write(f, "x", 1) == 1);
    T(expect(fd, fw, IN_MODIFY, NULL, 0));
    T(write(f, "y", 1) == 1);
    T(expect(fd, fw, IN_MODIFY, NULL, 0));       /* (associated again) */
    close(f);
    T(unlink("/tmp/lxino/self") == 0);
    T(expect(fd, fw, IN_DELETE_SELF, NULL, 0));
    T(expect(fd, fw, IN_IGNORED, NULL, 0));
    errno = 0;
    T(inotify_rm_watch(fd, fw) < 0 && errno == EINVAL);

    T(inotify_rm_watch(fd, wd) == 0);
    T(expect(fd, wd, IN_IGNORED, NULL, 0));
    rmdir("/tmp/lxino/sub");
    T(inotify_add_watch(-1, dir, IN_CREATE) < 0 && errno == EBADF);
    close(fd);
    /* many instances come and go: each one's thread ends */
    for (int i = 0; i < 50; i++) {
        int k = inotify_init();
        T(k >= 0 && inotify_add_watch(k, dir, IN_CREATE) > 0);
        close(k);
    }
    usleep(200000);
    char path[64], buf[4096];
    snprintf(path, sizeof path, "/proc/%d/task/%d/status", getpid(), (int)gettid());
    slurp(path, buf, sizeof buf);
    int threads = 0;
    char *t = strstr(buf, "Threads:");
    T(t && sscanf(t, "Threads: %d", &threads) == 1 && threads <= 3);
    rmdir(dir);
}

int main(int argc, char **argv)
{
    g_argc = argc;
    g_argv = argv;
    setenv("LXTEST_VAR", "1", 1);
    if (!getenv("LXTEST_REEXEC")) {              /* (so that LXTEST_VAR is in the initial environment) */
        setenv("LXTEST_REEXEC", "1", 1);
        execv("/proc/self/exe", argv);
        execv(argv[0], argv);
        perror("exec");
        return 1;
    }
    procfiles();
    prctls();
    statfss();
    sockets();
    memfds();
    futexes();
    inotifies();
    printf("lxtest: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
