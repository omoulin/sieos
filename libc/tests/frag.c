/*
 * frag.c - IP fragmentation (M25): UDP datagrams larger than the MTU over
 * IPv4 and IPv6 loopback, in order and with fragments reordered (uadmin
 * A_NETTEST), up to the largest datagram; a large ICMPv6 echo; and, when a
 * host UDP echo server answers at 10.0.2.2:7777, large datagrams to it and
 * back.  Run as root.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "sieos/syscall.h"
#include "sieos/sysinfo.h"

static int fails;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #c, errno, strerror(errno)); fails++; } } while (0)

static unsigned char out[65536], in[65536];

static void pattern(size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++)
        out[i] = (unsigned char)(i * 131 + seed);
}

static void addr_of(int family, int port, struct sockaddr_storage *a, socklen_t *len, const char *v4)
{
    memset(a, 0, sizeof(*a));
    if (family == AF_INET) {
        struct sockaddr_in *s = (void *)a;
        s->sin_family = AF_INET;
        s->sin_port = htons(port);
        inet_pton(AF_INET, v4 ? v4 : "127.0.0.1", &s->sin_addr);
        *len = sizeof(*s);
    } else {
        struct sockaddr_in6 *s = (void *)a;
        s->sin6_family = AF_INET6;
        s->sin6_port = htons(port);
        s->sin6_addr = in6addr_loopback;
        *len = sizeof(*s);
    }
}

/* Datagrams of each size through loopback; 0 on success. */
static int loopback(int family, int reorder)
{
    static const size_t sizes[] = { 1473, 1500, 2960, 3000, 8192, 16385, 32768, 65000, 65507 };
    syscall(SIEOS_SYS_uadmin, SIEOS_A_NETTEST, reorder << 16);
    int r = socket(family, SOCK_DGRAM, 0), w = socket(family, SOCK_DGRAM, 0);
    struct sockaddr_storage a;
    socklen_t al;
    addr_of(family, 7300 + family + reorder, &a, &al, NULL);
    T(bind(r, (void *)&a, al) == 0);
    int bad = 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t n = sizes[i];
        if (family == AF_INET6 && n > 65527)
            continue;
        pattern(n, (unsigned)i);
        ssize_t s = sendto(w, out, n, 0, (void *)&a, al);
        struct pollfd p = { r, POLLIN, 0 };
        ssize_t g = -1;
        for (int tries = 0; tries < 20 && g < 0; tries++) {   /* (a held fragment may need a nudge) */
            if (poll(&p, 1, 100) == 1)
                g = recv(r, in, sizeof(in), 0);
            else
                sendto(w, "", 0, 0, (void *)&a, al);             /* an empty datagram releases it */
        }
        while (g == 0 && poll(&p, 1, 100) == 1)
            g = recv(r, in, sizeof(in), 0);                    /* skip the nudges */
        if (s != (ssize_t)n || g != (ssize_t)n || memcmp(in, out, n)) {
            printf("FAIL %s %zu bytes%s: sent %zd, got %zd\n", family == AF_INET ? "IPv4" : "IPv6", n,
                   reorder ? " (reordered)" : "", s, g);
            bad++;
        }
        while (poll(&p, 1, 0) == 1)
            recv(r, in, sizeof(in), 0);
    }
    T(sendto(w, out, family == AF_INET ? 65508 : 65528, 0, (void *)&a, al) < 0 && errno == EMSGSIZE);
    close(r);
    close(w);
    syscall(SIEOS_SYS_uadmin, SIEOS_A_NETTEST, 0);
    printf("%s %s: datagrams up to %s bytes%s\n", bad ? "FAIL" : "ok  ", family == AF_INET ? "IPv4" : "IPv6",
           family == AF_INET ? "65,507" : "65,527", reorder ? ", fragments reordered" : "");
    return bad;
}

/* A large ICMPv6 echo to ::1 (the request and the reply both in fragments). */
static void ping_large(void)
{
    int s = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    if (s < 0) {
        printf("(no raw socket: not root?)\n");
        return;
    }
    size_t n = 8000;
    memset(out, 0, n);
    out[0] = 128;
    for (size_t i = 8; i < n; i++)
        out[i] = (unsigned char)i;
    struct sockaddr_storage a;
    socklen_t al;
    addr_of(AF_INET6, 0, &a, &al, NULL);
    T(sendto(s, out, n, 0, (void *)&a, al) == (ssize_t)n);
    struct pollfd p = { s, POLLIN, 0 };
    ssize_t g = 0;
    while (poll(&p, 1, 1000) == 1) {
        g = recv(s, in, sizeof(in), 0);
        if (g > 0 && in[0] == 129)
            break;
    }
    T(g == (ssize_t)n && in[0] == 129 && !memcmp(in + 8, out + 8, n - 8));
    printf("%s ping6 ::1 with %zu bytes\n", g == (ssize_t)n ? "ok  " : "FAIL", n);
    close(s);
}

/* To a host echo server (10.0.2.2:7777), if there is one. */
static void host_echo(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_storage a;
    socklen_t al;
    addr_of(AF_INET, 7777, &a, &al, "10.0.2.2");
    pattern(8, 1);
    sendto(s, out, 8, 0, (void *)&a, al);
    struct pollfd p = { s, POLLIN, 0 };
    if (poll(&p, 1, 1000) != 1) {
        printf("(no UDP echo server at 10.0.2.2:7777: skipped)\n");
        close(s);
        return;
    }
    recv(s, in, sizeof(in), 0);
    static const size_t sizes[] = { 3000, 20000, 60000 };
    for (size_t i = 0; i < 3; i++) {
        pattern(sizes[i], 7 + (unsigned)i);
        T(sendto(s, out, sizes[i], 0, (void *)&a, al) == (ssize_t)sizes[i]);
        ssize_t g = poll(&p, 1, 2000) == 1 ? recv(s, in, sizeof(in), 0) : -1;
        bool ok = g == (ssize_t)sizes[i] && !memcmp(in, out, sizes[i]);
        printf("%s host echo, %zu bytes out and back\n", ok ? "ok  " : "FAIL", sizes[i]);
        fails += !ok;
    }
    close(s);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    fails += loopback(AF_INET, 0);
    fails += loopback(AF_INET6, 0);
    fails += loopback(AF_INET, 300);
    fails += loopback(AF_INET6, 300);
    ping_large();
    host_echo();
    printf("frag: %s (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
