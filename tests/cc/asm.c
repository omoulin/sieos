/* GNU extended inline assembly: constraints, modifiers, register variables. Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
static long add(long a, long b) { asm("addq %1, %0" : "+r"(a) : "r"(b)); return a; }
static int sub_imm(int a) { asm("subl %1, %0" : "+r"(a) : "i"(7)); return a; }
static unsigned hi_lo(unsigned long v, unsigned *hi) { unsigned lo, h; asm("movl %k2, %0\n\tshrq $32, %2\n\tmovl %k2, %1" : "=&r"(lo), "=&r"(h), "+r"(v)); *hi = h; return lo; }
static void copy(void *d, const void *s, unsigned long n) { asm volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory"); }
static long mem_add(long *p, long v) { asm("addq %1, %0" : "+m"(*p) : "r"(v)); return *p; }
static int cpuid_vendor_ok(void) { unsigned a, b, c, d; asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0)); return b == 0x756e6547 || b == 0x68747541 || b != 0; }
static long regvar(long x) { register long r10 asm("r10") = x; long out; asm("movq %1, %0" : "=r"(out) : "r"(r10)); return out * 2; }
static int setc(unsigned a, unsigned b) { unsigned char c; asm("cmpl %2, %1; setb %0" : "=qm"(c) : "r"(a), "r"(b)); return c; }
static long named(long x) { long r; asm("leaq 5(%[in]), %[out]" : [out] "=r"(r) : [in] "r"(x)); return r; }
int main(void) {
    unsigned hi;
    unsigned lo = hi_lo(0x1122334455667788UL, &hi);
    printf("%ld %d %x %x\n", add(40, 2), sub_imm(10), lo, hi);
    char a[20] = "inline assembly", b[20] = { 0 }; copy(b, a, 16); printf("%s\n", b);
    long m = 5; printf("%ld %d %ld\n", mem_add(&m, 6), cpuid_vendor_ok(), regvar(21));
    printf("%d %d %ld\n", setc(1, 2), setc(3, 2), named(10));
    asm volatile("" ::: "memory");
    return 0;
}
