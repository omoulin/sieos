/*
 * http.c - An HTTP/1.1 client (RFC 9112) over TCP, or TLS for https://.
 *
 * A request is one line ("GET /path HTTP/1.1"), header lines, an empty
 * line, then an optional body. The response is a status line, headers, an
 * empty line, then the body: its length is given (Content-Length), or it
 * comes in "chunks" each preceded by its size in hexadecimal, or it runs
 * until the server closes the connection. One request per connection
 * ("Connection: close"): simple, and enough for fetch and the assistant.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "net.h"

typedef struct { char host[256], path[1024]; uint16_t port; int tls; } url_t;

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int url_parse(url_t *u, const char *s)
{
    if (!memcmp(s, "http://", 7)) { u->tls = 0; u->port = 80; s += 7; }
    else if (!memcmp(s, "https://", 8)) { u->tls = 1; u->port = 443; s += 8; }
    else return -EINVAL;
    size_t i = 0;
    while (*s && *s != '/' && *s != ':' && *s != '?' && i < sizeof u->host - 1) u->host[i++] = *s++;
    u->host[i] = 0;
    if (!i) return -EINVAL;
    if (*s == ':') {
        long p = 0;
        for (s++; *s >= '0' && *s <= '9'; s++) p = p * 10 + *s - '0';
        if (p < 1 || p > 65535) return -EINVAL;
        u->port = (uint16_t)p;
    }
    if (*s != '/' && *s) { u->path[0] = '/'; strlcpy(u->path + 1, s, sizeof u->path - 1); }
    else strlcpy(u->path, *s ? s : "/", sizeof u->path);
    return 0;
}

static long raw_read(http_t *h, void *buf, size_t n)   /* from the connection, through the read-ahead */
{
    if (h->pos == h->len) {
        long r = h->tls ? tls_read(h->tls, h->buf, sizeof h->buf, h->timeout_ms)
                        : net_recv(h->sock, h->buf, sizeof h->buf, h->timeout_ms);
        if (r <= 0) return r;
        h->pos = 0; h->len = (size_t)r;
    }
    size_t c = h->len - h->pos < n ? h->len - h->pos : n;
    memcpy(buf, h->buf + h->pos, c);
    h->pos += c;
    return (long)c;
}

static long raw_line(http_t *h, char *b, size_t n)      /* a line, without its CRLF */
{
    size_t i = 0;
    for (;;) {
        char c;
        long r = raw_read(h, &c, 1);
        if (r <= 0) return r < 0 ? r : -ECONNRESET;
        if (c == '\n') break;
        if (c != '\r' && i < n - 1) b[i++] = c;
    }
    b[i] = 0;
    return (long)i;
}

const char *http_header(http_t *h, const char *name, char *val, size_t n)
{
    size_t nl = strlen(name);
    for (const char *p = h->headers; *p;) {
        size_t i = 0;
        while (i < nl && p[i] && lower(p[i]) == lower(name[i])) i++;
        if (i == nl && p[i] == ':') {
            p += i + 1;
            while (*p == ' ') p++;
            size_t k = 0;
            while (*p && *p != '\r' && *p != '\n' && k < n - 1) val[k++] = *p++;
            val[k] = 0;
            return val;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return 0;
}

static long send_all(http_t *h, const void *b, size_t n)
{
    return h->tls ? tls_write(h->tls, b, n) : net_send(h->sock, b, n);
}

static void append(char *d, size_t cap, const char *s) { size_t l = strlen(d); strlcpy(d + l, s, cap - l); }

void http_close(http_t *h)
{
    if (h->tls) tls_close(h->tls);
    else if (h->sock > 0) net_close(h->sock);
    h->tls = 0; h->sock = 0;
}

long http_request(http_t *h, const char *method, const char *url, const char *extra,
                  const void *body, size_t blen, int flags, unsigned timeout_ms)
{
    static url_t u;
    static char cur[1300], req[3072];
    strlcpy(cur, url, sizeof cur);
    for (int hops = 0; ; hops++) {
        memset(h, 0, sizeof *h);
        h->timeout_ms = timeout_ms ? timeout_ms : 30000;
        long e = url_parse(&u, cur);
        if (e) return e;
        uint32_t ip;
        if ((e = net_resolve(u.host, &ip, h->timeout_ms)) < 0) return e;
        long s = net_connect(ip, u.port, h->timeout_ms);
        if (s < 0) return s;
        h->sock = s;
        if (u.tls && !(h->tls = tls_connect(s, u.host, flags, &e))) { h->sock = 0; return e; }
        char num[24];
        req[0] = 0;
        append(req, sizeof req, method); append(req, sizeof req, " ");
        append(req, sizeof req, u.path); append(req, sizeof req, " HTTP/1.1\r\nHost: ");
        append(req, sizeof req, u.host);
        if (u.port != (u.tls ? 443 : 80)) {
            char *p = num + sizeof num - 1; *p = 0;
            unsigned v = u.port; do *--p = (char)('0' + v % 10); while (v /= 10);
            append(req, sizeof req, ":"); append(req, sizeof req, p);
        }
        append(req, sizeof req, "\r\nUser-Agent: SIEOS/1\r\nAccept: */*\r\nConnection: close\r\n");
        if (body || !strcmp(method, "POST")) {
            char *p = num + sizeof num - 1; *p = 0;
            size_t v = blen; do *--p = (char)('0' + v % 10); while (v /= 10);
            append(req, sizeof req, "Content-Length: "); append(req, sizeof req, p); append(req, sizeof req, "\r\n");
        }
        if (extra) append(req, sizeof req, extra);
        append(req, sizeof req, "\r\n");
        if ((e = send_all(h, req, strlen(req))) < 0 || (blen && (e = send_all(h, body, blen)) < 0)) { http_close(h); return e; }
        /* the status line and headers */
        char line[1024];
        if ((e = raw_line(h, line, sizeof line)) < 0) { http_close(h); return e; }
        if (memcmp(line, "HTTP/1.", 7) || line[8] != ' ') { http_close(h); return -EPROTO; }
        h->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
        for (;;) {
            if ((e = raw_line(h, line, sizeof line)) < 0) { http_close(h); return e; }
            if (!line[0]) break;
            append(h->headers, sizeof h->headers, line);
            append(h->headers, sizeof h->headers, "\n");
        }
        char v[1024];
        h->left = -1;
        if (http_header(h, "transfer-encoding", v, sizeof v) && strchr(v, 'c')) { h->chunked = 1; h->left = 0; }
        else if (http_header(h, "content-length", v, sizeof v)) { h->left = strnum(v); if (h->left < 0) h->left = -1; }
        if (!strcmp(method, "HEAD") || h->status == 204 || h->status == 304) h->done = 1;
        int redirect = h->status == 301 || h->status == 302 || h->status == 303 || h->status == 307 || h->status == 308;
        if (redirect && !strcmp(method, "GET") && hops < 5 && http_header(h, "location", v, sizeof v)) {
            http_close(h);
            if (v[0] == '/') {                   /* relative: same scheme, host and port */
                char base[300];
                base[0] = 0;
                append(base, sizeof base, u.tls ? "https://" : "http://");
                append(base, sizeof base, u.host);
                if (u.port != (u.tls ? 443 : 80)) {
                    char *p = num + sizeof num - 1; *p = 0;
                    unsigned pv = u.port; do *--p = (char)('0' + pv % 10); while (pv /= 10);
                    append(base, sizeof base, ":"); append(base, sizeof base, p);
                }
                strlcpy(cur, base, sizeof cur); append(cur, sizeof cur, v);
            } else strlcpy(cur, v, sizeof cur);
            continue;
        }
        return 0;
    }
}

static long hexnum(const char *s)
{
    long v = 0;
    int d = 0;
    for (; *s; s++, d++) {
        int x = *s >= '0' && *s <= '9' ? *s - '0' : lower(*s) >= 'a' && lower(*s) <= 'f' ? lower(*s) - 'a' + 10 : -1;
        if (x < 0) break;
        if (v > (1L << 40)) return -1;
        v = v * 16 + x;
    }
    return d ? v : -1;
}

long http_read(http_t *h, void *buf, size_t n)
{
    if (h->done) return 0;
    if (h->chunked && h->left == 0) {            /* the next chunk's size */
        char line[128];
        long e = raw_line(h, line, sizeof line);
        if (e < 0) return e;
        long sz = hexnum(line);
        if (sz < 0) return -EPROTO;
        if (sz == 0) {                            /* the last chunk: skip the trailer */
            while ((e = raw_line(h, line, sizeof line)) > 0) ;
            h->done = 1;
            return 0;
        }
        h->left = sz;
    }
    if (h->left >= 0 && (int64_t)n > h->left) n = (size_t)h->left;
    if (h->left == 0) { h->done = 1; return 0; }
    long r = raw_read(h, buf, n);
    if (r == 0 && h->left < 0) { h->done = 1; return 0; }   /* until closed: closed */
    if (r <= 0) return r < 0 ? r : -ECONNRESET;
    if (h->left > 0) h->left -= r;
    if (h->chunked && h->left == 0) { char crlf[4]; raw_line(h, crlf, sizeof crlf); }
    else if (!h->chunked && h->left == 0) h->done = 1;
    return r;
}

long http_line(http_t *h, char *b, size_t n)
{
    size_t i = 0;
    for (;;) {
        char c;
        long r = http_read(h, &c, 1);
        if (r < 0) return r;
        if (r == 0) { if (!i) return -1; break; }   /* -1: the body ended */
        if (c == '\n') break;
        if (c != '\r' && i < n - 1) b[i++] = c;
    }
    b[i] = 0;
    return (long)i;
}
