/*
 * sbc.c - An SBC encoder (A2DP spec, appendix B), the one configuration
 * btd sends: stereo, joint stereo, 16 blocks, 8 subbands, loudness bit
 * allocation, 44.1 or 48 kHz, the bitpool the sink allows. Fixed point.
 *
 *   analysis   per block of 8 new samples: the 80-tap window (the spec's
 *              prototype filter, as h[0..40], symmetric) folded to 16, then
 *              the 8x16 cosine matrix; subband samples in Q12 of the PCM's units
 *   joint      per subband (not the last): mid/side when their scale factors
 *              sum to less than left/right's
 *   bits       the spec's loudness allocation over both channels
 *   frame      header, CRC-8, join bits, scale factors, samples, padding
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "btd.h"

#define F 12                                     /* the subband samples' fraction bits */
static const int32_t H[41] = {                   /* the prototype filter, Q20 (h[80 - n] = h[n]) */
    0, 164, 360, 582, 864, 1195, 1548, 1870, 2110, 2206, 2091, 1695, 946, -187, -1730, -3667, -5934, -8419,
    -10966, -13366, -15364, -16677, -17009, -16063, -13566, -9288, -3066, 5155, 15352, 27378, 40973, 55771,
    71302, 87016, 102315, 116598, 129252, 139738, 147591, 152452, 154094 };
static const int16_t COS[9] = { 16384, 16069, 15137, 13623, 11585, 9102, 6270, 3196, 0 };   /* cos(j pi/16), Q14 */
static int32_t C[80], M[8][16];

static void tables(void)
{
    for (int i = 0; i < 80; i++) C[i] = (i >> 4 & 1 ? -1 : 1) * H[i <= 40 ? i : 80 - i];   /* (the 16-fold's signs) */
    for (int k = 0; k < 8; k++)
        for (int i = 0; i < 16; i++) {                   /* cos((k + 1/2)(i - 4) pi / 8) */
            int j = ((2 * k + 1) * (i - 4) % 32 + 32) % 32, s = 1;
            if (j > 16) j = 32 - j;
            if (j > 8) { j = 16 - j; s = -1; }
            M[k][i] = s * COS[j];
        }
}

int sbc_len(int bitpool) { return 13 + 2 * bitpool; }

static uint8_t crc8(uint8_t c, const uint8_t *p, int n)
{
    while (n--) { c ^= *p++; for (int b = 0; b < 8; b++) c = (uint8_t)(c & 0x80 ? c << 1 ^ 0x1D : c << 1); }
    return c;
}
static int sf_of(const int32_t *s, int step)     /* the scale factor of 16 samples: |x| <= 2^(sf + 1) */
{
    int32_t m = 0;
    for (int b = 0; b < 16; b++) { int32_t a = s[b * step] < 0 ? -s[b * step] : s[b * step]; if (a > m) m = a; }
    int sf = 0;
    while (sf < 15 && m > 1 << (sf + 1 + F)) sf++;
    return sf;
}

/* One frame from 128 stereo frames of 16-bit PCM: pcm[(at + i) & mask],
 * interleaved, times s->gain (Q16) -> its length. */
int sbc_frame(sbc_t *s, const int16_t *pcm, unsigned at, unsigned mask, uint8_t *out)
{
    static int32_t sb[16][2][8];
    int sf[2][8], bits[2][8], need[2][8], join = 0, bp = s->bitpool;
    if (!C[40]) tables();
    for (int i = 0; i < 128; i++)
        for (int ch = 0; ch < 2; ch++) s->h[ch][80 + i] = (int16_t)(pcm[(at + 2 * i + ch) & mask] * s->gain >> 16);
    for (int ch = 0; ch < 2; ch++) {
        for (int b = 0; b < 16; b++) {
            const int16_t *x = s->h[ch] + 87 + 8 * b;    /* x[-i]: X[i], the newest first */
            int32_t y[16];
            for (int i = 0; i < 16; i++) {
                int64_t a = 0;
                for (int j = i; j < 80; j += 16) a += (int64_t)x[-j] * C[j];
                y[i] = (int32_t)(a >> 6);                /* Q14 */
            }
            for (int k = 0; k < 8; k++) {
                int64_t a = 0;
                for (int i = 0; i < 16; i++) a += (int64_t)M[k][i] * y[i];
                sb[b][ch][k] = (int32_t)(a >> 16);       /* Q12 */
            }
        }
        memmove(s->h[ch], s->h[ch] + 128, 80 * sizeof s->h[ch][0]);
    }
    for (int k = 0; k < 8; k++) {
        sf[0][k] = sf_of(&sb[0][0][k], 16); sf[1][k] = sf_of(&sb[0][1][k], 16);
        if (k == 7) break;
        int32_t ms[16][2];
        for (int b = 0; b < 16; b++) { ms[b][0] = (sb[b][0][k] + sb[b][1][k]) >> 1; ms[b][1] = (sb[b][0][k] - sb[b][1][k]) >> 1; }
        int m = sf_of(&ms[0][0], 2), d = sf_of(&ms[0][1], 2);
        if (m + d < sf[0][k] + sf[1][k]) {
            join |= 0x80 >> k; sf[0][k] = m; sf[1][k] = d;
            for (int b = 0; b < 16; b++) { sb[b][0][k] = ms[b][0]; sb[b][1][k] = ms[b][1]; }
        }
    }
    /* Loudness allocation, both channels together. */
    static const int8_t off[8] = { -4, 0, 0, 0, 0, 0, 1, 2 };   /* (44.1 and 48 kHz) */
    int maxn = -99, slice, count = 0, sc = 0;
    for (int ch = 0; ch < 2; ch++)
        for (int k = 0; k < 8; k++) {
            int l = sf[ch][k] - off[k];
            need[ch][k] = !sf[ch][k] ? -5 : l > 0 ? l / 2 : l;
            if (need[ch][k] > maxn) maxn = need[ch][k];
        }
    slice = maxn + 1;
    do {
        slice--; count += sc; sc = 0;
        for (int ch = 0; ch < 2; ch++)
            for (int k = 0; k < 8; k++)
                if (need[ch][k] > slice + 1 && need[ch][k] < slice + 16) sc++;
                else if (need[ch][k] == slice + 1) sc += 2;
    } while (count + sc < bp);
    if (count + sc == bp) { count += sc; slice--; }
    for (int ch = 0; ch < 2; ch++)
        for (int k = 0; k < 8; k++) bits[ch][k] = need[ch][k] < slice + 2 ? 0 : need[ch][k] - slice < 16 ? need[ch][k] - slice : 16;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; count < bp && i < 16; i++) {
            int *b = &bits[i & 1][i >> 1];
            if (pass ? *b < 16 : *b >= 2 && *b < 16) { (*b)++; count++; }
            else if (!pass && need[i & 1][i >> 1] == slice + 1 && bp > count + 1) { *b = 2; count += 2; }
        }
    /* The frame. */
    out[0] = 0x9C; out[1] = (uint8_t)(s->freq << 6 | 3 << 4 | 3 << 2 | 1); out[2] = (uint8_t)bp;
    out[4] = (uint8_t)join;
    for (int ch = 0; ch < 2; ch++) for (int k = 0; k < 8; k += 2) out[5 + ch * 4 + k / 2] = (uint8_t)(sf[ch][k] << 4 | sf[ch][k + 1]);
    out[3] = crc8(crc8(0x0F, out + 1, 2), out + 4, 9);
    uint32_t acc = 0;
    int n = 13, nb = 0;
    for (int b = 0; b < 16; b++)
        for (int ch = 0; ch < 2; ch++)
            for (int k = 0; k < 8; k++) {
                int w = bits[ch][k];
                if (!w) continue;
                int64_t lv = (1 << w) - 1, q = ((int64_t)sb[b][ch][k] + (1 << (sf[ch][k] + 1 + F))) * lv >> (sf[ch][k] + 2 + F);
                acc = acc << w | (uint32_t)(q < 0 ? 0 : q >= lv ? lv - 1 : q);
                for (nb += w; nb >= 8; nb -= 8) out[n++] = (uint8_t)(acc >> (nb - 8));
            }
    if (nb) out[n++] = (uint8_t)(acc << (8 - nb));
    while (n < 13 + 2 * bp) out[n++] = 0;               /* (every bit used: the length the header says) */
    return n;
}
