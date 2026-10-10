/* cflags: -march=armv8.2-a+dotprod */
/* t_neon_dot.c - the dot product intrinsics (ARMv8.2: the Pi 5), checked
 * against plain C. Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#include <arm_neon.h>
int printf(const char *, ...);
int main(void)
{
    signed char a[16], b[16]; unsigned char ua[16], ub[16]; int acc[4] = { 1, -2, 3, 100000 };
    unsigned s = 7;
    for (int k = 0; k < 16; k++) { s = s * 69069 + 1; a[k] = s >> 13; b[k] = s >> 21; ua[k] = s >> 9; ub[k] = s >> 17; }
    int32x4_t d = vdotq_s32(vld1q_s32(acc), vld1q_s8(a), vld1q_s8(b)), l = vdotq_laneq_s32(vld1q_s32(acc), vld1q_s8(a), vld1q_s8(b), 2);
    uint32x4_t u = vdotq_u32(vdupq_n_u32(5), vld1q_u8(ua), vld1q_u8(ub));
    int32x2_t h = vdot_s32(vld1_s32(acc), vld1_s8(a), vld1_s8(b));
    int ok = 1;
    for (int g = 0; g < 4; g++) {
        int r = acc[g], rl = acc[g]; unsigned ru = 5;
        for (int j = 0; j < 4; j++) { r += a[4 * g + j] * b[4 * g + j]; rl += a[4 * g + j] * b[8 + j]; ru += ua[4 * g + j] * ub[4 * g + j]; }
        ok &= d[g] == r && l[g] == rl && u[g] == ru && (g >= 2 || h[g] == r);
    }
    printf("dot product %s\n", ok ? "ok" : "FAIL");
    return !ok;
}
