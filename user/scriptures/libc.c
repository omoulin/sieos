/*
 * libc.c - The C library functions Scriptures' core uses (include/_libc.h)
 * that SIEOS's library (user/lib, lib/string.c) and /bin/cc's formatting
 * (cc/libc/fmt.c: snprintf, strtod, strtol, atoi) do not have: files on the
 * file server, directories, the clock in UTC, threads and mutexes, a little
 * maths for the font rasterizer, sscanf's %d and %s.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "include/_libc.h"

int errno;

/* ---- the environment: a few names the SIEOS parts set (plat_sieos.c) */
static struct { char *k, *v; } env[8];
char *getenv(const char *k)
{
    for (int i = 0; i < 8; i++) if (env[i].k && !strcmp(env[i].k, k)) return env[i].v;
    return 0;
}
int setenv(const char *k, const char *v, int over)
{
    int i = 0;
    while (i < 8 && env[i].k && strcmp(env[i].k, k)) i++;
    if (i == 8) return -1;
    if (env[i].k && !over) return 0;
    free(env[i].v);
    if (!env[i].k) env[i].k = strdup(k);
    env[i].v = strdup(v);
    return 0;
}

/* ---- stdlib */
void *calloc(size_t n, size_t m) { void *p = malloc(n * m); if (p) memset(p, 0, n * m); return p; }
void (*libc_atexit)(void);
_Noreturn void exit(int code) { if (libc_atexit) libc_atexit(); sys_exit(code); }
_Noreturn void abort(void) { con_write("scriptures: abort\n", 18); exit(134); }
double atof(const char *s) { return strtod(s, 0); }
int abs(int v) { return v < 0 ? -v : v; }
int rand(void)
{
    static uint32_t x = 2463534242u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (int)(x & RAND_MAX);
}
static void msort(char *a, char *t, size_t n, size_t s, int (*cmp)(const void *, const void *))
{
    if (n < 2) return;
    size_t h = n / 2;
    msort(a, t, h, s, cmp);
    msort(a + h * s, t, n - h, s, cmp);
    size_t i = 0, j = h, k = 0;
    while (i < h && j < n) memcpy(t + k++ * s, cmp(a + j * s, a + i * s) < 0 ? a + j++ * s : a + i++ * s, s);
    while (i < h) memcpy(t + k++ * s, a + i++ * s, s);
    while (j < n) memcpy(t + k++ * s, a + j++ * s, s);
    memcpy(a, t, n * s);
}
void qsort(void *base, size_t n, size_t s, int (*cmp)(const void *, const void *))
{
    char *t = n > 1 ? malloc(n * s) : 0;
    if (t) msort(base, t, n, s, cmp);
    free(t);
}

/* ---- strings */
size_t strspn(const char *s, const char *a) { size_t n = 0; while (s[n] && strchr(a, s[n])) n++; return n; }
size_t strcspn(const char *s, const char *r) { size_t n = 0; while (s[n] && !strchr(r, s[n])) n++; return n; }
int strncmp(const char *a, const char *b, size_t n)
{
    for (; n && *a && *a == *b; n--, a++, b++) ;
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
static int low(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n && *a && low((unsigned char)*a) == low((unsigned char)*b); n--, a++, b++) ;
    return n ? low((unsigned char)*a) - low((unsigned char)*b) : 0;
}
char *strrchr(const char *s, int c) { const char *r = 0; for (; *s; s++) if (*s == (char)c) r = s; return c ? (char *)r : (char *)s; }
char *strstr(const char *s, const char *t) { size_t n = strlen(t); for (; *s; s++) if (!strncmp(s, t, n)) return (char *)s; return n ? 0 : (char *)s; }
char *strcpy(char *d, const char *s) { memcpy(d, s, strlen(s) + 1); return d; }
char *strdup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); if (d) memcpy(d, s, n); return d; }
void *memchr(const void *s, int c, size_t n) { for (const unsigned char *p = s; n; n--, p++) if (*p == (unsigned char)c) return (void *)p; return 0; }

/* ---- stdio: a FILE is a whole file read at fopen; stdout and stderr the terminal */
struct FILE { char *b; size_t n, pos; };
static FILE out_, err_;
FILE *stdout = &out_, *stderr = &err_;
FILE *fopen(const char *path, const char *mode)
{
    siefs_stat_t st;
    if (mode[0] != 'r' || fs_stat(path, &st, 0) < 0) return 0;
    long h = fs_open(path, FS_RDONLY, 0);
    if (h < 0) return 0;
    FILE *f = calloc(1, sizeof *f);
    if (f && (f->b = malloc(st.size + 1))) {
        long r = st.size ? fs_read(h, 0, f->b, st.size) : 0;
        if (r >= 0) { f->n = (size_t)r; fs_close(h); return f; }
    }
    fs_close(h);
    if (f) free(f->b);
    free(f);
    return 0;
}
size_t fread(void *p, size_t size, size_t n, FILE *f)
{
    size_t want = size * n, left = f->n - f->pos;
    if (want > left) want = left - left % (size ? size : 1);
    memcpy(p, f->b + f->pos, want);
    f->pos += want;
    return size ? want / size : 0;
}
int fclose(FILE *f) { if (f != stdout && f != stderr) { free(f->b); free(f); } return 0; }
int fflush(FILE *f) { (void)f; return 0; }
int fprintf(FILE *f, const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    (void)f;
    con_write(b, n < (int)sizeof b ? (size_t)n : sizeof b - 1);
    return n;
}

/* sscanf: %d, %s and %Ns (the app's two uses), literal characters, spaces. */
int sscanf(const char *s, const char *f, ...)
{
    va_list ap;
    int got = 0;
    va_start(ap, f);
    for (; *f; f++) {
        if (*f == ' ') { while (*s == ' ' || *s == '\t' || *s == '\n') s++; continue; }
        if (*f != '%') { if (*s != *f) break; s++; continue; }
        f++;
        long width = 0;
        while (*f >= '0' && *f <= '9') width = width * 10 + *f++ - '0';
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;
        if (!*s) { if (!got) got = -1; break; }
        if (*f == 'd') {
            char *e;
            long v = strtol(s, &e, 10);
            if (e == s || (*s == '-' && e == s + 1)) break;
            if (*s == '-' && v > 0) v = -v;           /* (fmt.c's strtol reads the sign) */
            *va_arg(ap, int *) = (int)v;
            s = e;
        } else if (*f == 's') {
            char *d = va_arg(ap, char *);
            long i = 0;
            while (*s && *s != ' ' && *s != '\t' && *s != '\n' && (!width || i < width)) d[i++] = *s++;
            d[i] = 0;
        } else break;
        got++;
    }
    va_end(ap);
    return got;
}

/* ---- files: descriptors over the file server's handles */
#define NFD 32
static struct { long h; uint64_t pos; char *path; int used; } fds[NFD];   /* (h may be 0) */
static char fd_lock;
int open(const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int mode = flags & O_CREAT ? va_arg(ap, int) : 0;
    va_end(ap);
    long h = fs_open(path, flags, mode);
    if (h < 0) { errno = (int)-h; return -1; }
    while (__atomic_test_and_set(&fd_lock, __ATOMIC_ACQUIRE)) sys_yield();
    int fd = 3;
    while (fd < NFD && fds[fd].used) fd++;
    if (fd < NFD) { fds[fd].h = h; fds[fd].pos = 0; fds[fd].path = strdup(path); fds[fd].used = 1; }
    __atomic_clear(&fd_lock, __ATOMIC_RELEASE);
    if (fd == NFD) { fs_close(h); errno = EMFILE; return -1; }
    return fd;
}
static int bad(int fd) { if (fd < 3 || fd >= NFD || !fds[fd].used) { errno = EBADF; return 1; } return 0; }
int close(int fd)
{
    if (bad(fd)) return -1;
    fs_close(fds[fd].h);
    free(fds[fd].path);
    fds[fd].path = 0;
    fds[fd].used = 0;
    return 0;
}
ssize_t read(int fd, void *p, size_t n)
{
    if (bad(fd)) return -1;
    long r = fs_read(fds[fd].h, fds[fd].pos, p, n);
    if (r < 0) { errno = (int)-r; return -1; }
    fds[fd].pos += (uint64_t)r;
    return r;
}
ssize_t write(int fd, const void *p, size_t n)
{
    if (fd == 1 || fd == 2) return con_write(p, n);
    if (bad(fd)) return -1;
    long r = fs_write(fds[fd].h, fds[fd].pos, p, n);
    if (r < 0) { errno = (int)-r; return -1; }
    fds[fd].pos += (uint64_t)r;
    return r;
}
int fchmod(int fd, mode_t mode) { return bad(fd) || fs_chmod(fds[fd].path, (int)mode) < 0 ? -1 : 0; }
static int err(long r) { if (r < 0) { errno = (int)-r; return -1; } return 0; }
int unlink(const char *path) { return err(fs_unlink(path)); }
int rename(const char *from, const char *to) { return err(fs_rename(from, to)); }
int mkdir(const char *path, mode_t mode) { return err(fs_mkdir(path, (int)mode)); }
ssize_t readlink(const char *path, char *buf, size_t n) { (void)path; (void)buf; (void)n; errno = ENOENT; return -1; }
pid_t getpid(void) { mk_ident_t me; return sys_ident(0, &me) < 0 ? 1 : me.pid; }
int usleep(useconds_t us) { sys_sleep((uint64_t)us * 1000); return 0; }
unsigned sleep(unsigned s) { sys_sleep((uint64_t)s * 1000000000); return 0; }
int stat(const char *path, struct stat *st)
{
    siefs_stat_t s;
    if (err(fs_stat(path, &s, 0))) return -1;
    st->st_size = s.size; st->st_mode = s.mode; st->st_mtime = (time_t)(s.mtime / 1000000000);
    return 0;
}

struct DIR { long h; uint64_t cursor; char buf[2048]; const char *p, *end; struct dirent e; };
DIR *opendir(const char *path)
{
    long h = fs_open(path, FS_RDONLY | FS_DIRECTORY, 0);
    if (h < 0) { errno = (int)-h; return 0; }
    DIR *d = calloc(1, sizeof *d);
    if (!d) { fs_close(h); return 0; }
    d->h = h;
    return d;
}
struct dirent *readdir(DIR *d)
{
    for (;;) {
        uint64_t ino;
        int type;
        if (d->p && d->p < d->end && fs_dirent(&d->p, d->end, &ino, &type, d->e.d_name)) return &d->e;
        long n = fs_readdir(d->h, &d->cursor, d->buf, sizeof d->buf);
        if (n <= 0) return 0;
        d->p = d->buf; d->end = d->buf + n;
    }
}
int closedir(DIR *d) { fs_close(d->h); free(d); return 0; }

/* ---- time, in UTC */
time_t time(time_t *t) { time_t v = (time_t)(sys_time(0) / 1000000000); if (t) *t = v; return v; }
int clock_gettime(int clk, struct timespec *ts)
{
    int64_t ns = clk == CLOCK_MONOTONIC ? sys_clock() : sys_time(0);
    ts->tv_sec = ns / 1000000000; ts->tv_nsec = (long)(ns % 1000000000);
    return 0;
}
/* days since 1970-01-01 <-> a date (Howard Hinnant's algorithms) */
static int64_t days_from(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
struct tm *gmtime_r(const time_t *t, struct tm *tm)
{
    int64_t s = *t, z = (s >= 0 ? s : s - 86399) / 86400, sec = s - z * 86400;
    tm->tm_hour = (int)(sec / 3600); tm->tm_min = (int)(sec / 60 % 60); tm->tm_sec = (int)(sec % 60);
    tm->tm_wday = (int)((z % 7 + 11) % 7);
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1), m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y += m <= 2;
    tm->tm_year = (int)(y - 1900); tm->tm_mon = m - 1; tm->tm_mday = d;
    tm->tm_yday = (int)(days_from(y, m, d) - days_from(y, 1, 1));
    tm->tm_isdst = 0;
    return tm;
}
struct tm *localtime_r(const time_t *t, struct tm *tm) { return gmtime_r(t, tm); }
time_t timegm(struct tm *tm)
{
    int64_t y = tm->tm_year + 1900 + tm->tm_mon / 12, m = tm->tm_mon % 12;
    if (m < 0) { m += 12; y--; }
    return (time_t)(days_from(y, (int)m + 1, 1) + tm->tm_mday - 1) * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}
size_t strftime(char *s, size_t n, const char *f, const struct tm *tm)
{
    size_t k = 0;
    for (; *f && k + 1 < n; f++) {
        if (*f != '%' || !f[1]) { s[k++] = *f; continue; }
        int v, w = 2;
        switch (*++f) {
        case 'Y': v = tm->tm_year + 1900; w = 4; break;
        case 'm': v = tm->tm_mon + 1; break;
        case 'd': v = tm->tm_mday; break;
        case 'H': v = tm->tm_hour; break;
        case 'M': v = tm->tm_min; break;
        case 'S': v = tm->tm_sec; break;
        default: s[k++] = *f; continue;
        }
        k += (size_t)snprintf(s + k, n - k, "%0*d", w, v);
        if (k >= n) k = n - 1;
    }
    s[k] = 0;
    return k;
}

/* ---- threads: user/lib's, with 1 MiB stacks (TLS and JSON want more than 32 KiB) */
struct pthread { void *(*fn)(void *); void *arg, *ret; volatile int done; };
static void thread_main(void *a)
{
    struct pthread *t = a;
    t->ret = t->fn(t->arg);
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
    sys_futex(FUTEX_WAKE, &t->done, 1, 0);
    sys_thread_exit();
}
int pthread_create(pthread_t *out, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
    (void)a;
    struct pthread *t = calloc(1, sizeof *t);
    if (!t) return ENOMEM;
    t->fn = fn; t->arg = arg;
    if (thread_start(thread_main, t, 1 << 20) < 0) { free(t); return EAGAIN; }
    *out = t;
    return 0;
}
int pthread_detach(pthread_t t) { (void)t; return 0; }   /* (its 32 bytes stay) */
int pthread_join(pthread_t t, void **ret)
{
    while (!__atomic_load_n(&t->done, __ATOMIC_ACQUIRE)) sys_futex(FUTEX_WAIT, &t->done, 0, 0);
    if (ret) *ret = t->ret;
    free(t);
    return 0;
}
/* A mutex: 0 free, 1 held, 2 held with waiters (Drepper's "futexes are tricky"). */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) { (void)a; m->v = 0; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m)
{
    int c = 0;
    if (__atomic_compare_exchange_n(&m->v, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
    if (c != 2) c = __atomic_exchange_n(&m->v, 2, __ATOMIC_ACQUIRE);
    while (c) {
        sys_futex(FUTEX_WAIT, &m->v, 2, 0);
        c = __atomic_exchange_n(&m->v, 2, __ATOMIC_ACQUIRE);
    }
    return 0;
}
int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (__atomic_exchange_n(&m->v, 0, __ATOMIC_RELEASE) == 2) sys_futex(FUTEX_WAKE, &m->v, 1, 0);
    return 0;
}

/* ---- maths for stb_truetype and font_tt.c */
double floor(double x) { if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x; double i = (double)(int64_t)x; return i > x ? i - 1 : i; }
double ceil(double x) { return -floor(-x); }
float ceilf(float x) { return (float)ceil(x); }
long lroundf(float x) { return x < 0 ? -(long)(-x + 0.5f) : (long)(x + 0.5f); }
double fabs(double x) { return x < 0 ? -x : x; }
double fmod(double x, double y) { double q = x / y; q = q < 0 ? ceil(q) : floor(q); return x - q * y; }
double sqrt(double x)
{
#if defined(__aarch64__)
    asm("fsqrt %d0, %d1" : "=w"(x) : "w"(x));
#else
    asm("sqrtsd %1, %0" : "=x"(x) : "x"(x));
#endif
    return x;
}
