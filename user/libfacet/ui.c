/*
 * ui.c - libfacet: widgets and icons in the Facet desktop's style.
 * Every icon is drawn from primitives; sizes scale from a 48px design.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "facet/theme.h"
#include "facet/ui.h"

void skin_icon_draw(int skin, struct surface *s, int kind, int x, int y, int size);   /* skin_icons.c */

/* A bevelled surface: raised (light top-left) or sunken. */
static void bevel(struct surface *s, struct rect r, bool raised, int depth)
{
    gfx_bevel(s, r.x, r.y, r.w, r.h, depth, raised, raised ? C_FACE_LIGHT : C_FACE_SHADOW,
              raised ? C_FACE_SHADOW : C_FACE_LIGHT);
}

bool fct__button_pressed(struct rect r);          /* client.c: a view's button held down */

void ui_button(struct surface *s, struct rect r, const char *label, bool pressed)
{
    pressed = pressed || fct__button_pressed(r);
    if (fct_skin->id == FCT_SKIN_BEOS) {           /* soft grey, a rounded dark outline */
        gfx_round_rect_vgradient(s, r.x, r.y, r.w, r.h, 4, pressed ? RGB(0xB8, 0xB8, 0xB8) : RGB(0xFA, 0xFA, 0xFA),
                                 pressed ? RGB(0xD0, 0xD0, 0xD0) : RGB(0xD4, 0xD4, 0xD4));
        gfx_round_frame(s, r.x, r.y, r.w, r.h, 4, RGB(0x70, 0x70, 0x70));
    } else if (fct_skin->id == FCT_SKIN_IRIX || fct_skin->id == FCT_SKIN_CDE) {   /* Motif: square, a two-pixel bevel */
        gfx_fill(s, r.x, r.y, r.w, r.h, pressed ? color_shade(C_FACE, -14) : C_FACE);
        gfx_frame(s, r.x, r.y, r.w, r.h, C_FACE_DARK);
        bevel(s, rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2), !pressed, 2);
    } else if (fct_skin->id == FCT_SKIN_AMIGA) {   /* grey, white over black (inverted when pressed) */
        gfx_fill(s, r.x, r.y, r.w, r.h, pressed ? C_ACCENT : C_FACE);
        gfx_bevel(s, r.x, r.y, r.w, r.h, 1, !pressed, RGB(0xFF, 0xFF, 0xFF), RGB(0, 0, 0));
    }
    if (fct_skin->light) {
        if (label) {
            int o = pressed ? 1 : 0;
            gfx_text(s, r.x + (r.w - text_width(label)) / 2 + o, r.y + (r.h - FONT_H) / 2 + o, label, C_TEXT);
        }
        return;
    }
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
    if (fct_skin->light) {
        gfx_fill(s, r.x, r.y, r.w, r.h, sunken ? C_CONTENT : C_FACE);
        bevel(s, r, !sunken, fct_skin->id == FCT_SKIN_IRIX || fct_skin->id == FCT_SKIN_CDE ? 2 : 1);
        return;
    }
    gfx_fill(s, r.x, r.y, r.w, r.h, sunken ? C_CONTENT : C_FACE);
    gfx_frame(s, r.x, r.y, r.w, r.h, sunken ? C_FACE_DARK : C_LINE);
}

void ui_meter(struct surface *s, struct rect r, int percent, color_t fill)
{
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    if (fct_skin->light) {
        gfx_fill(s, r.x, r.y, r.w, r.h, C_CONTENT_ALT);
        bevel(s, r, false, 1);
    } else {
        gfx_fill(s, r.x, r.y, r.w, r.h, RGB(0x24, 0x2A, 0x2E));
        gfx_frame(s, r.x, r.y, r.w, r.h, C_FACE_DARK);
    }
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

void icon_browser_paint(struct surface *s, int x, int y, int size, color_t top, color_t bottom, color_t outline,
                        float ow, color_t star);   /* skin_icons.c */
void icon_mir_paint(struct surface *s, int x, int y, int size, color_t wand, color_t tip, color_t spark,
                    color_t outline, float ow);

void icon_draw(struct surface *s, int kind, int x, int y, int size)
{
    if (fct_skin->id != FCT_SKIN_STRATA) {
        skin_icon_draw(fct_skin->id, s, kind, x, y, size);
        return;
    }
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
    case ICON_BROWSER:
        icon_browser_paint(s, x, y, size, RGB(0x8C, 0xD4, 0xFF), RGB(0x1C, 0x6C, 0xD4), RGB(0x10, 0x12, 0x18),
                           size >= 32 ? 1.4f : 1.0f, RGB(0xFF, 0xFF, 0xFF));
        break;
    case ICON_MIR:
        icon_mir_paint(s, x, y, size, RGB(0x44, 0x4A, 0x58), RGB(0xE4, 0xE0, 0xD8), RGB(0xD9, 0xA1, 0x5F),
                       RGB(0x10, 0x12, 0x18), size >= 32 ? 1.4f : 1.0f);
        break;
    }
}

/* ---------------- the SIEOS logo: the Facet cube ---------------- */

/*
 * A three-faced blue cube seen from a corner (light top, middle left, deep
 * right), its dark seams meeting at an amber node: the assistant.  Drawn
 * from its 512-unit design (docs/logo.svg) with antialiased edges; at 24 px
 * and below the 16x16 pixel version is used.
 */
static const char *logo_px[16] = {
    "................",
    ".......tt.......",
    ".....tttttt.....",
    "...tttttttttt...",
    "..tttttttttttt..",
    "..kkttttttttkk..",
    "..llkktkktkkrr..",
    "..llllkaakrrrr..",
    "..llllkaakrrrr..",
    "..lllllkkrrrrr..",
    "..lllllkkrrrrr..",
    "..lllllkkrrrrr..",
    "...llllkkrrrr...",
    ".....llkkrr.....",
    ".......kk.......",
    "................",
};

#define LOGO_TOP   RGB(0x8F, 0xB8, 0xE6)
#define LOGO_LEFT  RGB(0x6A, 0x95, 0xD2)
#define LOGO_RIGHT RGB(0x4A, 0x78, 0xBC)
#define LOGO_SEAM  RGB(0x1C, 0x1C, 0x1C)
#define LOGO_NODE  RGB(0xD9, 0xA3, 0x5F)

static color_t logo_px_color(char c)
{
    switch (c) {
    case 't': return LOGO_TOP;
    case 'l': return LOGO_LEFT;
    case 'r': return LOGO_RIGHT;
    case 'k': return LOGO_SEAM;
    case 'a': return LOGO_NODE;
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

/* A polygon of the design (n points, 512 units), placed at (ox, oy), k pixels a unit */
static void logo_poly(struct surface *s, float ox, float oy, float k, const float *d, int n, color_t c)
{
    float xy[2 * 6];
    for (int i = 0; i < n; i++) {
        xy[2 * i] = ox + d[2 * i] * k;
        xy[2 * i + 1] = oy + d[2 * i + 1] * k;
    }
    gfx_poly(s, xy, n, c);
}

void logo_draw(struct surface *s, int cx, int cy, int size)
{
    if (size <= 24) {
        int scale = MAX(1, size / 16);
        logo_pixels(s, cx - 8 * scale, cy - 8 * scale, scale);
        return;
    }
    static const float top[] = { 256, 26, 456, 141, 256, 256, 56, 141 };
    static const float left[] = { 56, 141, 256, 256, 256, 486, 56, 371 };
    static const float right[] = { 456, 141, 456, 371, 256, 486, 256, 256 };
    /* the seams, 16 units wide, cut where they meet the cube's outline */
    static const float seam_l[] = { 260, 249.1f, 64, 136.4f, 56, 141, 56, 150.2f, 252, 262.9f };
    static const float seam_r[] = { 260, 262.9f, 456, 150.2f, 456, 141, 448, 136.4f, 252, 249.1f };
    static const float seam_b[] = { 248, 256, 248, 481.4f, 256, 486, 264, 481.4f, 264, 256 };
    float k = size / 512.0f, ox = cx - 256 * k, oy = cy - 256 * k;
    logo_poly(s, ox, oy, k, top, 4, LOGO_TOP);
    logo_poly(s, ox, oy, k, left, 4, LOGO_LEFT);
    logo_poly(s, ox, oy, k, right, 4, LOGO_RIGHT);
    logo_poly(s, ox, oy, k, seam_l, 5, LOGO_SEAM);
    logo_poly(s, ox, oy, k, seam_r, 5, LOGO_SEAM);
    logo_poly(s, ox, oy, k, seam_b, 5, LOGO_SEAM);
    gfx_ellipse_aa(s, cx, cy, 58 * k, 58 * k, LOGO_SEAM, LOGO_SEAM);   /* the node, ringed */
    gfx_ellipse_aa(s, cx, cy, 42 * k, 42 * k, LOGO_NODE, LOGO_NODE);
}
