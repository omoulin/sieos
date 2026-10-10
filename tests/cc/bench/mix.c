/* mix.c - benchmark: 32-bit rotations and additions (a SHA-256-like compression loop).
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
int printf(const char *fmt, ...);
typedef unsigned u32;
static u32 ror(u32 x, int n) { return x >> n | x << (32 - n); }
static void compress(u32 st[8], const u32 in[16])
{
    u32 w[64], a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 16; i++) w[i] = in[i];
    for (int i = 16; i < 64; i++) {
        u32 s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        u32 t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + 0x428a2f98u * (i + 1) + w[i];
        u32 t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}
int main(void)
{
    u32 st[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, in[16] = { 0 };
    for (int k = 0; k < 1500000; k++) { in[k & 15] ^= k; compress(st, in); }
    printf("%08x %08x\n", st[0], st[7]);
    return 0;
}
