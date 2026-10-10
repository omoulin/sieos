/* t_neon.c - arm_neon.h on AArch64: each intrinsic checked against plain C
 * on pseudo-random data (prints "name ok" per group, the .expect file).
 * Also: vectors passed and returned by value in v registers (AAPCS64).
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include <arm_neon.h>
int printf(const char *, ...);

static unsigned seed = 12345;
static unsigned rnd(void) { seed = seed * 1103515245 + 12345; return seed >> 8; }
static int bad;
static void check(const char *name, int ok) { printf("%s %s\n", name, ok ? "ok" : "FAIL"); bad |= !ok; }
static int near(float x, double r) { double d = x - r; return (d < 0 ? -d : d) <= (r < 0 ? -r : r) * 1e-7 + 1e-30; }   /* within one rounding */
#define FILL(a, n, expr) for (int k_ = 0; k_ < (n); k_++) a[k_] = (expr)

struct pair { float32x4_t a, b; };                   /* an HVA: two vectors in v0, v1 */
__attribute__((noinline)) static struct pair swap(struct pair p, int32x4_t k) { struct pair r = { p.b + vcvtq_f32_s32(k), p.a }; return r; }
__attribute__((noinline)) static uint8x16_t addb(uint8x16_t a, uint8x8_t lo, uint8x16_t b) { return a + b + vcombine_u8(lo, lo); }

int main(void)
{
    signed char s8a[16], s8b[16]; unsigned char u8a[16], u8b[16];
    short s16a[8], s16b[8]; int s32a[4], s32b[4]; float fa[4], fb[4], fc[4];
    FILL(s8a, 16, (signed char)rnd()); FILL(s8b, 16, (signed char)rnd());
    FILL(u8a, 16, (unsigned char)rnd()); FILL(u8b, 16, (unsigned char)rnd());
    FILL(s16a, 8, (short)rnd()); FILL(s16b, 8, (short)rnd());
    FILL(s32a, 4, (int)rnd() - 0x400000); FILL(s32b, 4, (int)(rnd() & 31));
    FILL(fa, 4, (float)(rnd() % 1000) / 7); FILL(fb, 4, (float)(rnd() % 1000) / 9); FILL(fc, 4, (float)(rnd() % 1000) / 11);
    int8x16_t A8 = vld1q_s8(s8a), B8 = vld1q_s8(s8b);
    uint8x16_t UA = vld1q_u8(u8a), UB = vld1q_u8(u8b);
    int16x8_t A16 = vld1q_s16(s16a), B16 = vld1q_s16(s16b);
    int32x4_t A32 = vld1q_s32(s32a), B32 = vld1q_s32(s32b);
    float32x4_t FA = vld1q_f32(fa), FB = vld1q_f32(fb), FC = vld1q_f32(fc);
    int ok;

    ok = 1; { int8x16_t r = vaddq_s8(A8, B8), s = vsubq_s8(A8, B8), m = vmulq_s8(A8, B8);
      for (int k = 0; k < 16; k++) ok &= r[k] == (signed char)(s8a[k] + s8b[k]) && s[k] == (signed char)(s8a[k] - s8b[k]) && m[k] == (signed char)(s8a[k] * s8b[k]); }
    check("add/sub/mul s8", ok);
    ok = 1; { int32x4_t r = vmlaq_s32(A32, A32, B32), m = vmaxq_s32(A32, B32), n = vminq_s32(A32, B32), x = vabsq_s32(A32);
      for (int k = 0; k < 4; k++) ok &= r[k] == s32a[k] + s32a[k] * s32b[k] && m[k] == (s32a[k] > s32b[k] ? s32a[k] : s32b[k]) &&
                                     n[k] == (s32a[k] < s32b[k] ? s32a[k] : s32b[k]) && x[k] == (s32a[k] < 0 ? -s32a[k] : s32a[k]); }
    check("mla/max/min/abs s32", ok);
    ok = 1; { uint8x16_t r = vqaddq_u8(UA, UB), s = vqsubq_u8(UA, UB), h = vrhaddq_u8(UA, UB);
      for (int k = 0; k < 16; k++) { int a = u8a[k], b = u8b[k];
        ok &= r[k] == (a + b > 255 ? 255 : a + b) && s[k] == (a - b < 0 ? 0 : a - b) && h[k] == (a + b + 1) >> 1; } }
    check("saturating/rounding u8", ok);
    ok = 1; { uint8x16_t a = vandq_u8(UA, vdupq_n_u8(15)), o = vorrq_u8(UA, UB), x = veorq_u8(UA, UB), c = vbicq_u8(UA, UB), n = vmvnq_u8(UA);
      for (int k = 0; k < 16; k++) ok &= a[k] == (u8a[k] & 15) && o[k] == (u8a[k] | u8b[k]) && x[k] == (u8a[k] ^ u8b[k]) &&
                                      c[k] == (u8a[k] & ~u8b[k] & 255) && n[k] == (~u8a[k] & 255); }
    check("logic u8", ok);
    ok = 1; { uint8x16_t s = vshrq_n_u8(UA, 4), l = vshlq_n_u8(UA, 3); int32x4_t v = vshlq_s32(A32, vnegq_s32(vdupq_n_s32(3))), w = vshrq_n_s32(A32, 5);
      for (int k = 0; k < 16; k++) ok &= s[k] == u8a[k] >> 4 && l[k] == (u8a[k] << 3 & 255);
      for (int k = 0; k < 4; k++) ok &= v[k] == s32a[k] >> 3 && w[k] == s32a[k] >> 5; }
    check("shifts", ok);
    ok = 1; { uint32x4_t g = vcgtq_s32(A32, B32), e = vceqq_s32(A32, A32), l = vcleq_f32(FA, FB);
      uint8x16_t c = vcgeq_u8(UA, UB);
      for (int k = 0; k < 4; k++) ok &= g[k] == (s32a[k] > s32b[k] ? ~0u : 0) && e[k] == ~0u && l[k] == (fa[k] <= fb[k] ? ~0u : 0);
      for (int k = 0; k < 16; k++) ok &= c[k] == (u8a[k] >= u8b[k] ? 255 : 0); }
    check("compares", ok);
    ok = 1; { float32x4_t s = vaddq_f32(FA, FB), m = vmulq_f32(FA, FB), d = vdivq_f32(FA, FB), f = vfmaq_f32(FC, FA, FB), n = vfmaq_n_f32(FC, FA, 2.5f);
      for (int k = 0; k < 4; k++) ok &= s[k] == fa[k] + fb[k] && m[k] == fa[k] * fb[k] && d[k] == fa[k] / fb[k] &&
                                     near(f[k], (double)fa[k] * fb[k] + fc[k]) && near(n[k], (double)fa[k] * 2.5 + fc[k]); }
    check("float arithmetic, fma", ok);
    ok = 1; { float sum = vaddvq_f32(FA); int is = vaddvq_s32(A32); unsigned short u16s = vaddvq_u16(vmovl_u8(vget_low_u8(UA)));
      float ref = (fa[0] + fa[1]) + (fa[2] + fa[3]); int iref = 0; unsigned uref = 0;
      for (int k = 0; k < 4; k++) iref += s32a[k];
      for (int k = 0; k < 8; k++) uref += u8a[k];
      ok = sum == ref && is == iref && u16s == (unsigned short)uref; }
    check("add across", ok);
    ok = 1; { int16x8_t m = vmull_s8(vget_low_s8(A8), vget_low_s8(B8)), h = vmull_high_s8(A8, B8), a = vmlal_s8(A16, vget_high_s8(A8), vget_high_s8(B8));
      int32x4_t p = vpaddlq_s16(A16), q = vpadalq_s16(A32, B16); int16x8_t pl = vpaddlq_s8(A8);
      for (int k = 0; k < 8; k++) ok &= m[k] == s8a[k] * s8b[k] && h[k] == s8a[k + 8] * s8b[k + 8] && a[k] == (short)(s16a[k] + s8a[k + 8] * s8b[k + 8]) &&
                                     pl[k] == s8a[2 * k] + s8a[2 * k + 1];
      for (int k = 0; k < 4; k++) ok &= p[k] == s16a[2 * k] + s16a[2 * k + 1] && q[k] == s32a[k] + s16b[2 * k] + s16b[2 * k + 1]; }
    check("widening", ok);
    ok = 1; { int8x8_t n = vmovn_s16(A16), q = vqmovn_s16(A16); uint8x8_t u = vqmovun_s16(A16); int16x8_t w = vmovl_high_s8(A8);
      for (int k = 0; k < 8; k++) { int x = s16a[k];
        ok &= n[k] == (signed char)x && q[k] == (x > 127 ? 127 : x < -128 ? -128 : x) && u[k] == (x > 255 ? 255 : x < 0 ? 0 : x) && w[k] == s8a[k + 8]; } }
    check("narrowing", ok);
    ok = 1; { int32x4_t z1 = vzip1q_s32(A32, B32), z2 = vzip2q_s32(A32, B32), u1 = vuzp1q_s32(A32, B32), t2 = vtrn2q_s32(A32, B32), e = vextq_s32(A32, B32, 1),
              d = vdupq_laneq_s32(A32, 2);
      ok = z1[0] == s32a[0] && z1[1] == s32b[0] && z1[2] == s32a[1] && z1[3] == s32b[1] && z2[0] == s32a[2] && z2[3] == s32b[3] &&
           u1[0] == s32a[0] && u1[1] == s32a[2] && u1[2] == s32b[0] && u1[3] == s32b[2] && t2[0] == s32a[1] && t2[1] == s32b[1] &&
           e[0] == s32a[1] && e[3] == s32b[0] && d[0] == s32a[2] && d[3] == s32a[2] && vgetq_lane_s32(A32, 3) == s32a[3] &&
           vsetq_lane_s32(7, A32, 1)[1] == 7; }
    check("permutes, lanes", ok);
    ok = 1; { uint8x16_t t = vqtbl1q_u8(UA, vandq_u8(UB, vdupq_n_u8(15))), c = vcntq_u8(UA), s = vbslq_u8(vcgtq_u8(UA, UB), UA, UB);
      for (int k = 0; k < 16; k++) ok &= t[k] == u8a[u8b[k] & 15] && c[k] == __builtin_popcount(u8a[k]) && s[k] == (u8a[k] > u8b[k] ? u8a[k] : u8b[k]); }
    check("table, count, select", ok);
    ok = 1; { int32x4_t i = vcvtq_s32_f32(FA), r = vcvtnq_s32_f32(FB); float32x4_t f = vcvtq_f32_s32(A32);
      unsigned short h[4]; float back[4]; float32x4_t x = vcvt_f32_f16(vcvt_f16_f32(FA));
      vst1_f16(h, vcvt_f16_f32(FA)); vst1q_f32(back, x);
      for (int k = 0; k < 4; k++) { float rr = fb[k]; int nr = (int)(rr + 0.5f); if (rr + 0.5f == nr && (nr & 1)) nr--;
        ok &= i[k] == (int)fa[k] && r[k] == nr && f[k] == (float)s32a[k] && back[k] - fa[k] < fa[k] / 512 + 0.01f && fa[k] - back[k] < fa[k] / 512 + 0.01f; } }
    check("conversions, half precision", ok);
    ok = 1; { struct pair p = { FA, FB }, q = swap(p, vdupq_n_s32(1)); uint8x16_t r = addb(UA, vget_low_u8(UB), UB);
      for (int k = 0; k < 4; k++) ok &= q.a[k] == fb[k] + 1 && q.b[k] == fa[k];
      for (int k = 0; k < 16; k++) ok &= r[k] == (unsigned char)(u8a[k] + u8b[k] + u8b[k & 7]); }
    check("passing in v registers", ok);
    return bad;
}
