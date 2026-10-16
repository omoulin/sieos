/*
 * fetch - Get a URL (http:// or https://), and the network's everyday
 * commands. One program, many names: /bin/ping, /bin/host... are links to
 * /bin/fetch, and the name it was started by says which command it is
 * (one copy of the network library on the disk instead of eight).
 *
 *   fetch [-v] [-k] [-o FILE] URL   the body on the screen, or into FILE
 *                                   -v: status and headers; -k: do not check
 *                                   the certificate (tests only)
 *   curl  [-v] [-k] [-o FILE] URL   the same
 *   wget  [-k] [-O FILE] URL        into a file named as the URL's last part
 *   ping HOST [-c COUNT]            ICMP echo (4 by default)
 *   host NAME, nslookup NAME        the address of a name (DNS)
 *   ifconfig                        the interface: address, gateway, DNS,
 *                                   card, link, counters
 *   netstat                         the sockets open, and their programs
 *   traceroute HOST                 the routers on the way
 *   nc [-u] HOST PORT               connect (TCP, or UDP): typed lines go
 *                                   there, what comes back is shown
 *   nc -l [-u] PORT                 the same, waiting for the other side
 *   fetch -i | -p HOST [COUNT] | -r HOST   ifconfig, ping, host (older names)
 *   fetch -l PORT                   answer one HTTP request on PORT (a test)
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "net.h"

static const char *me;                           /* the name we were started by */

static int fail(const char *what, long e) { printf("%s: %s: %s\n", me, what, net_strerror(e)); return 1; }

static int usage(void)
{
    printf("usage: fetch|curl [-v] [-k] [-o file] URL, wget [-O file] URL, ping host [-c count],\n"
           "       host|nslookup name, ifconfig, netstat, traceroute host, nc [-u] host port, nc -l [-u] port\n");
    return 2;
}

static int addr_of(const char *host, uint32_t *ip)
{
    long e = net_resolve(host, ip, 0);
    return e < 0 ? fail(host, e) : 0;
}

static int host(const char *name)
{
    uint32_t ip; char a[16];
    if (addr_of(name, &ip)) return 1;
    printf("%s has address %s\n", name, net_ntoa(ip, a));
    return 0;
}

static int ifconfig(void)
{
    net_info_t in;
    char a[16], b[16], c[16], d[16];
    long e = net_info(&in);
    if (e < 0) return fail("netd", e);
    if (!in.up) { printf("no network card\n"); return 1; }
    printf("%s: %s, link %s, card %02x:%02x:%02x:%02x:%02x:%02x, MTU %d\n", in.iface ? "wifi" : "nic0",
           in.iface ? "Wi-Fi" : "the cable", in.link > 0 ? "up" : in.link ? "unknown" : "down",
           in.mac[0], in.mac[1], in.mac[2], in.mac[3], in.mac[4], in.mac[5], in.mtu);
    printf("  address %s/%s, gateway %s, DNS %s\n", net_ntoa(in.ip, a), net_ntoa(in.mask, b), net_ntoa(in.gw, c), net_ntoa(in.dns, d));
    printf("  received %lu packets (%lu bytes), sent %lu (%lu bytes), %d sockets\n",
           in.rx_packets, in.rx_bytes, in.tx_packets, in.tx_bytes, in.sockets);
    printf("  time %ld (seconds since 1970, from the clock chip)\n", (long)in.time);
    return 0;
}

static int ping(const char *name, long count)
{
    uint32_t ip; char a[16];
    if (addr_of(name, &ip)) return 1;
    net_ntoa(ip, a);
    long got = 0;
    for (long k = 0; k < count; k++) {
        long us = net_ping(ip, 2000);
        if (us < 0) printf("%s: %s\n", a, net_strerror(us));
        else { printf("%s: reply in %ld.%03ld ms\n", a, us / 1000, us % 1000); got++; }
        if (k + 1 < count) sys_sleep(us < 0 ? 200000000L : 1000000000L);
    }
    printf("%ld sent, %ld received\n", count, got);
    return !got;
}

static int traceroute(const char *name)
{
    uint32_t ip, from; char a[16];
    if (addr_of(name, &ip)) return 1;
    for (int ttl = 1; ttl <= 30; ttl++) {
        long us = net_ping_ttl(ip, ttl, &from, 2000);
        if (us < 0) { printf("%2d  *\n", ttl); continue; }
        printf("%2d  %s  %ld.%03ld ms\n", ttl, net_ntoa(from, a), us / 1000, us % 1000);
        if (from == ip) return 0;
    }
    return 1;
}

static int netstat(void)
{
    static const char *st[] = { "closed", "listen", "syn-sent", "syn-received", "established", "fin-wait-1",
                                "fin-wait-2", "closing", "time-wait", "close-wait", "last-ack", "no network" };
    static net_sock_t s[64];
    long n = net_sockets(s, 64);
    if (n < 0) return fail("netd", n);
    printf("proto  local  remote                 state         pid\n");
    for (long i = 0; i < n; i++) {
        int tcp = s[i].type == NET_TCP;
        char r[24] = "*", *p;
        if (s[i].rip) {                          /* "a.b.c.d:port" */
            *(p = r + strlen(net_ntoa(s[i].rip, r))) = ':';
            for (unsigned d = 10000; d; d /= 10) if (s[i].rport >= d || d == 1) *++p = (char)('0' + s[i].rport / d % 10);
            p[1] = 0;
        }
        printf("%-6s %-6d %-22s %-13s %d\n", tcp ? "tcp" : "udp", s[i].lport, r,
               tcp && s[i].state < 12 ? st[s[i].state] : "", s[i].owner);
    }
    return 0;
}

/* nc: what comes from the other side is shown by a second thread. The
 * program ends at the end of input (Ctrl-D), or at the next line typed
 * once the other side has closed (a line being read cannot be taken back
 * from the console: ending in the middle would lose the next line typed). */
static long sk;
static uint32_t peer;
static uint16_t pport;
static int udp;
static volatile int closed;

static void nc_shown(void *arg)
{
    (void)arg;
    static char b[4096];
    for (;;) {
        long n = udp ? net_recvfrom(sk, b, sizeof b, &peer, &pport, 0) : net_recv(sk, b, sizeof b, 0);
        if (n <= 0) {
            if (!closed) printf("%s: %s (Enter: end)\n", me, n ? net_strerror(n) : "the other side closed");
            closed = 1;
            return;
        }
        con_write(b, (size_t)n);
    }
}

static int nc(int argc, char **argv)
{
    int listen = 0, k = 1;
    for (; k < argc && argv[k][0] == '-'; k++)
        if (!strcmp(argv[k], "-l")) listen = 1;
        else if (!strcmp(argv[k], "-u")) udp = 1;
        else return usage();
    if (k + 2 - listen != argc) return usage();
    uint16_t port = (uint16_t)strnum(argv[argc - 1]);
    if (!listen && addr_of(argv[k], &peer)) return 1;
    pport = port;
    if (udp) sk = net_udp(listen ? port : 0);
    else if (!listen) sk = net_connect(peer, port, 0);
    else if ((sk = net_listen(port, 1)) >= 0) {
        long l = sk;
        sk = net_accept(l, &peer, &pport, 0);
        net_close(l);
    }
    if (sk < 0) return fail(argv[argc - 1], sk);
    thread_start(nc_shown, 0, 8192);
    static char line[512];
    long n;
    while ((n = con_read(line, sizeof line)) > 0 && !closed) {
        long e = !udp ? net_send(sk, line, (size_t)n) : peer ? net_sendto(sk, peer, pport, line, (size_t)n) : 0;
        if (e < 0) return fail("send", e);
    }
    closed = 1;
    net_close(sk);
    return 0;
}

static int serve(uint16_t port)
{
    long l = net_listen(port, 4);
    if (l < 0) return fail("listen", l);
    printf("listening on port %d\n", port);
    uint32_t ip; uint16_t pp;
    long s = net_accept(l, &ip, &pp, 60000);
    if (s < 0) { net_close(l); return fail("accept", s); }
    char buf[2048], a[16];
    long n = net_recv(s, buf, sizeof buf - 1, 10000);
    buf[n > 0 ? n : 0] = 0;
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 17\r\nConnection: close\r\n\r\n"
                       "hello from SIEOS\n";
    net_send(s, resp, strlen(resp));
    printf("answered %s:%d (%ld bytes of request)\n", net_ntoa(ip, a), pp, n);
    net_close(s);
    net_close(l);
    return 0;
}

static int get(const char *url, const char *out, int verbose, int flags)
{
    static http_t h;
    int64_t t0 = sys_clock();
    long e = http_request(&h, "GET", url, 0, 0, 0, flags, 0);
    if (e < 0) return fail(url, e);
    if (verbose) {
        printf("HTTP %d\n%s", h.status, h.headers);
        if (h.tls) printf("cipher: %s\n", tls_cipher(h.tls));   /* "TLS 1.x ..." */
    }
    long fd = -1;
    if (out && (fd = fs_open(out, FS_WRONLY | FS_CREAT | FS_TRUNC, 0644)) < 0) {
        printf("%s: %s: cannot create (%ld)\n", me, out, fd);
        http_close(&h);
        return 1;
    }
    static char buf[32768];
    uint64_t total = 0;
    long r;
    while ((r = http_read(&h, buf, sizeof buf)) > 0) {
        if (fd >= 0) { if (fs_write(fd, total, buf, (size_t)r) < 0) { printf("%s: write failed\n", me); break; } }
        else con_write(buf, (size_t)r);
        total += (uint64_t)r;
    }
    http_close(&h);
    if (fd >= 0) fs_close(fd);
    int64_t ns = sys_clock() - t0;
    if (r < 0) printf("\n%s: %s\n", me, net_strerror(r));
    if (out || verbose)
        printf("%lu bytes in %ld ms (%lu KB/s), HTTP %d\n", total, (long)(ns / 1000000),
               ns ? total * 1000000000UL / (uint64_t)ns / 1024 : 0, h.status);
    return r < 0 || h.status >= 400;
}

int main(int argc, char **argv)
{
    me = argc ? argv[0] : "fetch";
    const char *a1 = argc > 1 ? argv[1] : 0;
    if (!strcmp(me, "ifconfig")) return ifconfig();
    if (!strcmp(me, "netstat")) return netstat();
    if (!strcmp(me, "nc")) return nc(argc, argv);
    if (!a1) return usage();
    if (!strcmp(me, "host") || !strcmp(me, "nslookup")) return host(a1);
    if (!strcmp(me, "traceroute")) return traceroute(a1);
    if (!strcmp(me, "ping")) return ping(a1, argc > 3 && !strcmp(argv[2], "-c") ? strnum(argv[3]) : 4);
    int verbose = 0, flags = 0, wget = !strcmp(me, "wget");
    const char *out = 0, *url = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-k")) flags |= TLS_INSECURE;
        else if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "-O")) && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-i")) return ifconfig();
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) return host(argv[i + 1]);
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) return ping(argv[i + 1], i + 2 < argc ? strnum(argv[i + 2]) : 3);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) return serve((uint16_t)strnum(argv[i + 1]));
        else if (argv[i][0] == '-') return usage();
        else url = argv[i];
    }
    if (!url) return usage();
    if (wget && !out) {                          /* the URL's last part, else index.html */
        const char *p = strchr(url, ':'), *slash = 0;
        for (p = p && p[1] == '/' && p[2] == '/' ? p + 3 : url; *p; p++) if (*p == '/') slash = p;
        out = slash && slash[1] ? slash + 1 : "index.html";
    }
    return get(url, out, verbose, flags);
}
