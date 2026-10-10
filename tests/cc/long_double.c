/* long_double.c - long double (x87 80-bit).
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
#include <float.h>
long double gl = 1.0L / 3, garr[3] = { 1.5L, -2.25L, 1e300L * 1e300L };
struct S { char c; long double x; int i; } gs = { 'a', 2.5L, 7 };
long double add(long double a, long double b) { return a + b; }
long double mix(int i, long double a, double d, float f, long double b, unsigned long u) { return a * i - b / d + f + u; }
long double sum(int n, ...) { va_list ap; va_start(ap, n); long double s = 0; for (int i = 0; i < n; i++) s += va_arg(ap, long double); va_end(ap); return s; }
struct S mk(long double x) { struct S s = { 'z', x, 3 }; return s; }
int main(void)
{
    long double a = 1.25L, b = 3;
    printf("%Lf %Lf %.20Lg\n", a + b, a * b, gl);
    printf("%Lg %Lg %Lg\n", garr[0], garr[1], garr[2]);
    printf("%c %Lf %d %zu %zu\n", gs.c, gs.x, gs.i, sizeof(long double), sizeof(struct S));
    printf("%Lf %Lf\n", add(a, b), mix(3, a, 2.0, 0.5f, b, 10));
    printf("%Lf\n", sum(3, 1.0L, 2.5L, -0.25L));
    long double c = a; c += 2; c *= c; c -= 1; c /= 4; printf("%.18Lg %d %d %d\n", c, c > a, a >= c, c == c);
    long double d = c++; printf("%Lg %Lg\n", d, c);
    printf("%ld %lu %d %u %f %f\n", (long)-c, (unsigned long)(1e19L), (int)c, (unsigned)4e9L, (double)c, (float)c);
    unsigned long big = 18446744073709551615UL; long double bl = big; printf("%Lf %lu\n", bl, (unsigned long)bl);
    long double e = -1e-4950L; printf("%Lg %d %d\n", e, !e, e ? 1 : 2);
    struct S s = mk(9.75L); printf("%c %Lf %d\n", s.c, s.x, s.i);
    long double arr[4]; for (int i = 0; i < 4; i++) arr[i] = i * 0.5L; printf("%Lf\n", arr[3] + arr[1]);
    printf("%Lg %Lg %d\n", LDBL_EPSILON, LDBL_MAX, LDBL_MANT_DIG);
    long double z = 0.0L / 0.0L; printf("%d %d\n", z == z, z != z);
    return 0;
}
