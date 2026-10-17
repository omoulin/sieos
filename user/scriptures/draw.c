/*
 * draw.c - The drawing layer (draw.h), after SIEOS Atlas's draw.c: the same
 * palettes, the same looks, the same arithmetic (integers only).
 *
 * Unlike Atlas (a display list rasterised in strips, straight to video
 * memory), the picture here is a whole image in memory, drawn in palette
 * colours; draw_present() then sends it through the look into the window.
 */
#include "draw.h"
#include "font.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

uint32_t pal[NPAL];
const char *LOOK_NAMES[NLOOK] = { "Phosphor", "Amber 1984", "Glass", "BeOS" };

static const uint32_t pal_dark[NPAL] = {           /* (the order of PAL_...; Atlas's) */
    0x0a0f1c, 0x141a28, 0x2a3245, 0x4be3c1, 0xe6ecf5, 0x8fa0b8, 0x262e40, 0x070b14, 0x9fe8c8, 0xff9b8a,
    0x1d5a52, 0x111829, 0x0a0f1c, 0x0a0f1c, 0x0a0f1c, 0x0a0f1c, 0x4be3c1 };
static const uint32_t pal_beos[NPAL] = {           /* BeOS R5: the blue desk, grey panels, the yellow tab */
    0x336698, 0xdedede, 0x8a8a8a, 0xffcb00, 0x000000, 0x505050, 0xc4c4c4, 0xffffff, 0x000000, 0xc81e1e,
    0xa8c4e8, 0x336698, 0x000000, 0xd8d8d8, 0xffffff, 0x505050, 0x1c4f8c };
/* Highlights: dark enough under bright text (and distinct in brightness, which is
 * all a phosphor screen shows); BeOS: the pastels of a highlighter on paper. */
static const uint32_t hl_dark[8] = { 0x6a6020, 0x2a6034, 0x284a7a, 0x7a3029, 0x553672, 0x7a5019, 0x76365a, 0x4a4c52 };
static const uint32_t hl_beos[8] = { 0xfff07a, 0xbfe8b0, 0xb9d6f5, 0xf6b7b0, 0xdcc6f2, 0xf9cf9f, 0xf6c1dd, 0xd8d6d0 };

static int look = -1;
static uint32_t lut[2][256];        /* the look's colour of each brightness: a row, a scanline row */
static uint32_t *P;                 /* the picture */
static int W, H;
static int cx0, cy0, cx1, cy1;      /* the clip */
static int clips[16][4], nclip;

void draw_look(int k)
{
    static const uint32_t tint[2] = { 0x46ff7a, 0xffb038 };   /* phosphor, amber: at full brightness */
    look = CLAMP(k, 0, NLOOK - 1);
    memcpy(pal, look == LOOK_BEOS ? pal_beos : pal_dark, sizeof pal);
    if (look != LOOK_PHOSPHOR && look != LOOK_AMBER) return;
    for (int s = 0; s < 2; s++)
        for (int l = 0; l < 256; l++) {
            int v = l * 5 / 4 > 255 ? 255 : l * 5 / 4;        /* (a little brighter: dim things stay legible) */
            if (s) v = look == LOOK_PHOSPHOR ? v * (140 + 116 * v / 255) / 256   /* bloom: the brighter, the less a */
                                             : v * (180 + 76 * v / 255) / 256;   /* scanline darkens (a CRT's beam spot) */
            uint32_t t = tint[look == LOOK_AMBER];
            lut[s][l] = ((t >> 16 & 255) * v / 255) << 16 | ((t >> 8 & 255) * v / 255) << 8 | (t & 255) * v / 255;
        }
}
int draw_look_get(void) { return look; }
uint32_t hl_color(int i) { return (look == LOOK_BEOS ? hl_beos : hl_dark)[i & 7]; }

static int scanline(int y) { return look == LOOK_PHOSPHOR ? y % 3 == 2 : y % 4 == 3; }
static int round_r(int r) { return look == LOOK_AMBER ? 0 : look == LOOK_BEOS ? (r > 3 ? 3 : r) : r; }
static uint32_t avg(uint32_t a, uint32_t b) { return ((a & 0xfefefe) >> 1) + ((b & 0xfefefe) >> 1); }
static uint32_t halo(uint32_t a, uint32_t c) { return avg(a, avg(a, c)); }   /* a quarter of c over a: text's glow */

void draw_target(uint32_t *px, int w, int h)
{
    P = px; W = w; H = h;
    nclip = 0;
    cx0 = cy0 = 0; cx1 = w; cy1 = h;
}

void draw_clip(int x0, int y0, int x1, int y1)
{
    if (nclip < 16) { clips[nclip][0] = cx0; clips[nclip][1] = cy0; clips[nclip][2] = cx1; clips[nclip][3] = cy1; nclip++; }
    cx0 = MAX(cx0, x0); cy0 = MAX(cy0, y0); cx1 = MIN(cx1, x1); cy1 = MIN(cy1, y1);
}
void draw_unclip(void)
{
    if (!nclip) { cx0 = cy0 = 0; cx1 = W; cy1 = H; return; }
    nclip--;
    cx0 = clips[nclip][0]; cy0 = clips[nclip][1]; cx1 = clips[nclip][2]; cy1 = clips[nclip][3];
}

static int isqrt(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }
static int inset(int y, int y0, int y1, int r)          /* how far a rounded corner pulls row y in */
{
    if (r <= 0) return 0;
    int d = y < y0 + r ? y0 + r - y : y >= y1 - r ? y - (y1 - r) + 1 : 0;
    return d ? r - isqrt(r * r - (d - 1) * (d - 1)) : 0;
}
static void span(int y, int x0, int x1, uint32_t c)
{
    if (y < cy0 || y >= cy1) return;
    x0 = MAX(x0, cx0); x1 = MIN(x1, cx1);
    uint32_t *d = P + (size_t)y * W;
    for (int x = x0; x < x1; x++) d[x] = c;
}

void draw_rect(int x0, int y0, int x1, int y1, int r, uint32_t c)
{
    r = MIN(round_r(r), MIN(x1 - x0, y1 - y0) / 2);
    for (int y = MAX(y0, cy0); y < MIN(y1, cy1); y++) {
        int i = inset(y, y0, y1, r);
        span(y, x0 + i, x1 - i, c);
    }
}

void draw_top_round(int x0, int y0, int x1, int y1, int r, uint32_t c)
{
    r = MIN(round_r(r), MIN(x1 - x0, y1 - y0) / 2);
    for (int y = MAX(y0, cy0); y < MIN(y1, cy1); y++) {
        int i = y < y0 + r ? inset(y, y0, y0 + 2 * r, r) : 0;
        span(y, x0 + i, x1 - i, c);
    }
}

void draw_frame(int x0, int y0, int x1, int y1, int r, int t, uint32_t c)
{
    r = MIN(round_r(r), MIN(x1 - x0, y1 - y0) / 2);
    int ir = r > t ? r - t : 0;
    for (int y = MAX(y0, cy0); y < MIN(y1, cy1); y++) {
        int i = inset(y, y0, y1, r);
        if (y < y0 + t || y >= y1 - t) { span(y, x0 + i, x1 - i, c); continue; }
        int j = inset(y, y0 + t, y1 - t, ir);
        span(y, x0 + i, x0 + t + j, c);
        span(y, x1 - t - j, x1 - i, c);
    }
}

void draw_shade(int x0, int y0, int x1, int y1, int r, int k)
{
    r = round_r(r);
    for (int y = MAX(y0, cy0); y < MIN(y1, cy1); y++) {
        int i = inset(y, y0, y1, r), a = MAX(x0 + i, cx0), b = MIN(x1 - i, cx1);
        uint32_t *d = P + (size_t)y * W;
        if (k >= 2) for (int x = a; x < b; x++) d[x] = d[x] >> 1 & 0x7f7f7f;
        else for (int x = a; x < b; x++) d[x] -= d[x] >> 2 & 0x3f3f3f;
    }
}

/* The desk: its colour, grid lines every `step` (Amber: dots where they cross), and in
 * the CRT looks a vignette: darker towards the corners, as a curved tube is. */
void draw_desk(int x0, int y0, int x1, int y1, int step)
{
    int crt = look == LOOK_PHOSPHOR || look == LOOK_AMBER;
    for (int y = MAX(y0, cy0); y < MIN(y1, cy1); y++) {
        int on = step > 0 && (y - y0) % step == 0;
        uint32_t *d = P + (size_t)y * W;
        int a = MAX(x0, cx0), b = MIN(x1, cx1);
        for (int x = a; x < b; x++) {
            int col = step > 0 && (x - x0) % step == 0;
            uint32_t c = look == LOOK_AMBER ? (on && col ? C_GRID : C_BG) : (on || col ? C_GRID : C_BG);
            if (look == LOOK_BEOS) c = C_BG;
            if (crt) {
                int tx = (int)((int64_t)abs(2 * (x - x0) - (x1 - x0)) * 256 / MAX(1, x1 - x0));
                int ty = (int)((int64_t)abs(2 * (y - y0) - (y1 - y0)) * 256 / MAX(1, y1 - y0));
                int vx = tx <= 150 ? 256 : 256 - (tx - 150) * 60 / 106, vy = ty <= 150 ? 256 : 256 - (ty - 150) * 60 / 106;
                uint32_t f = (uint32_t)(vx * vy) >> 8;
                if (f < 256) c = ((c >> 16 & 255) * f >> 8) << 16 | ((c >> 8 & 255) * f >> 8) << 8 | (c & 255) * f >> 8;
            }
            d[x] = c;
        }
    }
}

int draw_text(int x, int y, const char *s, int n, int face, int px, uint32_t c)
{
    int glow = look == LOOK_PHOSPHOR || look == LOOK_AMBER;
    for (int i = 0, k; i < n && s[i]; i += k) {
        int cp = utf8_dec(s + i, &k);
        if (cp == '\n' || cp == 0xad) continue;
        const glyph_t *g = font_glyph(face, px, cp);
        int gx = x + g->x0, gy = y + g->y0;
        if (gx < cx1 && gx + g->w > cx0 && gy < cy1 && gy + g->h > cy0)
            for (int r = 0; r < g->h; r++) {
                int yy = gy + r;
                if (yy < cy0 || yy >= cy1) continue;
                uint32_t *d = P + (size_t)yy * W;
                const uint8_t *src = g->a + r * g->w;
                for (int col = 0; col < g->w; col++) {
                    int xx = gx + col, a = src[col];
                    if (!a || xx < cx0 || xx >= cx1) continue;
                    a += a >> 7;                                 /* (0..255 -> 0..256) */
                    if (glow && a >= 200) {                      /* the halo: beside the stroke */
                        if (xx > cx0 && d[xx - 1] != c) d[xx - 1] = halo(d[xx - 1], c);
                        if (xx + 1 < cx1) d[xx + 1] = halo(d[xx + 1], c);
                    }
                    uint32_t o = d[xx];
                    d[xx] = a >= 256 ? c : ((((o >> 16 & 255) * (256 - a) + (c >> 16 & 255) * a) >> 8) << 16) |
                                           ((((o >> 8 & 255) * (256 - a) + (c >> 8 & 255) * a) >> 8) << 8) |
                                           (((o & 255) * (256 - a) + (c & 255) * a) >> 8);
                }
            }
        x += g->adv;
    }
    return x;
}

int text_w(const char *s, int n, int face, int px)
{
    int w = 0;
    for (int i = 0, k; i < n && s[i]; i += k) {
        int cp = utf8_dec(s + i, &k);
        if (cp != '\n' && cp != 0xad) w += font_glyph(face, px, cp)->adv;
    }
    return w;
}

int text_fit(const char *s, int n, int face, int px, int maxw)
{
    int w = 0, i = 0;
    for (int k; i < n && s[i]; i += k) {
        int a = font_glyph(face, px, utf8_dec(s + i, &k))->adv;
        if (w + a > maxw) break;
        w += a;
    }
    return i;
}

void draw_text_clipped(int x, int y, int maxw, const char *s, int face, int px, uint32_t c)
{
    int n = (int)strlen(s);
    if (text_w(s, n, face, px) <= maxw) { draw_text(x, y, s, n, face, px, c); return; }
    int ell = text_w("…", 3, face, px), k = text_fit(s, n, face, px, maxw - ell);
    x = draw_text(x, y, s, k, face, px, c);
    draw_text(x, y, "…", 3, face, px, c);
}

void draw_present(uint32_t *out, int pitch)
{
    for (int y = 0; y < H; y++) {
        const uint32_t *src = P + (size_t)y * W;
        uint32_t *d = out + (size_t)y * pitch;
        if (look == LOOK_PHOSPHOR || look == LOOK_AMBER) {    /* each pixel: its brightness, in the look's colour */
            const uint32_t *t = lut[scanline(y)];
            for (int x = 0; x < W; x++) {
                uint32_t c = src[x];
                d[x] = t[((c >> 16 & 255) * 77 + (c >> 8 & 255) * 150 + (c & 255) * 29) >> 8];
            }
        } else memcpy(d, src, (size_t)W * 4);
    }
}
