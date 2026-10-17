/*
 * util.h - Small helpers every part uses: growable buffers, UTF-8, files.
 * Portable C11; nothing here depends on the platform beyond POSIX files.
 */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define COUNT(a) ((int)(sizeof(a) / sizeof *(a)))

typedef struct { char *p; size_t n, cap; } buf_t;   /* p is always NUL-terminated once used */

void  buf_add(buf_t *b, const void *s, size_t n);
void  buf_puts(buf_t *b, const char *s);
void  buf_putc(buf_t *b, int c);
void  buf_printf(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void  buf_free(buf_t *b);
char *buf_take(buf_t *b);                 /* the string, and the buffer is empty again */

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t m);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

int   utf8_dec(const char *s, int *len);  /* the code point at s; *len: its bytes (>= 1) */
int   utf8_enc(char *out, int cp);         /* bytes written (1..4) */
int   utf8_len(const char *s, int n);      /* code points in the first n bytes */
int   utf8_prev(const char *s, int i);     /* the byte index of the code point before i */
int   fold_char(int cp);                   /* lower case, accents removed (for search) */

char *read_file(const char *path, size_t *n);           /* NUL-terminated; NULL if missing */
int   write_file(const char *path, const void *p, size_t n, int mode);   /* atomic (rename) */
int   mkdirs(const char *path, int mode);
int   file_exists(const char *path);
double now_s(void);
uint32_t hash_str(const char *s);
char *tok(char **s, const char *delims);   /* the next token of *s (reentrant strtok); NULL at the end */

#endif
