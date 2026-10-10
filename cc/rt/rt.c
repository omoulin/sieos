/*
 * rt.c - sicc's runtime: the operations its generated code calls instead of
 * inlining them (any target; needed on AArch64, where no other compiler's
 * runtime is linked in):
 *   __udivti3 __umodti3 __divti3 __modti3   128-bit division
 *   __floattidf __floatuntidf __floattisf __floatuntisf
 *   __fixdfti __fixunsdfti __fixsfti __fixunssfti
 *                                           128-bit integers to and from floating point
 *   __mulsc3 __muldc3 __divsc3 __divdc3     complex multiplication and division
 *     (with the infinities and NaNs of C11 Annex G)
 * Plain C: no division of 128-bit values inside (that would call itself).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */

typedef unsigned __int128 u128;
typedef __int128 i128;

static u128 udivmod(u128 n, u128 d, u128 *rem)
{
    if (!d) { *rem = 0; return 0; }               /* (division by zero: undefined; no trap here) */
    u128 q = 0, r = 0;
    if (!(n >> 64) && !(d >> 64)) {               /* both fit in 64 bits: the hardware divides */
        unsigned long a = (unsigned long)n, b = (unsigned long)d;
        *rem = a % b;
        return a / b;
    }
    for (int i = 127; i >= 0; i--) {              /* shift and subtract */
        r = r << 1 | (n >> i & 1);
        if (r >= d) { r -= d; q |= (u128)1 << i; }
    }
    *rem = r;
    return q;
}

u128 __udivti3(u128 a, u128 b) { u128 r; return udivmod(a, b, &r); }
u128 __umodti3(u128 a, u128 b) { u128 r; udivmod(a, b, &r); return r; }

i128 __divti3(i128 a, i128 b)
{
    int neg = (a < 0) != (b < 0);
    u128 r, q = udivmod(a < 0 ? -(u128)a : (u128)a, b < 0 ? -(u128)b : (u128)b, &r);
    return neg ? -(i128)q : (i128)q;
}

i128 __modti3(i128 a, i128 b)
{
    u128 r;
    udivmod(a < 0 ? -(u128)a : (u128)a, b < 0 ? -(u128)b : (u128)b, &r);
    return a < 0 ? -(i128)r : (i128)r;
}

/* ---- 128-bit integers and floating point. To floating point: the top 64
 * significant bits, with a "sticky" bit 0 for anything nonzero below them, are
 * rounded once by the hardware (the sticky bit is far below the rounding
 * point), then scaled exactly by a power of two. */
static int top64(u128 n, unsigned long *m)          /* n = *m * 2^shift (rounding aside); returns shift */
{
    int bits = (n >> 64) ? 128 - __builtin_clzl((unsigned long)(n >> 64)) : 64, shift = bits - 64;
    *m = (unsigned long)(n >> shift) | ((n & (((u128)1 << shift) - 1)) != 0);
    return shift;
}
static double pow2(int k) { union { double d; unsigned long u; } v = { .u = (unsigned long)(k + 1023) << 52 }; return v.d; }

double __floatuntidf(u128 n) { unsigned long m; int sh = top64(n, &m); return (double)m * pow2(sh); }
float __floatuntisf(u128 n) { unsigned long m; int sh = top64(n, &m); return (float)m * (float)pow2(sh); }
double __floattidf(i128 n) { return n < 0 ? -__floatuntidf(-(u128)n) : __floatuntidf((u128)n); }
float __floattisf(i128 n) { return n < 0 ? -__floatuntisf(-(u128)n) : __floatuntisf((u128)n); }

/* To integer, truncating (out of range: undefined, as in C). */
u128 __fixunsdfti(double x)
{
    if (!(x >= 1.0)) return 0;                      /* also NaN and negatives */
    if (x < 0x1p64) return (unsigned long)x;
    union { double d; unsigned long u; } v = { x };
    int e = (int)(v.u >> 52 & 0x7ff) - 1075;        /* x = m * 2^e, m of 53 bits; here e >= 12 */
    unsigned long m = (v.u & ((1UL << 52) - 1)) | 1UL << 52;
    return e >= 128 - 53 ? ~(u128)0 : (u128)m << e;
}
i128 __fixdfti(double x) { return x < 0 ? -(i128)__fixunsdfti(-x) : (i128)__fixunsdfti(x); }
u128 __fixunssfti(float x) { return __fixunsdfti(x); }
i128 __fixsfti(float x) { return __fixdfti(x); }

/* ---- complex numbers, by the rules of C11 Annex G (G.5.1) */
static int isnan_(double x) { return x != x; }
static int isinf_(double x) { return !isnan_(x) && isnan_(x - x); }
static double copysign_(double x, double s)
{
    union { double d; unsigned long u; } a = { x }, b = { s };
    a.u = (a.u & ~(1UL << 63)) | (b.u & 1UL << 63);
    return a.d;
}
static double fabs_(double x) { return copysign_(x, 1.0); }
static double fmax_(double a, double b) { return isnan_(a) ? b : isnan_(b) ? a : a > b ? a : b; }
static double inf_(void) { union { double d; unsigned long u; } v = { .u = 0x7ffUL << 52 }; return v.d; }
static double logb_(double x)                     /* the binary exponent of x (finite, not 0) */
{
    union { double d; unsigned long u; } v = { x };
    int e = (int)(v.u >> 52 & 0x7ff);
    if (!e) { v.d = x * 0x1p52; return (int)(v.u >> 52 & 0x7ff) - 1023 - 52; }
    return e - 1023;
}
static double scalbn_(double x, int n)            /* x * 2^n, in steps that stay representable */
{
    while (n > 1000) { x *= 0x1p1000; n -= 1000; }
    while (n < -1000) { x *= 0x1p-1000; n += 1000; }
    union { double d; unsigned long u; } v = { .u = (unsigned long)(n + 1023) << 52 };
    return x * v.d;
}

/* one template, two types: float arithmetic for the float versions (as other runtimes do) */
#define T double
#define MUL mul
#define DIV divide
static void MUL(T a, T b, T c, T d, T *re, T *im)
{
    T ac = a * c, bd = b * d, ad = a * d, bc = b * c;
    T x = ac - bd, y = ad + bc;
    if (isnan_(x) && isnan_(y)) {                 /* recover infinities that the formula lost */
        int recalc = 0;
        if (isinf_(a) || isinf_(b)) {
            a = (T)copysign_(isinf_(a) ? 1.0 : 0.0, a); b = (T)copysign_(isinf_(b) ? 1.0 : 0.0, b);
            if (isnan_(c)) c = (T)copysign_(0.0, c);
            if (isnan_(d)) d = (T)copysign_(0.0, d);
            recalc = 1;
        }
        if (isinf_(c) || isinf_(d)) {
            c = (T)copysign_(isinf_(c) ? 1.0 : 0.0, c); d = (T)copysign_(isinf_(d) ? 1.0 : 0.0, d);
            if (isnan_(a)) a = (T)copysign_(0.0, a);
            if (isnan_(b)) b = (T)copysign_(0.0, b);
            recalc = 1;
        }
        if (!recalc && (isinf_(ac) || isinf_(bd) || isinf_(ad) || isinf_(bc))) {
            if (isnan_(a)) a = (T)copysign_(0.0, a);
            if (isnan_(b)) b = (T)copysign_(0.0, b);
            if (isnan_(c)) c = (T)copysign_(0.0, c);
            if (isnan_(d)) d = (T)copysign_(0.0, d);
            recalc = 1;
        }
        if (recalc) { x = (T)inf_() * (a * c - b * d); y = (T)inf_() * (a * d + b * c); }
    }
    *re = x; *im = y;
}

static void DIV(T a, T b, T c, T d, T *re, T *im)
{
    int ilogbw = 0;
    T logbw = (T)fmax_(fabs_(c), fabs_(d));
    if (!isinf_(logbw) && !isnan_(logbw) && logbw != 0) { ilogbw = (int)logb_(logbw); c = (T)scalbn_(c, -ilogbw); d = (T)scalbn_(d, -ilogbw); }
    T denom = c * c + d * d;
    T x = (T)scalbn_((a * c + b * d) / denom, -ilogbw), y = (T)scalbn_((b * c - a * d) / denom, -ilogbw);
    if (isnan_(x) && isnan_(y)) {
        if (denom == 0.0 && (!isnan_(a) || !isnan_(b))) { x = (T)copysign_((T)inf_(), c) * a; y = (T)copysign_((T)inf_(), c) * b; }
        else if ((isinf_(a) || isinf_(b)) && !isinf_(c) && !isnan_(c) && !isinf_(d) && !isnan_(d)) {
            a = (T)copysign_(isinf_(a) ? 1.0 : 0.0, a); b = (T)copysign_(isinf_(b) ? 1.0 : 0.0, b);
            x = (T)inf_() * (a * c + b * d); y = (T)inf_() * (b * c - a * d);
        } else if (isinf_(logbw) && !isinf_(a) && !isnan_(a) && !isinf_(b) && !isnan_(b)) {
            c = (T)copysign_(isinf_(c) ? 1.0 : 0.0, c); d = (T)copysign_(isinf_(d) ? 1.0 : 0.0, d);
            x = 0.0 * (a * c + b * d); y = 0.0 * (b * c - a * d);
        }
    }
    *re = x; *im = y;
}

#undef T
#undef MUL
#undef DIV
#define T float
#define MUL mulf
#define DIV dividef
static void MUL(T a, T b, T c, T d, T *re, T *im)
{
    T ac = a * c, bd = b * d, ad = a * d, bc = b * c;
    T x = ac - bd, y = ad + bc;
    if (isnan_(x) && isnan_(y)) {                 /* recover infinities that the formula lost */
        int recalc = 0;
        if (isinf_(a) || isinf_(b)) {
            a = (T)copysign_(isinf_(a) ? 1.0 : 0.0, a); b = (T)copysign_(isinf_(b) ? 1.0 : 0.0, b);
            if (isnan_(c)) c = (T)copysign_(0.0, c);
            if (isnan_(d)) d = (T)copysign_(0.0, d);
            recalc = 1;
        }
        if (isinf_(c) || isinf_(d)) {
            c = (T)copysign_(isinf_(c) ? 1.0 : 0.0, c); d = (T)copysign_(isinf_(d) ? 1.0 : 0.0, d);
            if (isnan_(a)) a = (T)copysign_(0.0, a);
            if (isnan_(b)) b = (T)copysign_(0.0, b);
            recalc = 1;
        }
        if (!recalc && (isinf_(ac) || isinf_(bd) || isinf_(ad) || isinf_(bc))) {
            if (isnan_(a)) a = (T)copysign_(0.0, a);
            if (isnan_(b)) b = (T)copysign_(0.0, b);
            if (isnan_(c)) c = (T)copysign_(0.0, c);
            if (isnan_(d)) d = (T)copysign_(0.0, d);
            recalc = 1;
        }
        if (recalc) { x = (T)inf_() * (a * c - b * d); y = (T)inf_() * (a * d + b * c); }
    }
    *re = x; *im = y;
}

static void DIV(T a, T b, T c, T d, T *re, T *im)
{
    int ilogbw = 0;
    T logbw = (T)fmax_(fabs_(c), fabs_(d));
    if (!isinf_(logbw) && !isnan_(logbw) && logbw != 0) { ilogbw = (int)logb_(logbw); c = (T)scalbn_(c, -ilogbw); d = (T)scalbn_(d, -ilogbw); }
    T denom = c * c + d * d;
    T x = (T)scalbn_((a * c + b * d) / denom, -ilogbw), y = (T)scalbn_((b * c - a * d) / denom, -ilogbw);
    if (isnan_(x) && isnan_(y)) {
        if (denom == 0.0 && (!isnan_(a) || !isnan_(b))) { x = (T)copysign_((T)inf_(), c) * a; y = (T)copysign_((T)inf_(), c) * b; }
        else if ((isinf_(a) || isinf_(b)) && !isinf_(c) && !isnan_(c) && !isinf_(d) && !isnan_(d)) {
            a = (T)copysign_(isinf_(a) ? 1.0 : 0.0, a); b = (T)copysign_(isinf_(b) ? 1.0 : 0.0, b);
            x = (T)inf_() * (a * c + b * d); y = (T)inf_() * (b * c - a * d);
        } else if (isinf_(logbw) && !isinf_(a) && !isnan_(a) && !isinf_(b) && !isnan_(b)) {
            c = (T)copysign_(isinf_(c) ? 1.0 : 0.0, c); d = (T)copysign_(isinf_(d) ? 1.0 : 0.0, d);
            x = 0.0 * (a * c + b * d); y = 0.0 * (b * c - a * d);
        }
    }
    *re = x; *im = y;
}

#undef T
#undef MUL
#undef DIV

static double _Complex make(double x, double y) { double _Complex r; ((double *)&r)[0] = x; ((double *)&r)[1] = y; return r; }
static float _Complex makef(double x, double y) { float _Complex r; ((float *)&r)[0] = (float)x; ((float *)&r)[1] = (float)y; return r; }

double _Complex __muldc3(double a, double b, double c, double d) { double x, y; mul(a, b, c, d, &x, &y); return make(x, y); }
double _Complex __divdc3(double a, double b, double c, double d) { double x, y; divide(a, b, c, d, &x, &y); return make(x, y); }
float _Complex __mulsc3(float a, float b, float c, float d) { float x, y; mulf(a, b, c, d, &x, &y); return makef(x, y); }
float _Complex __divsc3(float a, float b, float c, float d) { float x, y; dividef(a, b, c, d, &x, &y); return makef(x, y); }
