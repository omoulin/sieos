/* int128.c - __int128 arithmetic.
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
typedef __int128 i128; typedef unsigned __int128 u128;
static void p(const char *s, u128 v) { printf("%s %016lx%016lx\n", s, (unsigned long)(v >> 64), (unsigned long)v); }
i128 g = -5, g2 = ((i128)1 << 100) + 7; u128 g3 = ~(u128)0 / 3;
struct S { char c; i128 x; } gs = { 1, -1 };
i128 add(i128 a, i128 b) { return a + b; }
u128 many(int a, int b, int c, int d, int e, u128 x, u128 y) { return x * y + a + b + c + d + e; }
i128 vsum(int n, ...) { va_list ap; va_start(ap, n); i128 s = 0; while (n--) s += va_arg(ap, i128); va_end(ap); return s; }
int main(void)
{
    i128 a = 1234567890123456789L, b = -987654321987654321L;
    p("mul", a * b); p("add", a + b); p("sub", a - b); p("neg", -a); p("not", ~b);
    p("and", a & b); p("or", a | b); p("xor", a ^ b);
    for (int k = 0; k < 128; k += 37) { p("shl", a << k); p("shr", (u128)b >> k); p("sar", b >> k); }
    p("div", (a * a) / b); p("mod", (a * a) % b); p("udiv", (u128)(a * a) / 12345); p("umod", (u128)(a * b) % 977);
    printf("%d %d %d %d %d %d\n", a < b, a > b, a <= a, b >= a, a == a, a != b);
    printf("%d %d\n", (u128)b > (u128)a, (u128)b < (u128)a);
    p("g", g); p("g2", g2); p("g3", g3); p("gs", gs.x);
    p("call", add(a, b)); p("many", many(1, 2, 3, 4, 5, (u128)a, 1000)); p("va", vsum(3, (i128)1, a, b));
    double d = (double)(a * 1000); long double ld = (long double)b; float f = (float)(u128)a;
    printf("%.17g %.19Lg %.9g\n", d, ld, f);
    p("fromd", (i128)-1e30); p("fromud", (u128)3.5e37); p("fromld", (i128)-12345678901234567890.0L);
    long x = (long)(a * b); unsigned u = (unsigned)(a >> 3); printf("%ld %u %d\n", x, u, (_Bool)(a ^ a));
    i128 c = a; c *= 3; c += 1; c <<= 2; c >>= 1; c /= 7; p("c", c); i128 old = c++; p("post", old); p("now", c);
    if (c) printf("nz\n"); if (!(c - c)) printf("z\n");
    unsigned long t = 1000000007, m = 0xFFFFFFFFFFFFUL; printf("%lu\n", (unsigned long)(((unsigned __int128)t * m) >> 7));
    return 0;
}
