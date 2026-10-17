/*
 * ptest-ipc (make posix-test): pipes and unix socket pairs (unixd),
 * descriptors passed (a memfd and a pipe end), poll on pipes and an eventfd,
 * posix_spawn with the child's output in a pipe and its exit status,
 * signals through sigaction, and the wall clock.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static int fails;
static void check(const char *what, int ok)
{
    printf("ptest ipc: %s: %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int late_fd;
static void *late_writer(void *a)
{
    (void)a;
    usleep(30000);
    if (write(late_fd, "x", 1) != 1) return (void *)1;
    return 0;
}
static int efd;
static void *late_event(void *a) { (void)a; usleep(30000); uint64_t v = 3; return (void *)(long)(write(efd, &v, 8) != 8); }

static volatile int got_sig, got_info;
static void on_usr1(int s) { got_sig = s; }
static void on_usr2(int s, siginfo_t *i, void *c) { (void)c; got_info = i ? i->si_signo : -s; }

static int child(int argc, char **argv)
{
    printf("from the child: %s %s %s\n", argc > 2 ? argv[2] : "?", getenv("PTEST") ? getenv("PTEST") : "?", getcwd((char[64]){ 0 }, 64));
    fflush(stdout);
    return 7;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child")) return child(argc, argv);
    char b[64] = { 0 };
    int p[2];
    check("pipe: written, read", pipe(p) == 0 && write(p[1], "hello", 5) == 5 && read(p[0], b, sizeof b) == 5 && !memcmp(b, "hello", 5));
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    check("a non-blocking read of an empty pipe", read(p[0], b, 1) == -1 && errno == EAGAIN);
    close(p[1]);
    check("the end: read gives 0", read(p[0], b, 1) == 0);
    close(p[0]);

    int s[2];
    check("socketpair (SOCK_SEQPACKET): messages kept apart", socketpair(AF_UNIX, SOCK_SEQPACKET, 0, s) == 0 &&
          send(s[0], "one", 3, 0) == 3 && send(s[0], "three", 5, 0) == 5 && recv(s[1], b, sizeof b, 0) == 3 &&
          recv(s[1], b, sizeof b, 0) == 5 && !memcmp(b, "three", 5));

    /* a memfd and a pipe end passed through the pair */
    int mf = memfd_create("ptest", MFD_CLOEXEC), q[2];
    char *m1 = 0;
    if (mf >= 0 && ftruncate(mf, 8192) == 0) m1 = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, mf, 0);
    if (m1 && m1 != MAP_FAILED) strcpy(m1 + 4096, "shared");
    pipe(q);
    char cbuf[CMSG_SPACE(2 * sizeof(int))];
    struct iovec iov = { "fds", 3 };
    struct msghdr h = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&h);
    cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(2 * sizeof(int));
    memcpy(CMSG_DATA(cm), (int[]){ mf, q[1] }, 2 * sizeof(int));
    long sent = sendmsg(s[0], &h, 0);
    close(mf); close(q[1]);                         /* (in flight: unixd holds them) */
    char rbuf[16];
    char rc[CMSG_SPACE(2 * sizeof(int))];
    struct iovec riov = { rbuf, sizeof rbuf };
    struct msghdr rh = { .msg_iov = &riov, .msg_iovlen = 1, .msg_control = rc, .msg_controllen = sizeof rc };
    long got = recvmsg(s[1], &rh, 0);
    int fv[2] = { -1, -1 };
    if (got > 0 && CMSG_FIRSTHDR(&rh)) memcpy(fv, CMSG_DATA(CMSG_FIRSTHDR(&rh)), 2 * sizeof(int));
    char *m2 = fv[0] >= 0 ? mmap(0, 4096, PROT_READ, MAP_SHARED, fv[0], 4096) : MAP_FAILED;
    check("a memfd passed: mapped, the same memory", sent == 3 && got == 3 && m2 != MAP_FAILED && !strcmp(m2, "shared"));
    if (m1 && m1 != MAP_FAILED) strcpy(m1 + 4096, "again");
    check("... changed on one side, seen on the other", m2 != MAP_FAILED && !strcmp(m2, "again"));
    check("a pipe end passed: written there, read here", fv[1] >= 0 && write(fv[1], "via", 3) == 3 && read(q[0], b, 3) == 3 && !memcmp(b, "via", 3));
    close(fv[1]);
    check("the passed end closed: the end of the pipe", read(q[0], b, 1) == 0);

    /* poll */
    pipe(p);
    struct pollfd pf = { p[0], POLLIN, 0 };
    double t0 = now();
    int r = poll(&pf, 1, 50);
    check("poll on an empty pipe times out", r == 0 && now() - t0 >= 0.045);
    late_fd = p[1];
    pthread_t th;
    pthread_create(&th, 0, late_writer, 0);
    t0 = now();
    r = poll(&pf, 1, 5000);
    pthread_join(th, 0);
    check("poll wakes when another thread writes", r == 1 && (pf.revents & POLLIN) && now() - t0 < 2);
    close(p[1]);
    read(p[0], b, 1);
    pf.revents = 0;
    check("poll sees the writer gone (POLLHUP)", poll(&pf, 1, 1000) == 1 && (pf.revents & POLLHUP));
    close(p[0]);
    int sp[2];                                  /* waiting to read an end that can be written */
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    late_fd = sp[1];
    pthread_create(&th, 0, late_writer, 0);
    struct pollfd ps = { sp[0], POLLIN, 0 };
    t0 = now();
    r = poll(&ps, 1, 5000);
    pthread_join(th, 0);
    check("poll for reading a socket pair end (writable): wakes when another thread writes",
          r == 1 && (ps.revents & POLLIN) && !(ps.revents & POLLOUT) && now() - t0 < 2);
    close(sp[0]); close(sp[1]);
    efd = eventfd(0, EFD_NONBLOCK);
    pthread_create(&th, 0, late_event, 0);
    struct pollfd pe = { efd, POLLIN, 0 };
    r = poll(&pe, 1, 5000);
    pthread_join(th, 0);
    uint64_t v = 0;
    check("eventfd: poll wakes, the count read", r == 1 && read(efd, &v, 8) == 8 && v == 3 && read(efd, &v, 8) == -1);

    /* posix_spawn: the child's output in a pipe, its folder, environment, exit status */
    pipe(p);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_addclose(&fa, p[0]);
    posix_spawn_file_actions_addchdir_np(&fa, "/tmp");
    char *cargv[] = { "ptest-ipc", "child", "argument", 0 }, *cenv[] = { "PTEST=environment", "PATH=/bin", 0 };
    pid_t pid = 0;
    int e = posix_spawnp(&pid, "ptest-ipc", &fa, 0, cargv, cenv);
    close(p[1]);
    char out[128] = { 0 };
    size_t n = 0;
    for (long k; n < sizeof out - 1 && (k = read(p[0], out + n, sizeof out - 1 - n)) > 0; ) n += k;
    int st = 0;
    pid_t w = e ? -1 : waitpid(pid, &st, 0);
    if (!strstr(out, "from the child: argument environment /tmp")) printf("ptest ipc: spawn: error %d, output '%s', status %d\n", e, out, st);
    check("posix_spawnp: the child's output through a pipe", e == 0 && strstr(out, "from the child: argument environment /tmp"));
    check("waitpid: its exit status", w == pid && WIFEXITED(st) && WEXITSTATUS(st) == 7);
    close(p[0]);
    check("posix_spawn of a missing program fails", posix_spawn(&pid, "/bin/nope", 0, 0, cargv, environ) == ENOENT);

    /* signals */
    struct sigaction sa = { .sa_handler = on_usr1 };
    sigaction(SIGUSR1, &sa, 0);
    raise(SIGUSR1);
    usleep(10000);
    struct sigaction si = { .sa_sigaction = on_usr2, .sa_flags = SA_SIGINFO };
    sigaction(SIGUSR2, &si, 0);
    kill(getpid(), SIGUSR2);
    usleep(10000);
    check("sigaction: a handler, and one with siginfo", got_sig == SIGUSR1 && got_info == SIGUSR2);
    signal(SIGUSR1, SIG_IGN);
    raise(SIGUSR1);
    check("an ignored signal", 1);

    /* the wall clock */
    struct timeval tv;
    gettimeofday(&tv, 0);
    time_t t = time(0);
    check("the wall clock: after 2026, gettimeofday agrees", t > 1767225600 && tv.tv_sec - t <= 1 && t - tv.tv_sec <= 1);
    printf("ptest ipc: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
