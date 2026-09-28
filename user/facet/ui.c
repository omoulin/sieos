/*
 * ui.c - Widgets and icons for the Facet desktop.
 * Every icon is drawn from primitives; sizes scale from a 48px design.
 */
#include "facet.h"

void ui_button(struct surface *s, struct rect r, const char *label, bool pressed)
{
    /* Strata: rounded graphite button with a soft top highlight */
    gfx_round_rect_vgradient(s, r.x, r.y, r.w, r.h, 6, pressed ? RGB(0x23, 0x26, 0x2A) : RGB(0x3A, 0x3E, 0x45),
                             pressed ? RGB(0x2A, 0x2D, 0x32) : RGB(0x2E, 0x31, 0x37));
    gfx_round_frame(s, r.x, r.y, r.w, r.h, 6, C_FACE_DARK);
    if (!pressed)
        gfx_blend_fill(s, r.x + 6, r.y + 1, r.w - 12, 1, RGB(0xFF, 0xFF, 0xFF), 40);
    if (label) {
        int o = pressed ? 1 : 0;
        gfx_text(s, r.x + (r.w - text_width(label)) / 2 + o, r.y + (r.h - FONT_H) / 2 + o, label, C_TEXT);
    }
}

void ui_panel(struct surface *s, struct rect r, bool sunken)
{
    gfx_fill(s, r.x, r.y, r.w, r.h, sunken ? C_CONTENT : C_FACE);
    gfx_frame(s, r.x, r.y, r.w, r.h, sunken ? C_FACE_DARK : C_LINE);
}

void ui_meter(struct surface *s, struct rect r, int percent, color_t fill)
{
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    gfx_fill(s, r.x, r.y, r.w, r.h, RGB(0x24, 0x2A, 0x2E));
    gfx_frame(s, r.x, r.y, r.w, r.h, C_FACE_DARK);
    int w = (r.w - 4) * percent / 100;
    if (w > 0)
        gfx_hgradient(s, r.x + 2, r.y + 2, w, r.h - 4, color_shade(fill, -40), fill);
}

/* ---------------- icons ---------------- */

#define S(v) ((v) * size / 48)

static void icon_folder(struct surface *s, int x, int y, int size, bool home)
{
    color_t body = RGB(0xD9, 0xA4, 0x4A), dark = RGB(0x8A, 0x5E, 0x1C), light = RGB(0xF6, 0xD2, 0x8A);
    gfx_fill(s, x + S(4), y + S(8), S(16), S(6), dark);                   /* tab */
    gfx_fill(s, x + S(4), y + S(12), S(40), S(28), body);
    gfx_frame(s, x + S(4), y + S(12), S(40), S(28), dark);
    gfx_triangle(s, x + S(5), y + S(13), x + S(26), y + S(13), x + S(5), y + S(30), light);  /* facet */
    if (home) {                                                           /* little house */
        int cx = x + S(28), cy = y + S(24);
        gfx_triangle(s, cx - S(9), cy, cx, cy - S(8), cx + S(9), cy, RGB(0x6A, 0x2E, 0x1E));
        gfx_fill(s, cx - S(6), cy, S(12), S(10), RGB(0xF4, 0xEF, 0xE2));
        gfx_fill(s, cx - S(2), cy + S(4), S(4), S(6), RGB(0x33, 0x74, 0x84));
    }
}

static void icon_terminal(struct surface *s, int x, int y, int size)
{
    gfx_fill(s, x + S(3), y + S(6), S(42), S(32), C_FACE);
    gfx_bevel(s, x + S(3), y + S(6), S(42), S(32), 1, true, C_FACE_LIGHT, C_FACE_DARK);
    gfx_fill(s, x + S(7), y + S(10), S(34), S(22), RGB(0x10, 0x18, 0x1C));
    gfx_fill(s, x + S(16), y + S(38), S(16), S(4), C_FACE_SHADOW);
    gfx_fill(s, x + S(10), y + S(42), S(28), S(3), C_FACE_DARK);
    if (size >= 32) {
        gfx_text(s, x + S(9), y + S(13), ">_", RGB(0x7C, 0xE0, 0x9A));
    } else {
        gfx_fill(s, x + S(10), y + S(20), S(8), S(3), RGB(0x7C, 0xE0, 0x9A));
    }
}

static void icon_file(struct surface *s, int x, int y, int size, bool program)
{
    color_t paper = RGB(0xF7, 0xF4, 0xEA), edge = RGB(0x6E, 0x6A, 0x60);
    int x0 = x + S(10), y0 = y + S(4), w = S(28), h = S(40), f = S(9);
    gfx_fill(s, x0, y0, w - f, h, paper);
    gfx_fill(s, x0 + w - f, y0 + f, f, h - f, paper);
    gfx_triangle(s, x0 + w - f, y0, x0 + w - f, y0 + f, x0 + w, y0 + f, RGB(0xC8, 0xC2, 0xB2));
    gfx_frame(s, x0, y0, w, h, edge);
    if (program) {
        int cx = x0 + w / 2, cy = y0 + h / 2 + S(2);
        gfx_triangle(s, cx - S(8), cy, cx, cy - S(8), cx + S(8), cy, C_TITLE_A1);
        gfx_triangle(s, cx - S(8), cy, cx, cy + S(8), cx + S(8), cy, C_TITLE_A2);
    } else {
        for (int i = 0; i < 5; i++)
            gfx_hline(s, x0 + S(4), y0 + S(14) + i * S(5), w - S(9), RGB(0x9A, 0x96, 0x8A));
    }
}

static void icon_monitor(struct surface *s, int x, int y, int size)
{
    gfx_fill(s, x + S(4), y + S(6), S(40), S(34), RGB(0x1C, 0x26, 0x2C));
    gfx_bevel(s, x + S(4), y + S(6), S(40), S(34), 1, true, C_FACE_LIGHT, C_FACE_DARK);
    static const int bars[] = { 10, 18, 14, 24, 20 };
    for (int i = 0; i < 5; i++) {
        int h = S(bars[i]);
        gfx_fill(s, x + S(9) + i * S(7), y + S(36) - h, S(5), h, i == 3 ? C_ACCENT : RGB(0x6C, 0xC0, 0x8A));
    }
}

static void icon_clock(struct surface *s, int x, int y, int size)
{
    int cx = x + S(24), cy = y + S(24), r = S(19);
    gfx_disc(s, cx, cy, r, C_FACE_DARK);
    gfx_disc(s, cx, cy, r - S(2), RGB(0xF7, 0xF4, 0xEA));
    gfx_thick_line(s, cx, cy, cx, cy - S(12), size >= 32 ? 3 : 1, C_TEXT);
    gfx_thick_line(s, cx, cy, cx + S(9), cy + S(4), size >= 32 ? 3 : 1, C_TEXT);
    gfx_disc(s, cx, cy, S(2), C_ACCENT);
}

static void icon_info(struct surface *s, int x, int y, int size)
{
    int cx = x + S(24), cy = y + S(24);
    gfx_disc(s, cx, cy, S(19), C_TITLE_A2);
    gfx_disc(s, cx, cy, S(16), C_TITLE_A1);
    gfx_fill(s, cx - S(3), cy - S(3), S(6), S(14), RGB(0xFF, 0xF4, 0xD6));
    gfx_disc(s, cx, cy - S(9), S(3), C_ACCENT);
}

static void icon_logout(struct surface *s, int x, int y, int size)
{
    gfx_fill(s, x + S(8), y + S(4), S(22), S(40), RGB(0x7A, 0x52, 0x34));
    gfx_frame(s, x + S(8), y + S(4), S(22), S(40), RGB(0x3A, 0x24, 0x14));
    gfx_disc(s, x + S(25), y + S(25), S(2), C_ACCENT);
    gfx_thick_line(s, x + S(28), y + S(24), x + S(44), y + S(24), size >= 32 ? 4 : 2, RGB(0xB0, 0x30, 0x28));
    gfx_triangle(s, x + S(44), y + S(17), x + S(44), y + S(31), x + S(48), y + S(24), RGB(0xB0, 0x30, 0x28));
}

static void icon_disk(struct surface *s, int x, int y, int size)
{
    /* a hard-drive slab, seen from slightly above */
    gfx_triangle(s, x + S(4), y + S(22), x + S(14), y + S(12), x + S(44), y + S(12), RGB(0xE8, 0xE8, 0xE8));
    gfx_triangle(s, x + S(4), y + S(22), x + S(44), y + S(12), x + S(38), y + S(22), RGB(0xE8, 0xE8, 0xE8));
    gfx_fill(s, x + S(4), y + S(22), S(34), S(14), RGB(0xB8, 0xB8, 0xB8));
    gfx_triangle(s, x + S(38), y + S(22), x + S(44), y + S(12), x + S(44), y + S(26), RGB(0x88, 0x88, 0x88));
    gfx_triangle(s, x + S(38), y + S(22), x + S(44), y + S(26), x + S(38), y + S(36), RGB(0x88, 0x88, 0x88));
    gfx_frame(s, x + S(4), y + S(22), S(34), S(14), C_FACE_DARK);
    gfx_fill(s, x + S(8), y + S(28), S(4), S(3), RGB(0x30, 0xD0, 0x40));   /* activity light */
    for (int i = 0; i < 4; i++)
        gfx_vline(s, x + S(20) + i * S(4), y + S(26), S(7), RGB(0x80, 0x80, 0x80));
}

static void icon_network(struct surface *s, int x, int y, int size)
{
    int cx = x + S(24), cy = y + S(24), r = S(19);
    gfx_disc(s, cx, cy, r, RGB(0x2E, 0x6C, 0xC0));
    gfx_disc(s, cx - S(5), cy - S(4), S(8), RGB(0x46, 0xA8, 0x58));      /* land masses */
    gfx_disc(s, cx + S(8), cy + S(6), S(6), RGB(0x46, 0xA8, 0x58));
    color_t line = RGB(0xC8, 0xE0, 0xFF);
    gfx_hline(s, cx - r + S(2), cy, 2 * r - S(4), line);
    gfx_hline(s, cx - r + S(5), cy - S(10), 2 * r - S(10), line);
    gfx_hline(s, cx - r + S(5), cy + S(10), 2 * r - S(10), line);
    gfx_vline(s, cx, cy - r + S(2), 2 * r - S(4), line);
    gfx_circle(s, cx, cy, r, RGB(0x16, 0x34, 0x66));
}

void icon_draw(struct surface *s, int kind, int x, int y, int size)
{
    switch (kind) {
    case ICON_TERMINAL: icon_terminal(s, x, y, size); break;
    case ICON_FOLDER:   icon_folder(s, x, y, size, false); break;
    case ICON_HOME:     icon_folder(s, x, y, size, true); break;
    case ICON_FILE:     icon_file(s, x, y, size, false); break;
    case ICON_PROGRAM:  icon_file(s, x, y, size, true); break;
    case ICON_MONITOR:  icon_monitor(s, x, y, size); break;
    case ICON_CLOCK:    icon_clock(s, x, y, size); break;
    case ICON_INFO:     icon_info(s, x, y, size); break;
    case ICON_LOGOUT:   icon_logout(s, x, y, size); break;
    case ICON_DISK:     icon_disk(s, x, y, size); break;
    case ICON_NETWORK:  icon_network(s, x, y, size); break;
    }
}

/* ---------------- the SIEOS logo: Orbit Node ---------------- */

/*
 * A blue ring crossed by a light horizontal stratum, with an amber node
 * riding the ring at one o'clock.  Drawn from its 200-unit design with
 * antialiased edges; at 24 px and below the 16x16 pixel version is used.
 */
static const char *logo_px[16] = {
    "................",
    ".....cccccc.....",
    "...cc......caa..",
    "..cc.......aaa..",
    "..c.........ac..",
    ".cc..........cc.",
    ".c............c.",
    "wwwwwwwwwwwwwwww",
    ".c............c.",
    ".cc..........cc.",
    "..c.........dd..",
    "..cc.......dd...",
    "...cc....ddd....",
    ".....dddddd.....",
    "................",
    "................",
};

static color_t logo_px_color(char c)
{
    switch (c) {
    case 'a': return RGB(0xD9, 0xA1, 0x5F);
    case 'c': return RGB(0x8F, 0xB4, 0xDC);
    case 'd': return RGB(0x4F, 0x7F, 0xB8);
    case 'w': return RGB(0xE4, 0xE0, 0xD8);
    default:  return 0;
    }
}

void logo_pixels(struct surface *s, int x, int y, int scale)
{
    for (int j = 0; j < 16; j++)
        for (int i = 0; i < 16; i++) {
            char c = logo_px[j][i];
            if (c != '.')
                gfx_fill(s, x + i * scale, y + j * scale, scale, scale, logo_px_color(c));
        }
}

static unsigned isqrt32(unsigned long v)
{
    unsigned long r = 0, bit = 1UL << 40;
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
    return (unsigned)r;
}

/* Antialiased annulus (inner = 0: a disc); colour picked per pixel. */
static void ring(struct surface *s, int cx16, int cy16, int r_out16, int r_in16,
                 color_t (*pick)(int dx, int dy, void *arg), void *arg, color_t c, int alpha)
{
    int x0 = (cx16 - r_out16) / 16 - 1, x1 = (cx16 + r_out16) / 16 + 1;
    int y0 = (cy16 - r_out16) / 16 - 1, y1 = (cy16 + r_out16) / 16 + 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            int dx = x * 16 + 8 - cx16, dy = y * 16 + 8 - cy16;
            int d = (int)isqrt32((unsigned long)(dx * dx + dy * dy));     /* 1/16 px */
            int cov_out = MAX(0, MIN(16, (r_out16 - d) + 8));
            int cov_in = r_in16 ? MAX(0, MIN(16, (d - r_in16) + 8)) : 16;
            int cov = MIN(cov_out, cov_in);
            if (cov <= 0)
                continue;
            color_t col = pick ? pick(dx, dy, arg) : c;
            gfx_blend_pixel(s, x, y, col, cov * alpha / 16);
        }
}

static color_t ring_color(int dx, int dy, void *arg)
{
    (void)arg;
    /* the lower-right quarter is the darker blue of the design */
    return dx > 0 && dy > 0 ? RGB(0x4F, 0x7F, 0xB8) : RGB(0x8F, 0xB4, 0xDC);
}

void logo_draw(struct surface *s, int cx, int cy, int size)
{
    if (size <= 24) {
        int scale = MAX(1, size / 16);
        logo_pixels(s, cx - 8 * scale, cy - 8 * scale, scale);
        return;
    }
    /* design units: 200 across, centre (100,100); work in 1/16 px */
    int k = size * 16;                                  /* 1/16 px per 200 units = k / 200 */
#define U(v) ((v) * k / 200)
    int ox = cx * 16 - U(100), oy = cy * 16 - U(100);
    ring(s, ox + U(100), oy + U(100), U(71), U(53), ring_color, NULL, 0, 256);
    /* the stratum: a rounded bar through the centre */
    int bx = (ox + U(22)) / 16, bw = U(156) / 16, by = (oy + U(93)) / 16, bh = MAX(2, U(14) / 16);
    gfx_round_rect(s, bx, by, bw, bh, bh / 2, RGB(0xE4, 0xE0, 0xD8));
    /* the node and its halo */
    ring(s, ox + U(146), oy + U(58), U(29), U(25), NULL, NULL, RGB(0xD9, 0xA1, 0x5F), 80);
    ring(s, ox + U(146), oy + U(58), U(17), 0, NULL, NULL, RGB(0xD9, 0xA1, 0x5F), 256);
#undef U
}
