/*
 * formatfloat.c - Floats as text for MicroPython, in place of its own
 * py/formatfloat.c: SIEOS's exact conversions (cc/libc/fmt.c, the C library
 * of /bin/cc). MicroPython's are exact only where long double is wider than
 * double, which is not so on SIEOS's AArch64.
 *
 * repr() is the fewest digits that read back as the same float (as in
 * CPython): written out up to 1e16, with an exponent beyond, or below 1e-4.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <math.h>
#include "py/formatfloat.h"

double strtod(const char *s, char **end);

int mp_format_float(mp_float_t f, char *buf, size_t size, char fmt, int prec, char sign)
{
    char spec[8] = "%", *p = spec + 1;
    if (sign) *p++ = sign;
    if (f != f) f = fabs(f);                          /* "nan", never "-nan" (as Python) */
    if (prec != MP_FLOAT_REPR_PREC || f != f || f - f != 0) {         /* (also nan, inf) */
        *p++ = '.', *p++ = '*', *p++ = prec == MP_FLOAT_REPR_PREC ? 'g' : fmt, *p = 0;
        int n = snprintf(buf, size, spec, prec < 0 || prec == MP_FLOAT_REPR_PREC ? 6 : prec, f);
        if (n < (int)size) return n;
        p[-1] = fmt >= 'A' && fmt <= 'Z' ? 'E' : 'e';           /* too long ('f' of 1e300): an exponent */
        return snprintf(buf, size, spec, prec < (int)size - 8 ? prec : (int)size - 8, f);
    }
    char e[40];                                       /* the shortest "[s]d.ddde+XX" */
    *p++ = '.', *p++ = '*', *p++ = 'e', *p = 0;
    for (int d = 0; d < 17; d++) {
        snprintf(e, sizeof e, spec, d, f);
        if (strtod(e, 0) == f) break;
    }
    char dig[20], *q = e, *o = buf;
    int nd = 0, x = 0;
    while (*q == '+' || *q == '-' || *q == ' ') *o++ = *q++;
    char *body = q;
    for (; *q != 'e'; q++) if (*q != '.') dig[nd++] = *q;
    for (char *t = q + 2; *t; t++) x = x * 10 + *t - '0';
    if (q[1] == '-') x = -x;
    if (x < -4 || x >= 16) {                          /* d.ddde+XX, as it is */
        while (*body) *o++ = *body++;
    } else if (x < 0) {                               /* 0.000ddd */
        *o++ = '0', *o++ = '.';
        for (int k = -1; k > x; k--) *o++ = '0';
        for (int k = 0; k < nd; k++) *o++ = dig[k];
    } else {                                          /* ddd.ddd, or ddd00 */
        for (int k = 0; k <= x; k++) *o++ = k < nd ? dig[k] : '0';
        if (nd > x + 1) { *o++ = '.'; for (int k = x + 1; k < nd; k++) *o++ = dig[k]; }
    }
    *o = 0;
    return (int)(o - buf);
}
