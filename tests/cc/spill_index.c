/* An address whose base and index both live in stack slots (many values
 * alive at once force the spills): both must be reloaded into different
 * scratch registers. (AArch64 once reloaded them into the same one: p + p.)
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
static unsigned char buf[256];
__attribute__((noinline)) static long f(const unsigned char *p, unsigned long i, long a, long b, long c, long d, long e, long g)
{
    long v0 = a * 3 + b, v1 = b * 5 - c, v2 = c * 7 + d, v3 = d * 11 - e, v4 = e * 13 + g, v5 = g * 17 - a;
    long v6 = a ^ b ^ c, v7 = d ^ e ^ g, v8 = a + g, v9 = b + e, v10 = c + d, v11 = a - g;
    long v12 = v0 * v1, v13 = v2 * v3, v14 = v4 * v5, v15 = v6 * v7, v16 = v8 * v9, v17 = v10 * v11;
    long r = p[i + 18] << 8 | p[i + 19];                         /* base p, index i: both spilled */
    long s = p[i + v6 % 7 + 20] + p[(i + 3) * 2];
    return r + s + v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11 + v12 + v13 + v14 + v15 + v16 + v17;
}
int main(void)
{
    for (int i = 0; i < 256; i++) buf[i] = (unsigned char)(i * 7 + 1);
    for (unsigned long i = 0; i < 8; i++) printf("%ld\n", f(buf, i, 1 + i, 2, 3, 4, 5, 6));
    return 0;
}
