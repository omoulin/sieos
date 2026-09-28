/*
 * m12.c - milestone 12 kernel features: priority-inheritance mutexes,
 * mount/umount2, RLIMIT_VMEM, RLIMIT_CORE and core files, sub-tick
 * nanosleep, and truncating block-mapped ext4 files (when /opt/bm holds
 * big.dat and its copy ref.dat on a block-mapped file system).  Run as root.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>

static int fails;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #c, errno, strerror(errno)); fails++; } } while (0)

/* ---------------- priority inheritance ---------------- */

static pthread_mutex_t pim;
static int counter;

static void *pi_worker(void *arg)
{
    for (int i = 0; i < 2000; i++) {
        pthread_mutex_lock(&pim);
        counter++;
        pthread_mutex_unlock(&pim);
    }
    return arg;
}

static void *pi_die(void *arg)
{
    pthread_mutex_lock((pthread_mutex_t *)arg);
    return 0;                                         /* exits holding it */
}

static void pi(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    T(pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT) == 0);
    T(pthread_mutex_init(&pim, &a) == 0);
    pthread_t t[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], 0, pi_worker, 0);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], 0);
    T(counter == 8000);
    T(pthread_mutex_trylock(&pim) == 0);
    T(pthread_mutex_unlock(&pim) == 0);
    /* robust + PI: the owner dies */
    pthread_mutex_t rm;
    pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST);
    T(pthread_mutex_init(&rm, &a) == 0);
    pthread_t d;
    pthread_create(&d, 0, pi_die, &rm);
    pthread_join(d, 0);
    T(pthread_mutex_lock(&rm) == EOWNERDEAD);
    T(pthread_mutex_consistent(&rm) == 0);
    T(pthread_mutex_unlock(&rm) == 0);
    T(pthread_mutex_lock(&rm) == 0 && pthread_mutex_unlock(&rm) == 0);
}

/* ---------------- mount ---------------- */

static int in_mnttab(const char *dir)
{
    FILE *f = fopen("/etc/mnttab", "r");
    char line[256];
    int found = 0;
    while (f && fgets(line, sizeof line, f)) {
        char spec[64], mp[128];
        if (sscanf(line, "%63s %127s", spec, mp) == 2 && !strcmp(mp, dir))
            found = 1;
    }
    if (f)
        fclose(f);
    return found;
}

static void mounts(void)
{
    const char *m = "/tmp/m12mnt", *p = "/tmp/m12proc";
    mkdir(m, 0755);
    mkdir(p, 0755);
    T(mount("swap", m, "tmpfs", 0, 0) == 0);
    struct statvfs sv;
    T(statvfs(m, &sv) == 0);
    T(in_mnttab(m));
    errno = 0;
    T(mount("swap", m, "tmpfs", 0, 0) < 0 && errno == EBUSY);   /* already a mount point */
    int fd = open("/tmp/m12mnt/f", O_CREAT | O_WRONLY, 0644);
    T(fd >= 0 && write(fd, "data", 4) == 4);
    close(fd);
    T(chdir(m) == 0);
    errno = 0;
    T(umount(m) < 0 && errno == EBUSY);                          /* our working directory */
    T(chdir("/") == 0);
    T(mount("swap", m, "tmpfs", MS_REMOUNT | MS_RDONLY, 0) == 0);
    errno = 0;
    T(open("/tmp/m12mnt/g", O_CREAT | O_WRONLY, 0644) < 0 && errno == EROFS);
    T(mount("swap", m, "tmpfs", MS_REMOUNT, 0) == 0);
    T(umount(m) == 0);
    T(!in_mnttab(m));
    T(access("/tmp/m12mnt/f", F_OK) < 0);                        /* the files went with it */
    T(mount("proc", p, "proc", 0, 0) == 0);
    T(access("/tmp/m12proc/self/psinfo", R_OK) == 0);
    T(umount2(p, 0) == 0);
    errno = 0;
    T(umount(p) < 0 && errno == EINVAL);                         /* not a mount point */
    errno = 0;
    T(mount("x", m, "nosuchfs", 0, 0) < 0 && errno == ENODEV);
    pid_t pid = fork();
    if (pid == 0) {
        setuid(100);
        _exit(mount("swap", m, "tmpfs", 0, 0) < 0 && errno == EPERM ? 0 : 1);
    }
    int st;
    T(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    rmdir(m);
    rmdir(p);
}

/* ---------------- rlimits ---------------- */

static void vmem(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        struct rlimit rl = { 64 << 20, 64 << 20 };
        if (setrlimit(RLIMIT_AS, &rl) < 0)
            _exit(2);
        void *a = mmap(0, 32 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        void *b = mmap(0, 48 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        int enomem = b == MAP_FAILED && errno == ENOMEM;
        void *volatile c = malloc(100 << 20);           /* volatile: GCC may assume malloc succeeds */
        _exit(a == MAP_FAILED ? 3 : !enomem ? 4 : c ? 5 : 0);
    }
    int st;
    T(waitpid(pid, &st, 0) == pid && WIFEXITED(st));
    if (WEXITSTATUS(st))
        printf("  RLIMIT_VMEM child: step %d failed\n", WEXITSTATUS(st));
    T(WEXITSTATUS(st) == 0);
}

static int crash(const char *dir, rlim_t lim)
{
    pid_t pid = fork();
    if (pid == 0) {
        struct rlimit rl = { lim, RLIM_INFINITY };
        setrlimit(RLIMIT_CORE, &rl);
        chdir(dir);
        signal(SIGABRT, SIG_DFL);
        abort();
    }
    int st;
    waitpid(pid, &st, 0);
    return st;
}

static void core(void)
{
    const char *d = "/tmp/m12core";
    mkdir(d, 0755);
    unlink("/tmp/m12core/core");
    int st = crash(d, 0);
    T(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT && !WCOREDUMP(st));
    T(access("/tmp/m12core/core", F_OK) < 0);
    st = crash(d, RLIM_INFINITY);
    T(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT && WCOREDUMP(st));
    struct stat s;
    T(stat("/tmp/m12core/core", &s) == 0 && s.st_size > 8192);
    unsigned char h[64] = { 0 };
    int fd = open("/tmp/m12core/core", O_RDONLY);
    T(fd >= 0 && read(fd, h, 64) == 64);
    close(fd);
    T(!memcmp(h, "\177ELF", 4) && h[16] == 4 && h[18] == 62);     /* ET_CORE, EM_X86_64 */
    st = crash(d, 8192);
    T(WCOREDUMP(st) && stat("/tmp/m12core/core", &s) == 0 && s.st_size == 8192);   /* cut at the limit */
    unlink("/tmp/m12core/core");
    rmdir(d);
}

/* ---------------- nanosleep ---------------- */

static long now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void sleeps(void)
{
    long worst = 0, total = 0, shortest = 1L << 40;
    for (int i = 0; i < 20; i++) {
        long t0 = now_us();
        struct timespec q = { 0, 1000000 };                        /* 1 ms */
        nanosleep(&q, 0);
        long d = now_us() - t0;
        total += d;
        if (d > worst) worst = d;
        if (d < shortest) shortest = d;
    }
    printf("nanosleep 1 ms: shortest %ld us, average %ld us, longest %ld us\n", shortest, total / 20, worst);
    T(shortest >= 1000);                                          /* never less than asked */
    T(total / 20 < 3000);                                         /* not a 10 ms tick */
}

/* ---------------- block-mapped truncate ---------------- */

static int same_prefix(const char *a, const char *b, off_t n)
{
    int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY);
    static char x[65536], y[65536];
    int ok = fa >= 0 && fb >= 0;
    for (off_t done = 0; ok && done < n;) {
        size_t k = n - done < (off_t)sizeof x ? (size_t)(n - done) : sizeof x;
        ok = read(fa, x, k) == (ssize_t)k && read(fb, y, k) == (ssize_t)k && !memcmp(x, y, k);
        done += k;
    }
    char c;
    ok = ok && read(fa, &c, 1) == 0;                              /* and nothing more */
    close(fa);
    close(fb);
    return ok;
}

static void blockmapped(void)
{
    const char *f = "/opt/bm/big.dat", *r = "/opt/bm/ref.dat";
    struct stat s0, s;
    if (stat(f, &s0) < 0) {
        printf("block-mapped truncate: no %s, skipped\n", f);
        return;
    }
    off_t sizes[] = { 9000000, 5000000, 4100000, 40000, 100 };
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        T(truncate(f, sizes[i]) == 0);
        T(stat(f, &s) == 0 && s.st_size == sizes[i] && s.st_blocks < s0.st_blocks);
        T(same_prefix(f, r, sizes[i]));
    }
    T(truncate(f, 0) == 0 && stat(f, &s) == 0 && s.st_blocks == 0);
}

int main(void)
{
    pi();
    mounts();
    vmem();
    core();
    sleeps();
    blockmapped();
    sync();
    printf("m12: %s (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
