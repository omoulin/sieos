/* t.h - what the sicc tests use from the host's C library (printf & co.).
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include <stddef.h>
#include <stdarg.h>
int printf(const char *fmt, ...);
int sprintf(char *s, const char *fmt, ...);
int vsprintf(char *s, const char *fmt, va_list ap);
int strcmp(const char *a, const char *b);
size_t strlen(const char *s);
char *strcpy(char *d, const char *s);
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *malloc(size_t n);
void free(void *p);
void exit(int code);
