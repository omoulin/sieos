/*
 * tests/net/tlsclient.c - user/lib/tls.c and x509.c built on the host, to
 * talk to openssl s_server (tests/net/tls.py). Usage:
 *   tlsclient [-k] PORT NAME ROOTS   connects to 127.0.0.1:PORT as NAME,
 * trusting the DER certificates in ROOTS, sends "GET /", prints the cipher
 * and the answer's first line. -k: no certificate check.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "net.h"
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/random.h>
#include <sys/socket.h>

static const char *roots;

long sys_random(void *buf, size_t n) { return getrandom(buf, n, 0); }
long net_close(long s) { return close((int)s); }
long net_info(net_info_t *in) { in->time = time(0); return 0; }

long net_send(long s, const void *buf, size_t n)
{
    for (size_t off = 0; off < n;) {
        ssize_t r = send((int)s, (const char *)buf + off, n - off, MSG_NOSIGNAL);
        if (r <= 0) return -ECONNRESET;
        off += (size_t)r;
    }
    return (long)n;
}

long net_recv(long s, void *buf, size_t n, unsigned timeout_ms)
{
    struct pollfd p = { (int)s, POLLIN, 0 };
    if (poll(&p, 1, timeout_ms ? (int)timeout_ms : 20000) <= 0) return -ETIMEDOUT;
    ssize_t r = recv((int)s, buf, n, 0);
    return r < 0 ? -ECONNRESET : r;
}

char *file_get(const char *path, size_t *len)
{
    FILE *f = fopen(strcmp(path, "/etc/ssl/roots") ? path : roots, "rb");
    if (!f) return 0;
    char *b = malloc(1 << 20);
    *len = fread(b, 1, (1 << 20) - 1, f);
    b[*len] = 0;
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    int flags = TLS_H2;                         /* h2 offered: the answer printed (ALPN) */
    if (argc > 1 && !strcmp(argv[1], "-k")) { flags |= TLS_INSECURE; argv++; argc--; }
    if (argc != 4) { fprintf(stderr, "usage: tlsclient [-k] PORT NAME ROOTS\n"); return 2; }
    roots = argv[3];
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[1])) };
    a.sin_addr.s_addr = htonl(0x7f000001);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { perror("connect"); return 1; }
    long e;
    tls_t *t = tls_connect(s, argv[2], flags, &e);
    if (!t) { printf("handshake failed: %ld\n", e); return 1; }
    printf("cipher: %s\n", tls_cipher(t));
    printf("alpn: %s\n", tls_alpn(t) ? tls_alpn(t) : "none");
    const char *req = "GET / HTTP/1.0\r\n\r\n";
    tls_write(t, req, strlen(req));
    char buf[65536];
    size_t n = 0;
    long r;
    while (n < sizeof buf - 1 && (r = tls_read(t, buf + n, sizeof buf - 1 - n, 10000)) > 0) n += (size_t)r;
    buf[n] = 0;
    char *nl = strchr(buf, '\r');
    if (nl) *nl = 0;
    printf("got %zu bytes: %s\n", n, buf);
    tls_close(t);
    return strncmp(buf, "HTTP/1.0 200", 12) != 0;
}
