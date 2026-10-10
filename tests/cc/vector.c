/* vector.c - GNU vector types: element-wise operators, shifts, compares,
 * scalars, subscripts, casts, shuffles, 8/16/32-byte vectors, passing.
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
typedef int v4si __attribute__((vector_size(16)));
typedef unsigned v4su __attribute__((vector_size(16)));
typedef short v8hi __attribute__((vector_size(16)));
typedef unsigned short v8hu __attribute__((vector_size(16)));
typedef signed char v16qi __attribute__((vector_size(16)));
typedef unsigned char v16qu __attribute__((vector_size(16)));
typedef long v2di __attribute__((vector_size(16)));
typedef float v4sf __attribute__((vector_size(16)));
typedef double v2df __attribute__((vector_size(16)));
typedef float __attribute__((vector_size(32))) v8sf;
typedef int v8si __attribute__((vector_size(32)));
typedef short v4hi __attribute__((vector_size(8)));

#define PR(v, n, fmt) do { for (int k_ = 0; k_ < n; k_++) printf(fmt " ", v[k_]); printf("\n"); } while (0)
static v4si add3(v4si a, v4si b, v4si c) { return a + b + c; }
static v8sf scale(v8sf x, float s) { return x * s + 1.0f; }

int main(void)
{
    v4si a = { 1, -2, 3, 0x7fffffff }, b = { 5, 6, -7, 1 };
    PR((a + b), 4, "%d"); PR((a - b), 4, "%d"); PR((a * b), 4, "%d"); PR((a / b), 4, "%d"); PR((a % b), 4, "%d");
    PR((a & b), 4, "%d"); PR((a | b), 4, "%d"); PR((a ^ b), 4, "%d"); PR((-a), 4, "%d"); PR((~a), 4, "%d");
    PR((a << 3), 4, "%d"); PR((a >> 1), 4, "%d"); PR((a << (b & 7)), 4, "%d"); PR((b >> (v4si){ 1, 2, 3, 0 }), 4, "%d");
    PR((a == b), 4, "%d"); PR((a < b), 4, "%d"); PR((a >= b), 4, "%d"); PR((a != a), 4, "%d"); PR((a > 2), 4, "%d");
    PR((a + 10), 4, "%d"); PR((100 - a), 4, "%d");
    v4su ua = (v4su)a, ub = (v4su)b;
    PR((ua >> 4), 4, "%u"); PR((ua < ub), 4, "%d"); PR((ua / ub), 4, "%u");
    v8hi h = { 1, -1, 300, -300, 32767, -32768, 7, 8 }, h2 = h * 3;
    PR(h2, 8, "%d"); PR((h >> 2), 8, "%d"); PR((h < h2), 8, "%d"); PR((h * h), 8, "%d");
    v8hu hu = (v8hu)h; PR((hu >> 3), 8, "%u"); PR((hu > (v8hu){ 5, 5, 5, 5, 5, 5, 5, 5 }), 8, "%d");
    v16qi q = { 1, 2, 3, -4, 5, -128, 127, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    PR((q * q), 16, "%d"); PR((q + q), 16, "%d"); PR((q == (v16qi){ 1 }), 16, "%d"); PR((q << 1), 16, "%d"); PR((q >> 2), 16, "%d");
    v16qu qu = (v16qu)q; PR((qu / 3), 16, "%u"); PR((qu > 100), 16, "%d");
    v2di d = { 1L << 40, -5 }; PR((d * d), 2, "%ld"); PR((d >> 3), 2, "%ld"); PR((d > 0), 2, "%ld"); PR((d == d), 2, "%ld");
    v4sf f = { 1.5f, -2.0f, 0.25f, 100.0f }, g = { 2.0f, 3.0f, -4.0f, 0.5f };
    PR((f + g), 4, "%g"); PR((f * g - 1), 4, "%g"); PR((f / g), 4, "%g"); PR((-f), 4, "%g"); PR((f < g), 4, "%d"); PR((f >= g), 4, "%d"); PR((f == f), 4, "%d");
    v2df x = { 1e100, -0.5 }, y = { 3.0, 0.0 }; PR((x / y), 2, "%g"); PR((x > y), 2, "%ld"); PR((-y), 2, "%g");
    v8sf w = { 1, 2, 3, 4, 5, 6, 7, 8 }; PR(scale(w, 0.5f), 8, "%g");
    v8si wi = { 1, 2, 3, 4, 5, 6, 7, 8 }; PR((wi * wi - (v8si)(w > 4.0f)), 8, "%d");
    v4hi s4 = { 1, 2, 3, 4 }; PR((s4 * s4 + 1), 4, "%d");
    PR(add3(a, b, a), 4, "%d");
    v4si m = a; m += b; m *= 2; m[2] = 99; m[0]++; PR(m, 4, "%d"); printf("%d %d\n", m[1], a[3]);
    PR(__builtin_shufflevector(a, b, 3, 2, 5, 4), 4, "%d");
    PR(__builtin_shufflevector(f, g, 0, 4, 1, 5, 2, 6, 3, 7), 8, "%g");
    printf("%d %d %d\n", (int)sizeof(v8sf), (int)_Alignof(v4si), (int)sizeof(v4hi));
    static v4si gv = { 7, 8, 9, 10 }; gv = gv + 1; PR(gv, 4, "%d");
    return 0;
}
