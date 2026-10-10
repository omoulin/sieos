/* abi.c - calls between sicc and the host compiler, every struct class,
 * both directions (with abi_gcc.c). Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
#include "abi.h"
long s_take(S1 a, S3 b, S8 c, S12 d, S16 e, S24 f, D2 g, DL h, IFD i, F3 j, C5 k, int x, double y)
{
    return a.c + b.a * 2 + b.b * 3 + c.a * 5 + c.b * 7 + d.a * 11 + d.b * 13 + e.a * 17 + e.b * 19 + f.a * 23 + f.b * 29 + f.c * 31 +
           (long)(g.x * 37 + g.y * 41 + h.x * 43) + h.y * 47 + i.i * 53 + (long)(i.f * 59 + i.d * 61 + j.a * 67 + j.b * 71 + j.c * 73) +
           k.s[0] * 79 + k.s[4] * 83 + x * 89 + (long)(y * 97);
}
S16 s_s16(int v) { S16 r = { v * 5L, v * 6L }; return r; }
S24 s_s24(int v) { S24 r = { v, v + 1, v + 2 }; return r; }
D2 s_d2(double v) { D2 r = { v, v * 2 }; return r; }
DL s_dl(double v) { DL r = { v, (long)v * 3 }; return r; }
IFD s_ifd(int v) { IFD r = { v, v * 0.5f, v * 0.25 }; return r; }
F3 s_f3(float v) { F3 r = { v, v + 1, v + 2 }; return r; }
C5 s_c5(int v) { C5 r = { { (char)v, 1, 2, 3, (char)(v + 4) } }; return r; }
int main(void)
{
    S1 a = { 1 }; S3 b = { 2, 3 }; S8 c = { 4, 5 }; S12 d = { 6, 7 }; S16 e = { 8, 9 }; S24 f = { 10, 11, 12 };
    D2 g = { 1.5, 2.5 }; DL h = { 3.5, 13 }; IFD i = { 14, 4.5f, 5.5 }; F3 j = { 6.5f, 7.5f, 8.5f }; C5 k = { { 15, 0, 0, 0, 16 } };
    printf("%ld %ld\n", g_take(a, b, c, d, e, f, g, h, i, j, k, 17, 9.5), s_take(a, b, c, d, e, f, g, h, i, j, k, 17, 9.5));
    S1 r1 = g_s1(7); S3 r3 = g_s3(8); S8 r8 = g_s8(9); S12 r12 = g_s12(10); S16 r16 = g_s16(11); S24 r24 = g_s24(12);
    D2 rd = g_d2(1.5); DL rl = g_dl(2.5); IFD ri = g_ifd(6); F3 rf = g_f3(3.5f); C5 rc = g_c5(9);
    printf("%d %d %d %d %d %ld %d %ld %ld %ld %ld %ld\n", r1.c, r3.a, r3.b, r8.a, r8.b, r12.a, r12.b, r16.a, r16.b, r24.a, r24.b, r24.c);
    printf("%.2f %.2f %.2f %ld %d %.2f %.2f %.2f %.2f %.2f %d %d\n", rd.x, rd.y, rl.x, rl.y, ri.i, ri.f, ri.d, rf.a, rf.b, rf.c, rc.s[0], rc.s[4]);
    S16 s = { 100, 200 }; S12 t = { 300, 400 };
    printf("%ld\n", g_many(1, 2, 3, 4, 5, 6, 7, 8, s, 1, 2, 3, 4, 5, 6, 7, 8, 9, t));
    printf("%ld\n", g_calls_back());
    return 0;
}
