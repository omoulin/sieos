/*
 * ipv6.c - IPv6 (M19): addresses and autoconfiguration, TCP and UDP over
 * ::1, dual-stack AF_INET6 sockets (IPv4-mapped peers), IPV6_V6ONLY and
 * port sharing, raw ICMPv6 echo to ourselves and to the router, and the
 * name functions.  Run as root (raw sockets).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "sieos/syscall.h"
#include "sieos/sysinfo.h"

static int fails;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #c, errno, strerror(errno)); fails++; } } while (0)

static struct sockaddr_in6 a6(const char *ip, int port)
{
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_port = htons(port);
    inet_pton(AF_INET6, ip, &a.sin6_addr);
    return a;
}

static struct sockaddr_in a4(const char *ip, int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

static const char *str6(const struct sockaddr_in6 *a, char *buf)
{
    char ip[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof(ip));
    sprintf(buf, "[%s]:%d", ip, ntohs(a->sin6_port));
    return buf;
}

static int listener6(const char *ip, int port, int v6only)
{
    int s = socket(AF_INET6, SOCK_STREAM, 0);
    if (v6only >= 0)
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    struct sockaddr_in6 a = a6(ip, port);
    if (bind(s, (void *)&a, sizeof(a)) < 0 || listen(s, 4) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static void names(void)
{
    struct in6_addr x;
    char b[INET6_ADDRSTRLEN];
    T(inet_pton(AF_INET6, "fe80::5054:ff:fe12:3456", &x) == 1 &&
      !strcmp(inet_ntop(AF_INET6, &x, b, sizeof(b)), "fe80::5054:ff:fe12:3456"));
    T(sizeof(struct sockaddr_in6) == 32);
    struct addrinfo h = { .ai_flags = AI_NUMERICHOST, .ai_socktype = SOCK_STREAM }, *r;
    T(getaddrinfo("::1", "80", &h, &r) == 0 && r->ai_family == AF_INET6 && r->ai_addrlen == 32 &&
      ntohs(((struct sockaddr_in6 *)r->ai_addr)->sin6_port) == 80);
    freeaddrinfo(r);
    char host[64], serv[16];
    struct sockaddr_in6 a = a6("::1", 22);
    T(getnameinfo((void *)&a, sizeof(a), host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) == 0 &&
      !strcmp(host, "::1") && !strcmp(serv, "22"));
}

/* The interface: a link-local address now, a global one once a router advertised a prefix. */
static int router(struct sieos_netinfo6 *ni)
{
    for (int i = 0; i < 50; i++) {                   /* up to 5 s */
        memset(ni, 0, sizeof(*ni));
        if (syscall(SIEOS_SYS_netinfo6, ni) < 0)
            return 0;
        int global = 0;
        for (int k = 0; k < ni->naddr; k++)
            global |= ni->addr[k].flags & SIEOS_NET6_AUTOCONF;
        if (global && ni->router[0])
            break;
        usleep(100000);
    }
    char b[INET6_ADDRSTRLEN];
    int ll = 0, global = 0;
    for (int k = 0; k < ni->naddr; k++) {
        printf("  inet6 %s/%d%s\n", inet_ntop(AF_INET6, ni->addr[k].addr, b, sizeof(b)), ni->addr[k].prefixlen,
               ni->addr[k].flags & SIEOS_NET6_LINKLOCAL ? " link-local" :
               ni->addr[k].flags & SIEOS_NET6_AUTOCONF ? " autoconf" : "");
        ll |= ni->addr[k].flags & SIEOS_NET6_LINKLOCAL;
        global |= ni->addr[k].flags & SIEOS_NET6_AUTOCONF;
    }
    T(ni->up && ll && ni->mtu >= 1280);
    if (ni->router[0]) {
        printf("  router %s", inet_ntop(AF_INET6, ni->router, b, sizeof(b)));
        printf("  dns %s  mtu %u\n", ni->dns[0] ? inet_ntop(AF_INET6, ni->dns, b, sizeof(b)) : "-", ni->mtu);
    }
    return global && ni->router[0];
}

static void tcp6(void)
{
    int l = listener6("::1", 5555, -1);
    T(l >= 0);
    int c = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 a = a6("::1", 5555), me, peer;
    T(connect(c, (void *)&a, sizeof(a)) == 0);
    socklen_t n = sizeof(peer);
    int s = accept(l, (void *)&peer, &n);
    T(s >= 0 && n == sizeof(peer) && peer.sin6_family == AF_INET6 && IN6_IS_ADDR_LOOPBACK(&peer.sin6_addr));
    n = sizeof(me);
    T(getsockname(c, (void *)&me, &n) == 0 && me.sin6_port == peer.sin6_port);
    n = sizeof(peer);
    T(getpeername(c, (void *)&peer, &n) == 0 && ntohs(peer.sin6_port) == 5555);
    static char big[100000], got[100000];
    for (size_t i = 0; i < sizeof(big); i++)
        big[i] = (char)(i * 7);
    if (fork() == 0) {                               /* more than the windows: both ends must move */
        close(s);
        size_t w = 0;
        while (w < sizeof(big)) {
            ssize_t k = write(c, big + w, sizeof(big) - w);
            if (k <= 0)
                _exit(1);
            w += k;
        }
        _exit(0);
    }
    size_t r = 0;
    while (r < sizeof(got)) {
        ssize_t k = read(s, got + r, sizeof(got) - r);
        if (k <= 0)
            break;
        r += k;
    }
    T(r == sizeof(got) && !memcmp(big, got, sizeof(got)));
    char b1[64], b2[64];
    printf("tcp6 %s -> %s: %zu bytes\n", str6(&me, b1), str6(&peer, b2), r);
    close(c);
    close(s);
    close(l);
}

static void dual(void)
{
    /* an AF_INET6 socket on :: takes IPv4 connections, with the peer mapped */
    int l = listener6("::", 5556, -1);
    T(l >= 0);
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = a4("127.0.0.1", 5556);
    T(connect(c, (void *)&a, sizeof(a)) == 0);
    struct sockaddr_in6 peer, me;
    socklen_t n = sizeof(peer);
    int s = accept(l, (void *)&peer, &n);
    char ps[INET6_ADDRSTRLEN];
    T(s >= 0 && IN6_IS_ADDR_V4MAPPED(&peer.sin6_addr) &&
      !strcmp(inet_ntop(AF_INET6, &peer.sin6_addr, ps, sizeof(ps)), "::ffff:127.0.0.1"));
    n = sizeof(me);
    T(getsockname(s, (void *)&me, &n) == 0 && IN6_IS_ADDR_V4MAPPED(&me.sin6_addr) && ntohs(me.sin6_port) == 5556);
    T(write(c, "v4", 2) == 2);
    char x[4] = { 0 };
    T(read(s, x, sizeof(x)) == 2 && !memcmp(x, "v4", 2));
    close(c);
    close(s);
    /* and it reaches IPv4 peers by mapped addresses */
    int c6 = socket(AF_INET6, SOCK_STREAM, 0);
    int l4 = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in b4 = a4("127.0.0.1", 5559);
    T(bind(l4, (void *)&b4, sizeof(b4)) == 0 && listen(l4, 2) == 0);
    struct sockaddr_in6 m = a6("::ffff:127.0.0.1", 5559);
    T(connect(c6, (void *)&m, sizeof(m)) == 0);
    struct sockaddr_in p4;
    n = sizeof(p4);
    s = accept(l4, (void *)&p4, &n);
    T(s >= 0 && n == sizeof(p4) && p4.sin_family == AF_INET && p4.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    close(s);
    close(c6);
    close(l4);

    /* the port is taken for AF_INET too ... */
    int b = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in any = a4("0.0.0.0", 5556);
    T(bind(b, (void *)&any, sizeof(any)) < 0 && errno == EADDRINUSE);
    close(b);
    close(l);

    /* ... unless the AF_INET6 one is IPV6_V6ONLY, which takes no IPv4 */
    l = listener6("::", 5557, 1);
    int v = 0;
    socklen_t vl = sizeof(v);
    T(l >= 0 && getsockopt(l, IPPROTO_IPV6, IPV6_V6ONLY, &v, &vl) == 0 && v == 1);
    c = socket(AF_INET, SOCK_STREAM, 0);
    a = a4("127.0.0.1", 5557);
    T(connect(c, (void *)&a, sizeof(a)) < 0 && errno == ECONNREFUSED);
    close(c);
    b = socket(AF_INET, SOCK_STREAM, 0);
    any = a4("0.0.0.0", 5557);
    T(bind(b, (void *)&any, sizeof(any)) == 0);
    close(b);
    c = socket(AF_INET6, SOCK_STREAM, 0);
    m = a6("::ffff:127.0.0.1", 5557);
    T(connect(c, (void *)&m, sizeof(m)) < 0);        /* nothing listens for IPv4 */
    close(c);
    close(l);
    /* an AF_INET socket refuses IPv6 addresses */
    c = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in6 one = a6("::1", 9);
    T(connect(c, (void *)&one, sizeof(one)) < 0 && errno == EAFNOSUPPORT);
    close(c);
}

static void udp6(void)
{
    int r = socket(AF_INET6, SOCK_DGRAM, 0), w = socket(AF_INET6, SOCK_DGRAM, 0);
    struct sockaddr_in6 a = a6("::1", 5560), from;
    T(bind(r, (void *)&a, sizeof(a)) == 0);
    T(sendto(w, "ping6", 5, 0, (void *)&a, sizeof(a)) == 5);
    char buf[16];
    socklen_t n = sizeof(from);
    struct pollfd p = { r, POLLIN, 0 };
    T(poll(&p, 1, 2000) == 1);
    T(recvfrom(r, buf, sizeof(buf), 0, (void *)&from, &n) == 5 && !memcmp(buf, "ping6", 5) &&
      IN6_IS_ADDR_LOOPBACK(&from.sin6_addr) && from.sin6_port);
    /* reply to the sender */
    T(sendto(r, "pong", 4, 0, (void *)&from, n) == 4);
    T(recv(w, buf, sizeof(buf), 0) == 4);
    close(w);
    /* a mapped destination from an AF_INET6 socket reaches an AF_INET one; a
     * socket bound to an IPv6 address cannot send there */
    int r4 = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in b4 = a4("127.0.0.1", 5561), f4;
    T(bind(r4, (void *)&b4, sizeof(b4)) == 0);
    struct sockaddr_in6 m = a6("::ffff:127.0.0.1", 5561);
    T(sendto(r, "four", 4, 0, (void *)&m, sizeof(m)) < 0 && errno == ENETUNREACH);
    int w6 = socket(AF_INET6, SOCK_DGRAM, 0);
    T(sendto(w6, "four", 4, 0, (void *)&m, sizeof(m)) == 4);
    struct sockaddr_in6 me;
    n = sizeof(me);
    T(getsockname(w6, (void *)&me, &n) == 0 && me.sin6_port != 0);   /* (the address stays ::) */
    n = sizeof(f4);
    p.fd = r4;
    T(poll(&p, 1, 2000) == 1 && recvfrom(r4, buf, sizeof(buf), 0, (void *)&f4, &n) == 4 &&
      f4.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && f4.sin_port == me.sin6_port);
    close(w6);
    close(r4);
    close(r);
}

/* ICMPv6 echo through a raw socket; the kernel computes the checksum. */
static int ping6(const unsigned char *dst, const char *what)
{
    int s = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    if (s < 0)
        return 0;
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    memcpy(&a.sin6_addr, dst, 16);
    unsigned char m[16] = { 128, 0, 0, 0, 0x12, 0x34, 0, 1, 'S', 'I', 'E', 'O', 'S', '6', '!', '!' };
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int ok = 0;
    if (sendto(s, m, sizeof(m), 0, (void *)&a, sizeof(a)) == sizeof(m)) {
        struct pollfd p = { s, POLLIN, 0 };
        while (!ok && poll(&p, 1, 2000) == 1) {
            unsigned char r[64];
            struct sockaddr_in6 f;
            socklen_t n = sizeof(f);
            ssize_t k = recvfrom(s, r, sizeof(r), 0, (void *)&f, &n);
            ok = k == sizeof(m) && r[0] == 129 && !memcmp(r + 4, m + 4, 12) && !memcmp(&f.sin6_addr, dst, 16);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    char b[INET6_ADDRSTRLEN];
    printf("ping6 %s (%s): %s, %ld us\n", inet_ntop(AF_INET6, dst, b, sizeof(b)), what, ok ? "echo reply" : "no reply",
           (t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000);
    close(s);
    return ok;
}

int main(void)
{
    names();
    struct sieos_netinfo6 ni;
    int have_router = router(&ni);
    tcp6();
    dual();
    udp6();
    static const unsigned char lo[16] = { [15] = 1 };
    T(ping6(lo, "loopback"));
    T(ping6(ni.addr[1].addr, "our link-local address"));
    if (have_router)
        T(ping6(ni.router, "the router"));
    else
        printf("no router advertisement: the router tests are skipped\n");
    printf("ipv6: %s (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
