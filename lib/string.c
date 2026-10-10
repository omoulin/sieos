/*
 * string.c - Memory and string functions, for the kernel and user programs.
 * On x86-64, memcpy/memset use the CPU's "rep" string instructions, which
 * modern processors execute very fast (whole cache lines at a time); on
 * arm64, 16 bytes per step (two 64-bit words, which the compiler pairs into
 * one ldp/stp), unaligned allowed on ordinary memory.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdint.h>
#include "mk/lib.h"
#include "mk/crypto.h"

#if defined(__aarch64__)
void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    uint64_t v = (unsigned char)c * 0x0101010101010101UL;
    for (; n >= 16; n -= 16, p += 16) { ((uint64_t *)p)[0] = v; ((uint64_t *)p)[1] = v; }
    for (; n; n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    for (; n >= 16; n -= 16, dp += 16, sp += 16) {
        uint64_t a = ((const uint64_t *)sp)[0], b = ((const uint64_t *)sp)[1];
        ((uint64_t *)dp)[0] = a; ((uint64_t *)dp)[1] = b;
    }
    for (; n; n--) *dp++ = *sp++;
    return d;
}
#else
void *memset(void *d, int c, size_t n)
{
    void *r = d;
    asm volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

void *memcpy(void *d, const void *s, size_t n)
{
    void *r = d;
    asm volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}
#endif

void *memmove(void *d, const void *s, size_t n)
{
    if (d <= s || (const char *)s + n <= (char *)d) return memcpy(d, s, n);
    for (char *dp = d; n--; ) dp[n] = ((const char *)s)[n];   /* overlap: copy backwards */
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    for (; *a && *a == *b; a++, b++) ;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return c ? 0 : (char *)s;
}

/* Copy a string into d (size bytes), always 0-terminated; returns strlen(s). */
size_t strlcpy(char *d, const char *s, size_t size)
{
    size_t n = strlen(s);
    if (size) { size_t c = n < size ? n : size - 1; memcpy(d, s, c); d[c] = 0; }
    return n;
}

/* For secrets (mk/crypto.h): compare in constant time, and erase. */
int ct_equal(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *x = a, *y = b;
    uint8_t d = 0;
    while (n--) d |= x[n] ^ y[n];
    return d == 0;
}

void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

/* A decimal number (digits only) -> its value, or -1 if s is not one. */
long strnum(const char *s)
{
    long v = 0;
    if (!*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000000000L) return -1;
        v = v * 10 + *s - '0';
    }
    return v;
}
