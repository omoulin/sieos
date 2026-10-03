/*
 * m78.c - event ports (descriptors, files, user events, timers, alerts),
 * POSIX timers (signals, threads, ports), ucred_get and getpeerucred,
 * arc4random, backtrace, the clock waits.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <poll.h>
#include <port.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <ucred.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/procset.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int fails;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #c, errno, strerror(errno)); fails++; } } while (0)

static struct timespec ms(long n)
{
    struct timespec t = { n / 1000, (n % 1000) * 1000000L };
    return t;
}

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void ports_fd(void)
{
    int port = port_create(), p[2];
    T(port >= 0);
    T(pipe(p) == 0);
    T(port_associate(port, PORT_SOURCE_FD, p[0], POLLIN, (void *)0x1234) == 0);
    port_event_t pe;
    struct timespec z = ms(0);
    errno = 0;
    T(port_get(port, &pe, &z) < 0 && errno == ETIME);
    T(write(p[1], "x", 1) == 1);
    struct timespec t = ms(1000);
    T(port_get(port, &pe, &t) == 0 && pe.portev_source == PORT_SOURCE_FD && pe.portev_object == (uintptr_t)p[0]
      && (pe.portev_events & POLLIN) && pe.portev_user == (void *)0x1234);
    /* reported once: dissociated */
    errno = 0;
    T(port_get(port, &pe, &z) < 0 && errno == ETIME);
    T(port_associate(port, PORT_SOURCE_FD, p[0], POLLIN, 0) == 0);
    T(port_dissociate(port, PORT_SOURCE_FD, p[0]) == 0);
    errno = 0;
    T(port_get(port, &pe, &z) < 0 && errno == ETIME);
    /* user events, getn */
    T(port_send(port, 7, (void *)1) == 0);
    T(port_send(port, 8, (void *)2) == 0);
    port_event_t v[4];
    unsigned n = 2;
    T(port_getn(port, v, 4, &n, &t) == 0 && n == 2 && v[0].portev_source == PORT_SOURCE_USER
      && v[0].portev_events == 7 && v[1].portev_events == 8);
    /* alert */
    T(port_alert(port, PORT_ALERT_SET, 99, (void *)3) == 0);
    T(port_get(port, &pe, &z) == 0 && pe.portev_source == PORT_SOURCE_ALERT && pe.portev_events == 99);
    T(port_get(port, &pe, &z) == 0 && pe.portev_source == PORT_SOURCE_ALERT);
    T(port_alert(port, PORT_ALERT_SET, 0, 0) == 0);
    errno = 0;
    T(port_get(port, &pe, &z) < 0 && errno == ETIME);
    close(p[0]);
    close(p[1]);
    close(port);
}

static int sendport;
static void *sender(void *a)
{
    (void)a;
    usleep(50000);
    port_send(sendport, 42, 0);
    return 0;
}

static void ports_wake(void)
{
    sendport = port_create();
    pthread_t th;
    pthread_create(&th, 0, sender, 0);
    port_event_t pe;
    long t0 = now_ms();
    T(port_get(sendport, &pe, 0) == 0 && pe.portev_events == 42);
    T(now_ms() - t0 >= 40);
    pthread_join(th, 0);
    close(sendport);
}

static void fill_obj(struct file_obj *fo, const char *path)
{
    struct stat st;
    memset(fo, 0, sizeof *fo);
    stat(path, &st);
    fo->fo_atime = st.st_atim;
    fo->fo_mtime = st.st_mtim;
    fo->fo_ctime = st.st_ctim;
    fo->fo_name = (char *)path;
}

static void ports_file(void)
{
    const char *f = "/tmp/m78.file", *d = "/tmp/m78.dir";
    unlink(f);
    mkdir(d, 0755);
    int fd = open(f, O_CREAT | O_RDWR | O_TRUNC, 0644);
    T(fd >= 0);
    int port = port_create();
    struct file_obj fo, dobj;
    fill_obj(&fo, f);
    T(port_associate(port, PORT_SOURCE_FILE, (uintptr_t)&fo, FILE_MODIFIED | FILE_ATTRIB, (void *)5) == 0);
    port_event_t pe;
    struct timespec z = ms(0), t = ms(1000);
    errno = 0;
    T(port_get(port, &pe, &z) < 0 && errno == ETIME);
    T(write(fd, "hello", 5) == 5);
    T(port_get(port, &pe, &t) == 0 && pe.portev_source == PORT_SOURCE_FILE && (pe.portev_events & FILE_MODIFIED)
      && pe.portev_object == (uintptr_t)&fo && pe.portev_user == (void *)5);
    /* attributes */
    fill_obj(&fo, f);
    T(port_associate(port, PORT_SOURCE_FILE, (uintptr_t)&fo, FILE_ATTRIB, 0) == 0);
    T(fchmod(fd, 0600) == 0);
    T(port_get(port, &pe, &t) == 0 && (pe.portev_events & FILE_ATTRIB));
    /* stale times: at once */
    struct file_obj old = fo;
    old.fo_mtime.tv_sec -= 100;
    T(port_associate(port, PORT_SOURCE_FILE, (uintptr_t)&old, FILE_MODIFIED, 0) == 0);
    T(port_get(port, &pe, &t) == 0 && (pe.portev_events & FILE_MODIFIED));
    /* a directory: a new name modifies it */
    fill_obj(&dobj, d);
    T(port_associate(port, PORT_SOURCE_FILE, (uintptr_t)&dobj, FILE_MODIFIED, 0) == 0);
    int g = open("/tmp/m78.dir/x", O_CREAT | O_WRONLY, 0644);
    close(g);
    T(port_get(port, &pe, &t) == 0 && pe.portev_object == (uintptr_t)&dobj && (pe.portev_events & FILE_MODIFIED));
    /* deletion: an exception, whatever was asked */
    fill_obj(&fo, f);
    T(port_associate(port, PORT_SOURCE_FILE, (uintptr_t)&fo, FILE_ACCESS, 0) == 0);
    T(unlink(f) == 0);
    T(port_get(port, &pe, &t) == 0 && (pe.portev_events & FILE_DELETE));
    /* rename */
    fill_obj(&fo, "/tmp/m78.dir/x");
    T(port_associate(port, PORT_SOURCE_FILE, (uintptr_t)&fo, 0, 0) == 0);
    T(rename("/tmp/m78.dir/x", "/tmp/m78.dir/y") == 0);
    T(port_get(port, &pe, &t) == 0 && (pe.portev_events & FILE_RENAME_FROM));
    unlink("/tmp/m78.dir/y");
    rmdir(d);
    close(fd);
    close(port);
}

static volatile int sigs, sig_timerid, sig_val;
static void on_alrm(int s, siginfo_t *si, void *uc)
{
    (void)s; (void)uc;
    sigs++;
    sig_val = si->si_value.sival_int;
    sig_timerid = si->si_code == SI_TIMER;
}

static volatile int thr_calls, thr_val;
static void on_thread(union sigval v)
{
    thr_val = v.sival_int;
    __atomic_add_fetch(&thr_calls, 1, __ATOMIC_SEQ_CST);
}

static void timers(void)
{
    struct sigaction sa = { 0 };
    sa.sa_sigaction = on_alrm;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, 0);
    struct sigevent ev = { 0 };
    ev.sigev_notify = SIGEV_SIGNAL;
    ev.sigev_signo = SIGUSR1;
    ev.sigev_value.sival_int = 77;
    timer_t t;
    T(timer_create(CLOCK_MONOTONIC, &ev, &t) == 0);
    struct itimerspec its = { ms(20), ms(20) }, cur;
    T(timer_settime(t, 0, &its, 0) == 0);
    T(timer_gettime(t, &cur) == 0 && cur.it_interval.tv_nsec == 20000000 && (cur.it_value.tv_sec || cur.it_value.tv_nsec));
    long t0 = now_ms();
    while (sigs < 5 && now_ms() - t0 < 2000)
        usleep(5000);
    T(sigs >= 5 && sig_val == 77 && sig_timerid);
    T(timer_getoverrun(t) >= 0);
    T(timer_delete(t) == 0);
    int s0 = sigs;
    usleep(60000);
    T(sigs == s0);
    errno = 0;
    T(timer_settime(t, 0, &its, 0) < 0 && errno == EINVAL);

    /* one-shot, absolute */
    T(timer_create(CLOCK_REALTIME, &ev, &t) == 0);
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct itimerspec abs = { { 0, 0 }, now };
    abs.it_value.tv_nsec += 30000000;
    if (abs.it_value.tv_nsec >= 1000000000) {
        abs.it_value.tv_sec++;
        abs.it_value.tv_nsec -= 1000000000;
    }
    s0 = sigs;
    T(timer_settime(t, TIMER_ABSTIME, &abs, 0) == 0);
    usleep(150000);
    T(sigs == s0 + 1);
    T(timer_delete(t) == 0);

    /* SIGEV_THREAD */
    struct sigevent tev = { 0 };
    tev.sigev_notify = SIGEV_THREAD;
    tev.sigev_notify_function = on_thread;
    tev.sigev_value.sival_int = 31;
    T(timer_create(CLOCK_MONOTONIC, &tev, &t) == 0);
    T(timer_settime(t, 0, &its, 0) == 0);
    t0 = now_ms();
    while (thr_calls < 3 && now_ms() - t0 < 2000)
        usleep(5000);
    T(thr_calls >= 3 && thr_val == 31);
    T(timer_delete(t) == 0);

    /* SIGEV_PORT */
    int port = port_create();
    port_notify_t pn = { port, (void *)0x55 };
    struct sigevent pev = { 0 };
    pev.sigev_notify = SIGEV_PORT;
    pev.sigev_value.sival_ptr = &pn;
    T(timer_create(CLOCK_MONOTONIC, &pev, &t) == 0);
    struct itimerspec one = { { 0, 0 }, ms(20) };
    T(timer_settime(t, 0, &one, 0) == 0);
    port_event_t pe;
    struct timespec w = ms(1000);
    T(port_get(port, &pe, &w) == 0 && pe.portev_source == PORT_SOURCE_TIMER && pe.portev_user == (void *)0x55
      && pe.portev_events >= 1);
    T(timer_delete(t) == 0);
    close(port);

    /* the timers go at exec and exit; a child has none */
    T(timer_create(CLOCK_MONOTONIC, &ev, &t) == 0);
    pid_t c = fork();
    if (c == 0) {
        errno = 0;
        _exit(timer_settime(t, 0, &its, 0) < 0 && errno == EINVAL ? 0 : 1);
    }
    int st;
    waitpid(c, &st, 0);
    T(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    T(timer_delete(t) == 0);
}

static void ucreds(void)
{
    ucred_t *uc = ucred_get(P_MYID);
    T(uc && ucred_getpid(uc) == getpid() && ucred_geteuid(uc) == geteuid() && ucred_getrgid(uc) == getgid());
    const gid_t *g;
    T(uc && ucred_getgroups(uc, &g) >= 0);
    ucred_free(uc);
    errno = 0;
    T(!ucred_get(999999) && errno == ESRCH);
    int sv[2];
    T(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    ucred_t *pc = 0;
    T(getpeerucred(sv[0], &pc) == 0 && ucred_getpid(pc) == getpid());
    ucred_free(pc);
    pc = 0;
    int p[2];
    pipe(p);
    errno = 0;
    T(getpeerucred(p[0], &pc) < 0 && errno == ENOTSOCK);
    close(sv[0]);
    close(sv[1]);
    close(p[0]);
    close(p[1]);
}

static void randoms(void)
{
    unsigned char a[64], b[64];
    arc4random_buf(a, sizeof a);
    arc4random_buf(b, sizeof b);
    T(memcmp(a, b, sizeof a));
    unsigned char big[2000];
    arc4random_buf(big, sizeof big);
    int seen[10] = { 0 };
    for (int i = 0; i < 1000; i++) {
        uint32_t v = arc4random_uniform(10);
        T(v < 10);
        seen[v]++;
    }
    for (int i = 0; i < 10; i++)
        T(seen[i] > 30);
    T(arc4random_uniform(1) == 0);
}

__attribute__((noinline)) static int bt_inner(void **pcs)
{
    return backtrace(pcs, 32);
}

static void backtraces(void)
{
    void *pcs[32];
    int n = bt_inner(pcs);
    T(n >= 2);
    char **s = backtrace_symbols(pcs, n);
    T(s && s[0][0]);
    free(s);
    char buf[256];
    T(addrtosymstr((void *)backtraces, buf, sizeof buf) > 0);
}

static void clockwaits(void)
{
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t c = PTHREAD_COND_INITIALIZER;
    struct timespec at;
    clock_gettime(CLOCK_MONOTONIC, &at);
    at.tv_nsec += 50000000;
    if (at.tv_nsec >= 1000000000) {
        at.tv_sec++;
        at.tv_nsec -= 1000000000;
    }
    long t0 = now_ms();
    pthread_mutex_lock(&m);
    T(pthread_cond_clockwait(&c, &m, CLOCK_MONOTONIC, &at) == ETIMEDOUT);
    pthread_mutex_unlock(&m);
    long d = now_ms() - t0;
    T(d >= 40 && d < 1000);
    sem_t s;
    sem_init(&s, 0, 0);
    clock_gettime(CLOCK_MONOTONIC, &at);
    at.tv_nsec += 30000000;
    if (at.tv_nsec >= 1000000000) {
        at.tv_sec++;
        at.tv_nsec -= 1000000000;
    }
    errno = 0;
    T(sem_clockwait(&s, CLOCK_MONOTONIC, &at) < 0 && errno == ETIMEDOUT);
    sem_post(&s);
    T(sem_clockwait(&s, CLOCK_MONOTONIC, &at) == 0);
    pthread_mutex_lock(&m);
    T(pthread_mutex_clocklock(&m, CLOCK_MONOTONIC, &at) == ETIMEDOUT);
    pthread_mutex_unlock(&m);
    T(pthread_cond_clockwait(&c, &m, 12345, &at) == EINVAL);
}

/* thousands of mappings (the faults' index): alternate protections keep them apart */
static void mappings(void)
{
    enum { N = 6000 };
    long pg = sysconf(_SC_PAGESIZE);
    char *base = mmap(0, N * pg, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    T(base != MAP_FAILED);
    for (int i = 0; i < N; i++)
        T(mmap(base + i * pg, pg, (i & 1) ? PROT_READ : PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == base + i * pg);
    for (int i = 0; i < N; i += 2)
        base[i * pg] = (char)i;
    for (int i = 0; i < N; i += 6)
        T(munmap(base + (i + 3) * pg, pg) == 0);
    for (int i = 1; i < N; i += 4)
        if (i % 6 != 3)
            T(mprotect(base + i * pg, pg, PROT_READ | PROT_WRITE) == 0);
        else {
            errno = 0;
            T(mprotect(base + i * pg, pg, PROT_READ | PROT_WRITE) < 0 && errno == ENOMEM);   /* (unmapped) */
        }
    int bad = 0;
    for (int i = 0; i < N; i++) {
        if (i % 6 == 3)
            continue;
        if (i % 4 == 1)
            base[i * pg] = 1;
        if (!(i & 1) && base[i * pg] != (char)i)
            bad++;
    }
    T(bad == 0);
    pid_t c = fork();
    if (c == 0) {
        int b = 0;
        for (int i = 0; i < N; i += 2)
            if (i % 6 != 3 && base[i * pg] != (char)i)
                b++;
        _exit(b != 0);
    }
    int st;
    waitpid(c, &st, 0);
    T(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    T(munmap(base, N * pg) == 0);
}

int main(void)
{
    mappings();
    ports_fd();
    ports_wake();
    ports_file();
    timers();
    ucreds();
    randoms();
    backtraces();
    clockwaits();
    printf("m78: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
