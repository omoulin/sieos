/* stdatomic.h - sicc's freestanding header: C11 atomics, on the __atomic
 * builtins. Objects of up to 8 bytes are lock-free (lock-prefixed
 * instructions, compare-and-swap loops). As with gcc, atomic_fetch_add on
 * an atomic pointer adds bytes, not elements. Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#ifndef __SICC_STDATOMIC_H
#define __SICC_STDATOMIC_H
typedef enum {
    memory_order_relaxed = __ATOMIC_RELAXED, memory_order_consume = __ATOMIC_CONSUME,
    memory_order_acquire = __ATOMIC_ACQUIRE, memory_order_release = __ATOMIC_RELEASE,
    memory_order_acq_rel = __ATOMIC_ACQ_REL, memory_order_seq_cst = __ATOMIC_SEQ_CST
} memory_order;

typedef _Atomic _Bool atomic_bool;
typedef _Atomic char atomic_char;
typedef _Atomic signed char atomic_schar;
typedef _Atomic unsigned char atomic_uchar;
typedef _Atomic short atomic_short;
typedef _Atomic unsigned short atomic_ushort;
typedef _Atomic int atomic_int;
typedef _Atomic unsigned int atomic_uint;
typedef _Atomic long atomic_long;
typedef _Atomic unsigned long atomic_ulong;
typedef _Atomic long long atomic_llong;
typedef _Atomic unsigned long long atomic_ullong;
typedef _Atomic unsigned short atomic_char16_t;
typedef _Atomic unsigned int atomic_char32_t;
typedef _Atomic int atomic_wchar_t;
typedef _Atomic long atomic_intptr_t;
typedef _Atomic unsigned long atomic_uintptr_t;
typedef _Atomic unsigned long atomic_size_t;
typedef _Atomic long atomic_ptrdiff_t;
typedef _Atomic long atomic_intmax_t;
typedef _Atomic unsigned long atomic_uintmax_t;
typedef _Atomic signed char atomic_int_least8_t, atomic_int_fast8_t;
typedef _Atomic unsigned char atomic_uint_least8_t, atomic_uint_fast8_t;
typedef _Atomic short atomic_int_least16_t;
typedef _Atomic unsigned short atomic_uint_least16_t;
typedef _Atomic int atomic_int_least32_t;
typedef _Atomic unsigned int atomic_uint_least32_t;
typedef _Atomic long atomic_int_least64_t, atomic_int_fast16_t, atomic_int_fast32_t, atomic_int_fast64_t;
typedef _Atomic unsigned long atomic_uint_least64_t, atomic_uint_fast16_t, atomic_uint_fast32_t, atomic_uint_fast64_t;

#define ATOMIC_BOOL_LOCK_FREE 2
#define ATOMIC_CHAR_LOCK_FREE 2
#define ATOMIC_CHAR16_T_LOCK_FREE 2
#define ATOMIC_CHAR32_T_LOCK_FREE 2
#define ATOMIC_WCHAR_T_LOCK_FREE 2
#define ATOMIC_SHORT_LOCK_FREE 2
#define ATOMIC_INT_LOCK_FREE 2
#define ATOMIC_LONG_LOCK_FREE 2
#define ATOMIC_LLONG_LOCK_FREE 2
#define ATOMIC_POINTER_LOCK_FREE 2

#define ATOMIC_VAR_INIT(v) (v)
#define atomic_init(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)
#define kill_dependency(y) (y)
#define atomic_thread_fence(mo) __atomic_thread_fence(mo)
#define atomic_signal_fence(mo) __atomic_signal_fence(mo)
#define atomic_is_lock_free(p) __atomic_is_lock_free(sizeof *(p), (p))

#define atomic_store_explicit(p, v, mo) __atomic_store_n((p), (v), (mo))
#define atomic_store(p, v) atomic_store_explicit(p, v, __ATOMIC_SEQ_CST)
#define atomic_load_explicit(p, mo) __atomic_load_n((p), (mo))
#define atomic_load(p) atomic_load_explicit(p, __ATOMIC_SEQ_CST)
#define atomic_exchange_explicit(p, v, mo) __atomic_exchange_n((p), (v), (mo))
#define atomic_exchange(p, v) atomic_exchange_explicit(p, v, __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_strong_explicit(p, e, d, s, f) __atomic_compare_exchange_n((p), (e), (d), 0, (s), (f))
#define atomic_compare_exchange_strong(p, e, d) atomic_compare_exchange_strong_explicit(p, e, d, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_weak_explicit(p, e, d, s, f) __atomic_compare_exchange_n((p), (e), (d), 1, (s), (f))
#define atomic_compare_exchange_weak(p, e, d) atomic_compare_exchange_weak_explicit(p, e, d, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_fetch_add_explicit(p, v, mo) __atomic_fetch_add((p), (v), (mo))
#define atomic_fetch_add(p, v) atomic_fetch_add_explicit(p, v, __ATOMIC_SEQ_CST)
#define atomic_fetch_sub_explicit(p, v, mo) __atomic_fetch_sub((p), (v), (mo))
#define atomic_fetch_sub(p, v) atomic_fetch_sub_explicit(p, v, __ATOMIC_SEQ_CST)
#define atomic_fetch_or_explicit(p, v, mo) __atomic_fetch_or((p), (v), (mo))
#define atomic_fetch_or(p, v) atomic_fetch_or_explicit(p, v, __ATOMIC_SEQ_CST)
#define atomic_fetch_xor_explicit(p, v, mo) __atomic_fetch_xor((p), (v), (mo))
#define atomic_fetch_xor(p, v) atomic_fetch_xor_explicit(p, v, __ATOMIC_SEQ_CST)
#define atomic_fetch_and_explicit(p, v, mo) __atomic_fetch_and((p), (v), (mo))
#define atomic_fetch_and(p, v) atomic_fetch_and_explicit(p, v, __ATOMIC_SEQ_CST)

typedef struct { _Atomic unsigned char __val; } atomic_flag;
#define ATOMIC_FLAG_INIT { 0 }
#define atomic_flag_test_and_set_explicit(p, mo) __atomic_test_and_set(&(p)->__val, (mo))
#define atomic_flag_test_and_set(p) atomic_flag_test_and_set_explicit(p, __ATOMIC_SEQ_CST)
#define atomic_flag_clear_explicit(p, mo) __atomic_clear(&(p)->__val, (mo))
#define atomic_flag_clear(p) atomic_flag_clear_explicit(p, __ATOMIC_SEQ_CST)
#endif
