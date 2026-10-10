/* xmmintrin.h - SSE intrinsics (__m128: four floats), for sicc.
 * The operations are vector operators, __builtin_shufflevector, and
 * __builtin_vec(operation, a, b, immediate): its operation numbers are
 * sicc's VOP_* (sicc.h). Intrinsics with an immediate are macros.
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#ifndef _XMMINTRIN_H
#define _XMMINTRIN_H
enum { __VOP_ADD, __VOP_SUB, __VOP_MUL, __VOP_DIV, __VOP_AND, __VOP_OR, __VOP_XOR, __VOP_SHL, __VOP_SHR, __VOP_NEG, __VOP_NOT,
       __VOP_EQ, __VOP_NE, __VOP_LT, __VOP_LE, __VOP_GT, __VOP_GE, __VOP_SHLI, __VOP_SHRI, __VOP_SARI, __VOP_MIN, __VOP_MAX,
       __VOP_SQRT, __VOP_ANDNOT, __VOP_UNPCKLO, __VOP_UNPCKHI, __VOP_CVTI2F, __VOP_CVTF2I, __VOP_MADD, __VOP_MADDUBS,
       __VOP_AVG, __VOP_ADDS, __VOP_SUBS, __VOP_PACKS, __VOP_PACKUS, __VOP_MULHI, __VOP_MULUDQ, __VOP_SAD, __VOP_SHUFB,
       __VOP_MOVEMASK, __VOP_SPLAT, __VOP_SHUF, __VOP_CVTF2IR, __VOP_SLLDQ, __VOP_SRLDQ, __VOP_PSHUFD, __VOP_ABS };
typedef float __m128 __attribute__((vector_size(16)));
typedef float __v4sf __attribute__((vector_size(16)));
typedef int __v4si __attribute__((vector_size(16)));
void *memcpy(void *, const void *, __SIZE_TYPE__);  /* (small and constant: inlined) */
#define __SI static __inline__ __attribute__((always_inline))
#define __V(op, a, b) __builtin_vec(__VOP_##op, a, b, 0)

__SI __m128 _mm_setzero_ps(void) { return (__m128){ 0, 0, 0, 0 }; }
__SI __m128 _mm_set1_ps(float x) { return (__m128){ x, x, x, x }; }
__SI __m128 _mm_set_ps1(float x) { return _mm_set1_ps(x); }
__SI __m128 _mm_set_ps(float d, float c, float b, float a) { return (__m128){ a, b, c, d }; }
__SI __m128 _mm_setr_ps(float a, float b, float c, float d) { return (__m128){ a, b, c, d }; }
__SI __m128 _mm_set_ss(float a) { return (__m128){ a, 0, 0, 0 }; }
__SI __m128 _mm_loadu_ps(const float *p) { __m128 r; memcpy(&r, p, 16); return r; }
__SI __m128 _mm_load_ps(const float *p) { return *(const __m128 *)p; }
__SI __m128 _mm_load1_ps(const float *p) { return _mm_set1_ps(*p); }
__SI void _mm_storeu_ps(float *p, __m128 a) { memcpy(p, &a, 16); }
__SI void _mm_store_ps(float *p, __m128 a) { *(__m128 *)p = a; }
__SI void _mm_store_ss(float *p, __m128 a) { *p = a[0]; }
__SI float _mm_cvtss_f32(__m128 a) { return a[0]; }
__SI __m128 _mm_add_ps(__m128 a, __m128 b) { return a + b; }
__SI __m128 _mm_sub_ps(__m128 a, __m128 b) { return a - b; }
__SI __m128 _mm_mul_ps(__m128 a, __m128 b) { return a * b; }
__SI __m128 _mm_div_ps(__m128 a, __m128 b) { return a / b; }
__SI __m128 _mm_add_ss(__m128 a, __m128 b) { a[0] += b[0]; return a; }
__SI __m128 _mm_sub_ss(__m128 a, __m128 b) { a[0] -= b[0]; return a; }
__SI __m128 _mm_mul_ss(__m128 a, __m128 b) { a[0] *= b[0]; return a; }
__SI __m128 _mm_div_ss(__m128 a, __m128 b) { a[0] /= b[0]; return a; }
__SI __m128 _mm_min_ps(__m128 a, __m128 b) { return __V(MIN, a, b); }
__SI __m128 _mm_max_ps(__m128 a, __m128 b) { return __V(MAX, a, b); }
__SI __m128 _mm_sqrt_ps(__m128 a) { return __V(SQRT, a, a); }
__SI __m128 _mm_and_ps(__m128 a, __m128 b) { return (__m128)((__v4si)a & (__v4si)b); }
__SI __m128 _mm_or_ps(__m128 a, __m128 b) { return (__m128)((__v4si)a | (__v4si)b); }
__SI __m128 _mm_xor_ps(__m128 a, __m128 b) { return (__m128)((__v4si)a ^ (__v4si)b); }
__SI __m128 _mm_andnot_ps(__m128 a, __m128 b) { return (__m128)__V(ANDNOT, (__v4si)a, (__v4si)b); }
__SI __m128 _mm_cmpeq_ps(__m128 a, __m128 b) { return __V(EQ, a, b); }
__SI __m128 _mm_cmpneq_ps(__m128 a, __m128 b) { return __V(NE, a, b); }
__SI __m128 _mm_cmplt_ps(__m128 a, __m128 b) { return __V(LT, a, b); }
__SI __m128 _mm_cmple_ps(__m128 a, __m128 b) { return __V(LE, a, b); }
__SI __m128 _mm_cmpgt_ps(__m128 a, __m128 b) { return __V(GT, a, b); }
__SI __m128 _mm_cmpge_ps(__m128 a, __m128 b) { return __V(GE, a, b); }
__SI __m128 _mm_unpacklo_ps(__m128 a, __m128 b) { return __V(UNPCKLO, a, b); }
__SI __m128 _mm_unpackhi_ps(__m128 a, __m128 b) { return __V(UNPCKHI, a, b); }
__SI __m128 _mm_movehl_ps(__m128 a, __m128 b) { return __builtin_shufflevector(a, b, 6, 7, 2, 3); }
__SI __m128 _mm_movelh_ps(__m128 a, __m128 b) { return __builtin_shufflevector(a, b, 0, 1, 4, 5); }
__SI int _mm_movemask_ps(__m128 a) { return __V(MOVEMASK, a, a); }
#define _mm_shuffle_ps(a, b, i) __builtin_shufflevector((__m128)(a), (__m128)(b), (i) & 3, ((i) >> 2) & 3, (((i) >> 4) & 3) + 4, (((i) >> 6) & 3) + 4)
#define _MM_SHUFFLE(z, y, x, w) (((z) << 6) | ((y) << 4) | ((x) << 2) | (w))
#endif
