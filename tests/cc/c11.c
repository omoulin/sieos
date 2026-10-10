/* c11.c - C11 features: _Generic, _Static_assert, _Alignas/_Alignof, anonymous
 * members, designated initializers, compound literals, flexible arrays, _Noreturn,
 * u8/u/U/L strings and characters, universal character names, restrict, inline.
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
#include <stdalign.h>
#include <stdnoreturn.h>
#include <stdint.h>
#define TYPENAME(x) _Generic((x), int: "int", long: "long", double: "double", char *: "char *", const char *: "const char *", default: "other")
_Static_assert(sizeof(int) == 4, "int is 4 bytes");
struct P { int kind; union { struct { short x, y; }; long v; }; };
struct F { int n; double d[]; };
static _Alignas(32) char buf32[5];
static inline int sq(int v) { return v * v; }
static int sum(const int *restrict a, int n) { int s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }
noreturn static void stop(int c) { exit(c); }
int main(void)
{
    printf("%s %s %s %s %s\n", TYPENAME(1), TYPENAME(2L), TYPENAME(1.5), TYPENAME("s"), TYPENAME((char)1));
    struct P p = { .kind = 2, .x = 3, .y = -4 };
    p.v &= 0xffffffff;
    printf("%d %d %d %ld\n", p.kind, p.x, p.y, (long)sizeof p);
    int arr[10] = { [2] = 5, [7] = 9, 1 };
    printf("%d %d %d %d\n", arr[2], arr[7], arr[8], sum((int[]){ 1, 2, 3, sq(4) }, 4));
    struct F *f = malloc(sizeof *f + 3 * sizeof(double));
    f->n = 3; for (int i = 0; i < 3; i++) f->d[i] = i * 1.5;
    printf("%zu %g\n", sizeof(struct F), f->d[2]);
    printf("%zu %zu %d\n", alignof(max_align_t), _Alignof(double), (int)((uintptr_t)buf32 % 32));
    const char *u8 = u8"héllo";
    const unsigned short *u = (const unsigned short *)u"€!";
    const unsigned *U = (const unsigned *)U"\U0001F600";
    printf("%zu %02x %04x %x %x %d\n", strlen(u8), (unsigned char)u8[1], u[0], U[0], (unsigned)L'é', (int)sizeof(U"ab"));
    int \u00e9t\u00e9 = 7, x\u00e9 = 2; printf("%d %d\n", été, xé);
    struct { int a; struct { int b, c; }; } an = { 1, { 2, 3 } };
    printf("%d %d %d\n", an.a, an.b, an.c);
    printf("%d\n", (int)_Generic(&p, struct P *: 1, default: 0));
    if (arr[0]) stop(1);
    return 0;
}
