/*
 * gfx.c - Software rendering primitives.
 */
#include "gfx.h"

struct rect rect_make(int x, int y, int w, int h)
{
    struct rect r = { x, y, w, h };
    return r;
}

struct rect rect_intersect(struct rect a, struct rect b)
{
    int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
    int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0)
        return rect_make(0, 0, 0, 0);
    return rect_make(x0, y0, x1 - x0, y1 - y0);
}

struct rect rect_union(struct rect a, struct rect b)
{
    if (rect_empty(a))
        return b;
    if (rect_empty(b))
        return a;
    int x0 = MIN(a.x, b.x), y0 = MIN(a.y, b.y);
    int x1 = MAX(a.x + a.w, b.x + b.w), y1 = MAX(a.y + a.h, b.y + b.h);
    return rect_make(x0, y0, x1 - x0, y1 - y0);
}

bool rect_empty(struct rect r)
{
    return r.w <= 0 || r.h <= 0;
}

bool rect_contains(struct rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

void gfx_set_clip(struct surface *s, struct rect r)
{
    s->clip = rect_intersect(r, rect_make(0, 0, s->w, s->h));
}

void gfx_fill(struct surface *s, int x, int y, int w, int h, color_t c)
{
    struct rect r = rect_intersect(rect_make(x, y, w, h), s->clip);
    for (int j = 0; j < r.h; j++) {
        uint32_t *p = s->px + (r.y + j) * s->stride + r.x;
        for (int i = 0; i < r.w; i++)
            p[i] = c;
    }
}

void gfx_hline(struct surface *s, int x, int y, int w, color_t c)
{
    gfx_fill(s, x, y, w, 1, c);
}

void gfx_vline(struct surface *s, int x, int y, int h, color_t c)
{
    gfx_fill(s, x, y, 1, h, c);
}

void gfx_frame(struct surface *s, int x, int y, int w, int h, color_t c)
{
    gfx_hline(s, x, y, w, c);
    gfx_hline(s, x, y + h - 1, w, c);
    gfx_vline(s, x, y, h, c);
    gfx_vline(s, x + w - 1, y, h, c);
}

void gfx_pixel(struct surface *s, int x, int y, color_t c)
{
    if (rect_contains(s->clip, x, y))
        s->px[y * s->stride + x] = c;
}

void gfx_line(struct surface *s, int x0, int y0, int x1, int y1, color_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(s, x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void gfx_thick_line(struct surface *s, int x0, int y0, int x1, int y1, int t, color_t c)
{
    for (int oy = -(t / 2); oy <= t / 2; oy++)
        for (int ox = -(t / 2); ox <= t / 2; ox++)
            if (ox * ox + oy * oy <= (t / 2) * (t / 2) + 1)
                gfx_line(s, x0 + ox, y0 + oy, x1 + ox, y1 + oy, c);
}

void gfx_circle(struct surface *s, int cx, int cy, int r, color_t c)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gfx_pixel(s, cx + x, cy + y, c);
        gfx_pixel(s, cx + y, cy + x, c);
        gfx_pixel(s, cx - y, cy + x, c);
        gfx_pixel(s, cx - x, cy + y, c);
        gfx_pixel(s, cx - x, cy - y, c);
        gfx_pixel(s, cx - y, cy - x, c);
        gfx_pixel(s, cx + y, cy - x, c);
        gfx_pixel(s, cx + x, cy - y, c);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void gfx_disc(struct surface *s, int cx, int cy, int r, color_t c)
{
    for (int y = -r; y <= r; y++) {
        int w = 0;
        while ((w + 1) * (w + 1) + y * y <= r * r)
            w++;
        gfx_hline(s, cx - w, cy + y, 2 * w + 1, c);
    }
}

color_t color_mix(color_t a, color_t b, int t)
{
    int r = (((a >> 16) & 255) * (256 - t) + ((b >> 16) & 255) * t) >> 8;
    int g = (((a >> 8) & 255) * (256 - t) + ((b >> 8) & 255) * t) >> 8;
    int bl = ((a & 255) * (256 - t) + (b & 255) * t) >> 8;
    return RGB(r, g, bl);
}

color_t color_shade(color_t c, int d)
{
    int r = ((c >> 16) & 255) + d, g = ((c >> 8) & 255) + d, b = (c & 255) + d;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return RGB(r, g, b);
}

void gfx_vgradient(struct surface *s, int x, int y, int w, int h, color_t top, color_t bottom)
{
    for (int j = 0; j < h; j++)
        gfx_hline(s, x, y + j, w, color_mix(top, bottom, h > 1 ? j * 256 / (h - 1) : 0));
}

void gfx_hgradient(struct surface *s, int x, int y, int w, int h, color_t left, color_t right)
{
    for (int i = 0; i < w; i++)
        gfx_vline(s, x + i, y, h, color_mix(left, right, w > 1 ? i * 256 / (w - 1) : 0));
}

/* A 3D bevel: light on top/left and dark on bottom/right when raised. */
void gfx_bevel(struct surface *s, int x, int y, int w, int h, int depth, bool raised,
               color_t light, color_t dark)
{
    color_t tl = raised ? light : dark, br = raised ? dark : light;
    for (int i = 0; i < depth; i++) {
        gfx_hline(s, x + i, y + i, w - 2 * i, tl);
        gfx_vline(s, x + i, y + i, h - 2 * i, tl);
        gfx_hline(s, x + i, y + h - 1 - i, w - 2 * i, br);
        gfx_vline(s, x + w - 1 - i, y + i, h - 2 * i, br);
    }
}

void gfx_triangle(struct surface *s, int x0, int y0, int x1, int y1, int x2, int y2, color_t c)
{
    /* sort by y */
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y2 < y0) { int t = y0; y0 = y2; y2 = t; t = x0; x0 = x2; x2 = t; }
    if (y2 < y1) { int t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    for (int y = y0; y <= y2; y++) {
        int xa = y2 != y0 ? x0 + (x2 - x0) * (y - y0) / (y2 - y0) : x0;
        int xb;
        if (y < y1)
            xb = y1 != y0 ? x0 + (x1 - x0) * (y - y0) / (y1 - y0) : x0;
        else
            xb = y2 != y1 ? x1 + (x2 - x1) * (y - y1) / (y2 - y1) : x1;
        if (xa > xb) { int t = xa; xa = xb; xb = t; }
        gfx_hline(s, xa, y, xb - xa + 1, c);
    }
}

void gfx_blit(struct surface *dst, int dx, int dy, const struct surface *src, struct rect from)
{
    struct rect to = rect_intersect(rect_make(dx, dy, from.w, from.h), dst->clip);
    for (int j = 0; j < to.h; j++) {
        const uint32_t *sp = src->px + (from.y + to.y - dy + j) * src->stride + from.x + to.x - dx;
        uint32_t *dp = dst->px + (to.y + j) * dst->stride + to.x;
        memcpy(dp, sp, to.w * 4);
    }
}

void gfx_char(struct surface *s, int x, int y, unsigned char ch, color_t fg, color_t bg, bool opaque)
{
    struct rect r = rect_intersect(rect_make(x, y, FONT_W, FONT_H), s->clip);
    if (rect_empty(r))
        return;
    const uint8_t *g = font8x16[ch];
    for (int j = r.y - y; j < r.y - y + r.h; j++) {
        uint32_t *p = s->px + (y + j) * s->stride;
        for (int i = r.x - x; i < r.x - x + r.w; i++) {
            if (g[j] & (0x80 >> i))
                p[x + i] = fg;
            else if (opaque)
                p[x + i] = bg;
        }
    }
}

int gfx_text(struct surface *s, int x, int y, const char *str, color_t fg)
{
    int x0 = x;
    for (; *str; str++, x += FONT_W)
        gfx_char(s, x, y, (unsigned char)*str, fg, 0, false);
    return x - x0;
}

int gfx_text_bg(struct surface *s, int x, int y, const char *str, color_t fg, color_t bg)
{
    int x0 = x;
    for (; *str; str++, x += FONT_W)
        gfx_char(s, x, y, (unsigned char)*str, fg, bg, true);
    return x - x0;
}

void gfx_text_scaled(struct surface *s, int x, int y, const char *str, int scale, color_t fg)
{
    for (; *str; str++, x += FONT_W * scale) {
        const uint8_t *g = font8x16[(unsigned char)*str];
        for (int j = 0; j < FONT_H; j++)
            for (int i = 0; i < FONT_W; i++)
                if (g[j] & (0x80 >> i))
                    gfx_fill(s, x + i * scale, y + j * scale, scale, scale, fg);
    }
}

int text_width(const char *str)
{
    return strlen(str) * FONT_W;
}

/* ---------------- blending, rounded shapes, shadows ---------------- */

static inline uint32_t blend(uint32_t dst, color_t c, int a)    /* a: 0..256 */
{
    int r = (((dst >> 16) & 255) * (256 - a) + ((c >> 16) & 255) * a) >> 8;
    int g = (((dst >> 8) & 255) * (256 - a) + ((c >> 8) & 255) * a) >> 8;
    int b = ((dst & 255) * (256 - a) + (c & 255) * a) >> 8;
    return RGB(r, g, b);
}

void gfx_blend_pixel(struct surface *s, int x, int y, color_t c, int alpha)
{
    if (alpha <= 0 || !rect_contains(s->clip, x, y))
        return;
    uint32_t *p = &s->px[y * s->stride + x];
    *p = alpha >= 256 ? c : blend(*p, c, alpha);
}

void gfx_blend_fill(struct surface *s, int x, int y, int w, int h, color_t c, int alpha)
{
    struct rect r = rect_intersect(rect_make(x, y, w, h), s->clip);
    if (alpha >= 256) {
        gfx_fill(s, r.x, r.y, r.w, r.h, c);
        return;
    }
    for (int j = r.y; j < r.y + r.h; j++) {
        uint32_t *p = s->px + j * s->stride + r.x;
        for (int i = 0; i < r.w; i++)
            p[i] = blend(p[i], c, alpha);
    }
}

static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* Horizontal inset (1/256 px) of a radius-rad corner at row j from the edge. */
static int corner_inset(int rad, int j)
{
    if (j >= rad)
        return 0;
    int64_t d = (int64_t)rad * 256 - (int64_t)j * 256 - 128;     /* distance from the centre row */
    int64_t v = (int64_t)rad * rad * 65536 - d * d;
    if (v <= 0)
        return rad * 256;
    return rad * 256 - (int)isqrt64((uint64_t)v);
}

/* Row span of a rounded rectangle, antialiased at both ends. */
static void round_span(struct surface *s, int x, int y, int w, int inset, color_t c, int alpha)
{
    int full = (inset + 255) >> 8, frac = 256 - (inset & 255);
    if (w - 2 * full > 0)
        gfx_blend_fill(s, x + full, y, w - 2 * full, 1, c, alpha);
    if ((inset & 255) && full > 0) {
        int a = frac * alpha >> 8;
        gfx_blend_pixel(s, x + full - 1, y, c, a);
        gfx_blend_pixel(s, x + w - full, y, c, a);
    }
}

void gfx_round_rect(struct surface *s, int x, int y, int w, int h, int rad, color_t c)
{
    gfx_round_rect_alpha(s, x, y, w, h, rad, c, 256);
}

void gfx_round_rect_alpha(struct surface *s, int x, int y, int w, int h, int rad, color_t c, int alpha)
{
    rad = MIN(rad, MIN(w, h) / 2);
    int y0 = MAX(y, s->clip.y), y1 = MIN(y + h, s->clip.y + s->clip.h);
    for (int yy = y0; yy < y1; yy++) {
        int j = yy - y, fromb = y + h - 1 - yy;
        int inset = corner_inset(rad, MIN(j, fromb));
        round_span(s, x, yy, w, inset, c, alpha);
    }
}

/* Rounded rectangle with only the top corners rounded. */
void gfx_round_rect_top(struct surface *s, int x, int y, int w, int h, int rad, color_t c)
{
    rad = MIN(rad, MIN(w / 2, h));
    int y0 = MAX(y, s->clip.y), y1 = MIN(y + h, s->clip.y + s->clip.h);
    for (int yy = y0; yy < y1; yy++)
        round_span(s, x, yy, w, corner_inset(rad, yy - y), c, 256);
}

/* 1-pixel outline of a rounded rectangle. */
void gfx_round_frame(struct surface *s, int x, int y, int w, int h, int rad, color_t c)
{
    rad = MIN(rad, MIN(w, h) / 2);
    int y0 = MAX(y, s->clip.y), y1 = MIN(y + h, s->clip.y + s->clip.h);
    for (int yy = y0; yy < y1; yy++) {
        int j = yy - y, fromb = y + h - 1 - yy, e = MIN(j, fromb);
        if (e == 0) {
            round_span(s, x, yy, w, corner_inset(rad, 0), c, 256);
            continue;
        }
        int o = corner_inset(rad, e), in = rad > 1 ? corner_inset(rad - 1, e - 1) + 256 : 256;
        int of = (o + 255) >> 8, inf = in >> 8;
        int edge = MAX(1, inf - of + 1);
        /* left and right edge pixels, the outermost one antialiased */
        if (o & 255) {
            int a = 256 - (o & 255);
            gfx_blend_pixel(s, x + of - 1, yy, c, a);
            gfx_blend_pixel(s, x + w - of, yy, c, a);
        }
        gfx_fill(s, x + of, yy, edge, 1, c);
        gfx_fill(s, x + w - of - edge, yy, edge, 1, c);
    }
}

/*
 * Soft drop shadow around r (rounded with radius rad), size pixels wide,
 * moved down by dy; only drawn outside r itself.
 */
void gfx_shadow(struct surface *s, struct rect r, int rad, int size, int dy, int max_alpha)
{
    static uint8_t sqrt_tab[4096];
    static bool tab_ready;
    if (!tab_ready) {
        for (int i = 0; i < 4096; i++)
            sqrt_tab[i] = (uint8_t)MIN(255, isqrt64((uint64_t)i * 16));   /* 4 * sqrt(i): quarter pixels */
        tab_ready = true;
    }
    struct rect sr = rect_make(r.x, r.y + dy, r.w, r.h);
    struct rect area = rect_intersect(rect_make(sr.x - size, sr.y - size, sr.w + 2 * size, sr.h + 2 * size), s->clip);
    int inner = rad;                                         /* the shadow shape's corner radius */
    for (int py = area.y; py < area.y + area.h; py++) {
        uint32_t *row = s->px + py * s->stride;
        for (int px = area.x; px < area.x + area.w; px++) {
            if (rect_contains(r, px, py)) {                  /* covered by the object itself */
                px = r.x + r.w - 1;
                continue;
            }
            int ddx = MAX(MAX(sr.x + inner - px, px - (sr.x + sr.w - 1 - inner)), 0);
            int ddy = MAX(MAX(sr.y + inner - py, py - (sr.y + sr.h - 1 - inner)), 0);
            int d2 = ddx * ddx + ddy * ddy;
            int dist4 = d2 < 4096 ? sqrt_tab[d2] : 255;      /* quarter pixels */
            int t = dist4 - inner * 4;                       /* outside the shape, quarter pixels */
            if (t >= size * 4)
                continue;
            int a = t <= 0 ? max_alpha : max_alpha * (size * 4 - t) * (size * 4 - t) / (size * size * 16);
            if (a > 0)
                row[px] = blend(row[px], RGB(0, 0, 0), a);
        }
    }
}

/* Text drawn twice, one pixel apart: the bitmap font's bold. */
int gfx_text_bold(struct surface *s, int x, int y, const char *str, color_t fg)
{
    gfx_text(s, x + 1, y, str, fg);
    return gfx_text(s, x, y, str, fg) + 1;
}

/* Rounded rectangle filled with a vertical gradient. */
void gfx_round_rect_vgradient(struct surface *s, int x, int y, int w, int h, int rad, color_t top, color_t bottom)
{
    rad = MIN(rad, MIN(w, h) / 2);
    int y0 = MAX(y, s->clip.y), y1 = MIN(y + h, s->clip.y + s->clip.h);
    for (int yy = y0; yy < y1; yy++) {
        int j = yy - y, fromb = y + h - 1 - yy;
        color_t c = color_mix(top, bottom, h > 1 ? j * 256 / (h - 1) : 0);
        round_span(s, x, yy, w, corner_inset(rad, MIN(j, fromb)), c, 256);
    }
}
