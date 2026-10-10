/* Integer arithmetic, promotions and conversions. Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
int g = -7; unsigned ug = 7; long lg = -123456789012L; unsigned long ulg = 18446744073709551615UL;
signed char sc = -100; unsigned char uc = 200; short ss = -30000; unsigned short us = 60000;
static int sq(int x) { return x * x; }
int main(void) {
    int a = 17, b = -5; unsigned u = 0xFFFFFFF0u; long l = 1L << 40;
    printf("%d %d %d %d %d\n", a + b, a - b, a * b, a / b, a % b);
    printf("%d %d %d %d\n", b / 2, b % 2, -b >> 1, b >> 1);
    printf("%u %u %u %u\n", u + 32, u * 3, u / 3, u % 7);
    printf("%u %u %d\n", u >> 4, u << 2, (int)u >> 4);
    printf("%ld %ld %ld %ld\n", l * 3 + 1, l / 7, l % 1000003, -l >> 3);
    printf("%lu %lu %lu\n", ulg / 3, ulg >> 60, ulg % 1000);
    printf("%d %d %d %d\n", sc + uc, ss + us, sc * sc, (unsigned char)(uc + 100));
    printf("%d %d %d\n", (char)300, (short)70000, (signed char)uc);
    printf("%d %d %d %d\n", g < ug, -1 < 0u, (long)-1 < 0, sc < uc);
    printf("%d %d %d %d %d %d\n", a == 17, a != 17, a < b, a <= b, a > b, a >= b);
    printf("%d %d %d\n", !a, !!b, ~a);
    printf("%d %d %d %d\n", a & b, a | b, a ^ b, a & 0xF0);
    printf("%ld %lu %ld\n", lg, (unsigned long)lg, lg * -1);
    int x = 5; x += 3; x -= 1; x *= 4; x /= 3; x %= 5; x <<= 3; x >>= 1; x &= 0xff; x |= 0x100; x ^= 0x11;
    printf("%d\n", x);
    int i = 0, j; j = i++; j += i++; int k1 = ++i; int k2 = i--; printf("%d %d %d %d\n", i, j, k1, k2);
    long big = 2147483647; big += 1; int wrap = 2147483647; wrap += 1;
    printf("%ld %d\n", big, wrap);
    printf("%d %d %d\n", sq(9), sq(-3) + sq(4), sq(sq(2)));
    unsigned long m = 0; for (int k = 0; k < 64; k += 7) m |= 1UL << k; printf("%lx\n", m);
    printf("%d %d\n", 1000000 * 3 / 3, (int)(3000000000u / 3));
    _Bool bo = 42; printf("%d %d\n", bo, (_Bool)0.5 + (_Bool)0);
    printf("%d %d %d\n", 7 / -2, -7 / 2, -7 % 3);
    unsigned short us2 = 65535; us2++; printf("%d\n", us2);
    char c = 'A'; c += 1; printf("%c %d\n", c, c);
    printf("%d\n", (int)sizeof(a + 1L) + (int)sizeof(char) + (int)sizeof(short));
    return 0;
}
