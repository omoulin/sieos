/*
 * neon.c - The AArch64 NEON kernels of the engine (built by sicc, which has
 * arm_neon.h). This file is compiled twice:
 *   - as it is: plain NEON, which every AArch64 CPU has (the Pi 4's Cortex-A72);
 *   - by neon_dot.c with NEON_SDOT, for -march=armv8.2-a+dotprod: the byte dot
 *     products use sdot (the Pi 5's Cortex-A76), four times fewer instructions.
 * quant.c picks one at load time (pick_kernels; the system tells it whether
 * the CPU has sdot: llm_env_t.cpu_dotprod).
 *
 * The integer kernels compute exactly what the plain C ones do (quant.c);
 * only the order of the final float additions differs.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#if defined(__aarch64__)
#include <arm_neon.h>
#include "internal.h"

#ifdef NEON_SDOT
#define NK(x) sdot_##x
#define TABLE neon_sdot
/* acc (4 x int32) += the dot products of a and b, 4 bytes at a time */
#define DOT16(acc, a, b) vdotq_s32(acc, a, b)
#else
#define NK(x) plain_##x
#define TABLE neon_plain
/* without sdot: 16-bit products of the low and high halves (two at most
 * 2 x 127 x 128, which fits), then pairs added into the 32-bit lanes */
static inline int32x4_t DOT16(int32x4_t acc, int8x16_t a, int8x16_t b)
{
    int16x8_t p = vmull_s8(vget_low_s8(a), vget_low_s8(b));
    return vpadalq_s16(acc, vmlal_high_s8(p, a, b));
}
#endif

#define S8(u) vreinterpretq_s8_u8(u)
#define F16(h) f16_to_f32(h)

static float NK(q8_0)(int n, const void *w, const void *xv)
{
    const bq8_0 *y = w; const aq8 *x = xv;
    float32x4_t acc = vdupq_n_f32(0);
    for (int b = 0; b < n / 32; b++) {
        int32x4_t s = DOT16(vdupq_n_s32(0), vld1q_s8(y[b].qs), vld1q_s8(x[b].qs));
        s = DOT16(s, vld1q_s8(y[b].qs + 16), vld1q_s8(x[b].qs + 16));
        acc = vfmaq_n_f32(acc, vcvtq_f32_s32(s), F16(y[b].d) * x[b].d);
    }
    return vaddvq_f32(acc);
}

/* Q4_0: 16 bytes of nibbles; the low ones go with x[0..15], the high ones with x[16..31] */
static float NK(q4_0)(int n, const void *w, const void *xv)
{
    const bq4_0 *y = w; const aq8 *x = xv;
    const uint8x16_t m4 = vdupq_n_u8(15);
    const int8x16_t eight = vdupq_n_s8(8);
    float32x4_t acc = vdupq_n_f32(0);
    for (int b = 0; b < n / 32; b++) {
        uint8x16_t q = vld1q_u8(y[b].qs);
        int32x4_t s = DOT16(vdupq_n_s32(0), vsubq_s8(S8(vandq_u8(q, m4)), eight), vld1q_s8(x[b].qs));
        s = DOT16(s, vsubq_s8(S8(vshrq_n_u8(q, 4)), eight), vld1q_s8(x[b].qs + 16));
        acc = vfmaq_n_f32(acc, vcvtq_f32_s32(s), F16(y[b].d) * x[b].d);
    }
    return vaddvq_f32(acc);
}

/* Q4_K's 12 bytes of 6-bit scales and mins -> 8 scales (sm[0..7]) and 8 mins (sm[8..15]) */
static void q4K_scales(const uint8_t *p, uint8_t sm[16])
{
    uint32_t u[4];
    memcpy(u, p, 12);
    u[3] = ((u[2] >> 4) & 0x0f0f0f0f) | (((u[1] >> 6) & 0x03030303) << 4);   /* mins 4..7 */
    uint32_t t = u[1] & 0x3f3f3f3f;                                         /* mins 0..3 */
    u[1] = (u[2] & 0x0f0f0f0f) | (((u[0] >> 6) & 0x03030303) << 4);         /* scales 4..7 */
    u[2] = t;
    u[0] &= 0x3f3f3f3f;                                                     /* scales 0..3 */
    memcpy(sm, u, 16);
}

/* Q4_K: per 64 values, low nibbles x the first 32 activations and high
 * nibbles x the next 32, each times its sub-block's scale; the mins multiply
 * the activations' sums (bsums). */
static float NK(q4_K)(int n, const void *w, const void *xv)
{
    const bq4_K *y = w; const aq8K *x = xv;
    const uint8x16_t m4 = vdupq_n_u8(15);
    const int32x4_t z = vdupq_n_s32(0);
    float s = 0;
    for (int b = 0; b < n / 256; b++) {
        uint8_t sm[16];
        q4K_scales(y[b].sc, sm);
        int summ = 0;
        for (int j = 0; j < 8; j++) summ += sm[8 + j] * (x[b].bsums[2 * j] + x[b].bsums[2 * j + 1]);
        int32x4_t isum = z;
        const uint8_t *q = y[b].qs; const int8_t *xq = x[b].qs;
        for (int j = 0; j < 4; j++, q += 32, xq += 64) {
            uint8x16_t q0 = vld1q_u8(q), q1 = vld1q_u8(q + 16);
            int32x4_t lo = DOT16(DOT16(z, S8(vandq_u8(q0, m4)), vld1q_s8(xq)), S8(vandq_u8(q1, m4)), vld1q_s8(xq + 16));
            int32x4_t hi = DOT16(DOT16(z, S8(vshrq_n_u8(q0, 4)), vld1q_s8(xq + 32)), S8(vshrq_n_u8(q1, 4)), vld1q_s8(xq + 48));
            isum = vmlaq_s32(isum, lo, vdupq_n_s32(sm[2 * j]));
            isum = vmlaq_s32(isum, hi, vdupq_n_s32(sm[2 * j + 1]));
        }
        s += x[b].d * (F16(y[b].d) * vaddvq_s32(isum) - F16(y[b].dmin) * summ);
    }
    return s;
}

/* Q6_K: 6-bit values rebuilt from 4 low bits (ql) and 2 high bits (qh),
 * minus 32 (signed bytes); 16 scales, one per 16 values. v[k]: the 32 values
 * that go with x[32k..32k+31] of this half (scales 2k, 2k+1 per 16). */
static void q6K_values(const uint8_t *ql, const uint8_t *qh, int8x16_t v[8])
{
    const uint8x16_t m4 = vdupq_n_u8(15), m3 = vdupq_n_u8(3);
    const int8x16_t k32 = vdupq_n_s8(32);
    for (int i = 0; i < 2; i++) {                 /* the two 16-byte halves of each 32 */
        uint8x16_t l0 = vld1q_u8(ql + 16 * i), l1 = vld1q_u8(ql + 32 + 16 * i), h = vld1q_u8(qh + 16 * i);
        v[0 + i] = vsubq_s8(S8(vorrq_u8(vandq_u8(l0, m4), vshlq_n_u8(vandq_u8(h, m3), 4))), k32);
        v[2 + i] = vsubq_s8(S8(vorrq_u8(vandq_u8(l1, m4), vshlq_n_u8(vandq_u8(vshrq_n_u8(h, 2), m3), 4))), k32);
        v[4 + i] = vsubq_s8(S8(vorrq_u8(vshrq_n_u8(l0, 4), vshlq_n_u8(vandq_u8(vshrq_n_u8(h, 4), m3), 4))), k32);
        v[6 + i] = vsubq_s8(S8(vorrq_u8(vshrq_n_u8(l1, 4), vshlq_n_u8(vshrq_n_u8(h, 6), 4))), k32);
    }
}

static float NK(q6_K)(int n, const void *w, const void *xv)
{
    const bq6_K *y = w; const aq8K *x = xv;
    const int32x4_t z = vdupq_n_s32(0);
    float s = 0;
    for (int b = 0; b < n / 256; b++) {
        int32x4_t isum = z;
        for (int h = 0; h < 2; h++) {
            int8x16_t v[8];
            q6K_values(y[b].ql + 64 * h, y[b].qh + 32 * h, v);
            const int8_t *sc = y[b].sc + 8 * h, *xq = x[b].qs + 128 * h;
            for (int k = 0; k < 8; k++)           /* 16 values each: scale 2*(k/2) + k%2 */
                isum = vmlaq_s32(isum, DOT16(z, v[k], vld1q_s8(xq + 16 * k)), vdupq_n_s32(sc[k]));
        }
        s += F16(y[b].d) * x[b].d * vaddvq_s32(isum);
    }
    return s;
}

/* ---- Several vectors at once (a prompt): each weight block is unpacked once */
static void NK(q4_K_n)(int n, const void *w, const void *xv, size_t xs, int B, float *yo, size_t ys)
{
    const bq4_K *yb = w;
    const uint8x16_t m4 = vdupq_n_u8(15);
    const int32x4_t z = vdupq_n_s32(0);
    for (int b = 0; b < B; b++) yo[b * ys] = 0;
    for (int i = 0; i < n / 256; i++) {
        uint8_t sm[16];
        q4K_scales(yb[i].sc, sm);
        int8x16_t q[16];                          /* per 64 values: low 0-15, 16-31, high 0-15, 16-31 */
        for (int j = 0; j < 4; j++) {
            uint8x16_t q0 = vld1q_u8(yb[i].qs + 32 * j), q1 = vld1q_u8(yb[i].qs + 32 * j + 16);
            q[4 * j] = S8(vandq_u8(q0, m4)); q[4 * j + 1] = S8(vandq_u8(q1, m4));
            q[4 * j + 2] = S8(vshrq_n_u8(q0, 4)); q[4 * j + 3] = S8(vshrq_n_u8(q1, 4));
        }
        float d = F16(yb[i].d), dmin = F16(yb[i].dmin);
        for (int b = 0; b < B; b++) {
            const aq8K *x = (const aq8K *)((const uint8_t *)xv + b * xs) + i;
            int summ = 0;
            for (int j = 0; j < 8; j++) summ += sm[8 + j] * (x->bsums[2 * j] + x->bsums[2 * j + 1]);
            int32x4_t isum = z;
            for (int j = 0; j < 4; j++) {
                const int8_t *xq = x->qs + 64 * j;
                int32x4_t lo = DOT16(DOT16(z, q[4 * j], vld1q_s8(xq)), q[4 * j + 1], vld1q_s8(xq + 16));
                int32x4_t hi = DOT16(DOT16(z, q[4 * j + 2], vld1q_s8(xq + 32)), q[4 * j + 3], vld1q_s8(xq + 48));
                isum = vmlaq_s32(isum, lo, vdupq_n_s32(sm[2 * j]));
                isum = vmlaq_s32(isum, hi, vdupq_n_s32(sm[2 * j + 1]));
            }
            yo[b * ys] += x->d * (d * vaddvq_s32(isum) - dmin * summ);
        }
    }
}

static void NK(q6_K_n)(int n, const void *w, const void *xv, size_t xs, int B, float *yo, size_t ys)
{
    const bq6_K *yb = w;
    const int32x4_t z = vdupq_n_s32(0);
    for (int b = 0; b < B; b++) yo[b * ys] = 0;
    for (int i = 0; i < n / 256; i++) {
        int8x16_t v[16];
        q6K_values(yb[i].ql, yb[i].qh, v);
        q6K_values(yb[i].ql + 64, yb[i].qh + 32, v + 8);
        float d = F16(yb[i].d);
        for (int b = 0; b < B; b++) {
            const aq8K *x = (const aq8K *)((const uint8_t *)xv + b * xs) + i;
            int32x4_t isum = z;
            for (int k = 0; k < 16; k++) isum = vmlaq_s32(isum, DOT16(z, v[k], vld1q_s8(x->qs + 16 * k)), vdupq_n_s32(yb[i].sc[k]));
            yo[b * ys] += d * x->d * vaddvq_s32(isum);
        }
    }
}

/* ---- Floating point: f32 and f16 weights, and the attention's helpers.
 * Half precision converts with fcvtl/fcvtn (round to nearest even). */
static float NK(f32)(int n, const void *w, const void *xv)
{
    const float *a = w, *x = xv;
    float32x4_t s0 = vdupq_n_f32(0), s1 = s0;
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        s0 = vfmaq_f32(s0, vld1q_f32(a + i), vld1q_f32(x + i));
        s1 = vfmaq_f32(s1, vld1q_f32(a + i + 4), vld1q_f32(x + i + 4));
    }
    float s = vaddvq_f32(vaddq_f32(s0, s1));
    for (; i < n; i++) s += a[i] * x[i];
    return s;
}

static float NK(dot_f16)(int n, const float *a, const f16 *b)
{
    float32x4_t s0 = vdupq_n_f32(0), s1 = s0;
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        s0 = vfmaq_f32(s0, vld1q_f32(a + i), vcvt_f32_f16(vld1_f16(b + i)));
        s1 = vfmaq_f32(s1, vld1q_f32(a + i + 4), vcvt_f32_f16(vld1_f16(b + i + 4)));
    }
    float s = vaddvq_f32(vaddq_f32(s0, s1));
    for (; i < n; i++) s += a[i] * F16(b[i]);
    return s;
}
static float NK(f16)(int n, const void *w, const void *xv) { return NK(dot_f16)(n, xv, w); }

static void NK(axpy_f16)(int n, float a, const f16 *x, float *y)   /* y += a*x */
{
    int i = 0;
    for (; i + 4 <= n; i += 4) vst1q_f32(y + i, vfmaq_n_f32(vld1q_f32(y + i), vcvt_f32_f16(vld1_f16(x + i)), a));
    for (; i < n; i++) y[i] += a * F16(x[i]);
}

static void NK(to_f16)(int n, const float *x, f16 *y)
{
    int i = 0;
    for (; i + 4 <= n; i += 4) vst1_f16(y + i, vcvt_f16_f32(vld1q_f32(x + i)));
    for (; i < n; i++) y[i] = f32_to_f16(x[i]);
}

const neon_kernels_t TABLE = {
    NK(q8_0), NK(q4_0), NK(q4_K), NK(q6_K), NK(f16), NK(f32), NK(q4_K_n), NK(q6_K_n),
    NK(dot_f16), NK(axpy_f16), NK(to_f16),
};
#endif
