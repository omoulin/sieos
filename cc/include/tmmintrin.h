/* tmmintrin.h - SSSE3 intrinsics, for sicc. Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#ifndef _TMMINTRIN_H
#define _TMMINTRIN_H
#include <emmintrin.h>
__SI __m128i _mm_maddubs_epi16(__m128i a, __m128i b) { return __I(__V(MADDUBS, (__v16qu)a, (__v16qi)b)); }
__SI __m128i _mm_shuffle_epi8(__m128i a, __m128i b) { return __I(__V(SHUFB, (__v16qi)a, (__v16qi)b)); }
__SI __m128i _mm_abs_epi8(__m128i a) { return __I(__V(ABS, (__v16qi)a, (__v16qi)a)); }
__SI __m128i _mm_abs_epi16(__m128i a) { return __I(__V(ABS, (__v8hi)a, (__v8hi)a)); }
__SI __m128i _mm_abs_epi32(__m128i a) { return __I(__V(ABS, (__v4si)a, (__v4si)a)); }
#endif
