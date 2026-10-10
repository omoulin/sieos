/* Floating point. Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
static double half(double x) { return x / 2; }
static float fsum(float a, float b, float c) { return a + b + c; }
static double many(double a, double b, double c, double d, double e, double f, double g, double h, double i, double j) { return a + b * c - d + e * f + g - h + i * j; }
double gd = 3.25; float gf = -1.5f;
int main(void) {
    double a = 1.5, b = -2.25;
    printf("%.4f %.4f %.4f %.4f\n", a + b, a - b, a * b, a / b);
    float f = 0.1f; f *= 3; printf("%.6f %.3f\n", f, fsum(1, 2.5f, -0.25f));
    printf("%d %ld %u %lu\n", (int)-3.9, (long)1e12, (unsigned)3e9, (unsigned long)1.8e19);
    printf("%.1f %.1f %.1f\n", (double)-7, (double)3000000000u, (double)18000000000000000000UL);
    printf("%d %d %d %d\n", a < b, a > b, a == 1.5, b != b);
    double nan = 0.0 / 0.0; printf("%d %d %d %d\n", nan == nan, nan != nan, nan < 1, !(nan >= 1));
    printf("%.3f %.3f\n", half(5), many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    printf("%.2f %.2f %.2f\n", gd, gf, -gd);
    double arr[3] = { 1, 2, 3 }; double s = 0; for (int i = 0; i < 3; i++) s += arr[i] * arr[i]; printf("%.1f\n", s);
    int i = 7; double d = i / 2; double e = i / 2.0; printf("%.1f %.1f\n", d, e);
    if (0.0) printf("bad\n"); if (a) printf("ok\n");
    float fx = 16777217; printf("%.1f\n", fx);
    printf("%g %g\n", 1e300 * 10, -1e-320);
    return 0;
}
