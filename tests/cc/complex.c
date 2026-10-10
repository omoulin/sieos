/* complex.c - _Complex arithmetic and passing.
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
#define I (__extension__ 1.0iF)
double _Complex gz = 1.5 + 2.0 * I, garr[2] = { 3.0, -1.0 * I };
float _Complex gf = { 1.0f, -2.0f };
long double _Complex gl = 2.0L - 3.0L * I;
struct S { char c; double _Complex z; } gs = { 'q', 4.0 + 0.5 * I };
double _Complex mul(double _Complex a, double _Complex b) { return a * b; }
float _Complex fmul(float _Complex a, float _Complex b) { return a * b + 1.0f; }
long double _Complex lops(long double _Complex a, long double _Complex b) { return a * b - a / b + ~a; }
double _Complex vsum(int n, ...) { va_list ap; va_start(ap, n); double _Complex s = 0; while (n--) s += va_arg(ap, double _Complex); va_end(ap); return s; }
static void pr(const char *s, double _Complex z) { printf("%s %g %g\n", s, __real__ z, __imag__ z); }
int main(void)
{
    double _Complex a = 1.0 + 2.0 * I, b = 3.0 - 1.0 * I;
    pr("add", a + b); pr("sub", a - b); pr("mul", a * b); pr("div", a / b); pr("neg", -a); pr("conj", ~a);
    pr("mix", a * 2.0 + 3); pr("int", 5 - a); printf("%d %d %d\n", a == a, a != b, a == 1.0 + 2.0 * I);
    pr("g", gz); pr("ga0", garr[0]); pr("ga1", garr[1]); printf("%g %g\n", __real__ gf, __imag__ gf);
    printf("%Lg %Lg %c\n", __real__ gl, __imag__ gl, gs.c); pr("gs", gs.z);
    pr("call", mul(a, b)); float _Complex f = fmul(2.0f + I, 1.0f - I); printf("%g %g\n", __real__ f, __imag__ f);
    long double _Complex l = lops(gl, 1.0L + 1.0L * I); printf("%.10Lg %.10Lg\n", __real__ l, __imag__ l);
    pr("va", vsum(2, a, b));
    double _Complex c = a; c += b; c *= 2; c /= a; c -= 1; pr("c", c);
    __real__ c = 9; __imag__ c *= 2; pr("parts", c);
    double r = a; _Bool nz = a; int i = (int)b; printf("%g %d %d\n", r, nz, i);
    float _Complex fc = a; long double _Complex lc = fc; printf("%g %Lg\n", __imag__ fc, __imag__ lc);
    if (a) printf("true\n");
    double _Complex z0 = 0; if (!z0) printf("zero\n");
    printf("%zu %zu %zu\n", sizeof(float _Complex), sizeof(double _Complex), sizeof(long double _Complex));
    return 0;
}
