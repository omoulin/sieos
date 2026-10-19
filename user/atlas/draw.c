/*
 * draw.c - The drawing layer of the desktop.
 *
 * The desktop describes a picture as a "display list": a background grid,
 * rectangles (rounded or not), outlines, text and small pixel images (the
 * icons: a map of characters, each one a colour of a palette). present(region) turns
 * that region of the picture into pixels, 128 rows at a time, in a strip
 * buffer (1920 x 208 pixels with what is around it: 1.6 MiB), and copies
 * each finished strip into video memory. So:
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
 * The look: glass. The desk is a deep navy lit by five wide glows, all
 * blues (wall_init: worked out at 1/16 of the screen once, then smoothed
 * to each pixel as it is drawn). Glass (a FROST colour: dl_rect, dl_glass)
 * shows what is drawn behind it, the desk or other windows, blurred and
 * more vivid, its tint laid over it as much as its ALPHA says: clear
 * (the bars, a window's frame), milky (menus, cards) or smoked (a
 * terminal); a light edge, a bright line inside its top. A GLOSS colour
 * (the accent) is solid with that top line. Glass needs what is around it:
 * a strip with glass in it is drawn with GM rows and columns more around
 * it (kept in the strip buffer, not shown), the blur worked out at 1/GQ
 * of the screen's size, and what will be hidden under something opaque
 * drawn later is not drawn. Other programs' windows' insides (dl_client)
 * are marked in the strip (bit 24, unused by colours) so a shadow falling
 * on them darkens them without tinting them.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "draw.h"

extern const uint8_t font[95][9];   /* font.c (tools/mkfont.py) */
extern const uint8_t font15[95][16][9];   /* font15.c (tools/mkfont15.py): the native one at 1.5, */
extern const uint8_t font18[95][20][11];  /* font18.c: titles, at 11/6 */
extern const uint8_t font13[95][15][8];   /* font13.c: small text, at 8/6 */
/* The native fonts: text at exactly this scale on the screen (16.16) is
 * drawn from them pixel for pixel. */
static const struct { int scale, w, h; const uint8_t *px; } natives[] = {
    { 3 << 15, 9, 16, &font15[0][0][0] }, { 120150, 11, 20, &font18[0][0][0] }, { 87382, 8, 15, &font13[0][0][0] },
};

#define STRIP 128
#define GQ 4                        /* glass: what is behind it blurred at 1/GQ of the screen's size, */
#define GB 7                        /* a tent this many of those wide each way (about 12 pixels' blur), */
#define GM 40                       /* so how far a glass pixel sees (GQ x (GB + 2), and some): drawn around a strip with glass */
#define MAXP  4096                  /* display list entries (more are dropped) */
#define ARENA (32 << 10)            /* the display list's text */

enum { P_DESK, P_RECT, P_FRAME, P_TEXT, P_PIXMAP, P_SHADE, P_IMAGE, P_PIC };
typedef struct {
    uint8_t type;
    uint16_t r, t, len;
    int scale;                      /* text: 16.16 */
    int x0, y0, x1, y1;             /* its box on the screen (may extend beyond it) */
    int cx0, cy0, cx1, cy1;         /* text: clipped to this box */
    uint32_t c, c2, text;           /* colours; text: where its characters are in the arena */
    const char *const *rows;        /* a pixmap: its rows, ... */
    const char *keys;               /* ... the characters of its palette, ... */
    const uint32_t *cols;           /* ... their colours ('.' and ' ': none); an image: its pixels */
} prim_t;

static uint32_t *fb;                /* video memory */
static int pitch, swap;             /* (screen_t) */
int PW = 1920, PH = 1080;          /* the screen, in pixels */
int SW = 2560, SH = 1440;           /* the picture atlas lays out: the screen / the zoom */
static int Z = 49152;               /* the zoom, 16.16: screen pixels per picture pixel (0.75: 100%) */
static uint32_t *strip;              /* PW x (STRIP + 2 GM) pixels (draw_init): a strip and around it, */
static int org;                     /* the screen row of its first */
static uint32_t *gq, *gh, *gv;      /* the same at 1/GQ: what is behind glass (gq), blurred across (gh), then down too (gv) */
static int qw, qh;                  /* (their size) */
static uint64_t *gacc;              /* qw x 3: the blur's sums down each column */
#define MAXCV 64                    /* (what hides what: covers()) */
static struct { int i, x0, y0, x1, y1; } cv[MAXCV];
static int ncv, cur;                /* (cur: the item being drawn) */
static int ox0, oy0, ox1, oy1;      /* the part of the strip that is shown */
static prim_t dl[MAXP];
static int np, arena_n, ptr_x = -100, ptr_y = -100;
static char arena[ARENA];
static uint32_t *wall;              /* the desk at 1/16 of the screen (wall_init) */
static int wcols, wrows;
#define WG 16

uint32_t pal[NPAL] = {             /* (the order of PAL_...) */
    0x34478f, FROST | ALPHA(23) | 0xf6f8fd, 0xb9c1d6, GLOSS | 0xffc93c, 0x1a2238, 0x4a5470, 0xd9e0f2, 0xfbfcff, 0x1a2238, 0xc8322a,
    0xbcd0f7, 0x3a4a8c, 0x2a1d00, 0xf3f5fa, 0xffffff, 0x2a3350, 0x8a5200,
    FROST | ALPHA(7) | 0xffffff, 0xffffff, 0xe2e8f8, ALPHA(7) | 0xffffff, ALPHA(8) | 0xffffff, ALPHA(5) | 0xffffff,
    FROST | ALPHA(21) | 0x0e1426, 0xc9d1ec };

static int isqrt(unsigned v)
{
    unsigned r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    for (; b; b >>= 2)
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1;
    return (int)r;
}

/* The desk's colours: a deep navy under five wide glows, all blues (no
 * rainbow), where (in thousandths of the screen's width and height), how
 * far (in thousandths of its width), each fading evenly to nothing, the
 * last on top. */
static const struct { int x, y, r; uint32_t c; } lights[] = {
    { 440, 460, 196, 0x7e8ff5 }, { 60, 920, 419, 0x3d52c8 }, { 620, 880, 348, 0x2e6fd6 },
    { 860, 260, 351, 0x6fa6f5 }, { 160, 200, 391, 0x4f74f0 } };
static void wall_init(void)
{
    wcols = PW / WG + 2; wrows = PH / WG + 2;
    free(wall);
    wall = malloc((size_t)wcols * wrows * 4);
    for (int j = 0; j < wrows; j++)
        for (int i = 0; i < wcols; i++) {
            int c[3] = { 0x1c, 0x26, 0x47 };
            for (int k = 0; k < (int)(sizeof lights / sizeof *lights); k++) {
                int dx = i * WG - lights[k].x * PW / 1000, dy = j * WG - lights[k].y * PH / 1000, r = lights[k].r * PW / 1000;
                int d = isqrt((unsigned)(dx * dx + dy * dy));
                if (d >= r) continue;
                int a = 256 - d * 256 / r;
                for (int ch = 0; ch < 3; ch++) c[ch] += (((int)(lights[k].c >> (16 - 8 * ch)) & 255) - c[ch]) * a / 256;
            }
            wall[j * wcols + i] = (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | (uint32_t)c[2];
        }
}
/* Row y of the desk, pixels [lo, hi), into d: each pixel between the four
 * nearest points of the 1/16 picture (down the column once per 16 pixels,
 * then a step added per pixel along the row). */
static void wall_row(int y, int lo, int hi, uint32_t *d)
{
    const uint32_t *a = wall + (y / WG) * wcols, *b = a + wcols;
    int fy = y % WG;
    for (int x = lo; x < hi;) {
        int i = x / WG, end = (i + 1) * WG < hi ? (i + 1) * WG : hi, fx = x % WG, v[3], dv[3];
        for (int ch = 0, s = 16; ch < 3; ch++, s -= 8) {
            int l = (int)(a[i] >> s & 255) * (WG - fy) + (int)(b[i] >> s & 255) * fy;         /* (x WG) */
            int r = (int)(a[i + 1] >> s & 255) * (WG - fy) + (int)(b[i + 1] >> s & 255) * fy;
            v[ch] = l * (WG - fx) + r * fx; dv[ch] = r - l;                                   /* (x WG x WG) */
        }
        for (; x < end; x++, v[0] += dv[0], v[1] += dv[1], v[2] += dv[2])
            d[x] = (uint32_t)(v[0] >> 8) << 16 | (uint32_t)(v[1] >> 8) << 8 | (uint32_t)(v[2] >> 8);
    }
}
static uint32_t mix(uint32_t a, uint32_t b, int k)   /* k/32 of the way from a to b */
{
    int r = (int)(a >> 16 & 255), g = (int)(a >> 8 & 255), bl = (int)(a & 255);
    r += ((int)(b >> 16 & 255) - r) * k >> 5; g += ((int)(b >> 8 & 255) - g) * k >> 5; bl += ((int)(b & 255) - bl) * k >> 5;
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl;
}
uint32_t draw_shown(uint32_t c)     /* (glass: over the desk's middle blue) */
{
    return c & FROST ? mix(0x34478f, c & 0xffffff, ALPHA_OF(c)) : c & ALPHA_M ? mix(0x34478f, c & 0xffffff, ALPHA_OF(c)) : c & 0xffffff;
}

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
    free(gq); free(gh); free(gv); free(gacc);
    strip = malloc((size_t)PW * (STRIP + 2 * GM) * 4);
    qw = PW / GQ + 2; qh = (STRIP + 2 * GM) / GQ + 2;
    gq = malloc((size_t)qw * qh * 4);
    gh = malloc((size_t)qw * qh * 4);
    gv = malloc((size_t)qw * qh * 4);
    gacc = malloc((size_t)qw * 3 * 8);
    wall_init();
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
    *p = (prim_t){ .type = type, .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1, .c = type == P_RECT ? c : type == P_FRAME ? c & ~(FROST | GLOSS) : c & ~(FROST | GLOSS | ALPHA_M) };   /* (glass, gloss:
                                       fills only; see-through: fills and outlines) */
    return p;
}

void dl_desk(void) { add(P_DESK, 0, 0, PW, PH, 0); }

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

void dl_glass(int x0, int y0, int x1, int y1, int r, uint32_t c, int hx0, int hy0, int hx1, int hy1)
{
    box(&x0, &y0, &x1, &y1);
    prim_t *p = add(P_RECT, x0, y0, x1, y1, c);
    if (!p) return;
    p->r = sz(r);
    if (hx1 > hx0 && hy1 > hy0) { box(&hx0, &hy0, &hx1, &hy1); p->cx0 = hx0; p->cy0 = hy0; p->cx1 = hx1; p->cy1 = hy1; }   /* (the hole) */
}

void dl_shade(int x0, int y0, int x1, int y1, int r, int k) { dl_shadow(x0, y0, x1, y1, r, k, 0, 0, 0, 0); }
void dl_shadow(int x0, int y0, int x1, int y1, int r, int k, int hx0, int hy0, int hx1, int hy1)
{
    box(&x0, &y0, &x1, &y1);
    prim_t *p = add(P_SHADE, x0, y0, x1, y1, 0);
    if (!p) return;
    p->r = sz(r); p->t = k;
    if (hx1 > hx0 && hy1 > hy0) { box(&hx0, &hy0, &hx1, &hy1); p->cx0 = hx0; p->cy0 = hy0; p->cx1 = hx1; p->cy1 = hy1; }   /* (the hole) */
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

#define RAW 0x01000000u                /* a strip pixel from another program: shown as it is */
void dl_image(int x0, int y0, int x1, int y1, const uint32_t *px, int w, int h) { dl_client(x0, y0, x1, y1, px, w, h, 0); }
void dl_client(int x0, int y0, int x1, int y1, const uint32_t *px, int w, int h, int raw)
{
    box(&x0, &y0, &x1, &y1);
    prim_t *p = add(P_IMAGE, x0, y0, x1, y1, raw ? RAW : 0);
    if (p) { p->cols = px; p->r = w; p->scale = (int)(((int64_t)w << 16) / (x1 - x0)); p->c2 = (uint32_t)(((int64_t)h << 16) / (y1 - y0)); }
}

void dl_pic(int x0, int y0, int x1, int y1, const uint8_t *px, const uint32_t *pal, int w, int h)
{
    box(&x0, &y0, &x1, &y1);
    prim_t *p = add(P_PIC, x0, y0, x1, y1, 0);
    if (p) { p->keys = (const char *)px; p->cols = pal; p->r = w; p->t = h; }
}

/* How far row y of a box [y0, y1) with corner radius r starts inside. */
static int inset(int y, int y0, int y1, int r)
{
    if (r <= 0) return 0;
    int d = y < y0 + r ? y0 + r - y : y >= y1 - r ? y - (y1 - r) + 1 : 0;
    return d ? r - isqrt((unsigned)(r * r - (d - 1) * (d - 1))) : 0;
}

/* Fill [x0, x1) of strip row `row`, clipped to [lo, hi) (see-through: over what is there). */
static void span(int row, int x0, int x1, int lo, int hi, uint32_t c)
{
    if (x0 < lo) x0 = lo;
    if (x1 > hi) x1 = hi;
    uint32_t *d = strip + row * PW;
    if (c & ALPHA_M) for (int k = ALPHA_OF(c), x = x0; x < x1; x++) d[x] = mix(d[x], c & 0xffffff, k);
    else for (int x = x0; x < x1; x++) d[x] = c;
}

/* Glass. What is behind it is already in the strip (drawn before it): it
 * is shrunk to 1/GQ (each GQ x GQ pixels' mean), blurred there, a tent GB
 * wide each way (across the rows, then down the columns: running sums, the
 * three colours side by side in one 64-bit word, so a pixel costs the same
 * however wide the blur), and spread back over the glass's pixels between
 * the four nearest; made more vivid, then its tint laid over it (its
 * alpha), a light edge and a bright line inside its top. */
#define PK(v) ((uint64_t)((v) >> 16 & 255) << 42 | (uint64_t)((v) >> 8 & 255) << 21 | ((v) & 255))
#define TENT ((65536 + (GB + 1) * (GB + 1) - 1) / ((GB + 1) * (GB + 1)))   /* (1 / its weights' sum, 16.16) */
static uint32_t unpk(uint64_t s)
{
    return (uint32_t)(((s >> 42 & 0x1fffff) * TENT) >> 16) << 16 | (uint32_t)(((s >> 21 & 0x1fffff) * TENT) >> 16) << 8 |
           (uint32_t)(((s & 0x1fffff) * TENT) >> 16);
}
/* Row s blurred across into d, [xa, xb) (beyond [lo, hi): its end values). */
static void tent_row(const uint32_t *s, uint32_t *d, int xa, int xb, int lo, int hi)
{
#define AT(i) PK(s[(i) < lo ? lo : (i) >= hi ? hi - 1 : (i)])
    uint64_t t = 0, A = 0, B = 0;    /* t: the tent at x; A: the GB + 1 values after x; B: x and the GB before */
    for (int k = -GB; k <= GB; k++) t += AT(xa + k) * (uint64_t)(GB + 1 - (k < 0 ? -k : k));
    for (int j = xa + 1; j <= xa + GB + 1; j++) A += AT(j);
    for (int j = xa - GB; j <= xa; j++) B += AT(j);
    for (int x = xa; x < xb; x++) {
        d[x] = unpk(t);
        t = t + A - B;
        A = A + AT(x + GB + 2) - AT(x + 1);
        B = B + AT(x + 1) - AT(x - GB);
    }
#undef AT
}
/* What is behind the strip's box [xa, xb) x [ya, yb), blurred, into gv
 * (at 1/GQ: gv's (i, j) is the block of screen pixels (GQ i, GQ j) on, its
 * row j - org / GQ). The blocks used: those in the strip (sy, n, lo, hi). */
static int qo;                      /* (the block row of gq's first row: org / GQ) */
static void blur(int xa, int xb, int ya, int yb, int sy, int n, int lo, int hi)
{
    int ba = lo / GQ, bb = (hi - 1) / GQ + 1, ra = sy / GQ, rb = (sy + n - 1) / GQ + 1;   /* (the blocks there are) */
    int i0 = xa / GQ - 1, i1 = (xb - 1) / GQ + 2, j0 = ya / GQ - 1, j1 = (yb - 1) / GQ + 2;   /* (the blocks wanted) */
    if (i0 < ba) i0 = ba;
    if (i1 > bb) i1 = bb;
    if (j0 < ra) j0 = ra;
    if (j1 > rb) j1 = rb;
    if (i0 >= i1 || j0 >= j1) return;
    int si0 = i0 - GB > ba ? i0 - GB : ba, si1 = i1 + GB < bb ? i1 + GB : bb;    /* (the blocks the blur reads) */
    int sj0 = j0 - GB > ra ? j0 - GB : ra, sj1 = j1 + GB < rb ? j1 + GB : rb;
    for (int j = sj0; j < sj1; j++) {                 /* shrunk: each block's mean (of its pixels in the strip) */
        int y0 = j * GQ > sy ? j * GQ : sy, y1 = j * GQ + GQ < sy + n ? j * GQ + GQ : sy + n;
        uint32_t *q = gq + (j - qo) * qw;
        for (int i = si0; i < si1; i++) {
            int x0 = i * GQ > lo ? i * GQ : lo, x1 = i * GQ + GQ < hi ? i * GQ + GQ : hi, r = 0, g = 0, b = 0, k = (x1 - x0) * (y1 - y0);
            for (int y = y0; y < y1; y++) {
                const uint32_t *s = strip + (y - org) * PW;
                for (int x = x0; x < x1; x++) { r += s[x] >> 16 & 255; g += s[x] >> 8 & 255; b += s[x] & 255; }
            }
            q[i] = (uint32_t)(r / k) << 16 | (uint32_t)(g / k) << 8 | (uint32_t)(b / k);
        }
        tent_row(q, gh + (j - qo) * qw, i0, i1, si0, si1);
    }
    uint64_t *T = gacc, *A = gacc + qw, *B = gacc + 2 * qw;   /* (the same as tent_row's, down each column) */
#define ROW(j) (gh + (((j) < sj0 ? sj0 : (j) >= sj1 ? sj1 - 1 : (j)) - qo) * qw)
    for (int i = i0; i < i1; i++) T[i] = A[i] = B[i] = 0;
    for (int k = -GB; k <= GB; k++) {
        const uint32_t *r = ROW(j0 + k);
        for (int i = i0; i < i1; i++) T[i] += PK(r[i]) * (uint64_t)(GB + 1 - (k < 0 ? -k : k));
    }
    for (int j = j0 + 1; j <= j0 + GB + 1; j++) { const uint32_t *r = ROW(j); for (int i = i0; i < i1; i++) A[i] += PK(r[i]); }
    for (int j = j0 - GB; j <= j0; j++) { const uint32_t *r = ROW(j); for (int i = i0; i < i1; i++) B[i] += PK(r[i]); }
    for (int j = j0; j < j1; j++) {
        uint32_t *d = gv + (j - qo) * qw;
        const uint32_t *pa = ROW(j + GB + 2), *pb = ROW(j + 1), *pc = ROW(j - GB);
        for (int i = i0; i < i1; i++) {
            d[i] = unpk(T[i]);
            T[i] = T[i] + A[i] - B[i];
            A[i] = A[i] + PK(pa[i]) - PK(pb[i]);
            B[i] = B[i] + PK(pb[i]) - PK(pc[i]);
        }
    }
#undef ROW
}
static uint32_t shine(uint32_t v, int l)   /* v lit: l/256 of the way to white */
{
    return ((v >> 16 & 255) + ((255 - (v >> 16 & 255)) * l >> 8)) << 16 | ((v >> 8 & 255) + ((255 - (v >> 8 & 255)) * l >> 8)) << 8 |
           ((v & 255) + ((255 - (v & 255)) * l >> 8));
}
/* Light on a glass or a gloss (filled with its colour first): its edge a
 * little lit, a bright line just inside its top (a gloss: its top row). */
static void lit(const prim_t *p, int lo, int hi, int ya, int yb)
{
    int fill = !(p->c & FROST), small = p->y1 - p->y0 < 6 || p->x1 - p->x0 < 6;
    for (int y = ya; y < yb; y++) {
        int i = inset(y, p->y0, p->y1, p->r), xl = p->x0 + i, xr = p->x1 - i - 1;
        int a = xl > lo ? xl : lo, b = xr + 1 < hi ? xr + 1 : hi, dt = y - p->y0;
        uint32_t *d = strip + (y - org) * PW;
        if (fill) for (int x = a; x < b; x++) d[x] = p->c & 0xffffff;
        if (small || a >= b) continue;
        if (fill) { if (dt == 0) for (int x = a; x < b; x++) d[x] = shine(d[x], 150); continue; }
        if (dt == 0 || y == p->y1 - 1) { for (int x = a; x < b; x++) d[x] = shine(d[x], 120); continue; }
        if (dt == 1) for (int x = a + 1; x < b - 1; x++) d[x] = shine(d[x], 170);
        if (xl >= lo && xl < hi) d[xl] = shine(d[xl], 120);
        if (xr >= lo && xr < hi && xr != xl) d[xr] = shine(d[xr], 120);
    }
}
static void glass(const prim_t *p, int sy, int n, int lo, int hi, int ya, int yb)
{
    int ca = lo, cb = hi, under = 0;                 /* (glass later over it, near it: it is worked out around the strip too) */
    for (int j = cur + 1; j < np && !under; j++)
        under = dl[j].type == P_RECT && dl[j].c & FROST && dl[j].x0 - GM < p->x1 && dl[j].x1 + GM > p->x0 && dl[j].y0 - GM < p->y1 && dl[j].y1 + GM > p->y0;
    if (!under) {
        if (ya < oy0) ya = oy0;
        if (yb > oy1) yb = oy1;
        if (ca < ox0) ca = ox0;
        if (cb > ox1) cb = ox1;
        if (ya >= yb || ca >= cb || p->x1 <= ca || p->x0 >= cb) return;
    }
    int gx0 = p->x0 > ca ? p->x0 : ca, gx1 = p->x1 < cb ? p->x1 : cb, hole = p->cx1 > p->cx0 && p->cy1 > p->cy0;
    qo = org / GQ;
    /* what is behind it blurred, around each part of it that shows (not its hole: what will cover it) */
    if (!hole) blur(gx0, gx1, ya, yb, sy, n, lo, hi);
    else {
        int ma = p->cy0 > ya ? p->cy0 : ya, mb = p->cy1 < yb ? p->cy1 : yb;
        if (p->cy0 > ya) blur(gx0, gx1, ya, p->cy0 < yb ? p->cy0 : yb, sy, n, lo, hi);
        if (p->cy1 < yb) blur(gx0, gx1, p->cy1 > ya ? p->cy1 : ya, yb, sy, n, lo, hi);
        if (ma < mb) {
            if (p->cx0 > gx0) blur(gx0, p->cx0 < gx1 ? p->cx0 : gx1, ma, mb, sy, n, lo, hi);
            if (p->cx1 < gx1) blur(p->cx1 > gx0 ? p->cx1 : gx0, gx1, ma, mb, sy, n, lo, hi);
        }
    }
    uint32_t c = p->c & 0xffffff;
    int al = ALPHA_OF(p->c), tr = (int)(c >> 16 & 255) * al, tg = (int)(c >> 8 & 255) * al, tb = (int)(c & 255) * al;
    int ba = lo / GQ, bb = (hi - 1) / GQ, ra = sy / GQ, rb = (sy + n - 1) / GQ;   /* (the blocks there are) */
    for (int y = ya; y < yb; y++) {
        int i = inset(y, p->y0, p->y1, p->r), xl = p->x0 + i, xr = p->x1 - i - 1;
        int a = xl > ca ? xl : ca, b = xr + 1 < cb ? xr + 1 : cb, ha = b, hb = b;
        if (hole && y >= p->cy0 && y < p->cy1) { ha = p->cx0 > a ? p->cx0 < b ? p->cx0 : b : a; hb = p->cx1 > ha ? p->cx1 < b ? p->cx1 : b : ha; }
        int v8 = 2 * y - GQ + 1, j = v8 >> 3, fy = v8 & 7, j2 = j + 1;   /* (between block rows j and j + 1: the centres GQ / 2 - 1/2 in) */
        j = j < ra ? ra : j > rb ? rb : j; j2 = j2 < ra ? ra : j2 > rb ? rb : j2;
        const uint32_t *r0 = gv + (j - qo) * qw, *r1 = gv + (j2 - qo) * qw;
        uint32_t *d = strip + (y - org) * PW;
        for (int x = a; x < b; x++) {
            if (x >= ha && x < hb) { x = hb - 1; continue; }
            int u8 = 2 * x - GQ + 1, k = u8 >> 3, fx = u8 & 7, k2 = k + 1;
            k = k < ba ? ba : k > bb ? bb : k; k2 = k2 < ba ? ba : k2 > bb ? bb : k2;
            uint32_t p00 = r0[k], p01 = r0[k2], p10 = r1[k], p11 = r1[k2];
            int w00 = (8 - fx) * (8 - fy), w01 = fx * (8 - fy), w10 = (8 - fx) * fy, w11 = fx * fy;
            int rr = (int)((p00 >> 16 & 255) * w00 + (p01 >> 16 & 255) * w01 + (p10 >> 16 & 255) * w10 + (p11 >> 16 & 255) * w11) >> 6;
            int gg = (int)((p00 >> 8 & 255) * w00 + (p01 >> 8 & 255) * w01 + (p10 >> 8 & 255) * w10 + (p11 >> 8 & 255) * w11) >> 6;
            int bl = (int)((p00 & 255) * w00 + (p01 & 255) * w01 + (p10 & 255) * w10 + (p11 & 255) * w11) >> 6;
            int m = (rr + gg + bl) / 3;
            rr += (rr - m) * 7 / 10; gg += (gg - m) * 7 / 10; bl += (bl - m) * 7 / 10;   /* (vibrancy: 1.7 times as saturated) */
            rr = rr < 0 ? 0 : rr > 255 ? 255 : rr; gg = gg < 0 ? 0 : gg > 255 ? 255 : gg; bl = bl < 0 ? 0 : bl > 255 ? 255 : bl;
            d[x] = (uint32_t)((rr * (32 - al) + tr) >> 5) << 16 | (uint32_t)((gg * (32 - al) + tg) >> 5) << 8 | (uint32_t)((bl * (32 - al) + tb) >> 5);
        }
    }
    lit(p, ca, cb, ya, yb);
}

/* What hides what, in the strip being drawn: opaque fills and other
 * programs' pixels, each shrunk by its corners and by 2 GM (glass drawn
 * before it sees a little under its edges), latest first. What lies wholly
 * in one drawn after it is not drawn; the desk, not where one is. */
static void covers(int x0, int y0, int x1, int y1)
{
    ncv = 0;
    for (int i = np - 1; i >= 0 && ncv < MAXCV; i--) {
        const prim_t *p = &dl[i];
        if (p->type != P_IMAGE && (p->type != P_RECT || p->c & (FROST | ALPHA_M))) continue;
        int k = (p->type == P_RECT ? p->r : 0) + 2 * GM, a = p->x0 + k, b = p->y0 + k, c = p->x1 - k, d = p->y1 - k;
        if (c <= a || d <= b || a >= x1 || c <= x0 || b >= y1 || d <= y0) continue;
        cv[ncv].i = i; cv[ncv].x0 = a; cv[ncv].y0 = b; cv[ncv].x1 = c; cv[ncv].y1 = d;
        ncv++;
    }
}
static int hidden(int i, int x0, int y0, int x1, int y1)
{
    for (int k = 0; k < ncv && cv[k].i > i; k++)
        if (x0 >= cv[k].x0 && x1 <= cv[k].x1 && y0 >= cv[k].y0 && y1 <= cv[k].y1) return 1;
    return 0;
}

/* Rasterize primitive p in screen rows [sy, sy + n), columns [lo, hi). */
static void raster(const prim_t *p, int sy, int n, int lo, int hi)
{
    int ya = p->y0 > sy ? p->y0 : sy, yb = p->y1 < sy + n ? p->y1 : sy + n;
    if (ya >= yb || p->x1 <= lo || p->x0 >= hi) return;
    switch (p->type) {
    case P_DESK:                     /* (between what will hide it) */
        for (int y = ya; y < yb; y++)
            for (int x = lo; x < hi;) {
                int next = hi, skip = 0;
                for (int k = 0; k < ncv && cv[k].i > cur; k++) {
                    if (y < cv[k].y0 || y >= cv[k].y1 || cv[k].x1 <= x) continue;
                    if (cv[k].x0 <= x) { x = cv[k].x1; skip = 1; break; }
                    if (cv[k].x0 < next) next = cv[k].x0;
                }
                if (skip) continue;
                wall_row(y, x, next, strip + (y - org) * PW);
                x = next;
            }
        break;
    case P_RECT:
        if (p->c & FROST) glass(p, sy, n, lo, hi, ya, yb);
        else if (p->c & GLOSS) lit(p, lo, hi, ya, yb);
        else for (int y = ya; y < yb; y++) { int i = inset(y, p->y0, p->y1, p->r); span(y - org, p->x0 + i, p->x1 - i, lo, hi, p->c & ~(FROST | GLOSS | RAW)); }
        break;
    case P_FRAME: {
        int r = p->r, t = p->t, ir = r > t ? r - t : 0;
        for (int y = ya; y < yb; y++) {
            int i = inset(y, p->y0, p->y1, r);
            if (y < p->y0 + t || y >= p->y1 - t) { span(y - org, p->x0 + i, p->x1 - i, lo, hi, p->c); continue; }
            int j = inset(y, p->y0 + t, p->y1 - t, ir);
            span(y - org, p->x0 + i, p->x0 + t + j, lo, hi, p->c);
            span(y - org, p->x1 - t - j, p->x1 - i, lo, hi, p->c);
        }
        break;
    }
    case P_SHADE:                    /* darken what is already there: 3/4 (k 1), 1/2 (k 2) or 7/8 (k 3) */
        for (int y = ya; y < yb; y++) {
            int i = inset(y, p->y0, p->y1, p->r), a = p->x0 + i > lo ? p->x0 + i : lo, b = p->x1 - i < hi ? p->x1 - i : hi;
            uint32_t *d = strip + (y - org) * PW;
            if (p->t == 3) {
                int ha = b, hb = b;          /* (the hole: what the window covers, skipped) */
                if (y >= p->cy0 && y < p->cy1 && p->cx1 > p->cx0) { ha = p->cx0 > a ? p->cx0 : a; hb = p->cx1 > ha ? p->cx1 : ha; }
                for (int x = a; x < ha; x++) d[x] -= d[x] >> 3 & 0x1f1f1f;
                for (int x = hb; x < b; x++) d[x] -= d[x] >> 3 & 0x1f1f1f;
            }
            else if (p->t == 2) for (int x = a; x < b; x++) d[x] = (d[x] >> 1 & 0x7f7f7f) | (d[x] & RAW);   /* (a program's pixels stay */
            else for (int x = a; x < b; x++) d[x] -= d[x] >> 2 & 0x3f3f3f;                          /*  its own: darkened, not tinted) */
        }
        break;
    case P_PIXMAP:                   /* each screen pixel: its map character's colour */
        for (int y = ya; y < yb; y++) {
            int ry = (int)(((int64_t)(y - p->y0) << 16) / p->scale);
            if (ry >= p->t) continue;
            const char *row = p->rows[ry];
            uint32_t *d = strip + (y - org) * PW;
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
    case P_IMAGE: {                  /* each screen pixel: the nearest image pixel (marked RAW: another program's) */
        int a = p->x0 > lo ? p->x0 : lo, b = p->x1 < hi ? p->x1 : hi;
        uint32_t mark = p->c;
        for (int y = ya; y < yb; y++) {
            const uint32_t *s = p->cols + ((uint64_t)(y - p->y0) * p->c2 >> 16) * p->r;
            uint32_t *d = strip + (y - org) * PW;
            uint64_t u = (uint64_t)(a - p->x0) * p->scale;
            if (p->scale == 65536) for (int x = a; x < b; x++, u += 65536) d[x] = (s[u >> 16] & 0xffffff) | mark;   /* (1:1, the usual case) */
            else for (int x = a; x < b; x++, u += p->scale) d[x] = (s[u >> 16] & 0xffffff) | mark;
        }
        break;
    }
    case P_PIC: {                    /* each screen pixel: between the four nearest picture pixels, laid over by their opacity */
        const uint8_t *ix = (const uint8_t *)p->keys;
        int a = p->x0 > lo ? p->x0 : lo, b = p->x1 < hi ? p->x1 : hi, w = p->r, h = p->t;
        int64_t ku = ((int64_t)w << 16) / (p->x1 - p->x0), kv = ((int64_t)h << 16) / (p->y1 - p->y0);   /* (picture pixels a screen pixel, 16.16) */
        for (int y = ya; y < yb; y++) {
            int64_t v = (y - p->y0) * kv + kv / 2 - 32768;
            int j = v < 0 ? 0 : (int)(v >> 16), fy = v < 0 ? 0 : (int)(v >> 12 & 15), j2 = j + 1 < h ? j + 1 : h - 1;
            if (j >= h) j = h - 1;
            const uint8_t *r0 = ix + j * w, *r1 = ix + j2 * w;
            uint32_t *d = strip + (y - org) * PW;
            for (int x = a; x < b; x++) {
                int64_t u = (x - p->x0) * ku + ku / 2 - 32768;
                int i = u < 0 ? 0 : (int)(u >> 16), fx = u < 0 ? 0 : (int)(u >> 12 & 15), i2 = i + 1 < w ? i + 1 : w - 1;
                if (i >= w) i = w - 1;
                uint32_t c[4] = { p->cols[r0[i]], p->cols[r0[i2]], p->cols[r1[i]], p->cols[r1[i2]] };
                int wt[4] = { (16 - fx) * (16 - fy), fx * (16 - fy), (16 - fx) * fy, fx * fy }, A = 0, R = 0, G = 0, B = 0;
                for (int k = 0; k < 4; k++) {           /* (weighed by their opacity too) */
                    int o = wt[k] * (int)(c[k] >> 24);
                    A += o; R += o * (int)(c[k] >> 16 & 255); G += o * (int)(c[k] >> 8 & 255); B += o * (int)(c[k] & 255);
                }
                if (!A) continue;
                d[x] = mix(d[x], (uint32_t)(R / A) << 16 | (uint32_t)(G / A) << 8 | (uint32_t)(B / A), (A >> 8) * 32 / 255);
            }
        }
        break;
    }
    case P_TEXT: {                   /* each screen pixel: how much of it the font's pixels cover */
        int s = p->scale, cl = p->x0 > lo ? p->x0 : lo, cr = p->x1 < hi ? p->x1 : hi;
        int nf = 0;
        while (nf < (int)(sizeof natives / sizeof *natives) && natives[nf].scale != s) nf++;
        if (nf < (int)(sizeof natives / sizeof *natives)) {   /* a native font, pixel for pixel (100%) */
            int fw = natives[nf].w, fh = natives[nf].h;
            const uint8_t *fp = natives[nf].px;
            for (int y = ya; y < yb; y++) {
                int row = y - p->cy0;
                if (row < 0 || row >= fh) continue;
                uint32_t *d = strip + (y - org) * PW;
                for (int x = cl; x < cr; x++) {
                    int k = (x - p->cx0) / fw, col = (x - p->cx0) % fw;
                    if (k >= p->len) break;
                    unsigned ch = (uint8_t)arena[p->text + k];
                    int a = fp[((ch >= 32 && ch < 127 ? ch - 32 : '?' - 32) * fh + row) * fw + col];
                    if (!a) continue;
                    a += a >> 7;                                 /* (0..255 -> 0..256) */
                    uint32_t o = d[x], c = p->c;
                    d[x] = a >= 256 ? c : ((((o >> 16 & 255) * (256 - a) + (c >> 16 & 255) * a) >> 8) << 16) |
                                          ((((o >> 8 & 255) * (256 - a) + (c >> 8 & 255) * a) >> 8) << 8) |
                                          (((o & 255) * (256 - a) + (c & 255) * a) >> 8);
                }
            }
            break;
        }
        int64_t step = ((int64_t)1 << 32) / s;          /* a screen pixel, in font pixels (16.16) */
        for (int y = ya; y < yb; y++) {
            int64_t v0 = ((int64_t)(y - p->cy0) << 32) / s, v1 = v0 + step;
            int r0 = (int)(v0 >> 16), r1 = (int)((v1 - 1) >> 16);
            if (r1 < 1 || r0 > 9) continue;              /* (font row 0 is blank; glyphs: rows 1..9) */
            uint32_t *d = strip + (y - org) * PW;
            int64_t u0 = ((int64_t)(cl - p->cx0) << 32) / s;
            for (int x = cl; x < cr; x++, u0 += step) {
                int64_t u1 = u0 + step, cov = 0;
                int c0 = (int)(u0 >> 16), c1 = (int)((u1 - 1) >> 16);
                for (int j = r0; j <= r1; j++) {
                    if (j < 1 || j > 9) continue;
                    int64_t oy = (v1 < ((int64_t)(j + 1) << 16) ? v1 : (int64_t)(j + 1) << 16) - (v0 > ((int64_t)j << 16) ? v0 : (int64_t)j << 16);
                    for (int i = c0; i <= c1; i++) {
                        int k = i / CW, b = i % CW;
                        if (k >= p->len || b >= 5) continue;
                        unsigned ch = (uint8_t)arena[p->text + k];
                        if (!(font[ch >= 32 && ch < 127 ? ch - 32 : '?' - 32][j - 1] & 0x10 >> b)) continue;
                        int64_t ox = (u1 < ((int64_t)(i + 1) << 16) ? u1 : (int64_t)(i + 1) << 16) - (u0 > ((int64_t)i << 16) ? u0 : (int64_t)i << 16);
                        cov += ox * oy >> 16;
                    }
                }
                if (!cov) continue;
                int a = (int)(cov * 256 / ((u1 - u0) * (v1 - v0) >> 16));   /* 0..256 */
                if (a >= 250) a = 256;
                uint32_t o = d[x], c = p->c;
                d[x] = a == 256 ? c : ((((o >> 16 & 255) * (256 - a) + (c >> 16 & 255) * a) >> 8) << 16) |
                                      ((((o >> 8 & 255) * (256 - a) + (c >> 8 & 255) * a) >> 8) << 8) |
                                      (((o & 255) * (256 - a) + (c & 255) * a) >> 8);
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
            strip[(y - org) * PW + x] = arrow[r][i] == 'X' ? 0x0a1322 : 0xffffff;
        }
    }
}

static void present_px(int x0, int y0, int x1, int y1, int grow);
void present(int x0, int y0, int x1, int y1)          /* (a box of the picture) */
{
    present_px(sc(x0), sc(y0), (int)(((int64_t)x1 * Z + 65535) >> 16), (int)(((int64_t)y1 * Z + 65535) >> 16), 1);
}
/* Only the pointer moved: where it was and where it is, at the arrow's own
 * size on the screen (it is not zoomed; nothing under glass changed). */
void present_pointer(int ox, int oy, int x, int y)
{
    present_px(sc(ox), sc(oy), sc(ox) + 13, sc(oy) + 20, 0);
    present_px(sc(x), sc(y), sc(x) + 13, sc(y) + 20, 0);
}
static int glass_in(int x0, int y0, int x1, int y1)  /* glass in this box of the screen: 1 + the last one's index, or 0 */
{
    for (int i = np - 1; i >= 0; i--)
        if (dl[i].type == P_RECT && dl[i].c & FROST && dl[i].x0 < x1 && dl[i].x1 > x0 && dl[i].y0 < y1 && dl[i].y1 > y0) return i + 1;
    return 0;
}
/* A box of the screen. Glass shows what is up to GM pixels around it: so
 * near glass, the box grows by that (grow: what changed shows in the glass
 * around it too), and each strip is drawn with that much around it, kept
 * in the strip buffer but not shown. */
static void present_px(int x0, int y0, int x1, int y1, int grow)
{
    if (grow && glass_in(x0 - GM, y0 - GM, x1 + GM, y1 + GM)) { x0 -= GM; y0 -= GM; x1 += GM; y1 += GM; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > PW) x1 = PW;
    if (y1 > PH) y1 = PH;
    if (x0 >= x1 || y0 >= y1) return;
    for (int sy = y0; sy < y1; sy += STRIP) {
        int n = y1 - sy < STRIP ? y1 - sy : STRIP, last = glass_in(x0, sy, x1, sy + n), m = last ? GM : 0;
        int top = sy - m > 0 ? sy - m : 0, bot = sy + n + m < PH ? sy + n + m : PH;
        int lo = x0 - m > 0 ? x0 - m : 0, hi = x1 + m < PW ? x1 + m : PW;
        org = top; ox0 = x0; ox1 = x1; oy0 = sy; oy1 = sy + n;
        covers(lo, top, hi, bot);
        for (int i = 0; i < np; i++) {               /* (after the last glass, around the strip is not needed) */
            int a = i < last ? lo : x0, b = i < last ? hi : x1, t = i < last ? top : sy, e = i < last ? bot : sy + n;
            const prim_t *p = &dl[i];
            if (hidden(i, p->x0 > a ? p->x0 : a, p->y0 > t ? p->y0 : t, p->x1 < b ? p->x1 : b, p->y1 < e ? p->y1 : e)) continue;
            cur = i;
            raster(p, t, e - t, a, b);
        }
        pointer(sy, n, x0, x1);
        for (int r = 0; r < n; r++) {
            uint32_t *d = fb + (sy + r) * pitch + x0, *src = strip + (sy - top + r) * PW + x0;
            if (!swap) memcpy(d, src, (x1 - x0) * 4);
            else for (int i = 0; i < x1 - x0; i++) { uint32_t c = src[i]; d[i] = (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF); }
        }
    }
}
