/*
 * font.h - Glyphs as coverage maps. Linux: font_tt.c renders DejaVu Sans Mono
 * (Atlas's face) with stb_truetype, with fallbacks for symbols and CJK.
 * SIEOS: Atlas's native fonts can stand in.
 */
#ifndef FONT_H
#define FONT_H

#include <stdint.h>

enum { F_REG, F_BOLD, F_ITALIC, NFACE };

typedef struct {
    int w, h;                 /* the coverage map */
    int x0, y0;               /* its top left, from the pen on the baseline (y0 < 0: above) */
    int adv;                  /* the pen's advance */
    uint8_t *a;               /* w * h coverage, 0..255 */
} glyph_t;

int  font_init(void);                          /* 0: ready */
const glyph_t *font_glyph(int face, int px, int cp);
int  font_ascent(int px);
int  font_descent(int px);                     /* (positive) */

#endif
