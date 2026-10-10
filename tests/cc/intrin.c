/* cflags: -mavx2 -mfma */
/* intrin.c - SSE to SSE4.1 and AVX2 intrinsics (sicc's headers; the host compiler's
 * for the reference), on fixed data; results printed as integers.
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
#include <immintrin.h>
static void p128(const char *s, __m128i v) { int x[4]; _mm_storeu_si128((__m128i *)x, v); printf("%s %08x %08x %08x %08x\n", s, x[0], x[1], x[2], x[3]); }
static void p256(const char *s, __m256i v) { int x[8]; _mm256_storeu_si256((__m256i *)x, v); printf("%s", s); for (int i = 0; i < 8; i++) printf(" %08x", x[i]); printf("\n"); }
static void pf(const char *s, __m128 v) { float x[4]; _mm_storeu_ps(x, v); printf("%s %g %g %g %g\n", s, x[0], x[1], x[2], x[3]); }
int main(void)
{
    unsigned char b1[32], b2[32];
    for (int i = 0; i < 32; i++) { b1[i] = i * 37 + 11; b2[i] = 200 - i * 13; }
    __m128i a = _mm_loadu_si128((__m128i *)b1), b = _mm_loadu_si128((__m128i *)b2);
    p128("add8", _mm_add_epi8(a, b)); p128("add16", _mm_add_epi16(a, b)); p128("add32", _mm_add_epi32(a, b)); p128("add64", _mm_add_epi64(a, b));
    p128("sub16", _mm_sub_epi16(a, b)); p128("adds8", _mm_adds_epi8(a, b)); p128("addsu8", _mm_adds_epu8(a, b)); p128("subsu16", _mm_subs_epu16(a, b));
    p128("mullo16", _mm_mullo_epi16(a, b)); p128("mulhi16", _mm_mulhi_epi16(a, b)); p128("mulhiu16", _mm_mulhi_epu16(a, b)); p128("muludq", _mm_mul_epu32(a, b));
    p128("madd", _mm_madd_epi16(a, b)); p128("maddubs", _mm_maddubs_epi16(a, b)); p128("avg8", _mm_avg_epu8(a, b)); p128("sad", _mm_sad_epu8(a, b));
    p128("min16", _mm_min_epi16(a, b)); p128("maxu8", _mm_max_epu8(a, b)); p128("min32", _mm_min_epi32(a, b)); p128("maxu32", _mm_max_epu32(a, b));
    p128("and", _mm_and_si128(a, b)); p128("andnot", _mm_andnot_si128(a, b)); p128("or", _mm_or_si128(a, b)); p128("xor", _mm_xor_si128(a, b));
    p128("cmpeq8", _mm_cmpeq_epi8(a, a)); p128("cmpgt16", _mm_cmpgt_epi16(a, b)); p128("cmpgt32", _mm_cmpgt_epi32(a, b)); p128("cmplt8", _mm_cmplt_epi8(a, b));
    p128("packs16", _mm_packs_epi16(a, b)); p128("packs32", _mm_packs_epi32(a, b)); p128("packus16", _mm_packus_epi16(a, b)); p128("packus32", _mm_packus_epi32(a, b));
    p128("unlo8", _mm_unpacklo_epi8(a, b)); p128("unhi16", _mm_unpackhi_epi16(a, b)); p128("unlo32", _mm_unpacklo_epi32(a, b)); p128("unhi64", _mm_unpackhi_epi64(a, b));
    p128("slli16", _mm_slli_epi16(a, 3)); p128("srli32", _mm_srli_epi32(a, 5)); p128("srai16", _mm_srai_epi16(a, 2)); p128("srai32", _mm_srai_epi32(b, 31));
    p128("slli64", _mm_slli_epi64(a, 7)); p128("bsl", _mm_slli_si128(a, 3)); p128("bsr", _mm_srli_si128(a, 5)); p128("shuf32", _mm_shuffle_epi32(a, 0x1b));
    p128("shufb", _mm_shuffle_epi8(a, _mm_and_si128(b, _mm_set1_epi8(0x8f)))); p128("abs8", _mm_abs_epi8(b)); p128("mullo32", _mm_mullo_epi32(a, b));
    p128("set", _mm_set_epi32(1, 2, 3, 4)); p128("set1", _mm_set1_epi16(-3)); p128("setr", _mm_setr_epi32(5, 6, 7, 8));
    printf("mask %x %d %d %d\n", _mm_movemask_epi8(a), _mm_cvtsi128_si32(b), _mm_extract_epi16(a, 5), _mm_extract_epi32(b, 2));
    __m128 f = _mm_setr_ps(1.5f, -2.25f, 3.0f, 0.5f), g = _mm_set1_ps(2.0f);
    pf("addps", _mm_add_ps(f, g)); pf("mulps", _mm_mul_ps(f, g)); pf("divps", _mm_div_ps(f, g)); pf("minps", _mm_min_ps(f, g)); pf("sqrt", _mm_sqrt_ps(_mm_max_ps(f, g)));
    pf("shufps", _mm_shuffle_ps(f, g, _MM_SHUFFLE(1, 0, 3, 2))); pf("unlo", _mm_unpacklo_ps(f, g)); pf("cvt", _mm_cvtepi32_ps(_mm_set_epi32(-7, 8, 9, 10)));
    p128("cvtt", _mm_cvttps_epi32(f)); p128("cmplt", _mm_castps_si128(_mm_cmplt_ps(f, g))); printf("maskps %d %g\n", _mm_movemask_ps(f), _mm_cvtss_f32(f));
    __m256i A = _mm256_loadu_si256((__m256i *)b1), B = _mm256_loadu_si256((__m256i *)b2);
    p256("add32", _mm256_add_epi32(A, B)); p256("maddubs", _mm256_maddubs_epi16(A, B)); p256("madd", _mm256_madd_epi16(A, B));
    p256("cmpgt8", _mm256_cmpgt_epi8(A, B)); p256("srai16", _mm256_srai_epi16(A, 3)); p256("mullo32", _mm256_mullo_epi32(A, B));
    printf("mask256 %x\n", _mm256_movemask_epi8(A));
    __m256 F = _mm256_set1_ps(1.25f); F = _mm256_fmadd_ps(F, F, F); float o[8]; _mm256_storeu_ps(o, F); printf("fma %g\n", o[7]);
    p128("hi128", _mm256_extracti128_si256(A, 1));
    return 0;
}
