/* packed_bitfields.c - packed bit-fields across storage units.
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
struct __attribute__((packed)) P { unsigned char a:3; unsigned int b:20; int c:13; long d:64; unsigned long e:61; signed char f:5; short g:12; };
struct __attribute__((packed)) Q { char x; int y:24; };
struct P gp = { 5, 0xABCDE, -1234, -5, 0x1123456789ABCDEUL, -7, 1000 };
int main(void)
{
    struct P p = { 0 };
    printf("%zu %zu\n", sizeof(struct P), sizeof(struct Q));
    p.a = 6; p.b = 0xFEDCB; p.c = -4000; p.d = 0x7123456789ABCDEFL; p.e = 0x1FEDCBA987654321UL; p.f = -9; p.g = -2048;
    printf("%u %x %d %lx %lx %d %d\n", p.a, p.b, p.c, p.d, p.e, p.f, p.g);
    printf("%u %x %d %ld %lx %d %d\n", gp.a, gp.b, gp.c, gp.d, gp.e, gp.f, gp.g);
    unsigned char *q = (unsigned char *)&p; for (unsigned i = 0; i < sizeof p; i++) printf("%02x", q[i]); printf("\n");
    q = (unsigned char *)&gp; for (unsigned i = 0; i < sizeof gp; i++) printf("%02x", q[i]); printf("\n");
    struct Q qq[2] = { { 1, -5 }, { 2, 0x123456 } };
    qq[0].y += 3; printf("%d %d %d\n", qq[0].y, qq[1].y, qq[1].x);
    p.d += 1; p.e -= 2; printf("%lx %lx %d\n", p.d, p.e, (int)(p.c = 77));
    return 0;
}
