/* Recursive calls: fib(35), and a struct-heavy loop. Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "../t.h"
typedef struct { long x, y; } P;
static long fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static P step(P p, long k) { P r = { p.y, p.x + p.y * k }; return r; }
int main(void) {
    P p = { 1, 1 };
    for (long i = 0; i < 100000000; i++) { p = step(p, i & 3); p.y &= 0xffffff; }
    printf("%ld %ld %ld\n", fib(35), p.x, p.y);
    return 0;
}
