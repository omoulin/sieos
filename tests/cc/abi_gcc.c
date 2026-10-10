/* abi_gcc.c - the host compiler's side of the ABI test.
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#include "abi.h"
long g_take(S1 a, S3 b, S8 c, S12 d, S16 e, S24 f, D2 g, DL h, IFD i, F3 j, C5 k, int x, double y)
{
    return a.c + b.a * 2 + b.b * 3 + c.a * 5 + c.b * 7 + d.a * 11 + d.b * 13 + e.a * 17 + e.b * 19 + f.a * 23 + f.b * 29 + f.c * 31 +
           (long)(g.x * 37 + g.y * 41 + h.x * 43) + h.y * 47 + i.i * 53 + (long)(i.f * 59 + i.d * 61 + j.a * 67 + j.b * 71 + j.c * 73) +
           k.s[0] * 79 + k.s[4] * 83 + x * 89 + (long)(y * 97);
}
S1 g_s1(int v) { S1 r = { (char)v }; return r; }
S3 g_s3(int v) { S3 r = { (short)v, (char)(v + 1) }; return r; }
S8 g_s8(int v) { S8 r = { v, v * 2 }; return r; }
S12 g_s12(int v) { S12 r = { v * 3L, v * 4 }; return r; }
S16 g_s16(int v) { S16 r = { v * 5L, v * 6L }; return r; }
S24 g_s24(int v) { S24 r = { v, v + 1, v + 2 }; return r; }
D2 g_d2(double v) { D2 r = { v, v * 2 }; return r; }
DL g_dl(double v) { DL r = { v, (long)v * 3 }; return r; }
IFD g_ifd(int v) { IFD r = { v, v * 0.5f, v * 0.25 }; return r; }
F3 g_f3(float v) { F3 r = { v, v + 1, v + 2 }; return r; }
C5 g_c5(int v) { C5 r = { { (char)v, 1, 2, 3, (char)(v + 4) } }; return r; }
long g_many(long a, long b, long c, long d, long e, long f, long g, long h, S16 s, double d1, double d2, double d3, double d4, double d5, double d6, double d7, double d8, double d9, S12 t)
{
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + s.a * 9 + s.b * 10 + (long)(d1 + d2 * 2 + d3 * 3 + d4 * 4 + d5 * 5 + d6 * 6 + d7 * 7 + d8 * 8 + d9 * 9) + t.a * 11 + t.b * 12;
}
long g_calls_back(void)
{
    S1 a = { 1 }; S3 b = { 2, 3 }; S8 c = { 4, 5 }; S12 d = { 6, 7 }; S16 e = { 8, 9 }; S24 f = { 10, 11, 12 };
    D2 g = { 1.5, 2.5 }; DL h = { 3.5, 13 }; IFD i = { 14, 4.5f, 5.5 }; F3 j = { 6.5f, 7.5f, 8.5f }; C5 k = { { 15, 0, 0, 0, 16 } };
    S16 r16 = s_s16(3); S24 r24 = s_s24(4); D2 rd2 = s_d2(1.25); DL rdl = s_dl(2.0); IFD rifd = s_ifd(8); F3 rf3 = s_f3(0.5f); C5 rc5 = s_c5(20);
    return s_take(a, b, c, d, e, f, g, h, i, j, k, 17, 9.5) + r16.a + r16.b + r24.a + r24.c + (long)(rd2.x * 4 + rd2.y * 4) + rdl.y +
           (long)rdl.x + rifd.i + (long)(rifd.f * 2 + rifd.d * 4 + rf3.a * 2 + rf3.c * 2) + rc5.s[0] + rc5.s[4];
}
