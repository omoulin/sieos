/*
 * draw.c - The drawing layer of the desktop.
 *
 * The desktop describes a picture as a "display list": a background grid,
 * rectangles (rounded or not), outlines, text and small pixel images (the
 * icons: a map of characters, each one a colour of a palette). present(region) turns
 * that region of the picture into pixels, 16 rows at a time, in a strip
 * buffer of 1920 x 16 pixels (120 KiB), and copies each finished strip into
 * video memory. So:
 *   - there is no second full-screen image in memory (8 MiB saved), yet
 *     nothing flickers: a pixel goes to the screen once, finished;
 *   - only the region that changed is redrawn (a moved pointer: two small
 *     squares; a terminal's new line: that window).
 * No floating point (programs are built without it): integers throughout.
 *
 * The zoom (draw_zoom): atlas lays out a picture of SW x SH, the screen's
 * PW x PH divided by the zoom; each item is scaled to the screen as it is
 * added (its box, corners, lines, text, icons), so it is drawn sharp at
 * any zoom, not a picture stretched. The pointer's position comes in as
 * a fraction of the screen, so it lands in the picture's coordinates.
 *
 * Looks (draw_look): the picture is drawn in the desktop's own colours,
 * then each pixel goes to the screen through the look:
 *   Phosphor    a green monitor: every colour becomes its brightness in
 *               phosphor green; every third row is darker (scanlines);
 *               text glows (a dim halo on each side of its strokes)
 *   Amber 1984  the same in amber, with square corners, a grid of dots
 *               and lighter scanlines (every fourth row)
 *   Glass       the colours as drawn
 * So everything (icons, the pointer, a window's insides) changes with it,
 * and nothing else in the desktop knows about looks.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "draw.h"

extern const uint8_t font[95][9];   /* font.c (tools/mkfont.py) */

#define STRIP 16
#define MAXP  4096                  /* display list entries (more are dropped) */
#define ARENA (32 << 10)            /* the display list's text */

enum { P_GRID, P_RECT, P_FRAME, P_TEXT, P_PIXMAP };
typedef struct {
    uint8_t type;
    uint16_t r, t, len;
    int scale;                      /* text: 16.16 */
    int x0, y0, x1, y1;             /* its box on the screen (may extend beyond it) */
    int cx0, cy0, cx1, cy1;         /* text: clipped to this box */
    uint32_t c, c2, text;           /* colours; text: where its characters are in the arena */
    const char *const *rows;        /* a pixmap: its rows, ... */
    const char *keys;               /* ... the characters of its palette, ... */
    const uint32_t *cols;           /* ... their colours ('.' and ' ': none) */
} prim_t;

static uint32_t *fb;                /* video memory */
static int pitch, swap;             /* (screen_t) */
int PW = 1920, PH = 1080;          /* the screen, in pixels */
int SW = 2560, SH = 1440;           /* the picture atlas lays out: the screen / the zoom */
static int Z = 49152;               /* the zoom, 16.16: screen pixels per picture pixel (0.75: 100%) */
static uint32_t *strip;              /* PW x STRIP pixels (draw_init) */
static prim_t dl[MAXP];
static int np, arena_n, ptr_x = -100, ptr_y = -100;
static char arena[ARENA];
static int look = -1;               /* LOOK_..., -1: none set yet (draw_look) */
static uint32_t lut[2][256];        /* the look's colour of each brightness: a row, a scanline row */

void draw_look(int k)
{
    static const uint32_t tint[2] = { 0x46ff7a, 0xffb038 };   /* phosphor, amber: at full brightness */
    look = k;
    if (k != LOOK_PHOSPHOR && k != LOOK_AMBER) return;
    for (int s = 0; s < 2; s++)
        for (int l = 0; l < 256; l++) {
            int v = l * 5 / 4 > 255 ? 255 : l * 5 / 4;        /* (a little brighter: dim things stay legible) */
            if (s) v = k == LOOK_PHOSPHOR ? v * 5 / 8 : v * 3 / 4;
            uint32_t t = tint[k == LOOK_AMBER];
            lut[s][l] = ((t >> 16 & 255) * v / 255) << 16 | ((t >> 8 & 255) * v / 255) << 8 | (t & 255) * v / 255;
        }
}
int draw_look_get(void) { return look; }
static int scanline(int y) { return look == LOOK_PHOSPHOR ? y % 3 == 2 : y % 4 == 3; }
static int round_r(int r) { return look == LOOK_AMBER ? 0 : r; }   /* (amber: square corners) */
static uint32_t avg(uint32_t a, uint32_t b) { return ((a & 0xfefefe) >> 1) + ((b & 0xfefefe) >> 1); }
static uint32_t halo(uint32_t a, uint32_t c) { return avg(a, avg(a, c)); }   /* a quarter of c over a: text's glow */

static int sc(int v) { return (int)(((int64_t)v * Z) >> 16); }                 /* a position */
static int sz(int v) { return v <= 0 ? 0 : (int)(((int64_t)v * Z + 32768) >> 16); }   /* a size, rounded */
void draw_zoom(int z)
{
    Z = z < 16384 ? 16384 : z > 262144 ? 262144 : z;
    SW = (int)(((int64_t)PW << 16) / Z); SH = (int)(((int64_t)PH << 16) / Z);
}
int draw_zoom_get(void) { return Z; }

void draw_init(const screen_t *s)
{
    fb = s->fb; pitch = s->pitch; swap = s->swap;
    free(strip);                    /* (again after a change of size) */
    strip = malloc((size_t)PW * STRIP * 4);
    draw_zoom(Z);                   /* (the picture's size, for this screen) */
}
void dl_reset(void) { np = 0; arena_n = 0; }
int  dl_count(void) { return np; }
int  text_w(int n, int scale) { return (int)((int64_t)n * CW * scale >> 16); }
void dl_pointer(int x, int y) { ptr_x = sc(x); ptr_y = sc(y); }

static prim_t *add(int type, int x0, int y0, int x1, int y1, uint32_t c)
{
    if (np == MAXP || x1 <= 0 || y1 <= 0 || x0 >= PW || y0 >= PH || x1 <= x0 || y1 <= y0) return 0;
    prim_t *p = &dl[np++];
    *p = (prim_t){ .type = type, .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1, .c = c };
    return p;
}

void dl_grid(int ox, int oy, int step, uint32_t bg, uint32_t line)
{
    prim_t *p = add(P_GRID, 0, 0, PW, PH, bg);
    step = sz(step);
    if (p) { p->c2 = line; p->cx0 = sc(ox); p->cy0 = sc(oy); p->r = step < 2 ? 2 : step; }
}

/* A box to the screen: a box at least a pixel wide stays one (a thin line
 * never vanishes when zoomed out). */
static void box(int *x0, int *y0, int *x1, int *y1)
{
    int a = sc(*x0), b = sc(*y0), c = sc(*x1), d = sc(*y1);
    if (c <= a && *x1 > *x0) c = a + 1;
    if (d <= b && *y1 > *y0) d = b + 1;
    *x0 = a; *y0 = b; *x1 = c; *y1 = d;
}

void dl_rect(int x0, int y0, int x1, int y1, int r, uint32_t c)
{
    box(&x0, &y0, &x1, &y1);
    prim_t *p = add(P_RECT, x0, y0, x1, y1, c);
    if (p) p->r = sz(r);
}

void dl_frame(int x0, int y0, int x1, int y1, int r, int t, uint32_t c)
{
    box(&x0, &y0, &x1, &y1);
    prim_t *p = add(P_FRAME, x0, y0, x1, y1, c);
    if (p) { p->r = sz(r); p->t = t > 0 && sz(t) < 1 ? 1 : sz(t); }
}

void dl_text(int x, int y, const char *s, int n, int scale, uint32_t c, int cx0, int cy0, int cx1, int cy1)
{
    if (n <= 0 || arena_n + n > ARENA) return;
    x = sc(x); y = sc(y); scale = (int)(((int64_t)scale * Z) >> 16);
    box(&cx0, &cy0, &cx1, &cy1);
    if (scale < 4096) scale = 4096;
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

void dl_pixmap(int x, int y, const char *const *rows, int w, int h, const char *keys, const uint32_t *cols, int scale)
{
    int s = (int)(((int64_t)scale * Z) < 16384 ? 16384 : (int64_t)scale * Z);   /* 16.16: screen pixels per map pixel */
    x = sc(x); y = sc(y);
    prim_t *p = add(P_PIXMAP, x, y, x + (int)(((int64_t)w * s + 65535) >> 16), y + (int)(((int64_t)h * s + 65535) >> 16), 0);
    if (p) { p->rows = rows; p->keys = keys; p->cols = cols; p->scale = s; p->r = w; p->t = h; }
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
    uint32_t *d = strip + row * PW;
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
            if (look == LOOK_AMBER) {                    /* (a grid of dots: only where the lines cross) */
                span(y - sy, lo, hi, lo, hi, p->c);
                if (!on) continue;
            } else {
                span(y - sy, lo, hi, lo, hi, on ? p->c2 : p->c);
                if (on) continue;
            }
            uint32_t *d = strip + (y - sy) * PW;
            for (int x = lo + ((p->cx0 - lo) % p->r + p->r) % p->r; x < hi; x += p->r) d[x] = p->c2;
        }
        break;
    case P_RECT:
        for (int y = ya; y < yb; y++) {
            int i = inset(y, p->y0, p->y1, round_r(p->r));
            span(y - sy, p->x0 + i, p->x1 - i, lo, hi, p->c);
        }
        break;
    case P_FRAME: {
        int r = round_r(p->r), t = p->t, ir = r > t ? r - t : 0;
        for (int y = ya; y < yb; y++) {
            int i = inset(y, p->y0, p->y1, r);
            if (y < p->y0 + t || y >= p->y1 - t) { span(y - sy, p->x0 + i, p->x1 - i, lo, hi, p->c); continue; }
            int j = inset(y, p->y0 + t, p->y1 - t, ir);
            span(y - sy, p->x0 + i, p->x0 + t + j, lo, hi, p->c);
            span(y - sy, p->x1 - t - j, p->x1 - i, lo, hi, p->c);
        }
        break;
    }
    case P_PIXMAP:                   /* each screen pixel: its map character's colour */
        for (int y = ya; y < yb; y++) {
            int ry = (int)(((int64_t)(y - p->y0) << 16) / p->scale);
            if (ry >= p->t) continue;
            const char *row = p->rows[ry];
            uint32_t *d = strip + (y - sy) * PW;
            for (int x = p->x0 > lo ? p->x0 : lo; x < p->x1 && x < hi; x++) {
                int rx = (int)(((int64_t)(x - p->x0) << 16) / p->scale);
                if (rx >= p->r) break;
                char ch = row[rx];
                if (ch == '.' || ch == ' ') continue;
                const char *k = strchr(p->keys, ch);
                if (k) d[x] = p->cols[k - p->keys];
            }
        }
        break;
    case P_TEXT: {                   /* each screen pixel looks up its font pixel (nearest) */
        int s = p->scale, cl = p->x0 > lo ? p->x0 : lo, cr = p->x1 < hi ? p->x1 : hi;
        int glow = look == LOOK_PHOSPHOR || look == LOOK_AMBER;
        for (int y = ya; y < yb; y++) {
            int gy = (int)(((int64_t)(y - p->cy0) << 16) / s) - 1;   /* glyph row (one blank row above) */
            if (gy < 0 || gy >= 9) continue;
            uint32_t *d = strip + (y - sy) * PW;
            int64_t step = ((int64_t)1 << 32) / s, acc = ((int64_t)(cl - p->cx0) << 32) / s;
            for (int x = cl; x < cr; x++, acc += step) {
                int fx = (int)(acc >> 16);                       /* font pixels from the text's start */
                int k = fx / CW, b = fx % CW;
                if (k >= p->len || b >= 5) continue;
                unsigned ch = (uint8_t)arena[p->text + k];
                if (!(font[ch >= 32 && ch < 127 ? ch - 32 : '?' - 32][gy] & 0x10 >> b)) continue;
                if (glow) {                                      /* (the halo: beside the stroke, never on one) */
                    if (x > cl && d[x - 1] != p->c) d[x - 1] = halo(d[x - 1], p->c);
                    if (x + 1 < cr) d[x + 1] = halo(d[x + 1], p->c);
                }
                d[x] = p->c;
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
            strip[(y - sy) * PW + x] = arrow[r][i] == 'X' ? 0x0a1322 : 0xffffff;
        }
    }
}

void present(int x0, int y0, int x1, int y1)          /* (a box of the picture) */
{
    x0 = sc(x0); y0 = sc(y0);
    x1 = (int)(((int64_t)x1 * Z + 65535) >> 16); y1 = (int)(((int64_t)y1 * Z + 65535) >> 16);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > PW) x1 = PW;
    if (y1 > PH) y1 = PH;
    if (x0 >= x1 || y0 >= y1) return;
    for (int sy = y0; sy < y1; sy += STRIP) {
        int n = y1 - sy < STRIP ? y1 - sy : STRIP;
        for (int i = 0; i < np; i++) raster(&dl[i], sy, n, x0, x1);
        pointer(sy, n, x0, x1);
        for (int r = 0; r < n; r++) {
            uint32_t *d = fb + (sy + r) * pitch + x0, *src = strip + r * PW + x0;
            if (look == LOOK_PHOSPHOR || look == LOOK_AMBER) {   /* each pixel: its brightness, in the look's colour */
                const uint32_t *t = lut[scanline(sy + r)];
                for (int i = 0; i < x1 - x0; i++) {
                    uint32_t c = src[i], v = t[((c >> 16 & 255) * 77 + (c >> 8 & 255) * 150 + (c & 255) * 29) >> 8];
                    d[i] = swap ? (v & 0xFF) << 16 | (v & 0xFF00) | (v >> 16 & 0xFF) : v;
                }
            } else if (!swap) memcpy(d, src, (x1 - x0) * 4);
            else for (int i = 0; i < x1 - x0; i++) { uint32_t c = src[i]; d[i] = (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF); }
        }
    }
}
