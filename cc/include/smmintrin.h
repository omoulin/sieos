/* smmintrin.h - SSE4.1 intrinsics, for sicc (-mavx/-mavx2: the instructions; else element by element).
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#ifndef _SMMINTRIN_H
#define _SMMINTRIN_H
#include <tmmintrin.h>
__SI __m128i _mm_mullo_epi32(__m128i a, __m128i b) { return __I((__v4si)a * (__v4si)b); }
__SI __m128i _mm_min_epi32(__m128i a, __m128i b) { return __I(__V(MIN, (__v4si)a, (__v4si)b)); }
__SI __m128i _mm_max_epi32(__m128i a, __m128i b) { return __I(__V(MAX, (__v4si)a, (__v4si)b)); }
__SI __m128i _mm_min_epu32(__m128i a, __m128i b) { return __I(__V(MIN, (__v4su)a, (__v4su)b)); }
__SI __m128i _mm_max_epu32(__m128i a, __m128i b) { return __I(__V(MAX, (__v4su)a, (__v4su)b)); }
__SI __m128i _mm_cmpeq_epi64(__m128i a, __m128i b) { return __I(a == b); }
__SI __m128i _mm_packus_epi32(__m128i a, __m128i b) { return __I(__V(PACKUS, (__v4si)a, (__v4si)b)); }
#define _mm_extract_epi32(a, i) (((__v4si)(a))[(i) & 3])
#define _mm_extract_epi8(a, i) ((int)(unsigned char)((__v16qi)(a))[(i) & 15])
#endif
