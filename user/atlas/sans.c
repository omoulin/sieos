/*
 * sans.c - Proportional text (fontsans.c's DejaVu Sans): measuring and
 * drawing, shared by the desktop (draw.c: titles, labels, buttons) and the
 * UI toolkit (user/lib/ui). A scale is 16.16, of the 6 x 11 font as
 * everywhere: at a native one (1.5: 15 px, 11/6: 18 px, 8/6: 13 px) its
 * font pixel for pixel; at a whole multiple of one (200%...) each pixel
 * n x n, sharp; otherwise the nearest bigger one (else the biggest), each
 * screen pixel sampled bilinearly.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "fontsans.h"

static const int native[3] = { 98304, 120150, 87382 };   /* sans_fonts' scales: 15, 18, 13 px */

int sans_pick(int s, int *r)
{
    int best = -1;
    for (int i = 0; i < 3 && best < 0; i++) {
        int64_t q = ((int64_t)s << 16) / native[i], k = (q + 32768) >> 16;
        if (k >= 1 && q - (k << 16) < 64 && (k << 16) - q < 64) best = i;
    }
    for (int i = 0; i < 3 && best < 0; i++)
        if (native[i] >= s && (best < 0 || native[i] < native[best])) best = i;
    if (best < 0) best = 1;                              /* (the biggest: titles) */
    *r = (int)(((int64_t)s << 16) / native[best]);
    return best;
}

static const sans_glyph_t *glyph(const sans_font_t *f, unsigned ch) { return &f->g[ch >= 32 && ch < 127 ? ch - 32 : '?' - 32]; }

int64_t sans_w(const char *s, int n, int scale)
{
    int r;
    const sans_font_t *f = &sans_fonts[sans_pick(scale, &r)];
    int64_t w = 0;
    for (int i = 0; i < n; i++) w += (int64_t)glyph(f, (uint8_t)s[i])->adv * r;
    return w;
}

int sans_fit(const char *s, int n, int scale, int64_t w)
{
    int r, k = 0;
    const sans_font_t *f = &sans_fonts[sans_pick(scale, &r)];
    for (int64_t acc = 0; k < n; k++)
        if ((acc += (int64_t)glyph(f, (uint8_t)s[k])->adv * r) > w) break;
    return k;
}

static inline uint32_t over(uint32_t o, uint32_t c, int a)   /* c over o, a of 256 */
{
    if (a >= 256) return c;
    return ((((o >> 16 & 255) * (256 - a) + (c >> 16 & 255) * a) >> 8) << 16) |
           ((((o >> 8 & 255) * (256 - a) + (c >> 8 & 255) * a) >> 8) << 8) |
           (((o & 255) * (256 - a) + (c & 255) * a) >> 8);
}

void sans_draw(uint32_t *px, int pitch, int oy, int x, int y, const char *s, int n, int scale, uint32_t c,
               int cx0, int cy0, int cx1, int cy1)
{
    int r, k;
    const sans_font_t *f = &sans_fonts[sans_pick(scale, &r)];
    k = (r + 32768) >> 16;
    if (r - (k << 16) >= 64 || (k << 16) - r >= 64) k = 0;   /* (a whole multiple: k; else bilinear) */
    int y0 = cy0 > y ? cy0 : y, y1 = y + (int)(((int64_t)f->h * r + 65535) >> 16);
    if (y1 > cy1) y1 = cy1;
    int64_t pen = (int64_t)x << 16;
    for (int i = 0; i < n && (pen >> 16) < cx1; i++) {   /* glyph after glyph, each over what is there */
        const sans_glyph_t *g = glyph(f, (uint8_t)s[i]);
        int64_t gx = pen + (int64_t)g->x * r;            /* its ink's left, 16.16 */
        int xa = (int)(gx >> 16) - 1, xb = (int)((gx + (int64_t)g->w * r) >> 16) + 2;
        pen += (int64_t)g->adv * r;
        if (!g->w || xb <= cx0 || xa >= cx1) continue;
        if (xa < cx0) xa = cx0;
        if (xb > cx1) xb = cx1;
        for (int yy = y0; yy < y1; yy++) {
            uint32_t *d = px + (int64_t)(yy - oy) * pitch;
            if (k) {
                int row = (yy - y) / k, gx0 = (int)(gx >> 16);
                if (row >= f->h) break;
                for (int xx = xa; xx < xb; xx++) {
                    int col = xx - gx0;
                    if (col < 0 || col >= g->w * k) continue;
                    int v = sans_px(f, g, col / k, row);
                    if (v) d[xx] = over(d[xx], c, v * 256 / 15);
                }
                continue;
            }
            int64_t v16 = ((((int64_t)(yy - y) << 16) + 32768) << 16) / r - 32768;
            int r0 = (int)(v16 >> 16), fy = (int)(v16 & 65535);
            for (int xx = xa; xx < xb; xx++) {
                int64_t u16 = ((((int64_t)xx << 16) + 32768 - gx) << 16) / r - 32768;
                int c0 = (int)(u16 >> 16), fx = (int)(u16 & 65535), v = 0;
                for (int j = 0; j < 4; j++) {
                    int cc = c0 + (j & 1), rr = r0 + (j >> 1);
                    if (cc < 0 || cc >= g->w || rr < 0 || rr >= f->h) continue;
                    int wgt = (int)((int64_t)(j & 1 ? fx : 65536 - fx) * (j >> 1 ? fy : 65536 - fy) >> 16);
                    v += (int)((int64_t)sans_px(f, g, cc, rr) * wgt >> 8);
                }
                if (v) d[xx] = over(d[xx], c, (int)((int64_t)v * 256 / (15 << 8)));
            }
        }
    }
}
