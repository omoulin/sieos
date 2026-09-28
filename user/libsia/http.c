/*
 * http.c - Minimal HTTP/1.1 client: one request per connection,
 * Content-Length or chunked response bodies, https via the TLS library.
 */
#include "http.h"
#include "../tls/tls.h"

#ifdef TLS_HOSTED
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#endif
#include <strings.h>
#define sia_strncasecmp strncasecmp

bool url_parse(const char *s, struct url *u)
{
    memset(u, 0, sizeof(*u));
    if (!strncmp(s, "https://", 8)) {
        u->https = true;
        u->port = 443;
        s += 8;
    } else if (!strncmp(s, "http://", 7)) {
        u->port = 80;
        s += 7;
    } else {
        return false;
    }
    size_t n = 0;
    while (*s && *s != '/' && *s != ':' && *s != '?' && n < sizeof(u->host) - 1)
        u->host[n++] = *s++;
    u->host[n] = 0;
    if (!n)
        return false;
    if (*s == ':') {
        s++;
        u->port = 0;
        while (*s >= '0' && *s <= '9')
            u->port = u->port * 10 + (*s++ - '0');
        if (u->port <= 0 || u->port > 65535)
            return false;
    }
    if (*s != '/')
        snprintf(u->path, sizeof(u->path), "/%s", s);
    else
        snprintf(u->path, sizeof(u->path), "%s", s);
    return true;
}

static int tcp_connect(const char *host, int port, int timeout_ms, char *err, size_t errlen)
{
#ifdef TLS_HOSTED
    struct hostent *he = gethostbyname(host);
    if (!he) {
        snprintf(err, errlen, "cannot resolve %s", host);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
    memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        snprintf(err, errlen, "cannot connect to %s:%d", host, port);
        close(fd);
        return -1;
    }
    return fd;
#else
    unsigned int addr;
    if (resolve_host(host, &addr) < 0) {
        snprintf(err, errlen, "cannot resolve %s (check the network and DNS)", host);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "socket: %s", strerror(errno));
        return -1;
    }
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa = make_addr(addr, (unsigned short)port);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        snprintf(err, errlen, "cannot connect to %s:%d: %s", host, port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
#endif
}

struct conn {
    int fd;
    struct tls *tls;
};

static long conn_read(struct conn *c, void *buf, size_t n)
{
    return c->tls ? tls_read(c->tls, buf, n) : read(c->fd, buf, n);
}

static bool conn_write(struct conn *c, const char *buf, size_t n)
{
    while (n) {
        long w = c->tls ? tls_write(c->tls, buf, n) : write(c->fd, buf, n);
        if (w <= 0)
            return false;
        buf += w;
        n -= w;
    }
    return true;
}

/* Find a header value in the header block (case-insensitive name). */
static const char *header(const char *hdrs, const char *name)
{
    size_t nl = strlen(name);
    for (const char *p = hdrs; p && *p; p = strstr(p, "\r\n") ? strstr(p, "\r\n") + 2 : NULL) {
        if (!sia_strncasecmp(p, name, nl) && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ')
                p++;
            return p;
        }
    }
    return NULL;
}

static bool dechunk(const char *in, size_t len, struct sbuf *out)
{
    size_t i = 0;
    for (;;) {
        size_t sz = 0;
        int digits = 0;
        while (i < len) {
            char c = in[i];
            int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (v < 0)
                break;
            sz = sz * 16 + v;
            digits++;
            i++;
        }
        const char *eol = NULL;
        for (size_t j = i; j + 1 < len; j++)
            if (in[j] == '\r' && in[j + 1] == '\n') {
                eol = in + j;
                break;
            }
        if (!digits || !eol)
            return false;
        i = (eol - in) + 2;
        if (sz == 0)
            return true;
        if (i + sz > len)
            return false;
        sb_putn(out, in + i, sz);
        i += sz + 2;
    }
}

int http_request(const char *method, const struct url *u, const char *extra_headers, const char *body,
                 size_t bodylen, struct sbuf *resp, char *err, size_t errlen, int timeout_ms)
{
    struct conn c = { -1, NULL };
    c.fd = tcp_connect(u->host, u->port, timeout_ms, err, errlen);
    if (c.fd < 0)
        return -1;
    if (u->https) {
        c.tls = tls_connect(c.fd, u->host, err, errlen);
        if (!c.tls) {
            close(c.fd);
            return -1;
        }
    }
    struct sbuf req;
    sb_init(&req);
    sb_printf(&req, "%s %s HTTP/1.1\r\n", method, u->path);
    if ((u->https && u->port == 443) || (!u->https && u->port == 80))
        sb_printf(&req, "Host: %s\r\n", u->host);
    else
        sb_printf(&req, "Host: %s:%d\r\n", u->host, u->port);
    sb_puts(&req, "User-Agent: sia/1.0 (SIEOS)\r\nAccept: application/json\r\nConnection: close\r\n");
    if (body)
        sb_printf(&req, "Content-Type: application/json\r\nContent-Length: %lu\r\n", (unsigned long)bodylen);
    if (extra_headers)
        sb_puts(&req, extra_headers);
    sb_puts(&req, "\r\n");
    bool ok = conn_write(&c, req.s, req.len) && (!body || conn_write(&c, body, bodylen));
    memset(req.s, 0, req.len);                         /* holds the API key */
    sb_free(&req);
    if (!ok) {
        snprintf(err, errlen, "connection lost while sending the request");
        goto fail;
    }

    struct sbuf raw;
    sb_init(&raw);
    char buf[4096];
    long n;
    while ((n = conn_read(&c, buf, sizeof(buf))) > 0)
        sb_putn(&raw, buf, n);
    if (n < 0 && raw.len == 0) {
        snprintf(err, errlen, "%s", c.tls && tls_error(c.tls)[0] ? tls_error(c.tls) : "no response (timeout)");
        sb_free(&raw);
        goto fail;
    }
    char *hend = strstr(raw.s, "\r\n\r\n");
    int status = 0;
    if (hend && !strncmp(raw.s, "HTTP/1.", 7) && raw.len > 12 && raw.s[8] == ' ')
        for (int i = 9; i < 12 && raw.s[i] >= '0' && raw.s[i] <= '9'; i++)
            status = status * 10 + (raw.s[i] - '0');
    if (!hend || status < 100) {
        snprintf(err, errlen, "malformed HTTP response");
        sb_free(&raw);
        goto fail;
    }
    *hend = 0;
    const char *te = header(raw.s, "Transfer-Encoding");
    const char *bodyp = hend + 4;
    size_t blen = raw.len - (bodyp - raw.s);
    if (te && !sia_strncasecmp(te, "chunked", 7)) {
        if (!dechunk(bodyp, blen, resp)) {
            snprintf(err, errlen, "truncated chunked response");
            sb_free(&raw);
            goto fail;
        }
    } else {
        const char *cl = header(raw.s, "Content-Length");
        if (cl) {
            size_t want = 0;
            while (*cl >= '0' && *cl <= '9')
                want = want * 10 + (size_t)(*cl++ - '0');
            if (want < blen)
                blen = want;
        }
        sb_putn(resp, bodyp, blen);
    }
    sb_free(&raw);
    if (c.tls)
        tls_close(c.tls);
    close(c.fd);
    return status;
fail:
    if (c.tls)
        tls_close(c.tls);
    close(c.fd);
    return -1;
}
