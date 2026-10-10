/* immintrin.h - SSE to SSE4.1, and the AVX/AVX2 256-bit types with a subset of
 * their intrinsics, for sicc. 256-bit operations run as two 128-bit halves
 * (the same results; sicc does not encode VEX instructions yet). _mm256_fmadd_*
 * multiplies and adds with two roundings (not fused).
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#ifndef _IMMINTRIN_H
#define _IMMINTRIN_H
#include <smmintrin.h>
typedef float __m256 __attribute__((vector_size(32)));
typedef double __m256d __attribute__((vector_size(32)));
typedef long long __m256i __attribute__((vector_size(32)));
typedef float __v8sf __attribute__((vector_size(32)));
typedef double __v4df __attribute__((vector_size(32)));
typedef int __v8si __attribute__((vector_size(32)));
typedef unsigned __v8su __attribute__((vector_size(32)));
typedef short __v16hi __attribute__((vector_size(32)));
typedef unsigned short __v16hu __attribute__((vector_size(32)));
typedef signed char __v32qi __attribute__((vector_size(32)));
typedef unsigned char __v32qu __attribute__((vector_size(32)));
typedef long long __v4di __attribute__((vector_size(32)));
#define __J(v) ((__m256i)(v))

__SI __m256 _mm256_setzero_ps(void) { return (__m256){ 0 }; }
__SI __m256d _mm256_setzero_pd(void) { return (__m256d){ 0 }; }
__SI __m256i _mm256_setzero_si256(void) { return (__m256i){ 0 }; }
__SI __m256 _mm256_set1_ps(float x) { return ((__m256){ 0 }) + x; }
__SI __m256d _mm256_set1_pd(double x) { return ((__m256d){ 0 }) + x; }
__SI __m256i _mm256_set1_epi8(char x) { return __J(((__v32qi){ 0 }) + (signed char)x); }
__SI __m256i _mm256_set1_epi16(short x) { return __J(((__v16hi){ 0 }) + x); }
__SI __m256i _mm256_set1_epi32(int x) { return __J(((__v8si){ 0 }) + x); }
__SI __m256i _mm256_set1_epi64x(long long x) { return (__m256i){ x, x, x, x }; }
__SI __m256 _mm256_loadu_ps(const float *p) { __m256 r; memcpy(&r, p, 32); return r; }
__SI __m256d _mm256_loadu_pd(const double *p) { __m256d r; memcpy(&r, p, 32); return r; }
__SI __m256i _mm256_loadu_si256(const void *p) { __m256i r; memcpy(&r, p, 32); return r; }
__SI void _mm256_storeu_ps(float *p, __m256 a) { memcpy(p, &a, 32); }
__SI void _mm256_storeu_pd(double *p, __m256d a) { memcpy(p, &a, 32); }
__SI void _mm256_storeu_si256(void *p, __m256i a) { memcpy(p, &a, 32); }
__SI __m256 _mm256_add_ps(__m256 a, __m256 b) { return a + b; }
__SI __m256 _mm256_sub_ps(__m256 a, __m256 b) { return a - b; }
__SI __m256 _mm256_mul_ps(__m256 a, __m256 b) { return a * b; }
__SI __m256 _mm256_div_ps(__m256 a, __m256 b) { return a / b; }
__SI __m256 _mm256_fmadd_ps(__m256 a, __m256 b, __m256 c) { return a * b + c; }
__SI __m256 _mm256_min_ps(__m256 a, __m256 b) { return __V(MIN, a, b); }
__SI __m256 _mm256_max_ps(__m256 a, __m256 b) { return __V(MAX, a, b); }
__SI __m256 _mm256_sqrt_ps(__m256 a) { return __V(SQRT, a, a); }
__SI __m256d _mm256_add_pd(__m256d a, __m256d b) { return a + b; }
__SI __m256d _mm256_sub_pd(__m256d a, __m256d b) { return a - b; }
__SI __m256d _mm256_mul_pd(__m256d a, __m256d b) { return a * b; }
__SI __m256d _mm256_div_pd(__m256d a, __m256d b) { return a / b; }
__SI __m256d _mm256_fmadd_pd(__m256d a, __m256d b, __m256d c) { return a * b + c; }
__SI __m256 _mm256_and_ps(__m256 a, __m256 b) { return (__m256)((__v8si)a & (__v8si)b); }
__SI __m256 _mm256_or_ps(__m256 a, __m256 b) { return (__m256)((__v8si)a | (__v8si)b); }
__SI __m256 _mm256_xor_ps(__m256 a, __m256 b) { return (__m256)((__v8si)a ^ (__v8si)b); }
__SI int _mm256_movemask_ps(__m256 a) { return __V(MOVEMASK, a, a); }
__SI __m256 _mm256_cvtepi32_ps(__m256i a) { return (__m256)__V(CVTI2F, (__v8si)a, (__v8si)a); }
__SI __m256i _mm256_cvttps_epi32(__m256 a) { return __J(__V(CVTF2I, a, a)); }
__SI __m256i _mm256_add_epi8(__m256i a, __m256i b) { return __J((__v32qi)a + (__v32qi)b); }
__SI __m256i _mm256_add_epi16(__m256i a, __m256i b) { return __J((__v16hi)a + (__v16hi)b); }
__SI __m256i _mm256_add_epi32(__m256i a, __m256i b) { return __J((__v8si)a + (__v8si)b); }
__SI __m256i _mm256_add_epi64(__m256i a, __m256i b) { return a + b; }
__SI __m256i _mm256_sub_epi8(__m256i a, __m256i b) { return __J((__v32qi)a - (__v32qi)b); }
__SI __m256i _mm256_sub_epi16(__m256i a, __m256i b) { return __J((__v16hi)a - (__v16hi)b); }
__SI __m256i _mm256_sub_epi32(__m256i a, __m256i b) { return __J((__v8si)a - (__v8si)b); }
__SI __m256i _mm256_mullo_epi16(__m256i a, __m256i b) { return __J((__v16hi)a * (__v16hi)b); }
__SI __m256i _mm256_mullo_epi32(__m256i a, __m256i b) { return __J((__v8si)a * (__v8si)b); }
__SI __m256i _mm256_madd_epi16(__m256i a, __m256i b) { return __J(__V(MADD, (__v16hi)a, (__v16hi)b)); }
__SI __m256i _mm256_maddubs_epi16(__m256i a, __m256i b) { return __J(__V(MADDUBS, (__v32qu)a, (__v32qi)b)); }
__SI __m256i _mm256_and_si256(__m256i a, __m256i b) { return a & b; }
__SI __m256i _mm256_or_si256(__m256i a, __m256i b) { return a | b; }
__SI __m256i _mm256_xor_si256(__m256i a, __m256i b) { return a ^ b; }
__SI __m256i _mm256_andnot_si256(__m256i a, __m256i b) { return __V(ANDNOT, a, b); }
__SI __m256i _mm256_cmpeq_epi8(__m256i a, __m256i b) { return __J((__v32qi)a == (__v32qi)b); }
__SI __m256i _mm256_cmpeq_epi32(__m256i a, __m256i b) { return __J((__v8si)a == (__v8si)b); }
__SI __m256i _mm256_cmpgt_epi8(__m256i a, __m256i b) { return __J((__v32qi)a > (__v32qi)b); }
__SI __m256i _mm256_cmpgt_epi32(__m256i a, __m256i b) { return __J((__v8si)a > (__v8si)b); }
__SI __m256i _mm256_max_epi32(__m256i a, __m256i b) { return __J(__V(MAX, (__v8si)a, (__v8si)b)); }
__SI __m256i _mm256_min_epi32(__m256i a, __m256i b) { return __J(__V(MIN, (__v8si)a, (__v8si)b)); }
__SI __m256i _mm256_abs_epi8(__m256i a) { return __J(__V(ABS, (__v32qi)a, (__v32qi)a)); }
__SI int _mm256_movemask_epi8(__m256i a) { return __V(MOVEMASK, (__v32qi)a, (__v32qi)a); }
__SI __m256i _mm256_unpacklo_epi8(__m256i a, __m256i b) { return __J(__V(UNPCKLO, (__v32qi)a, (__v32qi)b)); }
__SI __m256i _mm256_unpackhi_epi8(__m256i a, __m256i b) { return __J(__V(UNPCKHI, (__v32qi)a, (__v32qi)b)); }
__SI __m256i _mm256_packs_epi32(__m256i a, __m256i b) { return __J(__V(PACKS, (__v8si)a, (__v8si)b)); }
__SI __m128i _mm256_castsi256_si128(__m256i a) { return __builtin_shufflevector(a, a, 0, 1); }
__SI __m128 _mm256_castps256_ps128(__m256 a) { return __builtin_shufflevector(a, a, 0, 1, 2, 3); }
__SI __m256 _mm256_castsi256_ps(__m256i a) { return (__m256)a; }
__SI __m256i _mm256_castps_si256(__m256 a) { return (__m256i)a; }
#define _mm256_extracti128_si256(a, i) ((i) & 1 ? __builtin_shufflevector((__m256i)(a), (__m256i)(a), 2, 3) : __builtin_shufflevector((__m256i)(a), (__m256i)(a), 0, 1))
#define _mm256_extractf128_ps(a, i) ((i) & 1 ? __builtin_shufflevector((__m256)(a), (__m256)(a), 4, 5, 6, 7) : __builtin_shufflevector((__m256)(a), (__m256)(a), 0, 1, 2, 3))
#define _mm256_slli_epi16(a, n) __J(__builtin_vec(__VOP_SHLI, (__v16hi)(a), (__v16hi)(a), (n)))
#define _mm256_slli_epi32(a, n) __J(__builtin_vec(__VOP_SHLI, (__v8si)(a), (__v8si)(a), (n)))
#define _mm256_srli_epi16(a, n) __J(__builtin_vec(__VOP_SHRI, (__v16hu)(a), (__v16hu)(a), (n)))
#define _mm256_srli_epi32(a, n) __J(__builtin_vec(__VOP_SHRI, (__v8su)(a), (__v8su)(a), (n)))
#define _mm256_srai_epi16(a, n) __J(__builtin_vec(__VOP_SARI, (__v16hi)(a), (__v16hi)(a), (n)))
#define _mm256_srai_epi32(a, n) __J(__builtin_vec(__VOP_SARI, (__v8si)(a), (__v8si)(a), (n)))
#endif
