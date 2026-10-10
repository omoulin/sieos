/* Structs, unions, bit-fields, initializers, struct values. Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
struct P { int x, y; };
struct R { struct P a, b; char name[8]; };
struct Big { long v[6]; };
struct Mix { char c; double d; short s; };
union U { int i; float f; unsigned char b[4]; };
struct BF { unsigned a : 3, b : 5; int c : 7; unsigned d : 1; long e : 40; };
struct Anon { int k; union { int u1; long u2; }; struct { short s1, s2; }; };
static struct P mkp(int x, int y) { struct P p = { x, y }; return p; }
static struct Big mkbig(long k) { struct Big b; for (int i = 0; i < 6; i++) b.v[i] = k * i; return b; }
static long sumbig(struct Big b) { long s = 0; for (int i = 0; i < 6; i++) s += b.v[i]; return s; }
static int area(struct R r) { return (r.b.x - r.a.x) * (r.b.y - r.a.y); }
static struct Mix mkmix(char c, double d, short s) { struct Mix m = { c, d, s }; return m; }
struct R gr = { .b = { 5, 6 }, .a.x = 1, .name = "glob" };
int main(void) {
    struct P p = mkp(3, 4);
    struct R r = { { 1, 2 }, { 4, 8 }, "rect" };
    printf("%d %d %d %s\n", p.x, p.y, area(r), r.name);
    struct R r2 = r; r2.a.x = 0; printf("%d %d\n", r.a.x, r2.a.x);
    struct Big b = mkbig(3); printf("%ld %ld\n", b.v[5], sumbig(b));
    struct Mix m = mkmix('z', 2.5, -7); printf("%c %.2f %d\n", m.c, m.d, m.s);
    union U u; u.i = 0x41424344; printf("%x %c\n", u.b[0], u.b[3]);
    struct BF bf = { 5, 17, -9, 1, -123456789 };
    printf("%u %u %d %u %ld %d\n", bf.a, bf.b, bf.c, bf.d, bf.e, (int)sizeof bf);
    bf.a = 9; bf.c += 70; printf("%u %d\n", bf.a, bf.c);
    struct Anon an = { .k = 1, .u2 = 99, .s2 = 5 }; printf("%d %ld %d %d\n", an.k, an.u2, an.s1, an.s2);
    struct P *pp = &p; pp->y = 40; printf("%d\n", p.y);
    struct P arr[3] = { [2] = { 7, 8 }, [0].y = 3 };
    printf("%d %d %d %d\n", arr[0].x, arr[0].y, arr[2].x, arr[1].y);
    int *ci = (int[]){ 9, 8, 7 }; printf("%d\n", ci[1]);
    struct P cl = (struct P){ .y = 11 }; printf("%d %d\n", cl.x, cl.y);
    printf("%d %d %s\n", gr.a.x, gr.b.y, gr.name);
    printf("%d %d %d\n", (int)sizeof(struct Mix), (int)_Alignof(struct Mix), (int)__builtin_offsetof(struct Mix, s));
    struct __attribute__((packed)) Pk { char c; int i; } pk = { 1, 2 }; printf("%d %d\n", (int)sizeof pk, pk.i);
    return 0;
}
