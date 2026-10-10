/*
 * libc.c - A small C library for running AArch64 test programs (and sicc
 * itself) under QEMU's user-mode emulator on the development machine. Test
 * infrastructure only: it is not part of SIEOS. It uses the emulator's
 * system calls (svc #0, the number in x8).
 *
 * What it offers is what the tests and sicc (cc/sys.h) use: start-up,
 * exit, memory (mmap), strings, files (open/read/write), formatted output
 * (fmt.c), qsort (a stable merge sort), getenv, and thread-local storage
 * for the main thread.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

enum { SYS_unlinkat = 35, SYS_openat = 56, SYS_close = 57, SYS_lseek = 62, SYS_read = 63, SYS_write = 64,
       SYS_exit_group = 94, SYS_munmap = 215, SYS_mmap = 222 };

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
    register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c,
                  x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}
#define sys3(n, a, b, c) sys6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

long fmt_vformat(void (*p)(void *, char), void *ctx, const char *f, va_list ap);
void fmt_flush(void);
int vsnprintf(char *buf, size_t cap, const char *f, va_list ap);

int fmt_write(int fd, const char *p, long n)
{
    while (n > 0) {
        long w = sys3(SYS_write, fd, p, n);
        if (w <= 0) return -1;
        p += w; n -= w;
    }
    return 0;
}

_Noreturn void _exit(int code) { for (;;) sys3(SYS_exit_group, code, 0, 0); }

/* ---- strings and memory */
void *memcpy(void *d, const void *s, size_t n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n)
{
    char *a = d; const char *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else for (a += n, b += n; n--; ) *--a = *--b;
    return d;
}
void *memset(void *d, int c, size_t n) { char *a = d; while (n--) *a++ = (char)c; return d; }
int memcmp(const void *x, const void *y, size_t n) { const unsigned char *a = x, *b = y; for (; n; n--, a++, b++) if (*a != *b) return *a - *b; return 0; }
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { for (; *a && *a == *b; a++, b++) ; return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n) { for (; n && *a && *a == *b; n--, a++, b++) ; return n ? (unsigned char)*a - (unsigned char)*b : 0; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
char *strncpy(char *d, const char *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strchr(const char *s, int c) { for (; *s; s++) if (*s == (char)c) return (char *)s; return c ? 0 : (char *)s; }
char *strrchr(const char *s, int c) { const char *r = 0; for (; *s; s++) if (*s == (char)c) r = s; return c ? (char *)r : (char *)s; }
char *strstr(const char *s, const char *t) { size_t n = strlen(t); for (; *s; s++) if (!strncmp(s, t, n)) return (char *)s; return n ? 0 : (char *)s; }
char *strpbrk(const char *s, const char *a) { for (; *s; s++) if (strchr(a, *s)) return (char *)s; return 0; }

/* ---- memory: size classes of 2^k bytes (16 .. 2^20) with free lists; larger: mmap */
typedef struct Blk { size_t size; struct Blk *next; } Blk;      /* the header before each block (16 bytes) */
static Blk *freel[24];
static char *pool, *pool_end;

static void *map(size_t n)
{
    long p = sys6(SYS_mmap, 0, (long)n, 3, 0x22, -1, 0);       /* read+write, private anonymous */
    return p < 0 && p > -4096 ? 0 : (void *)p;
}

void *malloc(size_t n)
{
    size_t need = n + sizeof(Blk);
    int k = 4;
    while ((1UL << k) < need) k++;
    Blk *b;
    if (k > 20) {
        size_t sz = (need + 4095) & ~4095UL;
        b = map(sz);
        if (!b) return 0;
        b->size = sz;
        return b + 1;
    }
    if (freel[k]) { b = freel[k]; freel[k] = b->next; return b + 1; }
    size_t sz = 1UL << k;
    if (pool + sz > pool_end) {
        size_t chunk = 1 << 24;                                  /* 16 MiB at a time */
        pool = map(chunk);
        if (!pool) return 0;
        pool_end = pool + chunk;
    }
    b = (Blk *)pool;
    pool += sz;
    b->size = sz;
    return b + 1;
}

void free(void *p)
{
    if (!p) return;
    Blk *b = (Blk *)p - 1;
    if (b->size > (1UL << 20)) { sys3(SYS_munmap, b, b->size, 0); return; }
    int k = __builtin_ctzl(b->size);
    b->next = freel[k];
    freel[k] = b;
}

void *calloc(size_t n, size_t s) { void *p = malloc(n * s); if (p) memset(p, 0, n * s); return p; }

void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    Blk *b = (Blk *)p - 1;
    size_t have = b->size - sizeof(Blk);
    if (n <= have) return p;
    void *q = malloc(n);
    if (q) { memcpy(q, p, have); free(p); }
    return q;
}

/* ---- qsort: a stable merge sort (the order of equal elements is kept) */
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
    for (; i < h; i++, k++) memcpy(t + k * s, a + i * s, s);
    for (; j < n; j++, k++) memcpy(t + k * s, a + j * s, s);
    memcpy(a, t, n * s);
}
void qsort(void *base, size_t n, size_t s, int (*cmp)(const void *, const void *))
{
    char *t = malloc(n * s + 1);
    msort(base, t, n, s, cmp);
    free(t);
}

/* ---- files */
typedef struct FILE { int fd, eof; char *b; long n; } FILE;     /* output buffered */
static FILE s_in = { 0, 0, 0, 0 }, s_out = { 1, 0, 0, 0 }, s_err = { 2, 0, 0, 0 };
FILE *stdin = &s_in, *stdout = &s_out, *stderr = &s_err;

int fflush(FILE *f)
{
    if (!f) { fflush(stdout); fflush(stderr); return 0; }
    if (f == stdout) fmt_flush();
    if (f->n) { fmt_write(f->fd, f->b, f->n); f->n = 0; }
    return 0;
}

static void fput(void *ctx, char c)
{
    FILE *f = ctx;
    if (!f->b) f->b = malloc(65536);
    f->b[f->n++] = c;
    if (f->n == 65536 || f->fd == 2) fflush(f);
}

FILE *fopen(const char *path, const char *mode)
{
    int flags = mode[0] == 'r' ? (strchr(mode, '+') ? 2 : 0) : mode[0] == 'w' ? 0x241 : 0x441;   /* O_WRONLY|O_CREAT|O_TRUNC / O_APPEND */
    long fd = sys6(SYS_openat, -100, (long)path, flags, 0644, 0, 0);
    if (fd < 0) return 0;
    FILE *f = calloc(1, sizeof *f);
    f->fd = (int)fd;
    return f;
}
int fclose(FILE *f) { fflush(f); sys3(SYS_close, f->fd, 0, 0); if (f->b) free(f->b); if (f != stdin && f != stdout && f != stderr) free(f); return 0; }
size_t fread(void *p, size_t s, size_t n, FILE *f)
{
    size_t want = s * n, got = 0;
    while (got < want) {
        long r = sys3(SYS_read, f->fd, (char *)p + got, want - got);
        if (r <= 0) { f->eof = 1; break; }
        got += r;
    }
    return s ? got / s : 0;
}
size_t fwrite(const void *p, size_t s, size_t n, FILE *f)
{
    if (f == stdout) fmt_flush();
    fflush(f);
    if (fmt_write(f->fd, p, (long)(s * n)) < 0) return 0;
    return n;
}
int fputc(int c, FILE *f) { if (f == stdout) fmt_flush(); fput(f, (char)c); if (f == stdout) fflush(f); return (unsigned char)c; }
int fputs(const char *s, FILE *f) { return fwrite(s, 1, strlen(s), f) ? 0 : -1; }
int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    if (f == stdout) fmt_flush();
    long n = fmt_vformat(fput, f, fmt, ap);
    fflush(f);
    return (int)n;
}
int fprintf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = vfprintf(f, fmt, ap); va_end(ap); return n; }
int remove(const char *path) { return sys3(SYS_unlinkat, -100, path, 0) < 0 ? -1 : 0; }

/* ---- the environment, exit, start-up */
static char **envp_;
char *getenv(const char *name)
{
    size_t n = strlen(name);
    for (char **e = envp_; e && *e; e++) if (!strncmp(*e, name, n) && (*e)[n] == '=') return *e + n + 1;
    return 0;
}

_Noreturn void exit(int code) { fflush(stdout); fflush(stderr); _exit(code); }
_Noreturn void abort(void) { fflush(stdout); _exit(134); }

/* thread-local storage of the main thread: tpidr_el0 points 16 bytes below the block (AArch64's layout) */
extern char __tdata_start[], __tdata_end[], __tbss_size[], __tls_align[];

int main(int, char **, char **);

void __libc_start(int argc, char **argv, char **envp)
{
    envp_ = envp;
    long init = __tdata_end - __tdata_start, size = init + (long)__tbss_size, align = (long)__tls_align;
    if (align < 1) align = 1;
    long off = (16 + align - 1) / align * align;            /* where the linker put the block: tp + 16, aligned */
    long a = align < 16 ? 16 : align;
    char *raw = malloc(size + off + a), *tp = (char *)(((uintptr_t)raw + a - 1) & -a);
    char *block = tp + off;
    memcpy(block, __tdata_start, init);
    memset(block + init, 0, size - init);
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(tp));
    exit(main(argc, argv, envp));
}
