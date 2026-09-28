/*
 * unixsock.c - named FIFOs and AF_UNIX sockets: socketpair, a path-bound
 * stream server, datagrams, SCM_RIGHTS and poll.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #c, errno, strerror(errno)); fails++; } } while (0)

static void fifo(void)
{
    const char *path = "/tmp/ut.fifo";
    unlink(path);
    T(mkfifo(path, 0600) == 0);
    struct stat st;
    T(stat(path, &st) == 0 && S_ISFIFO(st.st_mode));
    errno = 0;
    T(open(path, O_WRONLY | O_NONBLOCK) < 0 && errno == ENXIO);
    int r = open(path, O_RDONLY | O_NONBLOCK);
    T(r >= 0);
    int w = open(path, O_WRONLY | O_NONBLOCK);
    T(w >= 0);
    T(write(w, "abc", 3) == 3);
    char b[8] = { 0 };
    T(read(r, b, sizeof b) == 3 && !memcmp(b, "abc", 3));
    close(w);
    T(read(r, b, sizeof b) == 0);                 /* EOF: no writer */
    close(r);
    pid_t pid = fork();                           /* blocking opens meet */
    if (pid == 0) {
        int fd = open(path, O_WRONLY);
        write(fd, "hello", 5);
        _exit(fd < 0);
    }
    int fd = open(path, O_RDONLY);
    T(fd >= 0);
    memset(b, 0, sizeof b);
    int n = 0, k;
    while ((k = read(fd, b + n, sizeof b - n)) > 0)
        n += k;
    T(n == 5 && !memcmp(b, "hello", 5));
    int status;
    T(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(fd);
    fd = open(path, O_RDWR);                      /* both ends, never waits */
    T(fd >= 0);
    T(write(fd, "x", 1) == 1 && read(fd, b, 1) == 1 && b[0] == 'x');
    close(fd);
    T(unlink(path) == 0);
}

static void pair(void)
{
    int sv[2];
    T(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    T(write(sv[0], "ping", 4) == 4);
    char b[64] = { 0 };
    T(read(sv[1], b, sizeof b) == 4 && !memcmp(b, "ping", 4));
    T(send(sv[1], "pong", 4, 0) == 4);
    T(recv(sv[0], b, sizeof b, 0) == 4 && !memcmp(b, "pong", 4));
    int type = 0;
    socklen_t len = sizeof type;
    T(getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &type, &len) == 0 && type == SOCK_STREAM);
    struct stat st;
    T(fstat(sv[0], &st) == 0 && S_ISSOCK(st.st_mode));
    /* a large write drains through the peer in another process */
    size_t big = 300000;
    pid_t pid = fork();
    if (pid == 0) {
        char *p = malloc(big);
        for (size_t i = 0; i < big; i++)
            p[i] = i * 7;
        size_t done = 0;
        while (done < big) {
            ssize_t k = write(sv[0], p + done, big - done);
            if (k <= 0)
                _exit(1);
            done += k;
        }
        _exit(0);
    }
    char *q = malloc(big);
    size_t got = 0;
    while (got < big) {
        ssize_t k = read(sv[1], q + got, big - got);
        if (k <= 0)
            break;
        got += k;
    }
    int ok = got == big;
    for (size_t i = 0; ok && i < big; i++)
        ok = q[i] == (char)(i * 7);
    T(ok);
    int status;
    T(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    /* shutdown and close give EOF, then EPIPE */
    T(shutdown(sv[0], SHUT_WR) == 0);
    T(read(sv[1], b, sizeof b) == 0);
    close(sv[0]);
    signal(SIGPIPE, SIG_IGN);
    errno = 0;
    T(write(sv[1], "x", 1) < 0 && errno == EPIPE);
    struct pollfd pf = { sv[1], POLLIN, 0 };
    T(poll(&pf, 1, 0) == 1 && (pf.revents & (POLLIN | POLLHUP)));
    close(sv[1]);
    free(q);

    T(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
    T(send(sv[0], "one", 3, 0) == 3 && send(sv[0], "two!", 4, 0) == 4);
    T(recv(sv[1], b, sizeof b, 0) == 3 && recv(sv[1], b, 2, 0) == 2);   /* the rest of "two!" is dropped */
    T(recv(sv[1], b, sizeof b, MSG_DONTWAIT) < 0 && errno == EAGAIN);
    close(sv[0]);
    close(sv[1]);
}

static void rights(void)
{
    int sv[2], p[2];
    T(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    T(pipe(p) == 0);
    char data = 'R';
    struct iovec iov = { &data, 1 };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } c;
    memset(&c, 0, sizeof c);
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = c.buf, .msg_controllen = sizeof c.buf };
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    h->cmsg_level = SOL_SOCKET;
    h->cmsg_type = SCM_RIGHTS;
    h->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(h), &p[1], sizeof(int));
    T(sendmsg(sv[0], &m, 0) == 1);
    close(p[1]);                                   /* the message holds the write end */
    char got = 0;
    iov.iov_base = &got;
    memset(&c, 0, sizeof c);
    m.msg_controllen = sizeof c.buf;
    T(recvmsg(sv[1], &m, 0) == 1 && got == 'R');
    h = CMSG_FIRSTHDR(&m);
    T(h && h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS);
    int fd = -1;
    if (h)
        memcpy(&fd, CMSG_DATA(h), sizeof(int));
    T(fd >= 0 && write(fd, "via", 3) == 3);
    char b[8] = { 0 };
    T(read(p[0], b, sizeof b) == 3 && !memcmp(b, "via", 3));
    close(fd);
    T(read(p[0], b, sizeof b) == 0);               /* last write end closed */
    close(p[0]);
    close(sv[0]);
    close(sv[1]);
}

static void server(void)
{
    const char *path = "/tmp/ut.sock";
    unlink(path);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    T(s >= 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strcpy(a.sun_path, path);
    T(bind(s, (struct sockaddr *)&a, sizeof a) == 0);
    struct stat st;
    T(stat(path, &st) == 0 && S_ISSOCK(st.st_mode));
    errno = 0;
    int s2 = socket(AF_UNIX, SOCK_STREAM, 0);
    T(bind(s2, (struct sockaddr *)&a, sizeof a) < 0 && errno == EADDRINUSE);
    close(s2);
    T(listen(s, 4) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(c, (struct sockaddr *)&a, sizeof a) < 0)
            _exit(2);
        struct sockaddr_un pa;
        socklen_t pl = sizeof pa;
        if (getpeername(c, (struct sockaddr *)&pa, &pl) < 0 || strcmp(pa.sun_path, path))
            _exit(3);
        write(c, "hi server", 9);
        char b[32] = { 0 };
        if (read(c, b, sizeof b) != 9 || memcmp(b, "hi client", 9))
            _exit(4);
        _exit(0);
    }
    struct pollfd pf = { s, POLLIN, 0 };
    T(poll(&pf, 1, 5000) == 1 && (pf.revents & POLLIN));
    int c = accept(s, NULL, NULL);
    T(c >= 0);
    char b[32] = { 0 };
    T(read(c, b, sizeof b) == 9 && !memcmp(b, "hi server", 9));
    T(write(c, "hi client", 9) == 9);
    int status;
    T(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(c);
    close(s);
    errno = 0;
    c = socket(AF_UNIX, SOCK_STREAM, 0);
    T(connect(c, (struct sockaddr *)&a, sizeof a) < 0 && errno == ECONNREFUSED);   /* nobody listens */
    close(c);
    unlink(path);
    errno = 0;
    c = socket(AF_UNIX, SOCK_STREAM, 0);
    T(connect(c, (struct sockaddr *)&a, sizeof a) < 0 && errno == ENOENT);
    close(c);
}

static void dgram(void)
{
    const char *sp = "/tmp/ut.dsrv", *cp = "/tmp/ut.dcli";
    unlink(sp);
    unlink(cp);
    int s = socket(AF_UNIX, SOCK_DGRAM, 0), c = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX }, ca = { .sun_family = AF_UNIX }, from;
    strcpy(sa.sun_path, sp);
    strcpy(ca.sun_path, cp);
    T(bind(s, (struct sockaddr *)&sa, sizeof sa) == 0);
    T(bind(c, (struct sockaddr *)&ca, sizeof ca) == 0);
    T(sendto(c, "dg1", 3, 0, (struct sockaddr *)&sa, sizeof sa) == 3);
    char b[16] = { 0 };
    socklen_t fl = sizeof from;
    T(recvfrom(s, b, sizeof b, 0, (struct sockaddr *)&from, &fl) == 3 && !strcmp(from.sun_path, cp));
    T(sendto(s, "back", 4, 0, (struct sockaddr *)&from, fl) == 4);
    T(recv(c, b, sizeof b, 0) == 4 && !memcmp(b, "back", 4));
    T(connect(c, (struct sockaddr *)&sa, sizeof sa) == 0);
    T(send(c, "dg2", 3, 0) == 3);
    T(recv(s, b, sizeof b, 0) == 3 && !memcmp(b, "dg2", 3));
    close(s);
    close(c);
    unlink(sp);
    unlink(cp);
}

int main(void)
{
    fifo();
    pair();
    rights();
    server();
    dgram();
    printf("unixsock: %s (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
