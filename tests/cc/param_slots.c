/* Parameters whose address is taken live in frame slots of their own size:
 * storing a char or short must not touch its neighbours.
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
static void show(const void *p, int n) { const unsigned char *b = p; for (int i = 0; i < n; i++) printf("%02x", b[i]); printf(" "); }
static int f(float a, signed char c, signed char d, short e, _Bool g, unsigned char h, double x)
{
    show(&a, 4); show(&c, 1); show(&d, 1); show(&e, 2); show(&g, 1); show(&h, 1); show(&x, 8);
    printf("\n%g %d %d %d %d %d %g\n", a, c, d, e, g, h, x);
    return c + d + e;
}
struct P { char c; short s; };
static int g(struct P p, char a, short b, char c) { char *q = &a; short *r = &b; return p.c + p.s + *q + *r + *&c; }
int main(void)
{
    printf("%d\n", f(1.5f, -3, 7, -9, 1, 200, 2.25));
    struct P p = { 1, 2 };
    printf("%d\n", g(p, 3, 4, 5));
    return 0;
}
