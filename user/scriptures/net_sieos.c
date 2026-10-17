/*
 * net_sieos.c - HTTPS on SIEOS (net.h): SIEOS's own HTTP/1.1 client and TLS
 * (user/lib/http.c, tls.c; certificates checked against /etc/ssl/roots).
 * GET, POST, PUT and DELETE, with the app's headers and the Church Account's
 * cookie; the whole body read into memory.
 *
 * Another site: a file "site" in the data folder (~/.scriptures/site) holding
 * an address ("https://10.0.2.2:8443") replaces SITE's for every request: a
 * mirror, or the test server of make scriptures-test.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#define http_t lib_http_t                /* (user/lib/net.h's response, not the app's) */
#include "../lib/net.h"
#undef http_t
#include "net.h"
#include "store.h"
#include "util.h"

int snprintf(char *s, size_t n, const char *fmt, ...);

#define UA "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36 scriptures/0.2 SIEOS"

static char *site;                       /* the replacement for SITE, or 0 */
static char site_lock, site_read;

static const char *base(void)
{
    while (__atomic_test_and_set(&site_lock, __ATOMIC_ACQUIRE)) sys_yield();
    if (!site_read) {
        site_read = 1;
        char *p = xprintf("%s/site", store_dir()), *s = read_file(p, NULL);
        free(p);
        if (s) {
            size_t n = strlen(s);
            while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '/')) s[--n] = 0;
            if (n) site = s; else free(s);
        }
    }
    __atomic_clear(&site_lock, __ATOMIC_RELEASE);
    return site;
}

int http_do(const char *method, const char *url, const char *cookie, const char *body, http_t *out)
{
    memset(out, 0, sizeof *out);
    const char *b = base();
    char *u = b && !memcmp(url, SITE, sizeof SITE - 1) ? xprintf("%s%s", b, url + sizeof SITE - 1) : xstrdup(url);
    buf_t hdr = {0};
    buf_puts(&hdr, "User-Agent: " UA "\r\nAccept: application/json\r\n");
    if (cookie) buf_printf(&hdr, "Cookie: %s\r\nOrigin: " SITE "\r\nReferer: " SITE "/notes\r\n", cookie);
    if (body) buf_puts(&hdr, "Content-Type: application/json\r\n");
    lib_http_t *h = xmalloc(sizeof *h);
    long e = http_request(h, method, u, hdr.p, body, body ? strlen(body) : 0, 0, 60000);
    buf_free(&hdr);
    free(u);
    buf_t got = {0};
    if (!e) {
        char tmp[4096];
        long n;
        while ((n = http_read(h, tmp, sizeof tmp)) > 0) buf_add(&got, tmp, (size_t)n);
        if (n < 0) e = n;                    /* (cut short: no answer, as curl would say) */
        else out->status = h->status;
    }
    http_close(h);
    free(h);
    if (e && !out->status) out->err = xstrdup(net_strerror(e));
    out->len = got.n;
    out->body = got.p ? buf_take(&got) : xstrdup("");
    return out->status ? 0 : -1;
}

void http_free(http_t *h) { free(h->body); free(h->err); memset(h, 0, sizeof *h); }

char *url_escape(const char *s)
{
    buf_t b = {0};
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            buf_putc(&b, c);
        else buf_printf(&b, "%%%02X", c);
    }
    return b.p ? buf_take(&b) : xstrdup("");
}
