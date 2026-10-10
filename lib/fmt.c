/*
 * fmt.c - printf-style formatting, shared by the kernel (kprintf) and user
 * programs (printf). Supports %c %s %d %i %u %x %p %%, the 'l' size, the
 * '-' (left-align) and '0' (zero-pad) flags and a width: "%-8s", "%08lx".
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdint.h>
#include "mk/lib.h"

void vformat(void (*put)(void *, char), void *ctx, const char *f, va_list ap)
{
    char buf[24];
    for (; *f; f++) {
        if (*f != '%') { put(ctx, *f); continue; }
        int left = 0, pad = ' ', width = 0, lng = 0, base = 10, neg = 0;
        for (f++; *f == '-' || *f == '0'; f++) if (*f == '-') left = 1; else pad = '0';
        for (; *f >= '0' && *f <= '9'; f++) width = width * 10 + *f - '0';
        for (; *f == 'l'; f++) lng = 1;
        const char *s = buf;     /* the text to print ... */
        int n = 1;               /* ... and its length */
        uint64_t v;
        switch (*f) {
        case 0:   return;
        case 'c': buf[0] = (char)va_arg(ap, int); break;
        case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; n = strlen(s); break;
        case 'd': case 'i': {
            int64_t x = lng ? va_arg(ap, long) : va_arg(ap, int);
            neg = x < 0;
            v = neg ? -(uint64_t)x : (uint64_t)x;
            goto number;
        }
        case 'p': lng = 1;                             /* fall through */
        case 'x': base = 16;                           /* fall through */
        case 'u': v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
        number: {
            char *p = buf + sizeof buf;                /* digits, written right to left */
            do *--p = "0123456789abcdef"[v % base]; while (v /= base);
            if (neg) *--p = '-';
            s = p;
            n = buf + sizeof buf - p;
            break;
        }
        default:  buf[0] = *f;                         /* "%%" and unknown ones */
        }
        if (!left) for (; width > n; width--) put(ctx, pad);
        for (int i = 0; i < n; i++) put(ctx, s[i]);
        for (; width > n; width--) put(ctx, ' ');
    }
}
