/*
 * util.c - Memory, buffers, tables and files for the rest of sicc.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "sicc.h"

/* ---- The arena: memory taken in 1 MiB chunks and handed out by moving a
 * pointer. Nothing is freed until the program ends. */
static char *a_cur, *a_end;

void *arena(long n)
{
    n = (n + 15) & ~15L;
    if (a_cur + n > a_end) {
        long chunk = n > (1 << 20) ? n : (1 << 20);
        a_cur = calloc(1, chunk);
        if (!a_cur) die("out of memory");
        a_end = a_cur + chunk;
    }
    void *p = a_cur;
    a_cur += n;
    return p;
}

char *xstrndup(const char *s, long n)
{
    char *p = arena(n + 1);
    memcpy(p, s, n);
    return p;
}

char *fmt(const char *f, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(tmp, sizeof tmp, f, ap);
    va_end(ap);
    if (n < (int)sizeof tmp) return xstrndup(tmp, n);
    char *p = arena(n + 1);
    va_start(ap, f);
    vsnprintf(p, n + 1, f, ap);
    va_end(ap);
    return p;
}

/* ---- Interned strings: equal strings get the same pointer, so names are
 * compared with ==. An open-addressing hash table of (string, length). */
typedef struct { const char *s; int len; unsigned h; } IEnt;
static IEnt *itab;
static int icap, icount;

static unsigned hash(const char *s, long n)
{
    unsigned h = 2166136261u;                    /* FNV-1a */
    for (long i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

const char *intern(const char *s, long n)
{
    if (icount * 2 >= icap) {                    /* grow at half full */
        int ncap = icap ? icap * 2 : 4096;
        IEnt *nt = calloc(ncap, sizeof *nt);
        if (!nt) die("out of memory");
        for (int i = 0; i < icap; i++)
            if (itab[i].s) {
                int j = itab[i].h & (ncap - 1);
                while (nt[j].s) j = (j + 1) & (ncap - 1);
                nt[j] = itab[i];
            }
        free(itab);
        itab = nt;
        icap = ncap;
    }
    unsigned h = hash(s, n);
    int i = h & (icap - 1);
    for (; itab[i].s; i = (i + 1) & (icap - 1))
        if (itab[i].h == h && itab[i].len == n && !memcmp(itab[i].s, s, n)) return itab[i].s;
    itab[i].s = xstrndup(s, n);
    itab[i].len = n;
    itab[i].h = h;
    icount++;
    return itab[i].s;
}

/* ---- Buffers and vectors (grown with realloc: they can be large) */
void buf_add(Buf *b, const void *p, long n)
{
    if (b->len + n + 1 > b->cap) {
        long c = b->cap ? b->cap * 2 : 256;
        while (c < b->len + n + 1) c *= 2;
        b->p = realloc(b->p, c);
        if (!b->p) die("out of memory");
        b->cap = c;
    }
    memcpy(b->p + b->len, p, n);
    b->len += n;
    b->p[b->len] = 0;                            /* always 0-terminated */
}
void buf_c(Buf *b, int c) { char ch = c; buf_add(b, &ch, 1); }
void buf_s(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
void buf_zero(Buf *b, long n) { while (n-- > 0) buf_c(b, 0); }

void buf_f(Buf *b, const char *f, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(tmp, sizeof tmp, f, ap);
    va_end(ap);
    if (n < (int)sizeof tmp) { buf_add(b, tmp, n); return; }
    char *p = malloc(n + 1);
    va_start(ap, f);
    vsnprintf(p, n + 1, f, ap);
    va_end(ap);
    buf_add(b, p, n);
    free(p);
}

void vec_push(Vec *v, void *x)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->v = realloc(v->v, v->cap * sizeof *v->v);
        if (!v->v) die("out of memory");
    }
    v->v[v->n++] = x;
}

/* ---- Maps from interned pointers: hashed by address. */
static unsigned phash(const void *k) { unsigned long x = (unsigned long)k; return (unsigned)(x >> 4 ^ x >> 13); }

void *map_get(Map *m, const void *k)
{
    if (!m->cap) return 0;
    for (int i = phash(k) & (m->cap - 1); m->k[i]; i = (i + 1) & (m->cap - 1))
        if (m->k[i] == k) return m->v[i];
    return 0;
}

void map_put(Map *m, const void *k, void *v)
{
    if (m->n * 2 >= m->cap) {
        Map nm = { 0 };
        nm.cap = m->cap ? m->cap * 2 : 16;
        nm.k = calloc(nm.cap, sizeof *nm.k);
        nm.v = calloc(nm.cap, sizeof *nm.v);
        if (!nm.k || !nm.v) die("out of memory");
        for (int i = 0; i < m->cap; i++) if (m->k[i]) map_put(&nm, m->k[i], m->v[i]);
        free(m->k);
        free(m->v);
        *m = nm;
    }
    int i = phash(k) & (m->cap - 1);
    for (; m->k[i]; i = (i + 1) & (m->cap - 1))
        if (m->k[i] == k) { m->v[i] = v; return; }
    m->k[i] = k;
    m->v[i] = v;
    m->n++;
}

/* ---- Files */
int read_file(const char *path, Buf *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf_add(out, tmp, n);
    fclose(f);
    if (!out->p) buf_add(out, "", 0);
    return 0;
}

int write_file(const char *path, const void *p, long n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = (long)fwrite(p, 1, n, f) == n;
    return fclose(f) == 0 && ok ? 0 : -1;
}

void die(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    fputs("sicc: ", stderr);
    vfprintf(stderr, f, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

void warnf(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    fputs("sicc: warning: ", stderr);
    vfprintf(stderr, f, ap);
    fputc('\n', stderr);
    va_end(ap);
}
