/*
 * util.c - Buffers, UTF-8, files (util.h).
 */
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
    return p;
}
void *xcalloc(size_t n, size_t m)
{
    void *p = calloc(n ? n : 1, m ? m : 1);
    if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
    return p;
}
void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
    return p;
}
char *xstrdup(const char *s) { return s ? xstrndup(s, strlen(s)) : NULL; }
char *xstrndup(const char *s, size_t n)
{
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}
char *xprintf(const char *fmt, ...)
{
    buf_t b = {0};
    va_list ap;
    va_start(ap, fmt);
    char tmp[512];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < (int)sizeof tmp) { buf_add(&b, tmp, n); return buf_take(&b); }
    char *big = xmalloc(n + 1);
    va_start(ap, fmt);
    vsnprintf(big, n + 1, fmt, ap);
    va_end(ap);
    return big;
}

void buf_add(buf_t *b, const void *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = MAX(b->cap * 2, b->n + n + 64);
        b->p = xrealloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
void buf_puts(buf_t *b, const char *s) { buf_add(b, s, strlen(s)); }
void buf_putc(buf_t *b, int c) { char ch = (char)c; buf_add(b, &ch, 1); }
void buf_printf(buf_t *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < (int)sizeof tmp) { buf_add(b, tmp, n); return; }
    char *big = xmalloc(n + 1);
    va_start(ap, fmt);
    vsnprintf(big, n + 1, fmt, ap);
    va_end(ap);
    buf_add(b, big, n);
    free(big);
}
void buf_free(buf_t *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }
char *buf_take(buf_t *b)
{
    char *p = b->p ? b->p : xstrdup("");
    b->p = NULL;
    b->n = b->cap = 0;
    return p;
}

int utf8_dec(const char *s, int *len)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *len = 1; return u[0]; }
    if ((u[0] & 0xe0) == 0xc0 && (u[1] & 0xc0) == 0x80) { *len = 2; return (u[0] & 0x1f) << 6 | (u[1] & 0x3f); }
    if ((u[0] & 0xf0) == 0xe0 && (u[1] & 0xc0) == 0x80 && (u[2] & 0xc0) == 0x80) {
        *len = 3;
        return (u[0] & 0x0f) << 12 | (u[1] & 0x3f) << 6 | (u[2] & 0x3f);
    }
    if ((u[0] & 0xf8) == 0xf0 && (u[1] & 0xc0) == 0x80 && (u[2] & 0xc0) == 0x80 && (u[3] & 0xc0) == 0x80) {
        *len = 4;
        return (u[0] & 0x07) << 18 | (u[1] & 0x3f) << 12 | (u[2] & 0x3f) << 6 | (u[3] & 0x3f);
    }
    *len = 1;
    return 0xfffd;
}
int utf8_enc(char *o, int cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xc0 | cp >> 6); o[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xe0 | cp >> 12); o[1] = (char)(0x80 | (cp >> 6 & 0x3f)); o[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    o[0] = (char)(0xf0 | cp >> 18); o[1] = (char)(0x80 | (cp >> 12 & 0x3f));
    o[2] = (char)(0x80 | (cp >> 6 & 0x3f)); o[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}
int utf8_len(const char *s, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xc0) != 0x80) c++;
    return c;
}
int utf8_prev(const char *s, int i)
{
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80) i--;
    return i;
}

/* Lower case without accents, for Latin, Greek and Cyrillic letters (search only). */
int fold_char(int c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c < 0xc0) return c;
    static const char *lat = "aaaaaaaceeeeiiiidnooooo/ouuuuyps"   /* 0xc0..0xdf */
                             "aaaaaaaceeeeiiiidnooooo/ouuuuypy";  /* 0xe0..0xff */
    if (c <= 0xff) { int f = lat[c - 0xc0]; return f == '/' ? c : f; }
    if (c >= 0x100 && c <= 0x17f) {                 /* Latin Extended-A: pairs, mostly accented */
        static const char *ext = "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiijjjjkkklllllllllllnnnnnnnnnoooooooorrrrrrsssssssstttttuuuuuuuuuuuuwwyyyzzzzzzs";
        int i = c - 0x100;
        return i < (int)strlen(ext) ? ext[i] : c;
    }
    if (c >= 0x391 && c <= 0x3a9) return c + 32;    /* Greek */
    if (c >= 0x410 && c <= 0x42f) return c + 32;    /* Cyrillic */
    if (c >= 0x400 && c <= 0x40f) return c + 80;
    if (c == 0x2019 || c == 0x2018) return '\'';
    return c;
}

char *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    buf_t b = {0};
    char tmp[65536];
    size_t k;
    while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) buf_add(&b, tmp, k);
    fclose(f);
    if (n) *n = b.n;
    return buf_take(&b);
}

int write_file(const char *path, const void *p, size_t n, int mode)
{
    char *tmp = xprintf("%s.tmp%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        char *dir = xstrdup(path), *slash = strrchr(dir, '/');
        if (slash) { *slash = 0; mkdirs(dir, 0755); }
        free(dir);
        fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    }
    if (fd < 0) { free(tmp); return -1; }
    const char *c = p;
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, c + done, n - done);
        if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); free(tmp); return -1; }
        done += (size_t)w;
    }
    fchmod(fd, mode);
    close(fd);
    int r = rename(tmp, path);
    free(tmp);
    return r;
}

int mkdirs(const char *path, int mode)
{
    char *p = xstrdup(path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, mode); *s = '/'; }
    int r = mkdir(p, mode);
    free(p);
    return r == 0 || errno == EEXIST ? 0 : -1;
}

int file_exists(const char *path) { struct stat st; return stat(path, &st) == 0; }

double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

char *tok(char **s, const char *delims)
{
    char *p = *s;
    if (!p) return NULL;
    p += strspn(p, delims);
    if (!*p) { *s = NULL; return NULL; }
    char *e = p + strcspn(p, delims);
    if (*e) { *e = 0; *s = e + 1; } else *s = NULL;
    return p;
}
