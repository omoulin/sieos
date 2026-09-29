/*
 * facet/gfx.h - Software rendering (libfacet): rectangles, lines, circles,
 * gradients, rounded shapes, alpha blending and text, into a 32-bit surface.
 * Text is drawn in rows of FONT_H pixels, with DejaVu Sans (TrueType, see
 * facet/font.h), or with the 8x16 bitmap font where the fonts are missing.  Colours are 0x00RRGGBB.  All drawing is clipped to the
 * surface's clip rectangle.
 */
#ifndef FACET_GFX_H
#define FACET_GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

typedef uint32_t color_t;

struct rect {
    int x, y, w, h;
};

struct surface {
    uint32_t *px;
    int w, h, stride;          /* stride in pixels */
    struct rect clip;
};

#define RGB(r, g, b) (((color_t)(r) << 16) | ((color_t)(g) << 8) | (color_t)(b))

extern const uint8_t font8x16[256][16];
#define FONT_W 8
#define FONT_H 16

struct rect rect_make(int x, int y, int w, int h);
struct rect rect_intersect(struct rect a, struct rect b);
struct rect rect_union(struct rect a, struct rect b);
bool rect_empty(struct rect r);
bool rect_contains(struct rect r, int x, int y);

void gfx_set_clip(struct surface *s, struct rect r);
void gfx_fill(struct surface *s, int x, int y, int w, int h, color_t c);
void gfx_frame(struct surface *s, int x, int y, int w, int h, color_t c);
void gfx_hline(struct surface *s, int x, int y, int w, color_t c);
void gfx_vline(struct surface *s, int x, int y, int h, color_t c);
void gfx_pixel(struct surface *s, int x, int y, color_t c);
void gfx_line(struct surface *s, int x0, int y0, int x1, int y1, color_t c);
void gfx_thick_line(struct surface *s, int x0, int y0, int x1, int y1, int t, color_t c);
void gfx_circle(struct surface *s, int cx, int cy, int r, color_t c);
void gfx_disc(struct surface *s, int cx, int cy, int r, color_t c);
void gfx_vgradient(struct surface *s, int x, int y, int w, int h, color_t top, color_t bottom);
void gfx_hgradient(struct surface *s, int x, int y, int w, int h, color_t left, color_t right);
void gfx_bevel(struct surface *s, int x, int y, int w, int h, int depth, bool raised,
               color_t light, color_t dark);
void gfx_triangle(struct surface *s, int x0, int y0, int x1, int y1, int x2, int y2, color_t c);
void gfx_blit(struct surface *dst, int dx, int dy, const struct surface *src, struct rect from);

/* Text (UTF-8) with its row's top at y; the functions return the width drawn. */
int  gfx_text(struct surface *s, int x, int y, const char *str, color_t fg);
int  gfx_text_bold(struct surface *s, int x, int y, const char *str, color_t fg);
int  gfx_text_bg(struct surface *s, int x, int y, const char *str, color_t fg, color_t bg);
void gfx_text_scaled(struct surface *s, int x, int y, const char *str, int scale, color_t fg);   /* bold, scale x the size */
int  text_width(const char *str);
int  text_width_bold(const char *str);
int  text_width_scaled(const char *str, int scale);
size_t text_fit(const char *str, int maxw);          /* bytes of str that fit in maxw pixels */

/* Character cells (terminals, tables): gfx_cell_w() x gfx_cell_h() pixels,
 * the monospace face's (8 x 16 with the bitmap font).  gfx_char draws one
 * byte (ISO 8859-15) in a cell; gfx_text_mono a string, one cell a byte. */
int  gfx_cell_w(void);
int  gfx_cell_h(void);
void gfx_char(struct surface *s, int x, int y, unsigned char ch, color_t fg, color_t bg, bool opaque);
int  gfx_text_mono(struct surface *s, int x, int y, const char *str, color_t fg);
int  text_width_mono(const char *str);

/* alpha is 0..256 */
void gfx_blend_pixel(struct surface *s, int x, int y, color_t c, int alpha);
void gfx_blend_fill(struct surface *s, int x, int y, int w, int h, color_t c, int alpha);
void gfx_round_rect(struct surface *s, int x, int y, int w, int h, int rad, color_t c);
void gfx_round_rect_alpha(struct surface *s, int x, int y, int w, int h, int rad, color_t c, int alpha);
void gfx_round_rect_top(struct surface *s, int x, int y, int w, int h, int rad, color_t c);
void gfx_round_rect_vgradient(struct surface *s, int x, int y, int w, int h, int rad, color_t top, color_t bottom);
void gfx_round_frame(struct surface *s, int x, int y, int w, int h, int rad, color_t c);
void gfx_shadow(struct surface *s, struct rect r, int rad, int size, int dy, int max_alpha);

color_t color_mix(color_t a, color_t b, int t256);   /* t=0 -> a, 256 -> b */

/* Antialiased shapes (float coordinates, pixel centres at +0.5): a polygon
 * of n points (xy pairs), filled, with a vertical gradient, or stroked; an
 * ellipse. */
void gfx_poly(struct surface *s, const float *xy, int n, color_t c);
void gfx_poly_vgradient(struct surface *s, const float *xy, int n, color_t top, color_t bottom);
void gfx_stroke(struct surface *s, const float *xy, int n, bool closed, float width, color_t c);
void gfx_ellipse_aa(struct surface *s, float cx, float cy, float rx, float ry, color_t top, color_t bottom);
color_t color_shade(color_t c, int delta);            /* lighten (+) / darken (-) */

#endif
