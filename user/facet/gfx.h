/*
 * gfx.h - Software rendering for the Facet desktop.
 * Colours are 0x00RRGGBB.  All drawing is clipped to the surface clip rect.
 */
#ifndef FACET_GFX_H
#define FACET_GFX_H

#include "sieos.h"

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

int  gfx_text(struct surface *s, int x, int y, const char *str, color_t fg);
int  gfx_text_bg(struct surface *s, int x, int y, const char *str, color_t fg, color_t bg);
void gfx_char(struct surface *s, int x, int y, unsigned char ch, color_t fg, color_t bg, bool opaque);
void gfx_text_scaled(struct surface *s, int x, int y, const char *str, int scale, color_t fg);
int  text_width(const char *str);

int  gfx_text_bold(struct surface *s, int x, int y, const char *str, color_t fg);

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
color_t color_shade(color_t c, int delta);            /* lighten (+) / darken (-) */

#endif
