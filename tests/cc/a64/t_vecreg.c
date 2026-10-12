/* t_vecreg.c - NEON vectors in registers (cc/vecreg.c): each case against
 * the same computation in scalar code. Many vectors alive at once (spills),
 * vectors alive across calls (v8-v15 keep only their low halves), loops with
 * accumulators, halves and combinations, branches, lanes (which keep a vector
 * in memory) mixed with promoted ones, vectors through calls.
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include <arm_neon.h>
int printf(const char *, ...);

static signed char A[256], B[256];
static float FA[64], FB[64];
static int fails;
static void check(int k, long got, long want)
{
    if (got == want) printf("case %d ok\n", k);
    else { printf("case %d: got %ld, want %ld\n", k, got, want); fails++; }
}

/* 1: the engine's kind of dot product (widening multiply, pairwise accumulate) */
static int dot_neon(const signed char *a, const signed char *b, int n)
{
    int32x4_t acc = vdupq_n_s32(0);
    for (int i = 0; i < n; i += 16) {
        int8x16_t x = vld1q_s8(a + i), y = vld1q_s8(b + i);
        int16x8_t p = vmull_s8(vget_low_s8(x), vget_low_s8(y));
        p = vmlal_s8(p, vget_high_s8(x), vget_high_s8(y));
        acc = vpadalq_s16(acc, p);
    }
    return vaddvq_s32(acc);
}

/* 2: more vectors alive than registers */
__attribute__((noinline)) static long many(const signed char *a)
{
    int32x4_t s0 = vdupq_n_s32(0), s1 = s0, s2 = s0, s3 = s0, s4 = s0, s5 = s0, s6 = s0, s7 = s0, s8 = s0, s9 = s0,
              s10 = s0, s11 = s0, s12 = s0, s13 = s0, s14 = s0, s15 = s0, s16 = s0, s17 = s0, s18 = s0, s19 = s0,
              s20 = s0, s21 = s0, s22 = s0, s23 = s0, s24 = s0, s25 = s0;
    for (int r = 0; r < 4; r++) {
        int32x4_t x = vmovl_s16(vget_low_s16(vmovl_s8(vld1_s8(a + 8 * r))));
        s0 = vaddq_s32(s0, x); s1 = vsubq_s32(s1, x); s2 = vaddq_s32(s2, vaddq_s32(x, x)); s3 = vmulq_s32(vaddq_s32(s3, x), vdupq_n_s32(2));
        s4 = vaddq_s32(s4, vdupq_n_s32(r)); s5 = vmaxq_s32(s5, x); s6 = vminq_s32(s6, x); s7 = vaddq_s32(s7, s0);
        s8 = vaddq_s32(s8, s1); s9 = vaddq_s32(s9, s2); s10 = veorq_s32(s10, x); s11 = vorrq_s32(s11, x);
        s12 = vandq_s32(vaddq_s32(s12, x), vdupq_n_s32(1023)); s13 = vaddq_s32(s13, s7); s14 = vsubq_s32(s14, s8);
        s15 = vaddq_s32(s15, s9); s16 = vaddq_s32(s16, s10); s17 = vaddq_s32(s17, s11); s18 = vaddq_s32(s18, s12);
        s19 = vaddq_s32(s19, s13); s20 = vaddq_s32(s20, s14); s21 = vaddq_s32(s21, s15); s22 = vaddq_s32(s22, s16);
        s23 = vaddq_s32(s23, s17); s24 = vaddq_s32(s24, s18); s25 = vaddq_s32(s25, vaddq_s32(s19, s20));
    }
    int32x4_t t = vaddq_s32(vaddq_s32(vaddq_s32(s0, s1), vaddq_s32(s2, s3)), vaddq_s32(vaddq_s32(s4, s5), vaddq_s32(s6, s7)));
    t = vaddq_s32(t, vaddq_s32(vaddq_s32(vaddq_s32(s8, s9), vaddq_s32(s10, s11)), vaddq_s32(vaddq_s32(s12, s13), vaddq_s32(s14, s15))));
    t = vaddq_s32(t, vaddq_s32(vaddq_s32(vaddq_s32(s16, s17), vaddq_s32(s18, s19)), vaddq_s32(vaddq_s32(s20, s21), vaddq_s32(s22, s23))));
    t = vaddq_s32(t, vaddq_s32(s24, s25));
    return vaddvq_s32(t);
}
static long many_scalar(const signed char *a)
{
    long s[26][4] = { { 0 } }, t = 0;
    for (int r = 0; r < 4; r++)
        for (int l = 0; l < 4; l++) {
            long x = a[8 * r + l];
            s[0][l] += x; s[1][l] -= x; s[2][l] += 2 * x; s[3][l] = (s[3][l] + x) * 2; s[4][l] += r;
            s[5][l] = s[5][l] > x ? s[5][l] : x; s[6][l] = s[6][l] < x ? s[6][l] : x; s[7][l] += s[0][l];
            s[8][l] += s[1][l]; s[9][l] += s[2][l]; s[10][l] = (int)s[10][l] ^ (int)x; s[11][l] = (int)s[11][l] | (int)x;
            s[12][l] = (s[12][l] + x) & 1023; s[13][l] += s[7][l]; s[14][l] -= s[8][l]; s[15][l] += s[9][l];
            s[16][l] += s[10][l]; s[17][l] += s[11][l]; s[18][l] += s[12][l]; s[19][l] += s[13][l]; s[20][l] += s[14][l];
            s[21][l] += s[15][l]; s[22][l] += s[16][l]; s[23][l] += s[17][l]; s[24][l] += s[18][l]; s[25][l] += s[19][l] + s[20][l];
        }
    for (int k = 0; k < 26; k++) for (int l = 0; l < 4; l++) t += (int)s[k][l];
    return (int)t;
}

/* 3: alive across calls that use vector registers themselves */
__attribute__((noinline)) static int32x4_t scramble(int32x4_t v)
{
    float32x4_t f = vcvtq_f32_s32(v);
    f = vmulq_f32(f, vdupq_n_f32(3.0f));
    return vcvtq_s32_f32(vaddq_f32(f, vdupq_n_f32(1.0f)));     /* 3v + 1 */
}
__attribute__((noinline)) static long across(void)
{
    int32x4_t keep1 = vdupq_n_s32(7), keep2 = { 1, 2, 3, 4 }, x = { 10, 20, 30, 40 };
    for (int k = 0; k < 3; k++) {
        x = scramble(x);
        keep1 = vaddq_s32(keep1, vdupq_n_s32(k));
        keep2 = vaddq_s32(keep2, keep1);
    }
    return vaddvq_s32(x) + vaddvq_s32(keep1) * 1000 + vaddvq_s32(keep2) * 100000L;
}
static long across_scalar(void)
{
    long x[4] = { 10, 20, 30, 40 }, k1 = 7, k2[4] = { 1, 2, 3, 4 }, s = 0, s2 = 0;
    for (int k = 0; k < 3; k++) {
        for (int l = 0; l < 4; l++) x[l] = 3 * x[l] + 1;
        k1 += k;
        for (int l = 0; l < 4; l++) k2[l] += k1;
    }
    for (int l = 0; l < 4; l++) { s += x[l]; s2 += k2[l]; }
    return s + 4 * k1 * 1000 + s2 * 100000L;
}

/* 4: halves, combinations, lanes (memory) mixed with registers */
static long halves(void)
{
    int8x16_t a = vld1q_s8(A), b = vld1q_s8(B);
    int8x16_t swapped = vcombine_s8(vget_high_s8(a), vget_low_s8(a));
    int8x16_t mixed = vcombine_s8(vget_low_s8(b), vget_high_s8(swapped));
    int8x16_t sum = vaddq_s8(swapped, mixed);
    long t = 0;
    for (int l = 0; l < 16; l++) t = t * 3 + sum[l];      /* lanes: through memory */
    return t + vgetq_lane_s8(a, 5) * 7;
}
static long halves_scalar(void)
{
    signed char sw[16], mx[16];
    long t = 0;
    for (int l = 0; l < 8; l++) { sw[l] = A[8 + l]; sw[8 + l] = A[l]; mx[l] = B[l]; }
    for (int l = 0; l < 8; l++) mx[8 + l] = sw[8 + l];
    for (int l = 0; l < 16; l++) t = t * 3 + (signed char)(sw[l] + mx[l]);
    return t + A[5] * 7;
}

/* 5: floating accumulators (fused multiply-add), branches inside the loop */
static long fmas(void)
{
    float32x4_t s0 = vdupq_n_f32(0), s1 = s0;
    for (int i = 0; i < 64; i += 4) {
        float32x4_t a = vld1q_f32(FA + i), b = vld1q_f32(FB + i);
        if (i & 4) s0 = vfmaq_f32(s0, a, b);
        else s1 = vfmaq_n_f32(s1, a, 2.0f);
    }
    float32x4_t s = vaddq_f32(s0, s1);
    return (long)vaddvq_f32(s);
}
static long fmas_scalar(void)
{
    float s0[4] = { 0 }, s1[4] = { 0 }, t = 0;
    for (int i = 0; i < 64; i += 4)
        for (int l = 0; l < 4; l++) { if (i & 4) s0[l] += FA[i + l] * FB[i + l]; else s1[l] += FA[i + l] * 2.0f; }
    float s[4];
    for (int l = 0; l < 4; l++) s[l] = s0[l] + s1[l];
    t = (s[0] + s[1]) + (s[2] + s[3]);
    return (long)t;
}

/* 6: select (bsl reads its destination), 8-byte vectors, through a call by value */
__attribute__((noinline)) static int16x4_t twice(int16x4_t v, int16x4_t w) { return vadd_s16(vadd_s16(v, v), w); }
__attribute__((noinline)) static long selects(void)
{
    uint8x16_t m = vcgtq_s8(vld1q_s8(A), vld1q_s8(B));
    int8x16_t r = vbslq_s8(m, vld1q_s8(A), vld1q_s8(B));    /* max */
    int16x4_t h = twice(vget_low_s16(vmovl_s8(vget_low_s8(r))), vdup_n_s16(5));
    long t = 0;
    for (int l = 0; l < 16; l++) t = t * 5 + r[l];
    for (int l = 0; l < 4; l++) t = t * 7 + h[l];
    return t;
}
static long selects_scalar(void)
{
    signed char r[16];
    long t = 0;
    for (int l = 0; l < 16; l++) r[l] = A[l] > B[l] ? A[l] : B[l];
    for (int l = 0; l < 16; l++) t = t * 5 + r[l];
    for (int l = 0; l < 4; l++) t = t * 7 + (short)(2 * r[l] + 5);
    return t;
}

int main(void)
{
    unsigned s = 12345;
    for (int i = 0; i < 256; i++) { s = s * 1103515245 + 12345; A[i] = s >> 16; s = s * 1103515245 + 12345; B[i] = s >> 16; }
    for (int i = 0; i < 64; i++) { FA[i] = (float)((i * 7) % 13 - 6); FB[i] = (float)((i * 5) % 11 - 5); }
    long want = 0;
    for (int i = 0; i < 256; i++) want += A[i] * B[i];
    check(1, dot_neon(A, B, 256), want);
    check(2, many(A), many_scalar(A));
    check(3, across(), across_scalar());
    check(4, halves(), halves_scalar());
    check(5, fmas(), fmas_scalar());
    check(6, selects(), selects_scalar());
    return fails;
}
