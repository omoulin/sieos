/* Builtins, _Generic, statement expressions, atomics, enums, typeof. Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
#include <stdint.h>
#include <stdbool.h>
enum Color { RED, GREEN = 5, BLUE, NEG = -2 };
#define TYPE(x) _Generic((x), int: "int", long: "long", double: "double", char *: "char*", default: "other")
static int counter;
static __attribute__((noinline)) int bump(void) { return ++counter; }
typedef struct { int n; } Box;
int main(void) {
    printf("%d %d %d %d\n", RED, GREEN, BLUE, NEG);
    printf("%s %s %s %s %s\n", TYPE(1), TYPE(1L), TYPE(1.0), TYPE((char *)0), TYPE('c'));
    int v = ({ int t = 4; t * t; });
    printf("%d\n", v);
    typeof(v) w = v + 1; __typeof__(1.5) d = 2.5; printf("%d %.1f\n", w, d);
    int a = 5;
    int fa = __atomic_fetch_add(&a, 3, __ATOMIC_SEQ_CST); printf("%d %d\n", fa, __atomic_load_n(&a, __ATOMIC_ACQUIRE));
    __atomic_store_n(&a, 42, __ATOMIC_RELEASE); printf("%d\n", a);
    int expect = 42; bool ok = __atomic_compare_exchange_n(&a, &expect, 7, 0, 5, 5); printf("%d %d\n", ok, a);
    char flag = 0; int t1 = __atomic_test_and_set(&flag, 5); int t2 = __atomic_test_and_set(&flag, 5); printf("%d %d\n", t1, t2);
    __atomic_clear(&flag, 3); printf("%d\n", flag);
    unsigned long x = 0x10; uint32_t y = 0x12345678;
    printf("%d %d %d %x %lx\n", __builtin_clzl(x), __builtin_ctz(48), __builtin_popcount(0xF0F), __builtin_bswap32(y), __builtin_bswap64(0x0102030405060708UL));
    printf("%d %d\n", __builtin_expect(a == 7, 1), __builtin_constant_p(3 + 4));
    int b1 = bump(); int b2 = bump(); printf("%d %d %d\n", b1, b2, counter);
    Box bx = { 3 }; Box *bp = &bx; printf("%d\n", bp->n);
    _Static_assert(sizeof(int64_t) == 8, "int64");
    int8_t i8 = -1; uint16_t u16 = 65535; printf("%d %d %d\n", i8, u16, (int)sizeof(uintptr_t));
    const char *s = __func__; printf("%s\n", s);
    int arr[] = { 1, 2, 3 }; printf("%d\n", (int)(sizeof arr / sizeof *arr));
    if (__builtin_types_compatible_p(int, int) && !__builtin_types_compatible_p(int, long)) printf("compat\n");
    return 0;
}
