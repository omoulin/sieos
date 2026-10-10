/*
 * fmt.c - printf-family formatting and strtod/strtol, for the AArch64 test
 * programs (a test-only C library, see libc.c). The same file, built by the
 * host compiler, replaces the host's printf in the reference programs, so
 * both sides format the same way.
 *
 * Floating-point conversions are exact, with big integers: a double is
 * m * 2^e; its decimal digits are those of m * 5^-e (e < 0) or m * 2^e,
 * rounded half-to-even. strtod divides exactly the same way. (long double is
 * double here, as on SIEOS's AArch64.)
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

int fmt_write(int fd, const char *p, long n);       /* the platform's output: write(2) or a system call */

/* ---- big unsigned integers: little-endian 32-bit limbs */
#define NL 300
typedef struct { uint32_t d[NL]; int n; } Big;

static void big_set(Big *a, uint64_t v) { a->n = 0; while (v) { a->d[a->n++] = (uint32_t)v; v >>= 32; } }
static void big_mul_small(Big *a, uint32_t m, uint32_t add)
{
    uint64_t c = add;
    for (int i = 0; i < a->n; i++) { c += (uint64_t)a->d[i] * m; a->d[i] = (uint32_t)c; c >>= 32; }
    if (c && a->n < NL) a->d[a->n++] = (uint32_t)c;
}
static uint32_t big_div_small(Big *a, uint32_t m)          /* a /= m, returns the remainder */
{
    uint64_t r = 0;
    for (int i = a->n - 1; i >= 0; i--) { r = r << 32 | a->d[i]; a->d[i] = (uint32_t)(r / m); r %= m; }
    while (a->n && !a->d[a->n - 1]) a->n--;
    return (uint32_t)r;
}
static void big_shl(Big *a, int k)
{
    int w = k / 32, b = k % 32;
    if (!a->n) return;
    if (a->n + w + 1 > NL) w = NL - a->n - 1;
    for (int i = a->n - 1 + w + 1; i >= 0; i--) {
        uint64_t hi = i - w < a->n && i - w >= 0 ? (uint64_t)a->d[i - w] << b : 0;
        uint64_t lo = b && i - w - 1 >= 0 && i - w - 1 < a->n ? a->d[i - w - 1] >> (32 - b) : 0;
        a->d[i] = (uint32_t)(hi | lo);
    }
    a->n += w + 1;
    while (a->n && !a->d[a->n - 1]) a->n--;
}
static int big_bits(const Big *a) { if (!a->n) return 0; return 32 * (a->n - 1) + 32 - __builtin_clz(a->d[a->n - 1]); }
static int big_cmp(const Big *a, const Big *b)
{
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--) if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}
static void big_sub(Big *a, const Big *b)                   /* a -= b (a >= b) */
{
    int64_t c = 0;
    for (int i = 0; i < a->n; i++) { c += (int64_t)a->d[i] - (i < b->n ? b->d[i] : 0); a->d[i] = (uint32_t)c; c >>= 32; }
    while (a->n && !a->d[a->n - 1]) a->n--;
}

/* ---- the exact decimal digits of a finite double: value = 0.D * 10^pos */
static int exact_digits(double x, char *dig, int *pos)
{
    union { double d; uint64_t u; } v = { x };
    uint64_t m = v.u & ((1ULL << 52) - 1);
    int be = (int)(v.u >> 52 & 0x7ff);
    if (!be && !m) { dig[0] = '0'; *pos = 1; return 1; }
    int e = be ? be - 1075 : -1074;
    if (be) m |= 1ULL << 52;
    static Big n;
    big_set(&n, m);
    if (e >= 0) big_shl(&n, e);
    else for (int k = 0; k < -e; k++) big_mul_small(&n, 5, 0);
    char tmp[1200];
    int len = 0;
    while (n.n) {
        uint32_t r = big_div_small(&n, 1000000000);
        for (int k = 0; k < 9; k++) { tmp[len++] = '0' + r % 10; r /= 10; }
    }
    while (len > 1 && tmp[len - 1] == '0') len--;
    for (int k = 0; k < len; k++) dig[k] = tmp[len - 1 - k];
    *pos = len + (e < 0 ? e : 0);
    return len;
}

/* keep the first `keep` digits, rounded half-to-even; may carry into a new first digit */
static int round_digits(char *dig, int len, int keep, int *pos)
{
    if (keep >= len) return len;
    if (keep < 0) { dig[0] = '0'; *pos += 1; return 0; }
    int up;
    char first = dig[keep];
    int rest = 0;
    for (int k = keep + 1; k < len; k++) if (dig[k] != '0') { rest = 1; break; }
    if (first > '5' || (first == '5' && rest)) up = 1;
    else if (first == '5') up = keep > 0 ? (dig[keep - 1] - '0') & 1 : 0;   /* a tie: to even */
    else up = 0;
    len = keep;
    if (up) {
        int k = keep - 1;
        for (; k >= 0 && dig[k] == '9'; k--) dig[k] = '0';
        if (k >= 0) dig[k]++;
        else { for (int j = len; j > 0; j--) dig[j] = dig[j - 1]; dig[0] = '1'; len++; *pos += 1; }
    }
    return len;
}

/* ---- the formatter: each character goes to out() */
typedef struct { void (*put)(void *, char); void *ctx; long count; } Sink;
static void put(Sink *s, char c) { s->put(s->ctx, c); s->count++; }
static void puts_n(Sink *s, const char *p, long n) { for (long i = 0; i < n; i++) put(s, p[i]); }

static long strlen_(const char *s) { long n = 0; while (s[n]) n++; return n; }

enum { F_MINUS = 1, F_PLUS = 2, F_SPACE = 4, F_HASH = 8, F_ZERO = 16 };

/* body: the digits (and point...), sign: "" "-" "+" " ", prefix: "0x"... ; zero padding goes after sign and prefix */
static void emit(Sink *s, const char *sign, const char *prefix, const char *body, long blen, int width, int flags, int zeros_ok)
{
    long n = strlen_(sign) + strlen_(prefix) + blen;
    long pad = width > n ? width - n : 0;
    if (!(flags & F_MINUS) && !((flags & F_ZERO) && zeros_ok)) for (long k = 0; k < pad; k++) put(s, ' ');
    puts_n(s, sign, strlen_(sign));
    puts_n(s, prefix, strlen_(prefix));
    if (!(flags & F_MINUS) && (flags & F_ZERO) && zeros_ok) for (long k = 0; k < pad; k++) put(s, '0');
    puts_n(s, body, blen);
    if (flags & F_MINUS) for (long k = 0; k < pad; k++) put(s, ' ');
}

static void fmt_float(Sink *s, double x, int conv, int prec, int width, int flags)
{
    int upper = conv >= 'A' && conv <= 'Z';
    char lc = conv | 0x20;
    union { double d; uint64_t u; } v = { x };
    const char *sign = v.u >> 63 ? "-" : flags & F_PLUS ? "+" : flags & F_SPACE ? " " : "";
    if (v.u >> 63) x = -x;
    if ((v.u >> 52 & 0x7ff) == 0x7ff) {
        const char *b = v.u & ((1ULL << 52) - 1) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        emit(s, sign, "", b, 3, width, flags & ~F_ZERO, 0);
        return;
    }
    if (prec < 0) prec = 6;
    static char dig[1300], body[1500];
    int pos, len = exact_digits(x, dig, &pos);
    if (len == 1 && dig[0] == '0') pos = 1;
    int g = lc == 'g', alt = flags & F_HASH;
    if (g) {                                       /* %g: %e or %f by the exponent, P significant digits */
        int P = prec ? prec : 1;
        static char cp[1300];
        for (int k = 0; k < len; k++) cp[k] = dig[k];
        int p2 = pos, l2 = round_digits(cp, len, P, &p2);   /* (on a copy: the real rounding comes below) */
        int X = (l2 == 0 || (l2 == 1 && dig[0] == '0' && x == 0)) ? 0 : p2 - 1;
        if (x == 0) X = 0;
        if (P > X && X >= -4) { lc = 'f'; prec = P - 1 - X; }
        else { lc = 'e'; prec = P - 1; }
    }
    long bl = 0;
    if (lc == 'f') {
        int keep = pos + prec;
        len = round_digits(dig, len, keep, &pos);
        if (len == 0) { pos = 1; }
        /* integer part */
        if (pos <= 0) body[bl++] = '0';
        else for (int k = 0; k < pos; k++) body[bl++] = k < len ? dig[k] : '0';
        if (prec || alt) body[bl++] = '.';
        for (int k = 0; k < prec; k++) { int i = pos + k; body[bl++] = i >= 0 && i < len ? dig[i] : '0'; }
    } else {                                       /* %e */
        len = round_digits(dig, len, prec + 1, &pos);
        int X = x == 0 ? 0 : pos - 1;
        body[bl++] = len > 0 ? dig[0] : '0';
        if (prec || alt) body[bl++] = '.';
        for (int k = 1; k <= prec; k++) body[bl++] = k < len ? dig[k] : '0';
        body[bl++] = upper ? 'E' : 'e';
        body[bl++] = X < 0 ? '-' : '+';
        if (X < 0) X = -X;
        char ex[8]; int en = 0;
        do { ex[en++] = '0' + X % 10; X /= 10; } while (X);
        if (en < 2) ex[en++] = '0';
        while (en) body[bl++] = ex[--en];
    }
    if (g && !alt) {                               /* %g: no trailing zeros, nor a lonely point */
        long e = bl, dot = -1;
        for (long k = 0; k < bl; k++) { if (body[k] == '.') dot = k; if (body[k] == 'e' || body[k] == 'E') { e = k; break; } }
        if (dot >= 0) {
            long t = e;
            while (t > dot + 1 && body[t - 1] == '0') t--;
            if (t == dot + 1) t = dot;
            for (long k = e; k < bl; k++) body[t + k - e] = body[k];
            bl -= e - t;
        }
    }
    emit(s, sign, "", body, bl, width, flags, 1);
}

static long vformat_sink(Sink *s, const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f != '%') { put(s, *f); continue; }
        f++;
        int flags = 0, width = 0, prec = -1;
        for (;; f++) {
            if (*f == '-') flags |= F_MINUS; else if (*f == '+') flags |= F_PLUS; else if (*f == ' ') flags |= F_SPACE;
            else if (*f == '#') flags |= F_HASH; else if (*f == '0') flags |= F_ZERO; else break;
        }
        if (*f == '*') { width = va_arg(ap, int); if (width < 0) { flags |= F_MINUS; width = -width; } f++; }
        else while (*f >= '0' && *f <= '9') width = width * 10 + *f++ - '0';
        if (*f == '.') {
            f++;
            prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; }
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + *f++ - '0';
        }
        int len = 0;                                /* 0 int, 1 char, 2 short, 3 long, 4 long long, 5 long double */
        for (;; f++) {
            if (*f == 'h') len = len == 2 ? 1 : 2;
            else if (*f == 'l') len = len == 3 ? 4 : 3;
            else if (*f == 'L') len = 5;
            else if (*f == 'z' || *f == 'j' || *f == 't') len = 3;
            else break;
        }
        char c = *f;
        if (!c) break;
        if (c == 'f' || c == 'F' || c == 'e' || c == 'E' || c == 'g' || c == 'G') {
            double d = len == 5 ? (double)va_arg(ap, long double) : va_arg(ap, double);
            fmt_float(s, d, c, prec, width, flags);
            continue;
        }
        if (c == 'c') { char ch = (char)va_arg(ap, int); emit(s, "", "", &ch, 1, width, flags, 0); continue; }
        if (c == 's') {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            long n = 0;
            while (str[n] && (prec < 0 || n < prec)) n++;
            emit(s, "", "", str, n, width, flags, 0);
            continue;
        }
        if (c == '%') { put(s, '%'); continue; }
        if (c == 'n') { int *p = va_arg(ap, int *); *p = (int)s->count; continue; }
        unsigned long long u;
        int neg = 0;
        if (c == 'd' || c == 'i') {
            long long x = len >= 3 ? va_arg(ap, long) : va_arg(ap, int);
            if (len == 1) x = (signed char)x; else if (len == 2) x = (short)x;
            neg = x < 0;
            u = neg ? -(unsigned long long)x : (unsigned long long)x;
        } else if (c == 'p') { u = (uintptr_t)va_arg(ap, void *); flags |= F_HASH; c = 'x'; len = 3; }
        else {
            u = len >= 3 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            if (len == 1) u = (unsigned char)u; else if (len == 2) u = (unsigned short)u;
        }
        int base = c == 'o' ? 8 : c == 'x' || c == 'X' ? 16 : 10;
        const char *digs = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        char num[64];
        int nd = 0;
        while (u) { num[nd++] = digs[u % base]; u /= base; }
        char body[96];
        int bl = 0;
        int minz = prec >= 0 ? prec : 1;
        if (c == 'o' && (flags & F_HASH) && minz <= nd) minz = nd + 1;
        for (int k = nd; k < minz; k++) body[bl++] = '0';
        while (nd) body[bl++] = num[--nd];
        const char *sign = neg ? "-" : (c == 'd' || c == 'i') && (flags & F_PLUS) ? "+" : (c == 'd' || c == 'i') && (flags & F_SPACE) ? " " : "";
        const char *prefix = (flags & F_HASH) && base == 16 && bl && !(bl == 1 && body[0] == '0' && c != 'x') ? (c == 'X' ? "0X" : "0x") : "";
        if ((flags & F_HASH) && base == 16 && body[0] == '0' && bl == 1 && *prefix) prefix = "";
        emit(s, sign, prefix, body, bl, width, flags, prec < 0);
    }
    return s->count;
}

/* ---- the printf family */
typedef struct { char *p; size_t cap, n; } SBuf;
static void sput(void *ctx, char c) { SBuf *b = ctx; if (b->n + 1 < b->cap) b->p[b->n] = c; b->n++; }

int vsnprintf(char *buf, size_t cap, const char *f, va_list ap)
{
    SBuf b = { buf, cap, 0 };
    Sink s = { sput, &b, 0 };
    vformat_sink(&s, f, ap);
    if (cap) buf[b.n < cap ? b.n : cap - 1] = 0;
    return (int)b.n;
}
int snprintf(char *buf, size_t cap, const char *f, ...) { va_list ap; va_start(ap, f); int n = vsnprintf(buf, cap, f, ap); va_end(ap); return n; }
int vsprintf(char *buf, const char *f, va_list ap) { return vsnprintf(buf, (size_t)-1 / 2, f, ap); }
int sprintf(char *buf, const char *f, ...) { va_list ap; va_start(ap, f); int n = vsnprintf(buf, (size_t)-1 / 2, f, ap); va_end(ap); return n; }

typedef struct { int fd; char b[4096]; long n; } OBuf;      /* standard output, buffered until a line ends */
static OBuf out1 = { 1, { 0 }, 0 };
void fmt_flush(void) { if (out1.n) fmt_write(1, out1.b, out1.n); out1.n = 0; }
static void oput(void *ctx, char c) { OBuf *o = ctx; o->b[o->n++] = c; if (o->n == sizeof o->b) fmt_flush(); }

int vprintf(const char *f, va_list ap) { Sink s = { oput, &out1, 0 }; long n = vformat_sink(&s, f, ap); fmt_flush(); return (int)n; }
int printf(const char *f, ...) { va_list ap; va_start(ap, f); int n = vprintf(f, ap); va_end(ap); return n; }
int putchar(int c) { oput(&out1, (char)c); fmt_flush(); return (unsigned char)c; }
int puts(const char *str) { while (*str) oput(&out1, *str++); oput(&out1, '\n'); fmt_flush(); return 1; }

/* the core, for other streams (libc.c) */
long fmt_vformat(void (*p)(void *, char), void *ctx, const char *f, va_list ap) { Sink s = { p, ctx, 0 }; return vformat_sink(&s, f, ap); }

/* ---- strtod: exact, rounded to nearest-even (subnormals included) */
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c | 0x20 : c; }

static double make_double(uint64_t q, int e2, int sticky)   /* q * 2^e2 (+ a little, if sticky), rounded */
{
    if (!q) return 0;
    int bits = 64 - __builtin_clzll(q);
    int exp = bits - 1 + e2;                       /* the value is in [2^exp, 2^(exp+1)) */
    int keep = exp >= -1022 ? 53 : 53 - (-1022 - exp);   /* subnormal: fewer bits */
    if (keep < 0) return 0;
    int drop = bits - keep;
    uint64_t m;
    if (drop > 0) {
        uint64_t half = 1ULL << (drop - 1), rest = q & ((drop >= 64 ? 0 : 1ULL << drop) - 1);
        m = drop >= 64 ? 0 : q >> drop;
        if (rest > half || (rest == half && (sticky || (m & 1)))) m++;
        e2 += drop;
        if (keep > 0 && m >> keep) { m >>= 1; e2++; }   /* carried into a new bit */
    } else { m = q << -drop; e2 += drop; }
    /* m has keep (or fewer, if 0) bits; the value is m * 2^e2 */
    int top = m ? 63 - __builtin_clzll(m) : 0;
    int E = top + e2;
    union { double d; uint64_t u; } v;
    if (E > 1023) { v.u = 0x7ffULL << 52; return v.d; }
    if (E < -1022 || top < 52) {                  /* subnormal (or the smallest normal reached by rounding) */
        int sh = e2 + 1074;
        uint64_t frac = sh >= 0 ? m << sh : m >> -sh;
        v.u = frac;
        return v.d;
    }
    v.u = (uint64_t)(E + 1023) << 52 | (m & ((1ULL << 52) - 1));
    return v.d;
}

double strtod(const char *s, char **end)
{
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    union { double d; uint64_t u; } r;
    if (lower(p[0]) == 'i' && lower(p[1]) == 'n' && lower(p[2]) == 'f') {
        p += 3;
        if (lower(p[0]) == 'i' && lower(p[1]) == 'n' && lower(p[2]) == 'i' && lower(p[3]) == 't' && lower(p[4]) == 'y') p += 5;
        r.u = 0x7ffULL << 52;
        if (end) *end = (char *)p;
        return neg ? -r.d : r.d;
    }
    if (lower(p[0]) == 'n' && lower(p[1]) == 'a' && lower(p[2]) == 'n') {
        p += 3;
        r.u = 0x7ff8000000000000ULL;
        if (end) *end = (char *)p;
        return neg ? -r.d : r.d;
    }
    if (p[0] == '0' && lower(p[1]) == 'x') {       /* hexadecimal: exact bits */
        const char *q = p + 2;
        uint64_t m = 0;
        int e2 = 0, any = 0, sticky = 0, dot = 0;
        for (;; q++) {
            int c = lower(*q), d;
            if (c == '.' && !dot) { dot = 1; continue; }
            if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10; else break;
            any = 1;
            if (m >> 59) { sticky |= d != 0; if (!dot) e2 += 4; }
            else { m = m << 4 | d; if (dot) e2 -= 4; }
        }
        if (!any) { if (end) *end = (char *)s; return 0; }
        if (lower(*q) == 'p') {
            const char *t = q + 1;
            int en = 0, x = 0, ok = 0;
            if (*t == '+' || *t == '-') en = *t++ == '-';
            while (*t >= '0' && *t <= '9') { if (x < 100000) x = x * 10 + *t - '0'; t++; ok = 1; }
            if (ok) { e2 += en ? -x : x; q = t; }
        }
        if (end) *end = (char *)q;
        double d = make_double(m, e2, sticky);
        return neg ? -d : d;
    }
    /* decimal: the significant digits (up to 800) and the exponent */
    static char dig[820];
    int nd = 0, dexp = 0, any = 0, dot = 0, sticky = 0;
    for (;; p++) {
        if (*p == '.' && !dot) { dot = 1; continue; }
        if (*p < '0' || *p > '9') break;
        any = 1;
        if (*p == '0' && !nd) { if (dot) dexp--; continue; }
        if (nd < 800) { dig[nd++] = *p; if (dot) dexp--; }
        else { sticky |= *p != '0'; if (!dot) dexp++; }
    }
    if (!any) { if (end) *end = (char *)s; return 0; }
    if (lower(*p) == 'e') {
        const char *t = p + 1;
        int en = 0, x = 0, ok = 0;
        if (*t == '+' || *t == '-') en = *t++ == '-';
        while (*t >= '0' && *t <= '9') { if (x < 100000) x = x * 10 + *t - '0'; t++; ok = 1; }
        if (ok) { dexp += en ? -x : x; p = t; }
    }
    if (end) *end = (char *)p;
    if (!nd) return neg ? -0.0 : 0.0;
    if (dexp + nd > 310) { r.u = 0x7ffULL << 52; return neg ? -r.d : r.d; }
    if (dexp + nd < -330) return neg ? -0.0 : 0.0;
    /* q = digits * 10^dexp * 2^k, exactly divided: 62 to 64 bits, the rest only "sticky" */
    static Big n2, dd, t;
    big_set(&n2, 0);
    for (int j = 0; j < nd; j++) { if (!n2.n) big_set(&n2, (uint64_t)(dig[j] - '0')); else big_mul_small(&n2, 10, dig[j] - '0'); }
    big_set(&dd, 1);
    if (dexp >= 0) for (int j = 0; j < dexp; j++) big_mul_small(&n2, 10, 0);
    else for (int j = 0; j < -dexp; j++) big_mul_small(&dd, 10, 0);
    int kk = 63 - (big_bits(&n2) - big_bits(&dd));
    if (kk > 0) big_shl(&n2, kk); else if (kk < 0) big_shl(&dd, -kk);
    uint64_t qq = 0;
    for (int b = 63; b >= 0; b--) {                  /* long division, one quotient bit at a time */
        t = dd;
        big_shl(&t, b);
        if (big_cmp(&n2, &t) >= 0) { big_sub(&n2, &t); qq |= 1ULL << b; }
    }
    sticky |= n2.n != 0;
    double d = make_double(qq, -kk, sticky);
    return neg ? -d : d;
}

long double strtold(const char *s, char **end) { return strtod(s, end); }

unsigned long long strtoull(const char *s, char **end, int base)
{
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && lower(p[1]) == 'x') { p += 2; base = 16; }
    else if (base == 0 && p[0] == '0') base = 8;
    else if (base == 0) base = 10;
    unsigned long long v = 0;
    const char *start = p;
    for (;; p++) {
        int c = lower(*p), d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : 99;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)(p == start ? s : p);
    return neg ? -v : v;
}
unsigned long strtoul(const char *s, char **end, int base) { return strtoull(s, end, base); }
long strtol(const char *s, char **end, int base) { return (long)strtoull(s, end, base); }
long long strtoll(const char *s, char **end, int base) { return (long long)strtoull(s, end, base); }
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
