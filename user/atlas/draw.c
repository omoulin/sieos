/*
 * draw.c - The drawing layer of the desktop.
 *
 * The desktop describes a picture as a "display list": a background grid,
 * rectangles (rounded or not), outlines and text. present(region) turns
 * that region of the picture into pixels, 16 rows at a time, in a strip
 * buffer of 1920 x 16 pixels (120 KiB), and copies each finished strip into
 * video memory. So:
 *   - there is no second full-screen image in memory (8 MiB saved), yet
 *     nothing flickers: a pixel goes to the screen once, finished;
 *   - only the region that changed is redrawn (a moved pointer: two small
 *     squares; a terminal's new line: that window).
 * No floating point (programs are built without it): integers throughout.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "draw.h"

extern const uint8_t font[95][9];   /* font.c (tools/mkfont.py) */

#define STRIP 16
#define MAXP  2048                  /* display list entries (more are dropped) */
#define ARENA (32 << 10)            /* the display list's text */

enum { P_GRID, P_RECT, P_FRAME, P_TEXT };
typedef struct {
    uint8_t type;
    uint16_t r, t, len;
    int scale;                      /* text: 16.16 */
    int x0, y0, x1, y1;             /* its box on the screen (may extend beyond it) */
    int cx0, cy0, cx1, cy1;         /* text: clipped to this box */
    uint32_t c, c2, text;           /* colours; text: where its characters are in the arena */
} prim_t;

static uint32_t *fb;                /* video memory */
static int pitch, swap;             /* (screen_t) */
int SW = 1920, SH = 1080;
static uint32_t *strip;              /* SW x STRIP pixels (draw_init) */
static prim_t dl[MAXP];
static int np, arena_n, ptr_x = -100, ptr_y = -100;
static char arena[ARENA];

void draw_init(const screen_t *s)
{
    fb = s->fb; pitch = s->pitch; swap = s->swap;
    strip = malloc((size_t)SW * STRIP * 4);
}
void dl_reset(void) { np = 0; arena_n = 0; }
int  dl_count(void) { return np; }
int  text_w(int n, int scale) { return (int)((int64_t)n * CW * scale >> 16); }
void dl_pointer(int x, int y) { ptr_x = x; ptr_y = y; }

static prim_t *add(int type, int x0, int y0, int x1, int y1, uint32_t c)
{
    if (np == MAXP || x1 <= 0 || y1 <= 0 || x0 >= SW || y0 >= SH || x1 <= x0 || y1 <= y0) return 0;
    prim_t *p = &dl[np++];
    *p = (prim_t){ .type = type, .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1, .c = c };
    return p;
}

void dl_grid(int ox, int oy, int step, uint32_t bg, uint32_t line)
{
    prim_t *p = add(P_GRID, 0, 0, SW, SH, bg);
    if (p) { p->c2 = line; p->cx0 = ox; p->cy0 = oy; p->r = step; }
}

void dl_rect(int x0, int y0, int x1, int y1, int r, uint32_t c)
{
    prim_t *p = add(P_RECT, x0, y0, x1, y1, c);
    if (p) p->r = r;
}

void dl_frame(int x0, int y0, int x1, int y1, int r, int t, uint32_t c)
{
    prim_t *p = add(P_FRAME, x0, y0, x1, y1, c);
    if (p) { p->r = r; p->t = t; }
}

void dl_text(int x, int y, const char *s, int n, int scale, uint32_t c, int cx0, int cy0, int cx1, int cy1)
{
    if (n <= 0 || arena_n + n > ARENA) return;
    if (cx0 < x) cx0 = x;
    if (cy0 < y) cy0 = y;
    int x1 = x + text_w(n, scale), y1 = y + (int)((int64_t)CH * scale >> 16);
    if (cx1 > x1) cx1 = x1;
    if (cy1 > y1) cy1 = y1;
    prim_t *p = add(P_TEXT, cx0, cy0, cx1, cy1, c);
    if (!p) return;
    p->cx0 = x; p->cy0 = y;          /* (for text, cx0/cy0 hold where the text starts) */
    p->scale = scale; p->len = n; p->text = arena_n;
    memcpy(arena + arena_n, s, n);
    arena_n += n;
}

static int isqrt(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }

/* How far row y of a box [y0, y1) with corner radius r starts inside. */
static int inset(int y, int y0, int y1, int r)
{
    if (r <= 0) return 0;
    int d = y < y0 + r ? y0 + r - y : y >= y1 - r ? y - (y1 - r) + 1 : 0;
    return d ? r - isqrt(r * r - (d - 1) * (d - 1)) : 0;
}

/* Fill [x0, x1) of strip row `row`, clipped to [lo, hi). */
static void span(int row, int x0, int x1, int lo, int hi, uint32_t c)
{
    if (x0 < lo) x0 = lo;
    if (x1 > hi) x1 = hi;
    uint32_t *d = strip + row * SW;
    for (int x = x0; x < x1; x++) d[x] = c;
}

/* Rasterize primitive p in screen rows [sy, sy + n), columns [lo, hi). */
static void raster(const prim_t *p, int sy, int n, int lo, int hi)
{
    int ya = p->y0 > sy ? p->y0 : sy, yb = p->y1 < sy + n ? p->y1 : sy + n;
    if (ya >= yb || p->x1 <= lo || p->x0 >= hi) return;
    switch (p->type) {
    case P_GRID:
        for (int y = ya; y < yb; y++) {
            int on = ((y - p->cy0) % p->r + p->r) % p->r == 0;
            span(y - sy, lo, hi, lo, hi, on ? p->c2 : p->c);
            if (on) continue;
            uint32_t *d = strip + (y - sy) * SW;
            for (int x = lo + ((p->cx0 - lo) % p->r + p->r) % p->r; x < hi; x += p->r) d[x] = p->c2;
        }
        break;
    case P_RECT:
        for (int y = ya; y < yb; y++) {
            int i = inset(y, p->y0, p->y1, p->r);
            span(y - sy, p->x0 + i, p->x1 - i, lo, hi, p->c);
        }
        break;
    case P_FRAME: {
        int t = p->t, ir = p->r > t ? p->r - t : 0;
        for (int y = ya; y < yb; y++) {
            int i = inset(y, p->y0, p->y1, p->r);
            if (y < p->y0 + t || y >= p->y1 - t) { span(y - sy, p->x0 + i, p->x1 - i, lo, hi, p->c); continue; }
            int j = inset(y, p->y0 + t, p->y1 - t, ir);
            span(y - sy, p->x0 + i, p->x0 + t + j, lo, hi, p->c);
            span(y - sy, p->x1 - t - j, p->x1 - i, lo, hi, p->c);
        }
        break;
    }
    case P_TEXT: {                   /* each screen pixel looks up its font pixel (nearest) */
        int s = p->scale, cl = p->x0 > lo ? p->x0 : lo, cr = p->x1 < hi ? p->x1 : hi;
        for (int y = ya; y < yb; y++) {
            int gy = (int)(((int64_t)(y - p->cy0) << 16) / s) - 1;   /* glyph row (one blank row above) */
            if (gy < 0 || gy >= 9) continue;
            uint32_t *d = strip + (y - sy) * SW;
            int64_t step = ((int64_t)1 << 32) / s, acc = ((int64_t)(cl - p->cx0) << 32) / s;
            for (int x = cl; x < cr; x++, acc += step) {
                int fx = (int)(acc >> 16);                       /* font pixels from the text's start */
                int k = fx / CW, b = fx % CW;
                if (k >= p->len || b >= 5) continue;
                unsigned ch = (uint8_t)arena[p->text + k];
                if (font[ch >= 32 && ch < 127 ? ch - 32 : '?' - 32][gy] & 0x10 >> b) d[x] = p->c;
            }
        }
        break;
    }
    }
}

/* The mouse pointer: an arrow, 12 x 19, white with a dark outline. */
static const char *arrow[19] = {
    "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X",
    "X........X", "X.........X", "X..........X", "X......XXXXX", "X...X..X", "X..XX..X",
    "X.X  X..X", "XX   X..X", "     X..X", "      XX" };

static void pointer(int sy, int n, int lo, int hi)
{
    for (int r = 0; r < 19; r++) {
        int y = ptr_y + r;
        if (y < sy || y >= sy + n) continue;
        for (int i = 0; arrow[r][i]; i++) {
            int x = ptr_x + i;
            if (x < lo || x >= hi || arrow[r][i] == ' ') continue;
            strip[(y - sy) * SW + x] = arrow[r][i] == 'X' ? 0x0a1322 : 0xffffff;
        }
    }
}

void present(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SW) x1 = SW;
    if (y1 > SH) y1 = SH;
    if (x0 >= x1 || y0 >= y1) return;
    for (int sy = y0; sy < y1; sy += STRIP) {
        int n = y1 - sy < STRIP ? y1 - sy : STRIP;
        for (int i = 0; i < np; i++) raster(&dl[i], sy, n, x0, x1);
        pointer(sy, n, x0, x1);
        for (int r = 0; r < n; r++) {
            uint32_t *d = fb + (sy + r) * pitch + x0, *src = strip + r * SW + x0;
            if (!swap) memcpy(d, src, (x1 - x0) * 4);
            else for (int i = 0; i < x1 - x0; i++) { uint32_t c = src[i]; d[i] = (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF); }
        }
    }
}
