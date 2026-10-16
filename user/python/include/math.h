/*
 * math.h - For MicroPython's sources: the double-precision functions of its
 * lib/libm_dbl (musl's), and the classification macros.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#define NAN       (0.0f / 0.0f)
#define INFINITY  (1e300f * 1e300f)
#define HUGE_VAL  (1e300 * 1e300)
typedef float float_t;
typedef double double_t;
enum { FP_NAN, FP_INFINITE, FP_ZERO, FP_SUBNORMAL, FP_NORMAL };
int __fpclassifyd(double x);
int __signbitd(double x);
#define fpclassify(x) __fpclassifyd(x)
#define signbit(x)    __signbitd(x)
#ifdef __GNUC__
#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#else                                   /* (sicc: x is a plain variable wherever MicroPython asks) */
#define isnan(x)      ((x) != (x))
#define isinf(x)      (!isnan(x) && isnan((x) - (x)))
#define isfinite(x)   (!isnan((x) - (x)))
#endif

double acos(double), asin(double), atan(double), atan2(double, double), cos(double), sin(double), tan(double);
double acosh(double), asinh(double), atanh(double), cosh(double), sinh(double), tanh(double);
double exp(double), expm1(double), log(double), log10(double), log1p(double), pow(double, double), sqrt(double);
double ceil(double), floor(double), trunc(double), round(double), rint(double), nearbyint(double), fabs(double);
double fmod(double, double), modf(double, double *), frexp(double, int *), ldexp(double, int), scalbn(double, int);
double copysign(double, double), erf(double), erfc(double), lgamma(double), tgamma(double);
double log2(double), nan(const char *);
#define sqrt(x) __builtin_sqrt(x)           /* (the instruction: sqrtsd, fsqrt) */
