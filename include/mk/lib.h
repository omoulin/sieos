/*
 * mk/lib.h - Small helpers compiled into both the kernel and user programs
 * (lib/string.c, lib/fmt.c): there is no C library underneath either.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stddef.h>
#include <stdarg.h>

void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
char  *strchr(const char *s, int c);
size_t strlcpy(char *d, const char *s, size_t size);
long   strnum(const char *s);             /* decimal digits -> value, or -1 */

/* printf-style formatting: each output character goes to put(ctx, c). */
void vformat(void (*put)(void *ctx, char c), void *ctx, const char *fmt, va_list ap);
