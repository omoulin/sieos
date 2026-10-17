/*
 * mime.c - Reading a mail message as text: its headers (RFC 5322, folded
 * lines unfolded), encoded words in them (RFC 2047: =?UTF-8?B?...?=), and
 * its MIME parts: the text/plain part is the text (else the HTML one,
 * its tags stripped, crudely), quoted-printable and base64 decoded,
 * Latin-1 turned into UTF-8; other parts are listed by name, and
 * decoded on demand (mime_part: saving, forwarding).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mail.h"

/* ---- A growing buffer. */
void b_add(buf_t *b, const void *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 256;
        while (c < b->len + n + 1) c *= 2;
        char *p = realloc(b->s, c);
        if (!p) return;
        b->s = p; b->cap = c;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}
void b_str(buf_t *b, const char *s) { b_add(b, s, strlen(s)); }
static void b_put(void *ctx, char c) { b_add(ctx, &c, 1); }
void b_fmt(buf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vformat(b_put, b, fmt, ap);
    va_end(ap);
}
void b_free(buf_t *b) { free(b->s); b->s = 0; b->len = b->cap = 0; }

int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int same_ci(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) if (lower((uint8_t)a[i]) != lower((uint8_t)b[i]) || !b[i]) return 0;
    return 1;
}
const char *find_ci(const char *s, const char *end, const char *what)
{
    size_t n = strlen(what);
    for (; s + n <= end; s++) if (same_ci(s, what, n)) return s;
    return 0;
}

/* ---- Headers. */
static const char *body_of(const char *m, size_t len)   /* after the blank line (or the end) */
{
    const char *e = m + len;
    for (const char *p = m; p < e; ) {
        if (*p == '\n') return p + 1;
        if (*p == '\r' && p + 1 < e && p[1] == '\n') return p + 2;
        while (p < e && *p != '\n') p++;
        p++;
    }
    return e;
}

int mime_header(const char *m, size_t len, const char *name, char *out, size_t cap)
{
    const char *e = body_of(m, len);
    size_t nl = strlen(name), o = 0;
    for (const char *p = m; p < e; ) {
        const char *l = p;
        while (p < e && *p != '\n') p++;
        p++;
        if (!(same_ci(l, name, nl) && l[nl] == ':')) continue;
        for (l += nl + 1; ; ) {                          /* the value, and its folded lines */
            while (l < p && (*l == ' ' || *l == '\t')) l++;
            if (o && o + 1 < cap) out[o++] = ' ';
            for (; l < p && *l != '\r' && *l != '\n'; l++) if (o + 1 < cap) out[o++] = *l;
            if (p >= e || (*p != ' ' && *p != '\t')) break;
            for (l = p; p < e && *p != '\n'; ) p++;
            p++;
        }
        out[o] = 0;
        return 1;
    }
    out[0] = 0;
    return 0;
}

/* A parameter of a header's value ("boundary", "charset", "name"...). */
static void param(const char *v, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name), o = 0;
    out[0] = 0;
    for (const char *p = strchr(v, ';'); p; p = strchr(p + 1, ';')) {
        const char *q = p + 1;
        while (*q == ' ' || *q == '\t') q++;
        if (!same_ci(q, name, nl)) continue;
        q += nl;
        int ext = *q == '*';                             /* RFC 2231: name*=UTF-8''file.pdf */
        if (ext) q++;
        if (*q != '=') continue;
        q++;
        if (ext) { const char *a = strchr(q, '\''); if (a && strchr(a + 1, '\'')) q = strchr(a + 1, '\'') + 1; }
        int quoted = *q == '"';
        if (quoted) q++;
        for (; *q && (quoted ? *q != '"' : *q != ';' && *q != ' '); q++) if (o + 1 < cap) out[o++] = *q;
        out[o] = 0;
        return;
    }
}

/* ---- Decoding. */
int hexv(int c) { c = lower(c); return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
static int b64v(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 :
           c == '+' ? 62 : c == '/' ? 63 : -1;
}
static void b64_dec(const char *s, size_t n, buf_t *o)
{
    unsigned acc = 0, bits = 0;
    for (size_t i = 0; i < n && s[i] != '='; i++) {
        int v = b64v((uint8_t)s[i]);
        if (v < 0) continue;
        acc = acc << 6 | v; bits += 6;
        if (bits >= 8) { bits -= 8; char c = (char)(acc >> bits); b_add(o, &c, 1); }
    }
}
static void qp_dec(const char *s, size_t n, buf_t *o, int words)   /* words: '_' is a space (RFC 2047) */
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '=' && i + 1 < n && (s[i + 1] == '\n' || (s[i + 1] == '\r' && i + 2 < n && s[i + 2] == '\n'))) { i += s[i + 1] == '\r' ? 2 : 1; continue; }
        if (c == '=' && i + 2 < n && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) { c = (char)(hexv(s[i + 1]) << 4 | hexv(s[i + 2])); i += 2; }
        else if (c == '_' && words) c = ' ';
        b_add(o, &c, 1);
    }
}
static int latin1(const char *cs)                         /* a charset we turn into UTF-8 byte by byte */
{
    return same_ci(cs, "iso-8859-1", 10) || same_ci(cs, "iso-8859-15", 11) || same_ci(cs, "windows-1252", 12) ||
           same_ci(cs, "latin1", 6) || same_ci(cs, "iso_8859-1", 10);
}
/* Text into the output, as UTF-8, without control characters but \n and \t (and no \r). */
static void put_text(buf_t *o, const char *s, size_t n, const char *cs)
{
    int l1 = cs && latin1(cs);
    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c >= 0x80 && l1) { char u[2] = { (char)(0xc0 | c >> 6), (char)(0x80 | (c & 63)) }; b_add(o, u, 2); continue; }
        if ((c < 32 && c != '\n' && c != '\t') || c == 127) continue;
        b_add(o, &c, 1);
    }
}

void mime_words(const char *in, char *out, size_t cap)
{
    buf_t o = { 0 }, d = { 0 };
    const char *p = in, *gap = 0;                       /* gap: after an encoded word */
    while (*p) {
        const char *w = p[0] == '=' && p[1] == '?' ? p : 0, *cs = 0, *q = 0, *end = 0;
        if (w) {                                         /* =?charset?enc?text?= */
            cs = w + 2;
            q = strchr(cs, '?');
            if (q && q[1] && q[2] == '?') { end = q + 3; while (*end && !(end[0] == '?' && end[1] == '=')) end++; }
            if (!end || !*end) w = 0;
        }
        if (!w) {
            if (gap && *p != ' ' && *p != '\t') { put_text(&o, gap, p - gap, 0); gap = 0; }
            if (!gap) put_text(&o, p, 1, 0);
            p++;
            continue;
        }
        gap = 0;
        char c[32];
        size_t cl = q - cs < 31 ? (size_t)(q - cs) : 31;
        memcpy(c, cs, cl); c[cl] = 0;
        d.len = 0;
        if (lower(q[1]) == 'b') b64_dec(q + 3, end - q - 3, &d); else qp_dec(q + 3, end - q - 3, &d, 1);
        put_text(&o, d.s ? d.s : "", d.len, c);
        p = end + 2;
        gap = p;                                         /* (blanks between two words go) */
        if (!*p) break;
    }
    strlcpy(out, o.s ? o.s : "", cap);
    b_free(&o); b_free(&d);
}

void mime_date(const char *s, char out[17])
{
    static const char mon[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    int v[5] = { 0 }, k = 0, m = 0;                     /* day, year, hour, minute */
    out[0] = 0;
    if (strchr(s, ',')) s = strchr(s, ',') + 1;
    for (; *s && k < 4; ) {
        while (*s == ' ' || *s == ':') s++;
        if (*s >= '0' && *s <= '9') { int n = 0; while (*s >= '0' && *s <= '9') n = n * 10 + *s++ - '0'; v[k++] = n; continue; }
        if (!m && *s) for (int i = 0; i < 12; i++) if (same_ci(s, mon + i * 3, 3)) m = i + 1;
        while (*s && *s != ' ') s++;
    }
    if (!m || k < 4) return;
    if (v[1] < 100) v[1] += 2000;
    int d[5] = { v[1], m, v[0], v[2], v[3] }, w[5] = { 4, 2, 2, 2, 2 };
    char *o = out;
    for (int i = 0; i < 5; i++) {
        for (int j = w[i] - 1, n = d[i]; j >= 0; j--, n /= 10) o[j] = '0' + n % 10;
        o += w[i];
        if (i < 4) *o++ = i < 2 ? '-' : i == 2 ? ' ' : ':';
    }
    *o = 0;
}

size_t b64_encode(const void *in, size_t n, char *out)
{
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *s = in;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = s[i] << 16 | (i + 1 < n ? s[i + 1] << 8 : 0) | (i + 2 < n ? s[i + 2] : 0);
        out[o++] = a[v >> 18]; out[o++] = a[v >> 12 & 63];
        out[o++] = i + 1 < n ? a[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? a[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

/* ---- HTML, crudely: the text between the tags, a line break for the
 * tags that make one, no style or script, the common entities. */
static void put_utf8(buf_t *o, unsigned u)
{
    char b[4];
    if (u < 0x80) { b[0] = (char)u; b_add(o, b, 1); }
    else if (u < 0x800) { b[0] = (char)(0xc0 | u >> 6); b[1] = (char)(0x80 | (u & 63)); b_add(o, b, 2); }
    else { b[0] = (char)(0xe0 | (u >> 12 & 15)); b[1] = (char)(0x80 | (u >> 6 & 63)); b[2] = (char)(0x80 | (u & 63)); b_add(o, b, 3); }
}
static void strip_html(const char *s, size_t n, buf_t *o)
{
    static const char *const brk[] = { "br", "p", "div", "tr", "li", "h1", "h2", "h3", "table", 0 };
    static const char *const ent[] = { "amp&", "lt<", "gt>", "quot\"", "nbsp ", "apos'", 0 };
    const char *e = s + n;
    int space = 0;
    while (s < e) {
        if (*s == '<') {
            const char *t = s + 1 + (s[1] == '/');
            if (same_ci(t, "style", 5) || same_ci(t, "script", 6) || same_ci(t, "head", 4)) {   /* skip to its end tag */
                char close[10] = "</";
                size_t k = 2;
                while (k < 9 && ((t[k - 2] >= 'a' && t[k - 2] <= 'z') || (t[k - 2] >= 'A' && t[k - 2] <= 'Z'))) { close[k] = t[k - 2]; k++; }
                close[k] = 0;
                const char *c = find_ci(t, e, close);
                s = c ? c + k : e;
            }
            for (int i = 0; brk[i]; i++) {
                size_t l = strlen(brk[i]);
                if (same_ci(t, brk[i], l) && (t[l] == '>' || t[l] == ' ' || t[l] == '/')) { b_str(o, "\n"); space = 0; }
            }
            while (s < e && *s != '>') s++;
            s++;
            continue;
        }
        if (*s == '&') {
            const char *x = s + 1;
            int done = 0;
            if (*x == '#') {               /* &#233; &#xe9; */
                unsigned u = 0, hex = lower(x[1]) == 'x';
                for (x += 1 + hex; x < e && *x != ';' && x - s < 10; x++) u = hex ? u * 16 + hexv(*x) : u * 10 + (*x - '0');
                if (x < e && *x == ';') { put_utf8(o, u); s = x + 1; done = 1; }
            }
            for (int i = 0; !done && ent[i]; i++) {
                size_t l = strlen(ent[i]) - 1;
                if (same_ci(x, ent[i], l) && x[l] == ';') { b_add(o, ent[i] + l, 1); s = x + l + 1; done = 1; }
            }
            if (done) { space = 0; continue; }
        }
        if (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') { if (!space && o->len && o->s[o->len - 1] != '\n') b_str(o, " "); space = 1; s++; continue; }
        space = 0;
        b_add(o, s++, 1);
    }
}

/* ---- The parts. */
#define MAXATT 16
typedef struct { const char *p; size_t n; char cte[24], cs[32], ct[64], name[160]; } part_t;
typedef struct { part_t plain, html, at[MAXATT]; int nat; } walk_t;
static walk_t w;                         /* (the message walked last) */

static void walk(const char *m, size_t len, int depth)
{
    char ct[256], v[128], disp[160], name[128];
    if (!mime_header(m, len, "content-type", ct, sizeof ct)) strlcpy(ct, "text/plain", sizeof ct);
    const char *b = body_of(m, len), *e = m + len;
    if (same_ci(ct, "multipart/", 10) && depth < 8) {
        param(ct, "boundary", v, sizeof v);
        size_t bl = strlen(v);
        if (!bl) return;
        const char *start = 0;
        for (const char *p = b; p + bl + 2 <= e; ) {      /* lines "--boundary" */
            if (p[0] == '-' && p[1] == '-' && !memcmp(p + 2, v, bl)) {
                if (start) {
                    const char *pe = p;                   /* (the line break before belongs to the delimiter) */
                    if (pe > start && pe[-1] == '\n') pe--;
                    if (pe > start && pe[-1] == '\r') pe--;
                    walk(start, pe - start, depth + 1);
                }
                if (p + bl + 4 <= e && p[bl + 2] == '-' && p[bl + 3] == '-') return;
                while (p < e && *p != '\n') p++;
                start = ++p;
                continue;
            }
            while (p < e && *p != '\n') p++;
            p++;
        }
        return;
    }
    mime_header(m, len, "content-disposition", disp, sizeof disp);
    param(disp, "filename", name, sizeof name);
    if (!name[0]) param(ct, "name", name, sizeof name);
    int att = same_ci(disp, "attachment", 10) || name[0];
    part_t *t = att ? 0 : same_ci(ct, "text/plain", 10) ? &w.plain : same_ci(ct, "text/html", 9) ? &w.html : 0;
    if (t && t->p) return;                               /* (a second text part: not shown) */
    if (!t && w.nat == MAXATT) return;
    if (!t) t = &w.at[w.nat++];
    t->p = b; t->n = e - b;
    mime_header(m, len, "content-transfer-encoding", t->cte, sizeof t->cte);
    param(ct, "charset", t->cs, sizeof t->cs);
    mime_words(name, t->name, sizeof t->name);
    for (char *c = ct; *c; c++) if (*c == ';') *c = 0;
    strlcpy(t->ct, ct, sizeof t->ct);
}
static void decode(const part_t *t, buf_t *d)            /* its bytes: base64, quoted-printable or as they are */
{
    if (same_ci(t->cte, "base64", 6)) b64_dec(t->p, t->n, d);
    else if (same_ci(t->cte, "quoted-printable", 16)) qp_dec(t->p, t->n, d, 0);
    else b_add(d, t->p, t->n);
}

void mime_read(const char *m, size_t len, buf_t *out)
{
    static const char *const hs[] = { "From", "Reply-To", "To", "Cc", "Date", "Subject", "Message-ID", 0 };
    char raw[1024], dec[1024];
    for (int i = 0; hs[i]; i++) {
        if (!mime_header(m, len, hs[i], raw, sizeof raw) && i != 5) continue;
        mime_words(raw, dec, sizeof dec);
        b_fmt(out, "%s: %s\n", hs[i], dec);
    }

    memset(&w, 0, sizeof w);
    walk(m, len, 0);
    for (int i = 0; i < w.nat; i++)
        b_fmt(out, "Attachment: %s (%s, %lu KB)\n", w.at[i].name[0] ? w.at[i].name : "unnamed", w.at[i].ct, (unsigned long)(w.at[i].n / 1024 + 1));
    b_str(out, "\n");
    part_t *t = w.plain.p ? &w.plain : w.html.p ? &w.html : 0;
    if (t) {
        buf_t d = { 0 }, h = { 0 };
        decode(t, &d);
        if (t == &w.html && d.s) { strip_html(d.s, d.len, &h); b_free(&d); d = h; }
        if (d.s) put_text(out, d.s, d.len, t->cs);
        b_free(&d);
    }
    if (!out->len || out->s[out->len - 1] != '\n') b_str(out, "\n");
}

int mime_part(const char *m, size_t len, int n, char *name, size_t cap, buf_t *data)
{

    memset(&w, 0, sizeof w);
    walk(m, len, 0);
    if (n < w.nat) { strlcpy(name, w.at[n].name, cap); decode(&w.at[n], data); }
    return w.nat;
}
