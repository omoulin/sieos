/*
 * sieos.c - The C library sicc needs (cc/sys.h), on SIEOS: what SIEOS's own
 * library (user/lib, lib/string.c) does not have, on top of it.
 *
 *   files      fopen/fread/fwrite/fclose on the file server (fs_*): a file
 *              read is read whole at fopen, a file written is written at
 *              fclose (sicc reads and writes whole files); stdout and stderr
 *              go to the console, stdin is empty
 *   memory     malloc, realloc, free are SIEOS's; calloc here
 *   formatting snprintf, vfprintf...: fmt.c (exact, as on the development
 *              machine); strtod, strtol, strtoull: fmt.c too
 *   the rest   qsort (stable), getenv (no environment: none), exit, remove,
 *              and the string functions lib/string.c lacks
 *
 * main: sicc wants more stack than a SIEOS program's 32 KiB (recursive
 * descent, big buffers): its own main (main.c, renamed sicc_main) runs on a
 * thread with 16 MiB, and the program ends with it.
 *
 * With it, sicc builds as a SIEOS program (/bin/cc): see the Makefile and
 * docs/cc.md.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include <stdarg.h>

long fmt_vformat(void (*p)(void *, char), void *ctx, const char *f, va_list ap);
int fmt_write(int fd, const char *p, long n);

struct FILE {
    long h;                     /* the file server's handle (-1: the console) */
    int write, err;
    char *b;                    /* read: the whole file; write: what was written */
    size_t n, cap, pos;
};
typedef struct FILE FILE;
static FILE in_ = { -1, 0, 0, 0, 0, 0, 0 }, out_ = { -1, 1, 0, 0, 0, 0, 0 }, err_ = { -1, 1, 0, 0, 0, 0, 0 };
FILE *stdin = &in_, *stdout = &out_, *stderr = &err_;

int fmt_write(int fd, const char *p, long n) { (void)fd; con_write(p, (size_t)n); return (int)n; }

void *memchr(const void *s, int c, size_t n) { const unsigned char *p = s; for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p; return 0; }

static int grow(FILE *f, size_t more)
{
    if (f->n + more <= f->cap) return 0;
    size_t cap = f->cap ? f->cap : 4096;
    while (cap < f->n + more) cap *= 2;
    char *b = realloc(f->b, cap);
    if (!b) return -1;
    f->b = b; f->cap = cap;
    return 0;
}

FILE *fopen(const char *path, const char *mode)
{
    int w = mode[0] == 'w' || mode[0] == 'a';
    long h = fs_open(path, w ? FS_WRONLY | FS_CREAT | FS_TRUNC : FS_RDONLY, 0644);
    if (h < 0) return 0;
    FILE *f = malloc(sizeof *f);
    if (!f) { fs_close(h); return 0; }
    memset(f, 0, sizeof *f);
    f->h = h; f->write = w;
    if (!w) {                                     /* read it all now */
        siefs_stat_t st;
        size_t size = fs_stat(path, &st, 0) ? 0 : (size_t)st.size;
        if (grow(f, size + 1)) { fs_close(h); free(f); return 0; }
        long r = size ? fs_read(h, 0, f->b, size) : 0;
        if (r < 0) { fs_close(h); free(f->b); free(f); return 0; }
        f->n = (size_t)r;
    }
    return f;
}

int fflush(FILE *f)
{
    if (f && f->h < 0 && f->n) { con_write(f->b, f->n); f->n = 0; }   /* (the console) */
    return 0;
}

int fclose(FILE *f)
{
    int e = 0;
    if (f->h >= 0 && f->write && f->n && fs_write(f->h, 0, f->b, f->n) != (long)f->n) e = -1;
    if (f->h >= 0) fs_close(f->h);
    else fflush(f);
    if (f != stdin && f != stdout && f != stderr) { free(f->b); free(f); }
    return e | (f->err ? -1 : 0);
}

size_t fread(void *p, size_t size, size_t n, FILE *f)
{
    if (!size || f->write) return 0;
    size_t want = size * n, left = f->n - f->pos;
    if (want > left) want = left - left % size;
    memcpy(p, f->b + f->pos, want);
    f->pos += want;
    return want / size;
}

size_t fwrite(const void *p, size_t size, size_t n, FILE *f)
{
    size_t len = size * n;
    if (!len) return 0;
    if (grow(f, len)) { f->err = 1; return 0; }
    memcpy(f->b + f->n, p, len);
    f->n += len;
    if (f->h < 0) fflush(f);                     /* (the console: at once; main may return without exit) */
    return n;
}

int fputc(int c, FILE *f) { char ch = (char)c; return fwrite(&ch, 1, 1, f) ? (unsigned char)c : -1; }
int fputs(const char *s, FILE *f) { return fwrite(s, 1, strlen(s), f) ? 0 : -1; }

static void fput(void *ctx, char c) { fputc(c, ctx); }
int vfprintf(FILE *f, const char *fmt, va_list ap) { return (int)fmt_vformat(fput, f, fmt, ap); }
int fprintf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = vfprintf(f, fmt, ap); va_end(ap); return n; }

_Noreturn void exit(int code);
int sicc_main(int argc, char **argv);
static struct { int argc; char **argv; } start;
static void compiler(void *arg) { (void)arg; exit(sicc_main(start.argc, start.argv)); }
int main(int argc, char **argv)
{
    start.argc = argc; start.argv = argv;
    if (thread_start(compiler, 0, 16 << 20) < 0) { fputs("cc: no memory for a stack\n", stderr); return 1; }
    for (;;) sys_sleep(1000000000000ULL);               /* (exit ends the program) */
}

int remove(const char *path) { return fs_unlink(path) < 0 ? -1 : 0; }
void sicc_executable(const char *path) { fs_chmod(path, 0755); }   /* (main.c: a program it linked) */
char *getenv(const char *name) { (void)name; return 0; }
_Noreturn void exit(int code) { fflush(stdout); fflush(stderr); sys_exit(code); }
void *calloc(size_t n, size_t size) { void *p = malloc(n * size); if (p) memset(p, 0, n * size); return p; }

/* qsort: a stable merge sort (sicc's output must not depend on how ties fall) */
static void msort(char *a, char *t, size_t n, size_t s, int (*cmp)(const void *, const void *))
{
    if (n < 2) return;
    size_t h = n / 2;
    msort(a, t, h, s, cmp);
    msort(a + h * s, t, n - h, s, cmp);
    size_t i = 0, j = h, k = 0;
    while (i < h && j < n) {
        if (cmp(a + j * s, a + i * s) < 0) { memcpy(t + k * s, a + j * s, s); j++; }
        else { memcpy(t + k * s, a + i * s, s); i++; }
        k++;
    }
    while (i < h) { memcpy(t + k * s, a + i * s, s); i++; k++; }
    while (j < n) { memcpy(t + k * s, a + j * s, s); j++; k++; }
    memcpy(a, t, n * s);
}
void qsort(void *base, size_t n, size_t s, int (*cmp)(const void *, const void *))
{
    char *t = malloc(n * s + 1);
    if (!t) return;
    msort(base, t, n, s, cmp);
    free(t);
}

/* what lib/string.c does not have */
int strncmp(const char *a, const char *b, size_t n) { for (; n && *a && *a == *b; n--, a++, b++) ; return n ? (unsigned char)*a - (unsigned char)*b : 0; }
char *strncpy(char *d, const char *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
char *strrchr(const char *s, int c) { const char *r = 0; for (; *s; s++) if (*s == (char)c) r = s; return c ? (char *)r : (char *)s; }
char *strstr(const char *s, const char *t) { size_t n = strlen(t); for (; *s; s++) if (!strncmp(s, t, n)) return (char *)s; return n ? 0 : (char *)s; }
char *strpbrk(const char *s, const char *a) { for (; *s; s++) if (strchr(a, *s)) return (char *)s; return 0; }
