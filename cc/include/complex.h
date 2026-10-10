/* complex.h - sicc's header: complex numbers. creal, cimag and conj are
 * operations of the compiler; the other functions (cabs, cexp...) belong to
 * the C library's mathematics. Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#ifndef __SICC_COMPLEX_H
#define __SICC_COMPLEX_H
#define complex _Complex
#define _Complex_I (__extension__ 1.0iF)
#define I _Complex_I
#define CMPLX(x, y) __builtin_complex((double)(x), (double)(y))
#define CMPLXF(x, y) __builtin_complex((float)(x), (float)(y))
#define CMPLXL(x, y) __builtin_complex((long double)(x), (long double)(y))
#define creal(z) ((double)__real__ (z))
#define crealf(z) ((float)__real__ (z))
#define creall(z) ((long double)__real__ (z))
#define cimag(z) ((double)__imag__ (z))
#define cimagf(z) ((float)__imag__ (z))
#define cimagl(z) ((long double)__imag__ (z))
#define conj(z) (~(double _Complex)(z))
#define conjf(z) (~(float _Complex)(z))
#define conjl(z) (~(long double _Complex)(z))
double cabs(double _Complex z);
float cabsf(float _Complex z);
long double cabsl(long double _Complex z);
double carg(double _Complex z);
float cargf(float _Complex z);
double _Complex cexp(double _Complex z);
double _Complex clog(double _Complex z);
double _Complex csqrt(double _Complex z);
double _Complex cpow(double _Complex x, double _Complex y);
double _Complex csin(double _Complex z);
double _Complex ccos(double _Complex z);
double _Complex ctan(double _Complex z);
double _Complex cproj(double _Complex z);
float _Complex cexpf(float _Complex z);
float _Complex csqrtf(float _Complex z);
#endif
