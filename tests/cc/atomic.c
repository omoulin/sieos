/* atomic.c - _Atomic and <stdatomic.h>.
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
#include <stdatomic.h>
_Atomic int gi = 5;
atomic_long gl = ATOMIC_VAR_INIT(100);
_Atomic(unsigned char) gc;
_Atomic double gd = 1.5;
int *_Atomic gp;
struct S { _Atomic short s; int x; } gs;
atomic_flag fl = ATOMIC_FLAG_INIT;
int arr[10];
int main(void)
{
    gi += 3; gi -= 1; gi *= 2; gi |= 0x100; gi &= 0x1FF; gi ^= 3; gi <<= 1; gi >>= 1; ++gi; gi++; --gi;
    int o = gi++; printf("%d %d\n", o, gi);
    gl += gi; long v = gl--; printf("%ld %ld\n", v, (long)gl);
    gc = 250; gc += 10; printf("%d\n", gc);
    gd += 2.25; gd *= 2; double od = gd++; printf("%g %g\n", od, (double)gd);
    gp = arr; gp += 3; printf("%ld\n", gp - arr);
    gs.s = -3; gs.s *= 1000; printf("%d\n", gs.s);
    { int x = atomic_fetch_add(&gi, 10); printf("%d %d\n", x, atomic_load(&gi)); }
    { int x = atomic_fetch_sub(&gi, 1); printf("%d %d\n", x, atomic_fetch_or(&gi, 0x1000)); }
    { int x = atomic_fetch_and(&gi, 0xFF); printf("%d %d\n", x, atomic_fetch_xor(&gi, 0x55)); }
    int exp = 1; { int x = atomic_compare_exchange_strong(&gi, &exp, 7); printf("%d %d\n", x, exp); }
    exp = gi; { int x = atomic_compare_exchange_weak(&gi, &exp, 7); printf("%d %d\n", x, (int)gi); }
    printf("%d\n", atomic_exchange(&gi, 42)); atomic_store(&gi, 43); printf("%d\n", atomic_load_explicit(&gi, memory_order_acquire));
    { int x = atomic_flag_test_and_set(&fl), y = atomic_flag_test_and_set(&fl); atomic_flag_clear(&fl); printf("%d %d %d\n", x, y, atomic_flag_test_and_set(&fl)); }
    { int x = __atomic_add_fetch(&gi, 5, 5); printf("%d %d\n", x, __atomic_nand_fetch(&gi, 0xF0, 5)); }
    printf("%d %d %d\n", __sync_sub_and_fetch(&gl, 1) > 0, (int)atomic_is_lock_free(&gl), __atomic_always_lock_free(sizeof(long), 0));
    atomic_store(&gd, 0.25); double dd = atomic_load(&gd); { double x = atomic_exchange(&gd, 8.0); printf("%g %g %g\n", dd, x, (double)gd); }
    long r, nv = 77; __atomic_load(&gl, &r, 5); __atomic_store(&gl, &nv, 5); printf("%ld %ld\n", r, (long)gl);
    long ex = 77, de = 88; { int x = __atomic_compare_exchange(&gl, &ex, &de, 0, 5, 5); printf("%d %ld\n", x, (long)gl); }
    atomic_thread_fence(memory_order_seq_cst);
    return 0;
}
