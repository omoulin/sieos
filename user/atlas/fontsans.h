/*
 * fontsans.h - The proportional fonts (fontsans.c, tools/mkfontsans.py):
 * DejaVu Sans at the mono fonts' three native sizes, 4-bit coverage, each
 * glyph only where it has ink. Shared by the desktop (draw.c) and the UI
 * toolkit (user/lib/ui).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

typedef struct {
    uint8_t adv, w;                 /* pixels to the next glyph; its ink's width */
    int8_t x;                       /* where its ink starts, from the pen */
    uint16_t off;                   /* its rows in px: (w + 1) / 2 bytes each, the left pixel high */
} sans_glyph_t;
typedef struct { int h; const sans_glyph_t *g; const uint8_t *px; } sans_font_t;
extern const sans_font_t sans_fonts[3];   /* 15 (9 x 16's cell), 18 (11 x 20), 13 (8 x 15) pixels */

/* sans.c: a scale is 16.16 of the 6 x 11 font; widths are pixels, 16.16. */
int     sans_pick(int scale, int *r);               /* -> the font drawn from; *r: scale over its own */
int64_t sans_w(const char *s, int n, int scale);    /* s's n characters' width */
int     sans_fit(const char *s, int n, int scale, int64_t w);   /* how many of them fit in w */
void    sans_draw(uint32_t *px, int pitch, int oy, int x, int y, const char *s, int n, int scale, uint32_t c,
                  int cx0, int cy0, int cx1, int cy1);   /* text with its cell's top left at (x, y), over the
                                                   pixels px (row y at px + (y - oy) * pitch), clipped */

/* A glyph's coverage at (col, row) of its ink, 0..15. */
static inline int sans_px(const sans_font_t *f, const sans_glyph_t *g, int col, int row)
{
    uint8_t b = f->px[g->off + row * ((g->w + 1) >> 1) + (col >> 1)];
    return col & 1 ? b & 15 : b >> 4;
}
