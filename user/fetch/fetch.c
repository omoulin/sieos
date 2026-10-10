/*
 * fetch - Get a URL (http:// or https://), and a few network checks.
 *
 *   fetch [-v] [-k] [-o FILE] URL   the body on the screen, or into FILE
 *                                   -v: status and headers; -k: do not check
 *                                   the certificate (tests only)
 *   fetch -i                        the network's configuration and counters
 *   fetch -p HOST [COUNT]           ping (ICMP echo)
 *   fetch -r HOST                   the address of a name (DNS)
 *   fetch -l PORT                   answer one HTTP request on PORT (a test)
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "net.h"

static int usage(void)
{
    printf("usage: fetch [-v] [-k] [-o file] URL | -i | -p host [count] | -r host | -l port\n");
    return 2;
}

static int addr_of(const char *host, uint32_t *ip)
{
    long e = net_resolve(host, ip, 0);
    if (e < 0) printf("fetch: %s: %s\n", host, net_strerror(e));
    return e < 0;
}

static int info(void)
{
    net_info_t in;
    char a[16], b[16], c[16], d[16];
    long e = net_info(&in);
    if (e < 0) { printf("fetch: %s\n", net_strerror(e)); return 1; }
    if (!in.up) { printf("no network card\n"); return 1; }
    printf("address %s/%s, gateway %s, DNS %s\n", net_ntoa(in.ip, a), net_ntoa(in.mask, b), net_ntoa(in.gw, c), net_ntoa(in.dns, d));
    printf("card %02x:%02x:%02x:%02x:%02x:%02x, MTU %d\n", in.mac[0], in.mac[1], in.mac[2], in.mac[3], in.mac[4], in.mac[5], in.mtu);
    printf("received %lu packets (%lu bytes), sent %lu (%lu bytes), %d sockets\n",
           in.rx_packets, in.rx_bytes, in.tx_packets, in.tx_bytes, in.sockets);
    printf("time %ld (seconds since 1970, from the clock chip)\n", (long)in.time);
    return 0;
}

static int serve(uint16_t port)
{
    long l = net_listen(port, 4);
    if (l < 0) { printf("fetch: listen %d: %s\n", port, net_strerror(l)); return 1; }
    printf("listening on port %d\n", port);
    uint32_t ip; uint16_t pp;
    long s = net_accept(l, &ip, &pp, 60000);
    if (s < 0) { printf("fetch: accept: %s\n", net_strerror(s)); net_close(l); return 1; }
    char buf[2048], a[16];
    long n = net_recv(s, buf, sizeof buf - 1, 10000);
    buf[n > 0 ? n : 0] = 0;
    const char *body = "hello from SIEOS\n";
    char resp[256] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 17\r\nConnection: close\r\n\r\n";
    size_t rl = strlen(resp);
    strlcpy(resp + rl, body, sizeof resp - rl);
    net_send(s, resp, strlen(resp));
    printf("answered %s:%d (%ld bytes of request)\n", net_ntoa(ip, a), pp, n);
    net_close(s);
    net_close(l);
    return 0;
}

int main(int argc, char **argv)
{
    int verbose = 0, flags = 0;
    const char *out = 0, *url = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-k")) flags |= TLS_INSECURE;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-i")) return info();
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            uint32_t ip; char a[16];
            if (addr_of(argv[i + 1], &ip)) return 1;
            printf("%s: %s\n", argv[i + 1], net_ntoa(ip, a));
            return 0;
        } else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            uint32_t ip; char a[16];
            long count = i + 2 < argc ? strnum(argv[i + 2]) : 3;
            if (addr_of(argv[i + 1], &ip)) return 1;
            int got = 0;
            for (long k = 0; k < count; k++) {
                long us = net_ping(ip, 2000);
                if (us < 0) printf("%s: %s\n", net_ntoa(ip, a), net_strerror(us));
                else { printf("%s: reply in %ld.%03ld ms\n", net_ntoa(ip, a), us / 1000, us % 1000); got++; }
                if (k + 1 < count) sys_sleep(200000000L);
            }
            return got ? 0 : 1;
        } else if (!strcmp(argv[i], "-l") && i + 1 < argc) return serve((uint16_t)strnum(argv[i + 1]));
        else if (argv[i][0] == '-') return usage();
        else url = argv[i];
    }
    if (!url) return usage();
    static http_t h;
    int64_t t0 = sys_clock();
    long e = http_request(&h, "GET", url, 0, 0, 0, flags, 0);
    if (e < 0) { printf("fetch: %s: %s\n", url, net_strerror(e)); return 1; }
    if (verbose) {
        printf("HTTP %d%s\n%s", h.status, h.tls ? (const char *)" (TLS 1.3)" : "", h.headers);
        if (h.tls) printf("cipher: %s\n", tls_cipher(h.tls));
    }
    long fd = -1;
    if (out && (fd = fs_open(out, FS_WRONLY | FS_CREAT | FS_TRUNC, 0644)) < 0) {
        printf("fetch: %s: cannot create (%ld)\n", out, fd);
        http_close(&h);
        return 1;
    }
    static char buf[32768];
    uint64_t total = 0;
    long r;
    while ((r = http_read(&h, buf, sizeof buf)) > 0) {
        if (fd >= 0) { if (fs_write(fd, total, buf, (size_t)r) < 0) { printf("fetch: write failed\n"); break; } }
        else con_write(buf, (size_t)r);
        total += (uint64_t)r;
    }
    http_close(&h);
    if (fd >= 0) fs_close(fd);
    int64_t ns = sys_clock() - t0;
    if (r < 0) printf("\nfetch: %s\n", net_strerror(r));
    if (out || verbose)
        printf("%lu bytes in %ld ms (%lu KB/s), HTTP %d\n", total, (long)(ns / 1000000),
               ns ? total * 1000000000UL / (uint64_t)ns / 1024 : 0, h.status);
    return r < 0 || h.status >= 400;
}
