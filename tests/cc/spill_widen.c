/* spill_widen.c - 32-bit values spilled under register pressure, then widened to
 * 64 bits (a spill slot holds only the low 4 bytes: the top must never be
 * assumed clear); the stack is filled with junk first.
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
__attribute__((noinline)) static void junk(unsigned s) { volatile unsigned char b[8192]; for (int i = 0; i < 8192; i++) b[i] = (unsigned char)((s = s * 1103515245 + 12345) >> 16) | 0x80; }
__attribute__((noinline)) static unsigned long mix(const unsigned *v, int n)
{
    unsigned a = v[0], b = v[1], c = v[2], d = v[3], e = v[4], f = v[5], g = v[6], h = v[7], i2 = v[8], j = v[9], k = v[10], l = v[11];
    unsigned long acc = 0;
    for (int r = 0; r < n; r++) {
        unsigned t = a * 3 + b, u = c ^ (d >> 3), w = e + f * 5, x = g - h, y = i2 | j, z = k & l;
        acc += (unsigned long)t + (unsigned long)u * 7 + (unsigned long)w + (unsigned long)x + (unsigned long)y + (unsigned long)z;
        acc += v[(t + u + w) & 15];                       /* a 32-bit value as an index */
        a = b; b = c; c = d; d = e; e = f; f = g; g = h; h = i2; i2 = j; j = k; k = l; l = t ^ u ^ w ^ x ^ y ^ z;
    }
    return acc ^ a ^ b ^ c ^ d ^ e ^ f ^ g ^ h ^ i2 ^ j ^ k ^ l;
}
int main(void)
{
    unsigned v[16];
    for (int i = 0; i < 16; i++) v[i] = 0x9e3779b9u * (i + 1);
    junk(1);
    unsigned long r1 = mix(v, 1000);
    junk(2);
    unsigned long r2 = mix(v, 1000);
    printf("%lx %d\n", r1, r1 == r2);
    return 0;
}
