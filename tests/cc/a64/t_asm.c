/* AArch64 GNU extended inline assembly: r/w/m/Q/i constraints, matching and
 * named operands, the w/x/s/d/q/c modifiers, register variables, clobbers,
 * system registers. (Output: t_asm.expect: the host cannot build this one.)
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "../t.h"
static long add(long a, long b) { asm("add %0, %0, %1" : "+r"(a) : "r"(b)); return a; }
static int sub_imm(int a) { asm("sub %w0, %w0, %1" : "+r"(a) : "I"(7)); return a; }
static unsigned hi_lo(unsigned long v, unsigned *hi)
{
    unsigned lo, h;
    asm("mov %w0, %w2\n\tlsr %x2, %x2, #32\n\tmov %w1, %w2" : "=&r"(lo), "=&r"(h), "+r"(v));
    *hi = h;
    return lo;
}
static long mem_add(long *p, long v) { long t; asm("ldr %0, %1\n\tadd %0, %0, %2\n\tstr %0, %1" : "=&r"(t), "+Q"(*p) : "r"(v)); return *p; }
static long mem_m(long *p) { long r; asm("ldr %0, %1" : "=r"(r) : "m"(p[2])); return r; }
static long regvar(long x) { register long r10 asm("x10") = x; long out; asm("add %0, %1, %1" : "=r"(out) : "r"(r10)); return out; }
static long named(long x) { long r; asm("add %[out], %[in], #5" : [out] "=r"(r) : [in] "r"(x)); return r; }
static long match(long x) { long r; asm("add %0, %0, #1" : "=r"(r) : "0"(x)); return r; }
static double fmadd(double a, double b, double c) { double r; asm("fmadd %d0, %d1, %d2, %d3" : "=w"(r) : "w"(a), "w"(b), "w"(c)); return r; }
static float fneg(float a) { float r; asm("fneg %s0, %s1" : "=w"(r) : "w"(a)); return r; }
static long constant(void) { long r; asm("mov %0, #%c1" : "=r"(r) : "i"(1234)); return r; }
static int clz(unsigned x) { int r; asm("clz %w0, %w1" : "=r"(r) : "r"(x)); return r; }
static long clobbers(long x)
{
    asm volatile("mov x9, #77\n\tmov x19, #88\n\tmov x0, #1" : : : "x0", "x9", "x19", "memory");
    return x * 3;
}
static unsigned long tp(void) { unsigned long r; asm volatile("mrs %0, tpidr_el0" : "=r"(r)); return r; }
static int cas(int *p, int old, int new)
{
    int seen, fail;
    asm volatile("1: ldaxr %w0, %2\n\tcmp %w0, %w3\n\tb.ne 2f\n\tstlxr %w1, %w4, %2\n\tcbnz %w1, 1b\n2:"
                 : "=&r"(seen), "=&r"(fail), "+Q"(*p) : "r"(old), "r"(new) : "cc", "memory");
    return seen;
}
static int unique(void) { int a, b; asm("mov %w0, #%=" : "=r"(a)); asm("mov %w0, #%=" : "=r"(b)); return a != b; }
int main(void)
{
    unsigned hi;
    unsigned lo = hi_lo(0x1122334455667788UL, &hi);
    printf("%ld %d %x %x\n", add(40, 2), sub_imm(10), lo, hi);
    long m = 5, arr[4] = { 1, 2, 3, 4 };
    printf("%ld %ld %ld %ld %ld\n", mem_add(&m, 6), mem_m(arr), regvar(21), named(10), match(41));
    printf("%g %g %ld %d %d\n", fmadd(2, 3, 4), fneg(1.5f), constant(), clz(1), clz(0x80000000u));
    printf("%ld %d\n", clobbers(14), tp() != 0);
    int v = 5;
    printf("%d %d %d %d\n", cas(&v, 5, 9), v, cas(&v, 5, 11), v);
    printf("%d\n", unique());
    asm volatile("" ::: "memory");
    return 0;
}
