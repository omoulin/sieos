/*
 * remote.c - sia's backend for a model elsewhere, behind an OpenAI-compatible
 * API: POST {url}/chat/completions with the conversation as JSON, and the
 * answer streamed back as Server-Sent Events, one small piece per line:
 *
 *   data: {"choices":[{"delta":{"content":"Hel"}}]}
 *   data: {"choices":[{"delta":{"content":"lo"}}]}
 *   data: [DONE]
 *
 * Settings (/etc/sia.conf, the key in /etc/sia.key):
 *   url=https://api.example.com/v1   (or http://10.0.2.2:8000/v1 for a server on the host)
 *   api_model=NAME                    the model's name at that service
 *   api_key=...                       sent as "Authorization: Bearer ..."
 * The url may end with /v1, with /chat/completions, or be just the server.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "net.h"
#include "backend.h"

static struct {
    char url[512], model[64], key[256];
    uint64_t pieces; int64_t ns;              /* for the speed in SIA_INFO */
} cfg;

typedef struct {
    http_t *h;
    int open, done, sse;
    char *pend; size_t plen, ppos;            /* decoded text not yet handed out */
    int64_t t0; uint64_t pieces;
} rs_t;

/* ---- A very small JSON reader: find a value by path, decode a string. */
static const char *ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }

static const char *skip(const char *p)       /* past one value, or 0 if malformed */
{
    p = ws(p);
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++) if (*p == '\\' && !*++p) return 0;
        return *p ? p + 1 : 0;
    }
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (close == '}') { if (!(p = skip(p))) return 0; p = ws(p); if (*p++ != ':') return 0; }
            if (!(p = skip(p))) return 0;
            p = ws(p);
            if (*p == ',') { p = ws(p + 1); continue; }
            return *p == close ? p + 1 : 0;
        }
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n') p++;   /* number, true... */
    return p;
}

/* The value at a path such as "choices", "#0", "delta", "content". */
static const char *find(const char *p, const char **path, int n)
{
    for (int k = 0; k < n; k++) {
        p = ws(p);
        if (path[k][0] == '#') {                 /* an array element */
            if (*p != '[') return 0;
            long idx = strnum(path[k] + 1);
            p = ws(p + 1);
            for (long i = 0; i < idx; i++) { if (!(p = skip(p))) return 0; p = ws(p); if (*p++ != ',') return 0; p = ws(p); }
            continue;
        }
        if (*p != '{') return 0;
        p = ws(p + 1);
        size_t kl = strlen(path[k]);
        for (;;) {
            if (*p != '"') return 0;
            int match = !memcmp(p + 1, path[k], kl) && p[1 + kl] == '"';
            if (!(p = skip(p))) return 0;
            p = ws(p);
            if (*p++ != ':') return 0;
            if (match) break;
            if (!(p = skip(p))) return 0;
            p = ws(p);
            if (*p++ != ',') return 0;
            p = ws(p);
        }
    }
    return ws(p);
}

static void put_utf8(char **o, unsigned c)
{
    char *d = *o;
    if (c < 0x80) *d++ = (char)c;
    else if (c < 0x800) { *d++ = (char)(0xc0 | c >> 6); *d++ = (char)(0x80 | (c & 63)); }
    else if (c < 0x10000) { *d++ = (char)(0xe0 | c >> 12); *d++ = (char)(0x80 | (c >> 6 & 63)); *d++ = (char)(0x80 | (c & 63)); }
    else { *d++ = (char)(0xf0 | c >> 18); *d++ = (char)(0x80 | (c >> 12 & 63)); *d++ = (char)(0x80 | (c >> 6 & 63)); *d++ = (char)(0x80 | (c & 63)); }
    *o = d;
}

static unsigned hex4(const char *p)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v = v * 16 + (unsigned)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
    }
    return v;
}

/* A JSON string (p at its opening quote) -> UTF-8 in out (malloc'd); its length. */
static long jstring(const char *p, char **out)
{
    if (*p != '"') return -1;
    const char *e = skip(p);
    if (!e) return -1;
    char *d = *out = malloc((size_t)(e - p) + 4), *o = d;
    if (!d) return -1;
    for (p++; p < e - 1; p++) {
        if (*p != '\\') { *o++ = *p; continue; }
        switch (*++p) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'u': {
            unsigned c = hex4(p + 1);
            p += 4;
            if (c >= 0xd800 && c < 0xdc00 && p[1] == '\\' && p[2] == 'u') {   /* a surrogate pair */
                unsigned lo = hex4(p + 3);
                if (lo >= 0xdc00 && lo < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00); p += 6; }
            }
            put_utf8(&o, c);
            break;
        }
        default: *o++ = *p;                      /* \" \\ \/ */
        }
    }
    *o = 0;
    return o - d;
}

/* ---- JSON out: the request body. */
static size_t jlen(const char *s)            /* length once escaped, quotes included */
{
    size_t n = 2;
    for (; *s; s++) n += (unsigned char)*s < 0x20 ? 6 : (*s == '"' || *s == '\\') ? 2 : 1;
    return n;
}
static char *jput(char *o, const char *s)
{
    *o++ = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { *o++ = '\\'; *o++ = (char)c; }
        else if (c == '\n') { *o++ = '\\'; *o++ = 'n'; }
        else if (c < 0x20) { static const char hx[] = "0123456789abcdef"; memcpy(o, "\\u00", 4); o[4] = hx[c >> 4]; o[5] = hx[c & 15]; o += 6; }
        else *o++ = (char)c;
    }
    *o++ = '"';
    return o;
}
static char *cat(char *o, const char *s) { size_t l = strlen(s); memcpy(o, s, l); return o + l; }

/* ---- The backend. */
static void conf_get(const char *conf, const char *key, char *val, size_t n)
{
    size_t kl = strlen(key);
    val[0] = 0;
    for (const char *p = conf; *p;) {
        if (!memcmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            size_t i = 0;
            while (*p && *p != '\n' && *p != '\r' && i < n - 1) val[i++] = *p++;
            while (i && val[i - 1] == ' ') i--;
            val[i] = 0;
            return;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
}

static int r_init(sia_backend_t *b, const char *conf)
{
    (void)b;
    char url[512];
    conf_get(conf, "url", url, sizeof url);
    conf_get(conf, "api_model", cfg.model, sizeof cfg.model);
    conf_get(conf, "api_key", cfg.key, sizeof cfg.key);
    if (!url[0]) return -EINVAL;
    if (memcmp(url, "http://", 7) && memcmp(url, "https://", 8)) return -EINVAL;
    size_t l = strlen(url);
    while (l && url[l - 1] == '/') url[--l] = 0;
    strlcpy(cfg.url, url, sizeof cfg.url);
    if (l >= 17 && !strcmp(url + l - 17, "/chat/completions")) ;            /* the full path */
    else if (l >= 3 && !strcmp(url + l - 3, "/v1")) strlcpy(cfg.url + l, "/chat/completions", sizeof cfg.url - l);
    else strlcpy(cfg.url + l, "/v1/chat/completions", sizeof cfg.url - l);
    if (!cfg.model[0]) strlcpy(cfg.model, "default", sizeof cfg.model);
    sia_log("sia: remote model %s at %s\n", cfg.model, cfg.url);
    return 0;
}

static void *r_new(sia_backend_t *b) { (void)b; rs_t *s = malloc(sizeof *s); if (s) memset(s, 0, sizeof *s); return s; }

static void r_end(rs_t *s)
{
    if (s->open) { http_close(s->h); s->open = 0; }
    free(s->h); s->h = 0;
    free(s->pend); s->pend = 0; s->plen = s->ppos = 0;
    if (s->pieces) { cfg.pieces = s->pieces; cfg.ns = sys_clock() - s->t0; }
}

static void r_free(sia_backend_t *b, void *v) { (void)b; if (v) { r_end(v); free(v); } }

static void say(rs_t *s, const char *text)    /* text the user will see as the answer */
{
    free(s->pend);
    size_t l = strlen(text);
    s->pend = malloc(l + 1);
    if (s->pend) memcpy(s->pend, text, l + 1);
    s->plen = s->pend ? l : 0; s->ppos = 0;
}

static int r_begin(sia_backend_t *b, void *v, const sia_turn_t *t, int n)
{
    (void)b;
    rs_t *s = v;
    static const char *roles[] = { "system", "user", "assistant" };
    r_end(s);
    s->done = 0; s->pieces = 0; s->t0 = sys_clock();
    size_t need = 128 + jlen(cfg.model);
    for (int i = 0; i < n; i++) need += 32 + jlen(t[i].text);
    char *body = malloc(need), *o = body;
    if (!body || !(s->h = malloc(sizeof *s->h))) { free(body); return -ENOMEM; }
    o = cat(o, "{\"model\":"); o = jput(o, cfg.model);
    o = cat(o, ",\"stream\":true,\"messages\":[");
    for (int i = 0; i < n; i++) {
        if (i) *o++ = ',';
        o = cat(o, "{\"role\":\""); o = cat(o, roles[t[i].role < 3 ? t[i].role : 1]);
        o = cat(o, "\",\"content\":"); o = jput(o, t[i].text); *o++ = '}';
    }
    o = cat(o, "]}");
    char extra[400] = "Content-Type: application/json\r\nAccept: text/event-stream\r\n";
    if (cfg.key[0]) {
        size_t l = strlen(extra);
        strlcpy(extra + l, "Authorization: Bearer ", sizeof extra - l);
        l = strlen(extra);
        strlcpy(extra + l, cfg.key, sizeof extra - l);
        l = strlen(extra);
        strlcpy(extra + l, "\r\n", sizeof extra - l);
    }
    long e = http_request(s->h, "POST", cfg.url, extra, body, (size_t)(o - body), 0, 120000);
    free(body);
    if (e < 0) {
        sia_log("sia: %s: %s\n", cfg.url, net_strerror(e));
        char m[160] = "(the remote model cannot be reached: ";
        size_t l = strlen(m);
        strlcpy(m + l, net_strerror(e), sizeof m - l);
        l = strlen(m); strlcpy(m + l, ")", sizeof m - l);
        say(s, m);
        s->done = 1;
        return 0;
    }
    s->open = 1;
    char ct[128];
    s->sse = http_header(s->h, "content-type", ct, sizeof ct) && strchr(ct, '/') && !memcmp(strchr(ct, '/'), "/event-stream", 13);
    if (s->h->status != 200 || !s->sse) {        /* an error, or a whole answer at once */
        static char resp[32768];
        size_t got = 0;
        long r;
        while (got < sizeof resp - 1 && (r = http_read(s->h, resp + got, sizeof resp - 1 - got)) > 0) got += (size_t)r;
        resp[got] = 0;
        const char *cpath[] = { "choices", "#0", "message", "content" }, *epath[] = { "error", "message" };
        const char *v = s->h->status == 200 ? find(resp, cpath, 4) : find(resp, epath, 2);
        char *txt = 0;
        if (v && jstring(v, &txt) >= 0 && s->h->status == 200) { say(s, txt); s->pieces = 1; }
        else {
            char m[400];
            char num[8] = { (char)('0' + s->h->status / 100), (char)('0' + s->h->status / 10 % 10), (char)('0' + s->h->status % 10), 0 };
            strlcpy(m, "(the remote model answered with an error: HTTP ", sizeof m);
            size_t l = strlen(m); strlcpy(m + l, num, sizeof m - l);
            if (txt) { l = strlen(m); strlcpy(m + l, ", ", sizeof m - l); l = strlen(m); strlcpy(m + l, txt, sizeof m - l); }
            l = strlen(m); strlcpy(m + l, ")", sizeof m - l);
            say(s, m);
            sia_log("sia: %s: HTTP %d\n", cfg.url, s->h->status);
        }
        free(txt);
        s->done = 1;
        http_close(s->h); s->open = 0;
    }
    return 0;
}

static int r_next(sia_backend_t *b, void *v, char *buf, int cap)
{
    (void)b;
    rs_t *s = v;
    while (s->ppos >= s->plen) {                 /* nothing pending: the next event */
        if (s->done || !s->open) { r_end(s); return 0; }
        static char line[65536];
        long r = http_line(s->h, line, sizeof line);
        if (r == -1) { s->done = 1; continue; }  /* the body ended */
        if (r < 0) { sia_log("sia: remote: %s\n", net_strerror(r)); s->done = 1; return -EIO; }
        if (memcmp(line, "data:", 5)) continue;  /* comments, event names, blank lines */
        const char *d = ws(line + 5);
        if (!memcmp(d, "[DONE]", 6)) { s->done = 1; continue; }
        const char *path[] = { "choices", "#0", "delta", "content" };
        const char *val = find(d, path, 4);
        char *txt;
        if (!val || *val != '"' || jstring(val, &txt) < 0) continue;   /* (null content, roles...) */
        free(s->pend);
        s->pend = txt; s->plen = strlen(txt); s->ppos = 0;
        s->pieces++;
    }
    size_t n = s->plen - s->ppos < (size_t)cap ? s->plen - s->ppos : (size_t)cap;
    if (n < s->plen - s->ppos)                   /* do not cut a UTF-8 character */
        while (n && ((unsigned char)s->pend[s->ppos + n] & 0xc0) == 0x80) n--;
    memcpy(buf, s->pend + s->ppos, n);
    s->ppos += n;
    return (int)n;
}

static void r_stop(sia_backend_t *b, void *v) { (void)b; rs_t *s = v; r_end(s); s->done = 1; }

static void r_info(sia_backend_t *b, char *model, int cap, uint64_t *mem, int *ctx, uint32_t *sp)
{
    (void)b;
    strlcpy(model, cfg.model, (size_t)cap);
    *mem = 0; *ctx = 0;
    *sp = cfg.ns > 0 ? (uint32_t)(cfg.pieces * 100000000000ULL / (uint64_t)cfg.ns) : 0;
}

sia_backend_t sia_remote = { "remote", r_init, r_new, r_free, r_begin, r_next, r_stop, r_info, 0, 0 };   /* (tokens: estimated by siad) */
