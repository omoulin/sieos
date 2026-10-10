/*
 * quant.c - Quantized numbers: unpacking weights, packing activations, and
 * the dot products every matrix-vector product is made of.
 *
 * A weight row is a run of blocks (internal.h). To multiply it by a vector,
 * the vector is first quantized to 8-bit integers in the same block size;
 * then each block is an integer dot product (fast) times two scales.
 * Each format has a plain C kernel (the reference), and an AVX2 one on
 * x86-64 or NEON ones on AArch64 (neon.c); the best the processor has is
 * chosen at load time (pick_kernels).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "internal.h"

const tinfo_t tinfo[T_NTYPES] = {
    [T_F32]  = { 1, 4, A_F32 },   [T_F16]  = { 1, 2, A_F32 },   [T_BF16] = { 1, 2, A_F32 },
    [T_Q4_0] = { 32, 18, A_Q8 },  [T_Q4_1] = { 32, 20, A_Q8 },
    [T_Q5_0] = { 32, 22, A_Q8 },  [T_Q5_1] = { 32, 24, A_Q8 },  [T_Q8_0] = { 32, 34, A_Q8 },
    [T_Q4_K] = { 256, 144, A_Q8K }, [T_Q5_K] = { 256, 176, A_Q8K }, [T_Q6_K] = { 256, 210, A_Q8K },
};

/* ---- Half precision <-> float (bit manipulation: no hardware needed). */
float f16_to_f32(f16 h)
{
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, f = h & 0x3FF, u;
    if (e == 0x1F) u = s | 0x7F800000 | (f << 13);                  /* inf, nan */
    else if (e) u = s | ((e + 112) << 23) | (f << 13);              /* normal */
    else if (!f) u = s;                                             /* zero */
    else {                                                          /* subnormal: normalise */
        e = 113;
        while (!(f & 0x400)) { f <<= 1; e--; }
        u = s | (e << 23) | ((f & 0x3FF) << 13);
    }
    union { uint32_t u; float f; } v = { .u = u };
    return v.f;
}

f16 f32_to_f16(float x)                        /* round to nearest even */
{
    union { float f; uint32_t u; } v = { .f = x };
    uint32_t s = (v.u >> 16) & 0x8000, e = (v.u >> 23) & 0xFF, f = v.u & 0x7FFFFF;
    if (e == 0xFF) return s | 0x7C00 | (f ? 0x200 : 0);
    int ne = (int)e - 112;
    if (ne >= 31) return s | 0x7C00;                                  /* too big: inf */
    if (ne <= 0) {                                                    /* subnormal or 0 */
        if (ne < -10) return s;
        f |= 0x800000;
        int sh = 14 - ne;
        uint32_t h = f >> sh, rem = f & ((1u << sh) - 1), half = 1u << (sh - 1);
        if (rem > half || (rem == half && (h & 1))) h++;
        return s | h;
    }
    uint32_t h = (ne << 10) | (f >> 13), rem = f & 0x1FFF;
    if (rem > 0x1000 || (rem == 0x1000 && (h & 1))) h++;            /* may carry into e: fine */
    return s | h;
}

static float bf16_to_f32(uint16_t b) { union { uint32_t u; float f; } v = { .u = (uint32_t)b << 16 }; return v.f; }

static int nearest_int(float f)               /* round, by the float trick (|f| < 2^22) */
{
    float v = f + 12582912.f;
    int32_t i; memcpy(&i, &v, 4);
    return (i & 0x007FFFFF) - 0x00400000;
}

/* The 6-bit scale and min of sub-block j of a Q4_K/Q5_K block. */
static void scale_min(int j, const uint8_t *q, int *d, int *m)
{
    if (j < 4) { *d = q[j] & 63; *m = q[j + 4] & 63; }
    else { *d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4); *m = (q[j + 4] >> 4) | ((q[j] >> 6) << 4); }
}

/* ---- Unpacking a row into floats (reference, and for embeddings). */
void dequant_row(int type, const void *row, float *y, int n)
{
    switch (type) {
    case T_F32: memcpy(y, row, n * 4); return;
    case T_F16: for (int i = 0; i < n; i++) y[i] = f16_to_f32(((const f16 *)row)[i]); return;
    case T_BF16: for (int i = 0; i < n; i++) y[i] = bf16_to_f32(((const uint16_t *)row)[i]); return;
    }
    for (int b = 0; b < n / tinfo[type].bs; b++) switch (type) {
    case T_Q4_0: { const bq4_0 *x = (const bq4_0 *)row + b; float d = f16_to_f32(x->d);
        for (int j = 0; j < 16; j++) { y[j] = d * ((x->qs[j] & 15) - 8); y[j + 16] = d * ((x->qs[j] >> 4) - 8); }
        y += 32; break; }
    case T_Q4_1: { const bq4_1 *x = (const bq4_1 *)row + b; float d = f16_to_f32(x->d), m = f16_to_f32(x->m);
        for (int j = 0; j < 16; j++) { y[j] = d * (x->qs[j] & 15) + m; y[j + 16] = d * (x->qs[j] >> 4) + m; }
        y += 32; break; }
    case T_Q5_0: case T_Q5_1: {
        const bq5_1 *x1 = (const bq5_1 *)((const uint8_t *)row + b * tinfo[type].bytes);
        const bq5_0 *x0 = (const bq5_0 *)x1;
        int one = type == T_Q5_1;
        float d = f16_to_f32(x0->d), m = one ? f16_to_f32(x1->m) : 0;
        const uint8_t *qh = one ? x1->qh : x0->qh, *qs = one ? x1->qs : x0->qs;
        uint32_t h; memcpy(&h, qh, 4);
        for (int j = 0; j < 16; j++) {
            int a = (qs[j] & 15) | (((h >> j) << 4) & 0x10), c = (qs[j] >> 4) | ((h >> (j + 12)) & 0x10);
            y[j] = one ? d * a + m : d * (a - 16);
            y[j + 16] = one ? d * c + m : d * (c - 16);
        }
        y += 32; break; }
    case T_Q8_0: { const bq8_0 *x = (const bq8_0 *)row + b; float d = f16_to_f32(x->d);
        for (int j = 0; j < 32; j++) y[j] = d * x->qs[j];
        y += 32; break; }
    case T_Q4_K: case T_Q5_K: {
        const bq5_K *x5 = (const bq5_K *)((const uint8_t *)row + b * tinfo[type].bytes);
        const bq4_K *x4 = (const bq4_K *)x5;
        int five = type == T_Q5_K;
        float d = f16_to_f32(x4->d), dmin = f16_to_f32(x4->dmin);
        const uint8_t *q = five ? x5->qs : x4->qs, *sc = x4->sc;
        for (int j = 0, is = 0; j < 256; j += 64, is += 2, q += 32) {
            int s1, m1, s2, m2;
            scale_min(is, sc, &s1, &m1); scale_min(is + 1, sc, &s2, &m2);
            for (int l = 0; l < 32; l++) {
                int h1 = five ? ((x5->qh[l] >> is) & 1) << 4 : 0, h2 = five ? ((x5->qh[l] >> (is + 1)) & 1) << 4 : 0;
                y[l] = d * s1 * ((q[l] & 15) + h1) - dmin * m1;
                y[l + 32] = d * s2 * ((q[l] >> 4) + h2) - dmin * m2;
            }
            y += 64;
        }
        break; }
    case T_Q6_K: { const bq6_K *x = (const bq6_K *)row + b; float d = f16_to_f32(x->d);
        const uint8_t *ql = x->ql, *qh = x->qh; const int8_t *sc = x->sc;
        for (int n2 = 0; n2 < 256; n2 += 128, y += 128, ql += 64, qh += 32, sc += 8)
            for (int l = 0; l < 32; l++) {
                int is = l / 16;
                y[l]      = d * sc[is]     * (((ql[l] & 15)      | ((qh[l] & 3) << 4)) - 32);
                y[l + 32] = d * sc[is + 2] * (((ql[l + 32] & 15) | (((qh[l] >> 2) & 3) << 4)) - 32);
                y[l + 64] = d * sc[is + 4] * (((ql[l] >> 4)      | (((qh[l] >> 4) & 3) << 4)) - 32);
                y[l + 96] = d * sc[is + 6] * (((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32);
            }
        break; }
    }
}

/* ---- Quantizing activations: 8 bits per value, one scale per block. */
size_t act_bytes(int act, int n)
{
    return act == A_Q8 ? (size_t)n / 32 * sizeof(aq8) : act == A_Q8K ? (size_t)n / 256 * sizeof(aq8K) : (size_t)n * 4;
}

void quant_act(int act, const float *x, void *out, int n)
{
    if (act == A_F32) { memcpy(out, x, n * 4); return; }
    int bs = act == A_Q8 ? 32 : 256;
    for (int b = 0; b < n / bs; b++, x += bs) {
        float amax = 0;
        for (int j = 0; j < bs; j++) { float a = x[j] < 0 ? -x[j] : x[j]; if (a > amax) amax = a; }
        float d = amax / 127, id = d ? 1 / d : 0;
        if (act == A_Q8) {
            aq8 *o = (aq8 *)out + b;
            int sum = 0;
            for (int j = 0; j < 32; j++) { o->qs[j] = nearest_int(x[j] * id); sum += o->qs[j]; }
            o->d = d; o->s = d * sum;
        } else {
            aq8K *o = (aq8K *)out + b;
            for (int j = 0; j < 256; j++) o->qs[j] = nearest_int(x[j] * id);
            for (int g = 0; g < 16; g++) { int s = 0; for (int j = 0; j < 16; j++) s += o->qs[g * 16 + j]; o->bsums[g] = s; }
            o->d = d;
        }
    }
}

/* ---- Reference dot products (plain C). x is the quantized activation. */
float dot_f32(int n, const float *a, const float *b) { float s = 0; for (int i = 0; i < n; i++) s += a[i] * b[i]; return s; }
static float s_dot_f16(int n, const float *a, const f16 *b) { float s = 0; for (int i = 0; i < n; i++) s += a[i] * f16_to_f32(b[i]); return s; }
static void s_axpy_f16(int n, float a, const f16 *x, float *y) { for (int i = 0; i < n; i++) y[i] += a * f16_to_f32(x[i]); }
static void s_to_f16(int n, const float *x, f16 *y) { for (int i = 0; i < n; i++) y[i] = f32_to_f16(x[i]); }
/* the attention's half-precision helpers, set by pick_kernels */
static float (*p_dot_f16)(int, const float *, const f16 *) = s_dot_f16;
static void (*p_axpy_f16)(int, float, const f16 *, float *) = s_axpy_f16;
static void (*p_to_f16)(int, const float *, f16 *) = s_to_f16;
float dot_f32_f16(int n, const float *a, const f16 *b) { return p_dot_f16(n, a, b); }
void axpy_f16(int n, float a, const f16 *x, float *y) { p_axpy_f16(n, a, x, y); }
void to_f16(int n, const float *x, f16 *y) { p_to_f16(n, x, y); }

static float d_f32(int n, const void *w, const void *x) { return dot_f32(n, w, x); }
static float d_f16(int n, const void *w, const void *x) { return dot_f32_f16(n, x, w); }
static float d_bf16(int n, const void *w, const void *x)
{
    float s = 0; for (int i = 0; i < n; i++) s += ((const float *)x)[i] * bf16_to_f32(((const uint16_t *)w)[i]); return s;
}

/* The 32-value formats: unpack each block's integers, dot with the activation's. */
static float d_q32(int type, int n, const void *w, const void *xv)
{
    const aq8 *x = xv;
    float s = 0;
    for (int b = 0; b < n / 32; b++, x++) {
        float y[32];
        int q[32], sum = 0;
        const uint8_t *blk = (const uint8_t *)w + b * tinfo[type].bytes;
        float d = f16_to_f32(*(const f16 *)blk), m = 0;
        switch (type) {
        case T_Q4_0: for (int j = 0; j < 16; j++) { q[j] = (blk[2 + j] & 15) - 8; q[j + 16] = (blk[2 + j] >> 4) - 8; } break;
        case T_Q4_1: m = f16_to_f32(((const f16 *)blk)[1]);
                     for (int j = 0; j < 16; j++) { q[j] = blk[4 + j] & 15; q[j + 16] = blk[4 + j] >> 4; } break;
        case T_Q8_0: for (int j = 0; j < 32; j++) q[j] = (int8_t)blk[2 + j]; break;
        default: /* Q5_0, Q5_1: reuse the unpacker, then back to integers */
            dequant_row(type, blk, y, 32);
            if (type == T_Q5_1) m = f16_to_f32(((const f16 *)blk)[1]);
            for (int j = 0; j < 32; j++) q[j] = d ? nearest_int((y[j] - m) / d) : 0;
        }
        for (int j = 0; j < 32; j++) sum += q[j] * x->qs[j];
        s += d * x->d * sum + m * x->s;
    }
    return s;
}
static float d_q4_0(int n, const void *w, const void *x) { return d_q32(T_Q4_0, n, w, x); }
static float d_q4_1(int n, const void *w, const void *x) { return d_q32(T_Q4_1, n, w, x); }
static float d_q5_0(int n, const void *w, const void *x) { return d_q32(T_Q5_0, n, w, x); }
static float d_q5_1(int n, const void *w, const void *x) { return d_q32(T_Q5_1, n, w, x); }
static float d_q8_0(int n, const void *w, const void *x) { return d_q32(T_Q8_0, n, w, x); }

/* Q4_K / Q5_K: 8 sub-blocks of 32, each with a 6-bit scale and min:
 * sum = d*dx*sum_j(s_j * q.x) - dmin*dx*sum_j(m_j * sum of x in sub-block j). */
static float d_qk45(int five, int n, const void *w, const void *xv)
{
    const aq8K *x = xv;
    float s = 0;
    for (int b = 0; b < n / 256; b++, x++) {
        const bq5_K *x5 = (const bq5_K *)((const uint8_t *)w + b * (five ? 176 : 144));
        const bq4_K *x4 = (const bq4_K *)x5;
        const uint8_t *q = five ? x5->qs : x4->qs;
        int sumi = 0, summ = 0;
        for (int j = 0; j < 8; j++) {
            int sc, m, dot = 0;
            scale_min(j, x4->sc, &sc, &m);
            const uint8_t *qq = q + (j / 2) * 32;
            for (int l = 0; l < 32; l++) {
                int v = j & 1 ? qq[l] >> 4 : qq[l] & 15;
                if (five) v |= ((x5->qh[l] >> j) & 1) << 4;
                dot += v * x->qs[j * 32 + l];
            }
            sumi += sc * dot;
            summ += m * (x->bsums[2 * j] + x->bsums[2 * j + 1]);
        }
        s += x->d * (f16_to_f32(x4->d) * sumi - f16_to_f32(x4->dmin) * summ);
    }
    return s;
}
static float d_q4_K(int n, const void *w, const void *x) { return d_qk45(0, n, w, x); }
static float d_q5_K(int n, const void *w, const void *x) { return d_qk45(1, n, w, x); }

/* Q6_K: 16 sub-blocks of 16 values (6 bits, minus 32), each with an 8-bit scale. */
static float d_q6_K(int n, const void *w, const void *xv)
{
    const aq8K *x = xv;
    float s = 0;
    for (int b = 0; b < n / 256; b++, x++) {
        const bq6_K *y = (const bq6_K *)w + b;
        int sumi = 0;
        for (int h = 0; h < 2; h++) {
            const uint8_t *ql = y->ql + h * 64, *qh = y->qh + h * 32;
            const int8_t *sc = y->sc + h * 8, *xq = x->qs + h * 128;
            for (int l = 0; l < 32; l++) {
                int is = l / 16;
                sumi += sc[is]     * (((ql[l] & 15)      | ((qh[l] & 3) << 4)) - 32) * xq[l];
                sumi += sc[is + 2] * (((ql[l + 32] & 15) | (((qh[l] >> 2) & 3) << 4)) - 32) * xq[l + 32];
                sumi += sc[is + 4] * (((ql[l] >> 4)      | (((qh[l] >> 4) & 3) << 4)) - 32) * xq[l + 64];
                sumi += sc[is + 6] * (((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32) * xq[l + 96];
            }
        }
        s += f16_to_f32(y->d) * x->d * sumi;
    }
    return s;
}

/* ---- AVX2 kernels (x86-64 only). Compiled for AVX2+FMA+F16C whatever the
 * build flags say; used only if the processor has them. */
#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#define AVX2 __attribute__((target("avx2,fma,f16c")))
#define VNNI __attribute__((target("avx2,fma,f16c,avxvnni")))
/* Inside AVX code, never call plain-SSE functions: switching between the two
 * instruction encodings costs dozens of cycles each time. The conversions
 * use the processor's own (VEX-encoded) instructions instead. */
#define H(h)  _cvtsh_ss(h)
#define HF(f) ((f16)_cvtss_sh((f), 0))

AVX2 static inline float hsum8(__m256 v)
{
    __m128 r = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    r = _mm_add_ps(r, _mm_movehl_ps(r, r));
    r = _mm_add_ss(r, _mm_movehdup_ps(r));
    return _mm_cvtss_f32(r);
}
AVX2 static inline __m256i ones16(void) { return _mm256_set1_epi16(1); }
/* signed x signed bytes: sum of products of byte pairs, as 8 int32 */
AVX2 static inline __m256i dot_ss(__m256i a, __m256i b)
{
    __m256i p = _mm256_maddubs_epi16(_mm256_sign_epi8(a, a), _mm256_sign_epi8(b, a));
    return _mm256_madd_epi16(p, ones16());
}
VNNI static inline __m256i dot_ss_vnni(__m256i a, __m256i b)
{
    return _mm256_dpbusd_avx_epi32(_mm256_setzero_si256(), _mm256_sign_epi8(a, a), _mm256_sign_epi8(b, a));
}
/* 16 bytes of nibbles -> 32 bytes: low nibbles first, then high nibbles */
AVX2 static inline __m256i nibbles(const uint8_t *p)
{
    __m128i t = _mm_loadu_si128((const __m128i *)p);
    return _mm256_and_si256(_mm256_set_m128i(_mm_srli_epi16(t, 4), t), _mm256_set1_epi8(15));
}
/* 32 bits -> 32 bytes, 0xFF where the bit is set */
AVX2 static inline __m256i bitbytes(const uint8_t *p)
{
    uint32_t v; memcpy(&v, p, 4);
    __m256i b = _mm256_shuffle_epi8(_mm256_set1_epi32(v), _mm256_set_epi64x(0x0303030303030303, 0x0202020202020202, 0x0101010101010101, 0));
    b = _mm256_or_si256(b, _mm256_set1_epi64x(0x7fbfdfeff7fbfdfe));
    return _mm256_cmpeq_epi8(b, _mm256_set1_epi64x(-1));
}

#define Q32_KERNEL(NAME, ATTR, DOT)                                                     \
ATTR static float NAME##_q8_0(int n, const void *w, const void *xv)                     \
{                                                                                       \
    const bq8_0 *y = w; const aq8 *x = xv; __m256 acc = _mm256_setzero_ps();            \
    for (int b = 0; b < n / 32; b++) {                                                  \
        __m256i p = DOT(_mm256_loadu_si256((const __m256i *)y[b].qs), _mm256_loadu_si256((const __m256i *)x[b].qs)); \
        acc = _mm256_fmadd_ps(_mm256_set1_ps(H(y[b].d) * x[b].d), _mm256_cvtepi32_ps(p), acc); \
    }                                                                                   \
    return hsum8(acc);                                                                  \
}                                                                                       \
ATTR static float NAME##_q4_0(int n, const void *w, const void *xv)                     \
{                                                                                       \
    const bq4_0 *y = w; const aq8 *x = xv; __m256 acc = _mm256_setzero_ps();            \
    for (int b = 0; b < n / 32; b++) {                                                  \
        __m256i q = _mm256_sub_epi8(nibbles(y[b].qs), _mm256_set1_epi8(8));            \
        __m256i p = DOT(q, _mm256_loadu_si256((const __m256i *)x[b].qs));               \
        acc = _mm256_fmadd_ps(_mm256_set1_ps(H(y[b].d) * x[b].d), _mm256_cvtepi32_ps(p), acc); \
    }                                                                                   \
    return hsum8(acc);                                                                  \
}                                                                                       \
ATTR static float NAME##_q5_0(int n, const void *w, const void *xv)                     \
{                                                                                       \
    const bq5_0 *y = w; const aq8 *x = xv; __m256 acc = _mm256_setzero_ps();            \
    for (int b = 0; b < n / 32; b++) {                                                  \
        __m256i hi = _mm256_andnot_si256(bitbytes(y[b].qh), _mm256_set1_epi8((char)0xF0)); \
        __m256i q = _mm256_or_si256(nibbles(y[b].qs), hi);   /* q - 16, as a signed byte */ \
        __m256i p = DOT(q, _mm256_loadu_si256((const __m256i *)x[b].qs));               \
        acc = _mm256_fmadd_ps(_mm256_set1_ps(H(y[b].d) * x[b].d), _mm256_cvtepi32_ps(p), acc); \
    }                                                                                   \
    return hsum8(acc);                                                                  \
}
Q32_KERNEL(avx2, AVX2, dot_ss)
Q32_KERNEL(vnni, VNNI, dot_ss_vnni)

/* Q4_K. The 12 bytes of 6-bit scales and mins are unpacked with a few
 * 32-bit operations into 8 scales and 8 mins (16-bit lanes); a byte shuffle
 * then repeats a scale across a vector. Per 64 values: low nibbles x the
 * first 32 activations, high nibbles x the next 32 (unsigned x signed bytes
 * = maddubs), times the sub-block scales (madd). The mins multiply the
 * activations' sums (bsums), kept per block. */
AVX2 static inline __m256i scale_shuf(int i)       /* bytes 2i, 2i+1 repeated */
{
    return _mm256_set1_epi16((short)((2 * i) | ((2 * i + 1) << 8)));
}
AVX2 static float avx2_q4_K(int n, const void *w, const void *xv)
{
    const bq4_K *y = w; const aq8K *x = xv;
    const __m256i m4 = _mm256_set1_epi8(15);
    __m256 acc = _mm256_setzero_ps();
    __m128 accm = _mm_setzero_ps();
    for (int b = 0; b < n / 256; b++) {
        uint32_t u[4];
        memcpy(u, y[b].sc, 12);
        u[3] = ((u[2] >> 4) & 0x0f0f0f0f) | (((u[1] >> 6) & 0x03030303) << 4);   /* mins 4..7 */
        uint32_t t = u[1] & 0x3f3f3f3f;                                         /* mins 0..3 */
        u[1] = (u[2] & 0x0f0f0f0f) | (((u[0] >> 6) & 0x03030303) << 4);         /* scales 4..7 */
        u[2] = t;
        u[0] &= 0x3f3f3f3f;                                                     /* scales 0..3 */
        __m256i sm = _mm256_cvtepu8_epi16(_mm_set_epi32(u[3], u[2], u[1], u[0])); /* 8 scales, 8 mins */
        __m256i bs = _mm256_loadu_si256((const __m256i *)x[b].bsums);
        __m128i s8 = _mm_hadd_epi16(_mm256_castsi256_si128(bs), _mm256_extracti128_si256(bs, 1));
        __m128i pm = _mm_madd_epi16(_mm256_extracti128_si256(sm, 1), s8);
        accm = _mm_fmadd_ps(_mm_set1_ps(-H(y[b].dmin) * x[b].d), _mm_cvtepi32_ps(pm), accm);
        __m128i sc = _mm256_castsi256_si128(sm);
        __m256i scales = _mm256_set_m128i(sc, sc);
        __m256i s1 = _mm256_setzero_si256(), s2 = _mm256_setzero_si256();
        const uint8_t *q = y[b].qs; const int8_t *xq = x[b].qs;
        for (int j = 0; j < 4; j++, q += 32, xq += 64) {
            __m256i q4 = _mm256_loadu_si256((const __m256i *)q);
            __m256i pl = _mm256_maddubs_epi16(_mm256_and_si256(q4, m4), _mm256_loadu_si256((const __m256i *)xq));
            __m256i ph = _mm256_maddubs_epi16(_mm256_and_si256(_mm256_srli_epi16(q4, 4), m4), _mm256_loadu_si256((const __m256i *)(xq + 32)));
            s1 = _mm256_add_epi32(s1, _mm256_madd_epi16(_mm256_shuffle_epi8(scales, scale_shuf(2 * j)), pl));
            s2 = _mm256_add_epi32(s2, _mm256_madd_epi16(_mm256_shuffle_epi8(scales, scale_shuf(2 * j + 1)), ph));
        }
        acc = _mm256_fmadd_ps(_mm256_set1_ps(H(y[b].d) * x[b].d), _mm256_cvtepi32_ps(_mm256_add_epi32(s1, s2)), acc);
    }
    __m128 r = _mm_add_ps(accm, _mm_movehl_ps(accm, accm));
    r = _mm_add_ss(r, _mm_movehdup_ps(r));
    return hsum8(acc) + _mm_cvtss_f32(r);
}

/* Q6_K: 6-bit values (0..63) rebuilt from 4 low bits (ql) and 2 high bits
 * (qh); sum (q-32)*x = sum q*x - 32*sum x, the sums of x from bsums. The 16
 * signed scales: per group of 16 values; shuffled into place per 32. */
AVX2 static float avx2_q6_K(int n, const void *w, const void *xv)
{
    const bq6_K *y = w; const aq8K *x = xv;
    __m256 acc = _mm256_setzero_ps();
    const __m256i m4 = _mm256_set1_epi8(15), m3 = _mm256_set1_epi8(3);
    for (int b = 0; b < n / 256; b++) {
        __m256i sc16 = _mm256_cvtepi8_epi16(_mm_loadu_si128((const __m128i *)y[b].sc));     /* 16 scales */
        __m256i bias = _mm256_madd_epi16(sc16, _mm256_loadu_si256((const __m256i *)x[b].bsums));
        __m256i sumi = _mm256_slli_epi32(bias, 5);                                          /* 32*bias, subtracted below */
        sumi = _mm256_sub_epi32(_mm256_setzero_si256(), sumi);
        for (int h = 0; h < 2; h++) {
            const uint8_t *ql = y[b].ql + h * 64, *qh = y[b].qh + h * 32;
            const int8_t *xq = x[b].qs + h * 128;
            __m128i s8 = _mm_loadl_epi64((const __m128i *)(y[b].sc + h * 8));               /* 8 scales of this half */
            __m256i sc = _mm256_cvtepi8_epi16(_mm_unpacklo_epi64(s8, s8));                  /* as 16-bit, twice */
            __m256i l0 = _mm256_loadu_si256((const __m256i *)ql), l1 = _mm256_loadu_si256((const __m256i *)(ql + 32));
            __m256i hh = _mm256_loadu_si256((const __m256i *)qh);
            __m256i v[4] = {
                _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(hh, m3), 4)),
                _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hh, 2), m3), 4)),
                _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hh, 4), m3), 4)),
                _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hh, 6), m3), 4)),
            };
            for (int k = 0; k < 4; k++) {
                __m256i p = _mm256_maddubs_epi16(v[k], _mm256_loadu_si256((const __m256i *)(xq + 32 * k)));
                /* low 128 bits: scale 2k; high: scale 2k+1 */
                __m256i s = _mm256_shuffle_epi8(sc, _mm256_set_m128i(_mm_set1_epi16((short)((4 * k + 2) | ((4 * k + 3) << 8))),
                                                                     _mm_set1_epi16((short)((4 * k) | ((4 * k + 1) << 8)))));
                sumi = _mm256_add_epi32(sumi, _mm256_madd_epi16(p, s));
            }
        }
        acc = _mm256_fmadd_ps(_mm256_set1_ps(H(y[b].d) * x[b].d), _mm256_cvtepi32_ps(sumi), acc);
    }
    return hsum8(acc);
}

/* ---- Several vectors at once (reading a prompt): each weight block is
 * unpacked once, then multiplied by every vector; the per-vector sums stay
 * in vector registers in memory (L1) until the end of the row.
 * y[b * ys] = dot(row, x + b * xs) for b < B (B <= BMAX). */
AVX2 static void avx2_q4_K_n(int n, const void *w, const void *xv, size_t xs, int B, float *y, size_t ys)
{
    const bq4_K *yb = w;
    const __m256i m4 = _mm256_set1_epi8(15);
    __m256 acc[BMAX]; float accm[BMAX];
    for (int b = 0; b < B; b++) { acc[b] = _mm256_setzero_ps(); accm[b] = 0; }
    for (int i = 0; i < n / 256; i++) {
        uint32_t u[4];
        memcpy(u, yb[i].sc, 12);
        u[3] = ((u[2] >> 4) & 0x0f0f0f0f) | (((u[1] >> 6) & 0x03030303) << 4);
        uint32_t t = u[1] & 0x3f3f3f3f;
        u[1] = (u[2] & 0x0f0f0f0f) | (((u[0] >> 6) & 0x03030303) << 4);
        u[2] = t;
        u[0] &= 0x3f3f3f3f;
        __m256i sm = _mm256_cvtepu8_epi16(_mm_set_epi32(u[3], u[2], u[1], u[0]));
        __m128i mins = _mm256_extracti128_si256(sm, 1), sc = _mm256_castsi256_si128(sm);
        __m256i scales = _mm256_set_m128i(sc, sc), q[8], sv[8];
        for (int j = 0; j < 4; j++) {
            __m256i q4 = _mm256_loadu_si256((const __m256i *)(yb[i].qs + 32 * j));
            q[2 * j] = _mm256_and_si256(q4, m4);
            q[2 * j + 1] = _mm256_and_si256(_mm256_srli_epi16(q4, 4), m4);
            sv[2 * j] = _mm256_shuffle_epi8(scales, scale_shuf(2 * j));
            sv[2 * j + 1] = _mm256_shuffle_epi8(scales, scale_shuf(2 * j + 1));
        }
        float d = H(yb[i].d), dmin = H(yb[i].dmin);
        for (int b = 0; b < B; b++) {
            const aq8K *x = (const aq8K *)((const uint8_t *)xv + b * xs) + i;
            __m256i bs = _mm256_loadu_si256((const __m256i *)x->bsums);
            __m128i s8 = _mm_hadd_epi16(_mm256_castsi256_si128(bs), _mm256_extracti128_si256(bs, 1));
            __m128i pm = _mm_madd_epi16(mins, s8);
            pm = _mm_add_epi32(pm, _mm_shuffle_epi32(pm, 0x4E));
            pm = _mm_add_epi32(pm, _mm_shuffle_epi32(pm, 0xB1));
            accm[b] -= dmin * x->d * _mm_cvtsi128_si32(pm);
            __m256i s1 = _mm256_setzero_si256(), s2 = _mm256_setzero_si256();
            for (int j = 0; j < 4; j++) {
                s1 = _mm256_add_epi32(s1, _mm256_madd_epi16(sv[2 * j], _mm256_maddubs_epi16(q[2 * j], _mm256_loadu_si256((const __m256i *)(x->qs + 64 * j)))));
                s2 = _mm256_add_epi32(s2, _mm256_madd_epi16(sv[2 * j + 1], _mm256_maddubs_epi16(q[2 * j + 1], _mm256_loadu_si256((const __m256i *)(x->qs + 64 * j + 32)))));
            }
            acc[b] = _mm256_fmadd_ps(_mm256_set1_ps(d * x->d), _mm256_cvtepi32_ps(_mm256_add_epi32(s1, s2)), acc[b]);
        }
    }
    for (int b = 0; b < B; b++) y[b * ys] = hsum8(acc[b]) + accm[b];
}

AVX2 static void avx2_q6_K_n(int n, const void *w, const void *xv, size_t xs, int B, float *y, size_t ys)
{
    const bq6_K *yb = w;
    const __m256i m4 = _mm256_set1_epi8(15), m3 = _mm256_set1_epi8(3);
    __m256 acc[BMAX];
    for (int b = 0; b < B; b++) acc[b] = _mm256_setzero_ps();
    for (int i = 0; i < n / 256; i++) {
        __m256i v[8], sv[8];
        for (int h = 0; h < 2; h++) {
            const uint8_t *ql = yb[i].ql + h * 64, *qh = yb[i].qh + h * 32;
            __m128i s8 = _mm_loadl_epi64((const __m128i *)(yb[i].sc + h * 8));
            __m256i sc = _mm256_cvtepi8_epi16(_mm_unpacklo_epi64(s8, s8));
            __m256i l0 = _mm256_loadu_si256((const __m256i *)ql), l1 = _mm256_loadu_si256((const __m256i *)(ql + 32));
            __m256i hh = _mm256_loadu_si256((const __m256i *)qh);
            v[4 * h + 0] = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(hh, m3), 4));
            v[4 * h + 1] = _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hh, 2), m3), 4));
            v[4 * h + 2] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hh, 4), m3), 4));
            v[4 * h + 3] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hh, 6), m3), 4));
            for (int k = 0; k < 4; k++)
                sv[4 * h + k] = _mm256_shuffle_epi8(sc, _mm256_set_m128i(_mm_set1_epi16((short)((4 * k + 2) | ((4 * k + 3) << 8))),
                                                                         _mm_set1_epi16((short)((4 * k) | ((4 * k + 1) << 8)))));
        }
        __m256i sc16 = _mm256_cvtepi8_epi16(_mm_loadu_si128((const __m128i *)yb[i].sc));
        float d = H(yb[i].d);
        for (int b = 0; b < B; b++) {
            const aq8K *x = (const aq8K *)((const uint8_t *)xv + b * xs) + i;
            __m256i sumi = _mm256_sub_epi32(_mm256_setzero_si256(),
                           _mm256_slli_epi32(_mm256_madd_epi16(sc16, _mm256_loadu_si256((const __m256i *)x->bsums)), 5));
            for (int k = 0; k < 8; k++)
                sumi = _mm256_add_epi32(sumi, _mm256_madd_epi16(_mm256_maddubs_epi16(v[k], _mm256_loadu_si256((const __m256i *)(x->qs + 32 * k))), sv[k]));
            acc[b] = _mm256_fmadd_ps(_mm256_set1_ps(d * x->d), _mm256_cvtepi32_ps(sumi), acc[b]);
        }
    }
    for (int b = 0; b < B; b++) y[b * ys] = hsum8(acc[b]);
}

AVX2 static float avx2_f16(int n, const void *w, const void *xv)
{
    const f16 *a = w; const float *x = xv;
    __m256 acc = _mm256_setzero_ps(), acc2 = _mm256_setzero_ps();
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        acc = _mm256_fmadd_ps(_mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(a + i))), _mm256_loadu_ps(x + i), acc);
        acc2 = _mm256_fmadd_ps(_mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(a + i + 8))), _mm256_loadu_ps(x + i + 8), acc2);
    }
    float s = hsum8(_mm256_add_ps(acc, acc2));
    for (; i < n; i++) s += H(a[i]) * x[i];
    return s;
}
AVX2 static float avx2_f32(int n, const void *w, const void *xv)
{
    const float *a = w, *x = xv;
    __m256 acc = _mm256_setzero_ps(), acc2 = _mm256_setzero_ps();
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(x + i), acc);
        acc2 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(x + i + 8), acc2);
    }
    float s = hsum8(_mm256_add_ps(acc, acc2));
    for (; i < n; i++) s += a[i] * x[i];
    return s;
}

AVX2 static float a_dot_f16(int n, const float *a, const f16 *b)
{
    __m256 acc = _mm256_setzero_ps(); int i = 0;
    for (; i + 8 <= n; i += 8) acc = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(b + i))), acc);
    float s = hsum8(acc);
    for (; i < n; i++) s += a[i] * H(b[i]);
    return s;
}
AVX2 static void a_axpy_f16(int n, float a, const f16 *x, float *y)
{
    __m256 va = _mm256_set1_ps(a); int i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(y + i, _mm256_fmadd_ps(va, _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(x + i))), _mm256_loadu_ps(y + i)));
    for (; i < n; i++) y[i] += a * H(x[i]);
}
AVX2 static void a_to_f16(int n, const float *x, f16 *y)
{
    int i = 0;
    for (; i + 8 <= n; i += 8) _mm_storeu_si128((__m128i *)(y + i), _mm256_cvtps_ph(_mm256_loadu_ps(x + i), 0));
    for (; i < n; i++) y[i] = HF(x[i]);
}

static int has_avx2, has_vnni;
static void cpu_detect(void)
{
    unsigned a, b, c, d;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    int fma = c >> 12 & 1, f16c = c >> 29 & 1, osx = c >> 27 & 1;
    unsigned lo = 0, hi = 0;
    if (osx) __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));   /* does the OS save the AVX state? */
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    int avx2 = b >> 5 & 1;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(1));
    has_avx2 = avx2 && fma && f16c && (lo & 6) == 6;
    has_vnni = has_avx2 && (a >> 4 & 1);
}
#else
static int has_avx2, has_vnni;
static void cpu_detect(void) {}
#endif

int cpu_dotprod;
const char *llm_kernel_name(int k)
{
    static const char *n[] = { "auto", "scalar", "avx2", "avx2+vnni", "neon", "neon+dot" };
    return k >= 0 && k < 6 ? n[k] : "?";
}

int pick_kernels(int want)
{
    cpu_detect();
#if defined(__aarch64__)
    /* every AArch64 CPU has NEON; sdot (ARMv8.2) only if the system said so:
     * user mode cannot read the CPU's ID registers itself */
    int k = want == LLM_K_SCALAR ? LLM_K_SCALAR : want == LLM_K_NEON ? LLM_K_NEON
          : want == LLM_K_DOT || cpu_dotprod ? LLM_K_DOT : LLM_K_NEON;
    const neon_kernels_t *nk = k == LLM_K_DOT ? &neon_sdot : &neon_plain;
    if (k != LLM_K_SCALAR) { p_dot_f16 = nk->dot_f16; p_axpy_f16 = nk->axpy_f16; p_to_f16 = nk->to_f16; }
    else { p_dot_f16 = s_dot_f16; p_axpy_f16 = s_axpy_f16; p_to_f16 = s_to_f16; }
    return k;
#else
    int k = want == LLM_K_SCALAR ? LLM_K_SCALAR : want == LLM_K_AVX2 ? (has_avx2 ? LLM_K_AVX2 : LLM_K_SCALAR)
          : has_vnni ? LLM_K_VNNI : has_avx2 ? LLM_K_AVX2 : LLM_K_SCALAR;
#if defined(__x86_64__) && defined(__GNUC__)
    if (k != LLM_K_SCALAR) { p_dot_f16 = a_dot_f16; p_axpy_f16 = a_axpy_f16; p_to_f16 = a_to_f16; }
    else { p_dot_f16 = s_dot_f16; p_axpy_f16 = s_axpy_f16; p_to_f16 = s_to_f16; }
#endif
    return k;
#endif
}

void set_dot(mat_t *m, int k)
{
    static float (*const ref[T_NTYPES])(int, const void *, const void *) = {
        [T_F32] = d_f32, [T_F16] = d_f16, [T_BF16] = d_bf16, [T_Q4_0] = d_q4_0, [T_Q4_1] = d_q4_1,
        [T_Q5_0] = d_q5_0, [T_Q5_1] = d_q5_1, [T_Q8_0] = d_q8_0, [T_Q4_K] = d_q4_K, [T_Q5_K] = d_q5_K, [T_Q6_K] = d_q6_K,
    };
    m->dot = ref[m->type];
    m->dotn = 0;
#if defined(__x86_64__) && defined(__GNUC__)
    if (k == LLM_K_SCALAR) return;
    switch (m->type) {
    case T_F32:  m->dot = avx2_f32; break;
    case T_F16:  m->dot = avx2_f16; break;
    case T_Q8_0: m->dot = k == LLM_K_VNNI ? vnni_q8_0 : avx2_q8_0; break;
    case T_Q4_0: m->dot = k == LLM_K_VNNI ? vnni_q4_0 : avx2_q4_0; break;
    case T_Q5_0: m->dot = k == LLM_K_VNNI ? vnni_q5_0 : avx2_q5_0; break;
    case T_Q4_K: m->dot = avx2_q4_K; m->dotn = avx2_q4_K_n; break;
    case T_Q6_K: m->dot = avx2_q6_K; m->dotn = avx2_q6_K_n; break;
    }
#elif defined(__aarch64__)
    if (k == LLM_K_SCALAR) return;
    const neon_kernels_t *nk = k == LLM_K_DOT ? &neon_sdot : &neon_plain;
    switch (m->type) {
    case T_F32:  m->dot = nk->f32; break;
    case T_F16:  m->dot = nk->f16; break;
    case T_Q8_0: m->dot = nk->q8_0; break;
    case T_Q4_0: m->dot = nk->q4_0; break;
    case T_Q4_K: m->dot = nk->q4_K; m->dotn = nk->q4_K_n; break;
    case T_Q6_K: m->dot = nk->q6_K; m->dotn = nk->q6_K_n; break;
    }
#else
    (void)k;
#endif
}
