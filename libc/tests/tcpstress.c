/*
 * tcpstress.c - TCP under loss and reordering (M24): 8 MB (2 MB with loss) over loopback
 * (IPv4 and IPv6) while the kernel drops and reorders loopback packets
 * (uadmin A_NETTEST), checked by length and checksum; the time for each.
 * Run as root.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "sieos/syscall.h"
#include "sieos/sysinfo.h"


static int fails;

static uint64_t fnv(uint64_t h, const unsigned char *p, size_t n)
{
    while (n--)
        h = (h ^ *p++) * 0x100000001b3ULL;
    return h;
}

static void fill(unsigned char *b, size_t n, uint64_t *state)
{
    for (size_t i = 0; i < n; i++) {
        *state ^= *state << 13;
        *state ^= *state >> 7;
        *state ^= *state << 17;
        b[i] = (unsigned char)*state;
    }
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int run(int family, int drop, int reorder, int port)
{
    uint32_t SIZE = drop ? 2u << 20 : 8u << 20;
    syscall(SIEOS_SYS_uadmin, SIEOS_A_NETTEST, drop | reorder << 16);
    int l = socket(family, SOCK_STREAM, 0);
    struct sockaddr_storage a;
    socklen_t alen;
    memset(&a, 0, sizeof(a));
    if (family == AF_INET) {
        struct sockaddr_in *s = (void *)&a;
        s->sin_family = AF_INET;
        s->sin_port = htons(port);
        s->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        alen = sizeof(*s);
    } else {
        struct sockaddr_in6 *s = (void *)&a;
        s->sin6_family = AF_INET6;
        s->sin6_port = htons(port);
        s->sin6_addr = in6addr_loopback;
        alen = sizeof(*s);
    }
    if (bind(l, (void *)&a, alen) < 0 || listen(l, 1) < 0) {
        printf("FAIL bind/listen: %s\n", strerror(errno));
        return 1;
    }
    pid_t pid = fork();
    if (pid == 0) {                              /* the receiver: count, sum, report */
        int c = accept(l, NULL, NULL);
        static unsigned char buf[65536];
        uint64_t h = 0xcbf29ce484222325ULL, total = 0;
        ssize_t n;
        while ((n = read(c, buf, sizeof(buf))) > 0) {
            h = fnv(h, buf, n);
            total += n;
        }
        uint64_t r[2] = { total, h };
        write(c, r, sizeof(r));
        close(c);
        _exit(0);
    }
    close(l);
    int s = socket(family, SOCK_STREAM, 0);
    double t0 = now();
    if (connect(s, (void *)&a, alen) < 0) {
        printf("FAIL connect: %s\n", strerror(errno));
        return 1;
    }
    static unsigned char buf[65536];
    uint64_t st = 88172645463325252ULL, h = 0xcbf29ce484222325ULL;
    for (uint32_t sent = 0; sent < SIZE; sent += sizeof(buf)) {
        fill(buf, sizeof(buf), &st);
        h = fnv(h, buf, sizeof(buf));
        for (size_t off = 0; off < sizeof(buf);) {
            ssize_t w = write(s, buf + off, sizeof(buf) - off);
            if (w <= 0) {
                printf("FAIL write: %s\n", strerror(errno));
                return 1;
            }
            off += w;
        }
    }
    shutdown(s, SHUT_WR);
    uint64_t r[2] = { 0, 0 };
    ssize_t got = 0;
    while (got < (ssize_t)sizeof(r)) {
        ssize_t n = read(s, (char *)r + got, sizeof(r) - got);
        if (n <= 0)
            break;
        got += n;
    }
    double dt = now() - t0;
    close(s);
    waitpid(pid, NULL, 0);
    syscall(SIEOS_SYS_uadmin, SIEOS_A_NETTEST, 0);
    bool ok = got == sizeof(r) && r[0] == SIZE && r[1] == h;
    printf("%s %s: %u MB with %d.%d%% loss, %d.%d%% reordering: %.2f s (%.1f MB/s)%s\n", ok ? "ok  " : "FAIL",
           family == AF_INET ? "IPv4" : "IPv6", SIZE >> 20, drop / 10, drop % 10, reorder / 10, reorder % 10, dt,
           (SIZE >> 20) / dt, ok ? "" : " - data differs");
    return !ok;
}

int main(void)
{
    static const int cases[][2] = { { 0, 0 }, { 0, 50 }, { 10, 0 }, { 20, 50 }, { 50, 100 } };
    setvbuf(stdout, NULL, _IONBF, 0);
    int port = 6100;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fails += run(AF_INET, cases[i][0], cases[i][1], port++);
        fails += run(AF_INET6, cases[i][0], cases[i][1], port++);
    }
    printf("tcpstress: %s (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
