/*
 * math.c - The few mathematical functions the engine needs, since it runs
 * without a C library: exp (softmax, SiLU), log (rotary frequencies), sine
 * and cosine (rotary embeddings), square root (normalisation).
 *
 * Each reduces its argument to a small range, then sums a short series.
 * Computed in double precision, they are exact to float precision.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "internal.h"

static double ldexp2(double x, int k)          /* x * 2^k, for |k| < 1023 */
{
    union { double d; uint64_t u; } s = { .u = (uint64_t)(k + 1023) << 52 };
    return x * s.d;
}

static double nearest(double x) { return x >= 0 ? (double)(int64_t)(x + 0.5) : -(double)(int64_t)(0.5 - x); }

/* e^x: x = k*ln2 + r with |r| <= ln2/2, e^x = 2^k * e^r (Taylor series of e^r). */
double m_exp(double x)
{
    if (x > 709) return 1e308 * 10;            /* +infinity */
    if (x < -708) return 0;
    double k = nearest(x * 1.4426950408889634);
    double r = x - k * 0.6931471805599453;
    double p = 1 + r * (1 + r * (1.0/2 + r * (1.0/6 + r * (1.0/24 + r * (1.0/120 + r * (1.0/720 + r * (1.0/5040 + r / 40320)))))));
    return ldexp2(p, (int)k);
}

float m_expf(float x) { return x > 88.8f ? (float)1e39 : x < -104 ? 0 : (float)m_exp(x); }

/* ln x for x > 0: x = m * 2^e with m in [0.71, 1.41), ln m = 2 atanh((m-1)/(m+1)). */
double m_log(double x)
{
    union { double d; uint64_t u; } v = { .d = x };
    int e = (int)((v.u >> 52) & 0x7FF) - 1023;
    v.u = (v.u & ((1ULL << 52) - 1)) | (1023ULL << 52);  /* m in [1, 2) */
    double m = v.d;
    if (m > 1.4142135623730951) { m /= 2; e++; }
    double s = (m - 1) / (m + 1), s2 = s * s, t = s, sum = 0;
    for (int i = 1; i < 30; i += 2) { sum += t / i; t *= s2; }
    return 2 * sum + e * 0.6931471805599453;
}

/* sin and cos: a = k*pi/2 + r with |r| <= pi/4 (pi/2 split in two parts so
 * the subtraction stays exact), then Taylor series, then the quadrant. */
void m_sincos(double a, double *s, double *c)
{
    double k = nearest(a * 0.6366197723675814);
    double r = (a - k * 1.57079632673412561417e+00) - k * 6.07710050650619224932e-11;
    double r2 = r * r;
    double sn = r * (1 - r2 / 6 * (1 - r2 / 20 * (1 - r2 / 42 * (1 - r2 / 72 * (1 - r2 / 110 * (1 - r2 / 156))))));
    double cs = 1 - r2 / 2 * (1 - r2 / 12 * (1 - r2 / 30 * (1 - r2 / 56 * (1 - r2 / 90 * (1 - r2 / 132 * (1 - r2 / 182))))));
    switch ((int64_t)k & 3) {
    case 0: *s = sn;  *c = cs;  break;
    case 1: *s = cs;  *c = -sn; break;
    case 2: *s = -sn; *c = -cs; break;
    default: *s = -cs; *c = sn;
    }
}

float m_sqrtf(float x) { return __builtin_sqrtf(x); }   /* one instruction (sqrtss) */
