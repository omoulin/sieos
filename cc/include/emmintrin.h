/* emmintrin.h - SSE2 intrinsics (__m128d: two doubles; __m128i: integers), for sicc.
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#ifndef _EMMINTRIN_H
#define _EMMINTRIN_H
#include <xmmintrin.h>
typedef double __m128d __attribute__((vector_size(16)));
typedef long long __m128i __attribute__((vector_size(16)));
typedef double __v2df __attribute__((vector_size(16)));
typedef long long __v2di __attribute__((vector_size(16)));
typedef unsigned long long __v2du __attribute__((vector_size(16)));
typedef unsigned __v4su __attribute__((vector_size(16)));
typedef short __v8hi __attribute__((vector_size(16)));
typedef unsigned short __v8hu __attribute__((vector_size(16)));
typedef signed char __v16qi __attribute__((vector_size(16)));
typedef unsigned char __v16qu __attribute__((vector_size(16)));
#define __I(v) ((__m128i)(v))

/* doubles */
__SI __m128d _mm_setzero_pd(void) { return (__m128d){ 0, 0 }; }
__SI __m128d _mm_set1_pd(double x) { return (__m128d){ x, x }; }
__SI __m128d _mm_set_pd(double b, double a) { return (__m128d){ a, b }; }
__SI __m128d _mm_setr_pd(double a, double b) { return (__m128d){ a, b }; }
__SI __m128d _mm_loadu_pd(const double *p) { __m128d r; memcpy(&r, p, 16); return r; }
__SI __m128d _mm_load_pd(const double *p) { return *(const __m128d *)p; }
__SI void _mm_storeu_pd(double *p, __m128d a) { memcpy(p, &a, 16); }
__SI void _mm_store_pd(double *p, __m128d a) { *(__m128d *)p = a; }
__SI double _mm_cvtsd_f64(__m128d a) { return a[0]; }
__SI __m128d _mm_add_pd(__m128d a, __m128d b) { return a + b; }
__SI __m128d _mm_sub_pd(__m128d a, __m128d b) { return a - b; }
__SI __m128d _mm_mul_pd(__m128d a, __m128d b) { return a * b; }
__SI __m128d _mm_div_pd(__m128d a, __m128d b) { return a / b; }
__SI __m128d _mm_min_pd(__m128d a, __m128d b) { return __V(MIN, a, b); }
__SI __m128d _mm_max_pd(__m128d a, __m128d b) { return __V(MAX, a, b); }
__SI __m128d _mm_sqrt_pd(__m128d a) { return __V(SQRT, a, a); }
__SI __m128d _mm_and_pd(__m128d a, __m128d b) { return (__m128d)((__v2di)a & (__v2di)b); }
__SI __m128d _mm_or_pd(__m128d a, __m128d b) { return (__m128d)((__v2di)a | (__v2di)b); }
__SI __m128d _mm_xor_pd(__m128d a, __m128d b) { return (__m128d)((__v2di)a ^ (__v2di)b); }
__SI __m128d _mm_cmpeq_pd(__m128d a, __m128d b) { return __V(EQ, a, b); }
__SI __m128d _mm_cmplt_pd(__m128d a, __m128d b) { return __V(LT, a, b); }
__SI __m128d _mm_cmple_pd(__m128d a, __m128d b) { return __V(LE, a, b); }
__SI __m128d _mm_cmpgt_pd(__m128d a, __m128d b) { return __V(GT, a, b); }
__SI __m128d _mm_unpacklo_pd(__m128d a, __m128d b) { return __V(UNPCKLO, a, b); }
__SI __m128d _mm_unpackhi_pd(__m128d a, __m128d b) { return __V(UNPCKHI, a, b); }
__SI int _mm_movemask_pd(__m128d a) { return __V(MOVEMASK, a, a); }
__SI __m128 _mm_castpd_ps(__m128d a) { return (__m128)a; }
__SI __m128d _mm_castps_pd(__m128 a) { return (__m128d)a; }
__SI __m128i _mm_castps_si128(__m128 a) { return (__m128i)a; }
__SI __m128 _mm_castsi128_ps(__m128i a) { return (__m128)a; }
__SI __m128i _mm_castpd_si128(__m128d a) { return (__m128i)a; }
__SI __m128d _mm_castsi128_pd(__m128i a) { return (__m128d)a; }

/* integers */
__SI __m128i _mm_setzero_si128(void) { return (__m128i){ 0, 0 }; }
__SI __m128i _mm_set1_epi8(char x) { return __I(((__v16qi){ 0 }) + (signed char)x); }
__SI __m128i _mm_set1_epi16(short x) { return __I(((__v8hi){ 0 }) + x); }
__SI __m128i _mm_set1_epi32(int x) { return __I(((__v4si){ 0 }) + x); }
__SI __m128i _mm_set1_epi64x(long long x) { return (__m128i){ x, x }; }
__SI __m128i _mm_set_epi32(int d, int c, int b, int a) { return __I(((__v4si){ a, b, c, d })); }
__SI __m128i _mm_setr_epi32(int a, int b, int c, int d) { return __I(((__v4si){ a, b, c, d })); }
__SI __m128i _mm_set_epi64x(long long b, long long a) { return (__m128i){ a, b }; }
__SI __m128i _mm_set_epi16(short h, short g, short f, short e, short d, short c, short b, short a) { return __I(((__v8hi){ a, b, c, d, e, f, g, h })); }
__SI __m128i _mm_set_epi8(char p, char o, char n, char m, char l, char k, char j, char i, char h, char g, char f, char e, char d, char c, char b, char a)
{ return __I(((__v16qi){ a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p })); }
__SI __m128i _mm_loadu_si128(const void *p) { __m128i r; memcpy(&r, p, 16); return r; }
__SI __m128i _mm_load_si128(const __m128i *p) { return *p; }
__SI __m128i _mm_loadl_epi64(const void *p) { long long x; memcpy(&x, p, 8); return (__m128i){ x, 0 }; }
__SI void _mm_storeu_si128(void *p, __m128i a) { memcpy(p, &a, 16); }
__SI void _mm_store_si128(__m128i *p, __m128i a) { *p = a; }
__SI void _mm_storel_epi64(void *p, __m128i a) { long long x = a[0]; memcpy(p, &x, 8); }
__SI int _mm_cvtsi128_si32(__m128i a) { return ((__v4si)a)[0]; }
__SI long long _mm_cvtsi128_si64(__m128i a) { return a[0]; }
__SI __m128i _mm_cvtsi32_si128(int a) { return __I(((__v4si){ a, 0, 0, 0 })); }
__SI __m128i _mm_cvtsi64_si128(long long a) { return (__m128i){ a, 0 }; }
__SI __m128i _mm_add_epi8(__m128i a, __m128i b) { return __I((__v16qi)a + (__v16qi)b); }
__SI __m128i _mm_add_epi16(__m128i a, __m128i b) { return __I((__v8hi)a + (__v8hi)b); }
__SI __m128i _mm_add_epi32(__m128i a, __m128i b) { return __I((__v4si)a + (__v4si)b); }
__SI __m128i _mm_add_epi64(__m128i a, __m128i b) { return a + b; }
__SI __m128i _mm_sub_epi8(__m128i a, __m128i b) { return __I((__v16qi)a - (__v16qi)b); }
__SI __m128i _mm_sub_epi16(__m128i a, __m128i b) { return __I((__v8hi)a - (__v8hi)b); }
__SI __m128i _mm_sub_epi32(__m128i a, __m128i b) { return __I((__v4si)a - (__v4si)b); }
__SI __m128i _mm_sub_epi64(__m128i a, __m128i b) { return a - b; }
__SI __m128i _mm_adds_epi8(__m128i a, __m128i b) { return __I(__V(ADDS, (__v16qi)a, (__v16qi)b)); }
__SI __m128i _mm_adds_epi16(__m128i a, __m128i b) { return __I(__V(ADDS, (__v8hi)a, (__v8hi)b)); }
__SI __m128i _mm_adds_epu8(__m128i a, __m128i b) { return __I(__V(ADDS, (__v16qu)a, (__v16qu)b)); }
__SI __m128i _mm_adds_epu16(__m128i a, __m128i b) { return __I(__V(ADDS, (__v8hu)a, (__v8hu)b)); }
__SI __m128i _mm_subs_epi8(__m128i a, __m128i b) { return __I(__V(SUBS, (__v16qi)a, (__v16qi)b)); }
__SI __m128i _mm_subs_epi16(__m128i a, __m128i b) { return __I(__V(SUBS, (__v8hi)a, (__v8hi)b)); }
__SI __m128i _mm_subs_epu8(__m128i a, __m128i b) { return __I(__V(SUBS, (__v16qu)a, (__v16qu)b)); }
__SI __m128i _mm_subs_epu16(__m128i a, __m128i b) { return __I(__V(SUBS, (__v8hu)a, (__v8hu)b)); }
__SI __m128i _mm_mullo_epi16(__m128i a, __m128i b) { return __I((__v8hi)a * (__v8hi)b); }
__SI __m128i _mm_mulhi_epi16(__m128i a, __m128i b) { return __I(__V(MULHI, (__v8hi)a, (__v8hi)b)); }
__SI __m128i _mm_mulhi_epu16(__m128i a, __m128i b) { return __I(__V(MULHI, (__v8hu)a, (__v8hu)b)); }
__SI __m128i _mm_mul_epu32(__m128i a, __m128i b) { return __I(__V(MULUDQ, (__v4su)a, (__v4su)b)); }
__SI __m128i _mm_madd_epi16(__m128i a, __m128i b) { return __I(__V(MADD, (__v8hi)a, (__v8hi)b)); }
__SI __m128i _mm_avg_epu8(__m128i a, __m128i b) { return __I(__V(AVG, (__v16qu)a, (__v16qu)b)); }
__SI __m128i _mm_avg_epu16(__m128i a, __m128i b) { return __I(__V(AVG, (__v8hu)a, (__v8hu)b)); }
__SI __m128i _mm_sad_epu8(__m128i a, __m128i b) { return __I(__V(SAD, (__v16qu)a, (__v16qu)b)); }
__SI __m128i _mm_min_epi16(__m128i a, __m128i b) { return __I(__V(MIN, (__v8hi)a, (__v8hi)b)); }
__SI __m128i _mm_max_epi16(__m128i a, __m128i b) { return __I(__V(MAX, (__v8hi)a, (__v8hi)b)); }
__SI __m128i _mm_min_epu8(__m128i a, __m128i b) { return __I(__V(MIN, (__v16qu)a, (__v16qu)b)); }
__SI __m128i _mm_max_epu8(__m128i a, __m128i b) { return __I(__V(MAX, (__v16qu)a, (__v16qu)b)); }
__SI __m128i _mm_and_si128(__m128i a, __m128i b) { return a & b; }
__SI __m128i _mm_or_si128(__m128i a, __m128i b) { return a | b; }
__SI __m128i _mm_xor_si128(__m128i a, __m128i b) { return a ^ b; }
__SI __m128i _mm_andnot_si128(__m128i a, __m128i b) { return __V(ANDNOT, a, b); }
__SI __m128i _mm_cmpeq_epi8(__m128i a, __m128i b) { return __I((__v16qi)a == (__v16qi)b); }
__SI __m128i _mm_cmpeq_epi16(__m128i a, __m128i b) { return __I((__v8hi)a == (__v8hi)b); }
__SI __m128i _mm_cmpeq_epi32(__m128i a, __m128i b) { return __I((__v4si)a == (__v4si)b); }
__SI __m128i _mm_cmpgt_epi8(__m128i a, __m128i b) { return __I((__v16qi)a > (__v16qi)b); }
__SI __m128i _mm_cmpgt_epi16(__m128i a, __m128i b) { return __I((__v8hi)a > (__v8hi)b); }
__SI __m128i _mm_cmpgt_epi32(__m128i a, __m128i b) { return __I((__v4si)a > (__v4si)b); }
__SI __m128i _mm_cmplt_epi8(__m128i a, __m128i b) { return __I((__v16qi)a < (__v16qi)b); }
__SI __m128i _mm_cmplt_epi16(__m128i a, __m128i b) { return __I((__v8hi)a < (__v8hi)b); }
__SI __m128i _mm_cmplt_epi32(__m128i a, __m128i b) { return __I((__v4si)a < (__v4si)b); }
__SI __m128i _mm_packs_epi16(__m128i a, __m128i b) { return __I(__V(PACKS, (__v8hi)a, (__v8hi)b)); }
__SI __m128i _mm_packs_epi32(__m128i a, __m128i b) { return __I(__V(PACKS, (__v4si)a, (__v4si)b)); }
__SI __m128i _mm_packus_epi16(__m128i a, __m128i b) { return __I(__V(PACKUS, (__v8hi)a, (__v8hi)b)); }
__SI __m128i _mm_unpacklo_epi8(__m128i a, __m128i b) { return __I(__V(UNPCKLO, (__v16qi)a, (__v16qi)b)); }
__SI __m128i _mm_unpacklo_epi16(__m128i a, __m128i b) { return __I(__V(UNPCKLO, (__v8hi)a, (__v8hi)b)); }
__SI __m128i _mm_unpacklo_epi32(__m128i a, __m128i b) { return __I(__V(UNPCKLO, (__v4si)a, (__v4si)b)); }
__SI __m128i _mm_unpacklo_epi64(__m128i a, __m128i b) { return __V(UNPCKLO, a, b); }
__SI __m128i _mm_unpackhi_epi8(__m128i a, __m128i b) { return __I(__V(UNPCKHI, (__v16qi)a, (__v16qi)b)); }
__SI __m128i _mm_unpackhi_epi16(__m128i a, __m128i b) { return __I(__V(UNPCKHI, (__v8hi)a, (__v8hi)b)); }
__SI __m128i _mm_unpackhi_epi32(__m128i a, __m128i b) { return __I(__V(UNPCKHI, (__v4si)a, (__v4si)b)); }
__SI __m128i _mm_unpackhi_epi64(__m128i a, __m128i b) { return __V(UNPCKHI, a, b); }
__SI int _mm_movemask_epi8(__m128i a) { return __V(MOVEMASK, (__v16qi)a, (__v16qi)a); }
__SI __m128 _mm_cvtepi32_ps(__m128i a) { return (__m128)__V(CVTI2F, (__v4si)a, (__v4si)a); }
__SI __m128i _mm_cvttps_epi32(__m128 a) { return __I(__V(CVTF2I, a, a)); }
__SI __m128i _mm_cvtps_epi32(__m128 a) { return __I(__V(CVTF2IR, a, a)); }
#define _mm_slli_epi16(a, n) __I(__builtin_vec(__VOP_SHLI, (__v8hi)(a), (__v8hi)(a), (n)))
#define _mm_slli_epi32(a, n) __I(__builtin_vec(__VOP_SHLI, (__v4si)(a), (__v4si)(a), (n)))
#define _mm_slli_epi64(a, n) __I(__builtin_vec(__VOP_SHLI, (__v2di)(a), (__v2di)(a), (n)))
#define _mm_srli_epi16(a, n) __I(__builtin_vec(__VOP_SHRI, (__v8hu)(a), (__v8hu)(a), (n)))
#define _mm_srli_epi32(a, n) __I(__builtin_vec(__VOP_SHRI, (__v4su)(a), (__v4su)(a), (n)))
#define _mm_srli_epi64(a, n) __I(__builtin_vec(__VOP_SHRI, (__v2du)(a), (__v2du)(a), (n)))
#define _mm_srai_epi16(a, n) __I(__builtin_vec(__VOP_SARI, (__v8hi)(a), (__v8hi)(a), (n)))
#define _mm_srai_epi32(a, n) __I(__builtin_vec(__VOP_SARI, (__v4si)(a), (__v4si)(a), (n)))
#define _mm_slli_si128(a, n) __I(__builtin_vec(__VOP_SLLDQ, (__v2di)(a), (__v2di)(a), (n)))
#define _mm_srli_si128(a, n) __I(__builtin_vec(__VOP_SRLDQ, (__v2di)(a), (__v2di)(a), (n)))
#define _mm_bslli_si128 _mm_slli_si128
#define _mm_bsrli_si128 _mm_srli_si128
#define _mm_shuffle_epi32(a, i) __I(__builtin_vec(__VOP_PSHUFD, (__v4si)(a), (__v4si)(a), (i)))
#define _mm_extract_epi16(a, i) ((int)(unsigned short)((__v8hi)(a))[(i) & 7])
#define _mm_shuffle_pd(a, b, i) __builtin_shufflevector((__m128d)(a), (__m128d)(b), (i) & 1, (((i) >> 1) & 1) + 2)
#endif
