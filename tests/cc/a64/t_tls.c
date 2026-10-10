/* Thread-local storage (local-exec, AArch64: tpidr_el0 + the offset the
 * linker computes): initialized and zeroed variables, alignments up to 64,
 * arrays, addresses, structs, more than 4 KiB of offset (both halves of the
 * offset), statics in functions.
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "../t.h"
_Thread_local int counter = 5;
__thread long big[600];                          /* 4800 bytes: later variables need the high offset part */
__thread char name[16] = "main";
static __thread double scale = 1.5;
__thread _Alignas(64) char aligned[3] = { 1, 2, 3 };
__thread struct { short a; long b; } pair = { 7, 8 };
__thread int after_big;
static int next(void) { static _Thread_local int n; return ++n; }
int main(void)
{
    for (int i = 0; i < 600; i++) big[i] = i;
    long sum = 0;
    for (int i = 0; i < 600; i++) sum += big[i];
    counter += 10;
    scale *= 3;
    name[0] = 'M';
    after_big = 99;
    int *p = &counter;
    *p += 1;
    next(); next();
    printf("%d %ld %s %g %d %d %d %ld %d %d\n", counter, sum, name, scale, aligned[2], ((unsigned long)aligned & 63) == 0,
           pair.a, pair.b, after_big, next());
    printf("%d %d\n", &big[599] - &big[0], (char *)&after_big != (char *)&big[599]);
    return 0;
}
