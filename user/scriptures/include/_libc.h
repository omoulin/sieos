/*
 * _libc.h - The C library Scriptures' portable core asks for (stdio.h,
 * stdlib.h, string.h, unistd.h, time.h, pthread.h...: each of those names
 * here is this one file), on SIEOS's own library: user/scriptures/libc.c
 * implements what user/lib and lib/string.c lack; formatting and number
 * parsing are /bin/cc's (cc/libc/fmt.c). Only what the app uses.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "mk/abi.h"                      /* the E... error numbers */
#include "mk/lib.h"                      /* mem*, strlen, strcmp, strchr, strlcpy */

typedef long ssize_t;
typedef long off_t;
typedef int pid_t;
typedef unsigned mode_t;
typedef int64_t time_t;
typedef unsigned useconds_t;

/* errno: one for the program (the app only tests EINTR and EEXIST) */
extern int errno;

/* ---- stdlib.h */
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff
void *malloc(size_t n);
void *realloc(void *p, size_t n);
void *calloc(size_t n, size_t m);
void  free(void *p);
_Noreturn void exit(int code);
_Noreturn void abort(void);
char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   atoi(const char *s);
double atof(const char *s);
long  strtol(const char *s, char **end, int base);
double strtod(const char *s, char **end);
int   abs(int v);
int   rand(void);
void  qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

/* ---- string.h */
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
int    strncmp(const char *a, const char *b, size_t n);
int    strncasecmp(const char *a, const char *b, size_t n);
char  *strrchr(const char *s, int c);
char  *strstr(const char *s, const char *t);
char  *strcpy(char *d, const char *s);
char  *strdup(const char *s);
void  *memchr(const void *s, int c, size_t n);

/* ---- stdio.h: FILE reads a whole file at fopen (the app only reads with it) */
typedef struct FILE FILE;
extern FILE *stdout, *stderr;
FILE  *fopen(const char *path, const char *mode);
size_t fread(void *p, size_t size, size_t n, FILE *f);
int    fclose(FILE *f);
int    fflush(FILE *f);
int    printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int    fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int    snprintf(char *s, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int    vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
int    sscanf(const char *s, const char *fmt, ...);   /* %d and %Ns only */

/* ---- unistd.h, fcntl.h, sys/stat.h, dirent.h: files on the file server */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
int     open(const char *path, int flags, ...);
int     close(int fd);
ssize_t read(int fd, void *p, size_t n);
ssize_t write(int fd, const void *p, size_t n);
int     unlink(const char *path);
int     rename(const char *from, const char *to);
int     mkdir(const char *path, mode_t mode);
int     fchmod(int fd, mode_t mode);
ssize_t readlink(const char *path, char *buf, size_t n);
pid_t   getpid(void);
int     usleep(useconds_t us);
unsigned sleep(unsigned s);
struct stat { uint64_t st_size; unsigned st_mode; time_t st_mtime; };
int     stat(const char *path, struct stat *st);
#define S_ISDIR(m) (((m) & 0170000) == 0040000)
struct dirent { char d_name[256]; };
typedef struct DIR DIR;
DIR    *opendir(const char *path);
struct dirent *readdir(DIR *d);
int     closedir(DIR *d);

/* ---- time.h: the wall clock in UTC (SIEOS has no time zones yet) */
struct timespec { time_t tv_sec; long tv_nsec; };
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
time_t     time(time_t *t);
int        clock_gettime(int clk, struct timespec *ts);
struct tm *gmtime_r(const time_t *t, struct tm *tm);
struct tm *localtime_r(const time_t *t, struct tm *tm);
time_t     timegm(struct tm *tm);
size_t     strftime(char *s, size_t n, const char *fmt, const struct tm *tm);   /* %Y %m %d %H %M %S */

/* ---- pthread.h: threads (user/lib's), mutexes on futexes */
typedef struct pthread *pthread_t;
typedef struct { volatile int v; } pthread_mutex_t;
typedef int pthread_attr_t, pthread_mutexattr_t;
#define PTHREAD_MUTEX_INITIALIZER { 0 }
int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
int pthread_detach(pthread_t t);
int pthread_join(pthread_t t, void **ret);
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);

/* ---- math.h (font_tt.c, stb_truetype) */
double floor(double), ceil(double), sqrt(double), fabs(double), fmod(double, double);
double pow(double, double), cos(double), acos(double);
float  ceilf(float);
long   lroundf(float);

/* ---- assert.h */
#define assert(e) ((void)0)
