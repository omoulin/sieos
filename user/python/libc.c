/*
 * libc.c - The few C library functions MicroPython uses that SIEOS's library
 * (lib/string.c, user/lib/lib.c) does not have.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "mk.h"

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n && *a && *a == *b; n--, a++, b++) ;
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

char *strrchr(const char *s, int c)
{
    const char *r = 0;
    for (; *s; s++) if (*s == (char)c) r = s;
    return c ? (char *)r : (char *)s;
}

void *memchr(const void *s, int c, size_t n)
{
    for (const unsigned char *p = s; n; n--, p++) if (*p == (unsigned char)c) return (void *)p;
    return 0;
}

_Noreturn void abort(void) { printf("python: abort\n"); sys_exit(134); }
_Noreturn void exit(int code) { sys_exit(code); }

/* sqrt for the math module's table (calls elsewhere are the instruction itself, math.h) */
#undef sqrt
double sqrt(double x)
{
#if defined(__aarch64__)
    asm("fsqrt %d0, %d1" : "=w"(x) : "w"(x));
#else
    asm("sqrtsd %1, %0" : "=x"(x) : "x"(x));
#endif
    return x;
}
double fabs(double x) { union { double d; uint64_t i; } u = { x }; u.i &= ~(1ULL << 63); return u.d; }
double nan(const char *s) { (void)s; return NAN; }
/* (exact for powers of two: log2(8) is 3) */
double log2(double x)
{
    int e;
    if (!(x > 0) || isinf(x)) return log(x);
    double m = frexp(x, &e);
    return e + log(m) / 0.69314718055994530942;
}
