/*
 * skin_icons.c - The icons of the BeOS-style and IRIX-style skins
 * (original drawings in the manner of those desktops).
 *
 * BeOS style: three-quarter views from above, bold dark outlines,
 * saturated gradients.  IRIX style: soft pastel volumes with thin outlines,
 * in the manner of Indigo Magic.  Every shape is an antialiased polygon on
 * a 48-unit design grid, scaled to the icon size.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <math.h>
#include "facet/theme.h"
#include "facet/ui.h"

#define NONE 0xFF000000U
#define MAXPTS 48

/* A polygon of n design points (x, y pairs, 0..48) at (x, y), size pixels across. */
static void shape(struct surface *s, int x, int y, int size, color_t top, color_t bottom, color_t outline, float ow,
                  int n, const float *d)
{
    float xy[2 * MAXPTS];
    for (int i = 0; i < n && i < MAXPTS; i++) {
        xy[2 * i] = x + d[2 * i] * size / 48.0f;
        xy[2 * i + 1] = y + d[2 * i + 1] * size / 48.0f;
    }
    gfx_poly_vgradient(s, xy, n, top, bottom);
    if (outline != NONE) {
        float w = ow * size / 48.0f;
        gfx_stroke(s, xy, n, true, w < 1 ? 1 : w, outline);
    }
}

static void line(struct surface *s, int x, int y, int size, float x0, float y0, float x1, float y1, float w,
                 color_t c)
{
    float xy[4] = { x + x0 * size / 48.0f, y + y0 * size / 48.0f, x + x1 * size / 48.0f, y + y1 * size / 48.0f };
    float ww = w * size / 48.0f;
    gfx_stroke(s, xy, 2, false, ww < 1 ? 1 : ww, c);
}

static void oval(struct surface *s, int x, int y, int size, float cx, float cy, float rx, float ry, color_t top,
                 color_t bottom, color_t outline, float ow)
{
    float xy[2 * 40];
    for (int i = 0; i < 40; i++) {
        float t = i * 6.2831853f / 40;
        xy[2 * i] = cx + rx * cosf(t);
        xy[2 * i + 1] = cy + ry * sinf(t);
    }
    shape(s, x, y, size, top, bottom, outline, ow, 40, xy);
}

#define SHAPE(top, bot, out, ow, ...) do { static const float d_[] = { __VA_ARGS__ }; \
        shape(s, x, y, size, top, bot, out, ow, (int)(sizeof(d_) / sizeof(d_[0]) / 2), d_); } while (0)

/* The web browser (NetSurf): a globe with dashed meridian and equator and a
 * six-pointed star, after NetSurf's own icon; each skin gives its colours. */
void icon_browser_paint(struct surface *s, int x, int y, int size, color_t top, color_t bottom, color_t outline,
                        float ow, color_t star)
{
    oval(s, x, y, size, 24, 25, 19, 19, top, bottom, outline, ow);
    float w = size >= 32 ? 1.6f : 1.0f;
    for (int arc = 0; arc < 2; arc++)            /* dashes: every other tenth of each arc */
        for (int seg = 0; seg < 10; seg += 2) {
            float xy[2 * 4];
            for (int j = 0; j < 4; j++) {
                float t = (seg + j / 3.0f) / 10.0f;
                float a = arc == 0 ? -1.5708f + t * 3.1416f : t * 3.1416f;
                float dx = arc == 0 ? 24 + 9 * cosf(a) : 24 + 19 * cosf(a);
                float dy = arc == 0 ? 25 + 19 * sinf(a) : 27 + 7 * sinf(a);
                xy[2 * j] = x + dx * size / 48.0f;
                xy[2 * j + 1] = y + dy * size / 48.0f;
            }
            gfx_stroke(s, xy, 4, false, w, outline == NONE ? bottom : outline);
        }
    float st[2 * 12];                            /* the star, upper right */
    for (int i = 0; i < 12; i++) {
        float a = -1.5708f + i * 3.1416f / 6, r = i & 1 ? 3.4f : 8.0f;
        st[2 * i] = x + (33 + r * cosf(a)) * size / 48.0f;
        st[2 * i + 1] = y + (15 + r * sinf(a)) * size / 48.0f;
    }
    gfx_poly(s, st, 12, star);
    gfx_stroke(s, st, 12, true, w, outline == NONE ? bottom : outline);
}

/* ================= BeOS style ================= */

#define BO RGB(0x1C, 0x1C, 0x1C)                   /* the bold outline */
#define BW 1.6f

static void be_folder(struct surface *s, int x, int y, int size, bool home)
{
    SHAPE(RGB(0xF2, 0xB8, 0x10), RGB(0xC8, 0x86, 0x00), BO, BW, 8, 10, 20, 10, 24, 15, 43, 15, 41, 36, 6, 36);
    SHAPE(RGB(0xFF, 0xE2, 0x5A), RGB(0xF4, 0xB0, 0x00), BO, BW, 3, 20, 37, 20, 44, 40, 10, 40);
    line(s, x, y, size, 7, 23, 36, 23, 1.2f, RGB(0xFF, 0xF4, 0xB0));
    if (home) {
        SHAPE(RGB(0xE0, 0x3C, 0x2C), RGB(0xA8, 0x20, 0x14), BO, 1.2f, 18, 30, 25, 23, 32, 30);
        SHAPE(RGB(0xFF, 0xFF, 0xFF), RGB(0xD8, 0xD8, 0xD8), BO, 1.2f, 20, 30, 30, 30, 30, 37, 20, 37);
    }
}

static void be_terminal(struct surface *s, int x, int y, int size)
{
    SHAPE(RGB(0xFF, 0xFF, 0xFF), RGB(0xDC, 0xDC, 0xDC), BO, BW, 6, 12, 14, 6, 44, 6, 36, 12);
    SHAPE(RGB(0x9C, 0x9C, 0x9C), RGB(0x70, 0x70, 0x70), BO, BW, 36, 12, 44, 6, 44, 32, 36, 38);
    SHAPE(RGB(0xE8, 0xE8, 0xE8), RGB(0xB4, 0xB4, 0xB4), BO, BW, 6, 12, 36, 12, 36, 38, 6, 38);
    SHAPE(RGB(0x24, 0x24, 0x24), RGB(0x08, 0x08, 0x08), NONE, 0, 9, 15, 33, 15, 33, 34, 9, 34);
    line(s, x, y, size, 12, 20, 16, 23, 1.6f, RGB(0xFF, 0xC8, 0x30));
    line(s, x, y, size, 16, 23, 12, 26, 1.6f, RGB(0xFF, 0xC8, 0x30));
    line(s, x, y, size, 18, 27, 24, 27, 1.6f, RGB(0xFF, 0xC8, 0x30));
}

static void be_file(struct surface *s, int x, int y, int size, bool program)
{
    if (program) {                                 /* a blue brick */
        SHAPE(RGB(0xAE, 0xD0, 0xFF), RGB(0x78, 0xA4, 0xF0), BO, BW, 24, 6, 43, 14, 24, 22, 5, 14);
        SHAPE(RGB(0x3A, 0x6E, 0xD8), RGB(0x22, 0x46, 0xA0), BO, BW, 5, 14, 24, 22, 24, 42, 5, 34);
        SHAPE(RGB(0x5C, 0x8E, 0xE6), RGB(0x34, 0x62, 0xC0), BO, BW, 24, 22, 43, 14, 43, 34, 24, 42);
        return;
    }
    SHAPE(RGB(0xFF, 0xFF, 0xFF), RGB(0xE2, 0xE2, 0xE2), BO, BW, 11, 5, 30, 5, 38, 13, 38, 43, 11, 43);
    SHAPE(RGB(0xC8, 0xC8, 0xC8), RGB(0xA8, 0xA8, 0xA8), BO, 1.2f, 30, 5, 30, 13, 38, 13);
    for (int i = 0; i < 5; i++)
        line(s, x, y, size, 15, 19 + i * 5, 34, 19 + i * 5, 1.1f, RGB(0x6E, 0x8C, 0xC8));
}

static void be_monitor(struct surface *s, int x, int y, int size)
{
    SHAPE(RGB(0xF4, 0xF4, 0xF4), RGB(0xC0, 0xC0, 0xC0), BO, BW, 4, 12, 36, 12, 36, 40, 4, 40);
    SHAPE(RGB(0x98, 0x98, 0x98), RGB(0x78, 0x78, 0x78), BO, BW, 36, 12, 44, 6, 44, 34, 36, 40);
    SHAPE(RGB(0xFF, 0xFF, 0xFF), RGB(0xE4, 0xE4, 0xE4), BO, BW, 4, 12, 12, 6, 44, 6, 36, 12);
    static const float h[] = { 12, 20, 9, 16 };
    static const color_t c[] = { RGB(0xE8, 0x40, 0x30), RGB(0x40, 0xB8, 0x40), RGB(0x40, 0x70, 0xE0),
                                 RGB(0xF0, 0xC0, 0x20) };
    for (int i = 0; i < 4; i++) {
        float x0 = 8 + i * 7, y0 = 37 - h[i];
        float d[] = { x0, y0, x0 + 5, y0, x0 + 5, 37, x0, 37 };
        shape(s, x, y, size, c[i], color_shade(c[i], -50), BO, 1.0f, 4, d);
    }
}

static void be_clock(struct surface *s, int x, int y, int size)
{
    oval(s, x, y, size, 24, 26, 19, 17, RGB(0xC0, 0xC0, 0xC0), RGB(0x70, 0x70, 0x70), BO, BW);
    oval(s, x, y, size, 24, 25, 15, 13, RGB(0xFF, 0xFF, 0xFF), RGB(0xE4, 0xE4, 0xE4), BO, 1.0f);
    line(s, x, y, size, 24, 25, 24, 15, 2.4f, BO);
    line(s, x, y, size, 24, 25, 32, 28, 2.4f, BO);
    line(s, x, y, size, 24, 25, 17, 31, 1.0f, RGB(0xD0, 0x20, 0x20));
}

static void be_info(struct surface *s, int x, int y, int size)
{
    oval(s, x, y, size, 24, 24, 19, 19, RGB(0x6E, 0xA0, 0xF0), RGB(0x2A, 0x56, 0xB8), BO, BW);
    oval(s, x, y, size, 24, 14, 3.2f, 3.2f, RGB(0xFF, 0xFF, 0xFF), RGB(0xE8, 0xE8, 0xE8), NONE, 0);
    SHAPE(RGB(0xFF, 0xFF, 0xFF), RGB(0xE0, 0xE0, 0xE0), NONE, 0, 21, 20, 27, 20, 27, 35, 21, 35);
}

static void be_logout(struct surface *s, int x, int y, int size)
{
    SHAPE(RGB(0x5A, 0x3A, 0x22), RGB(0x3A, 0x24, 0x14), BO, BW, 8, 6, 30, 6, 30, 44, 8, 44);
    SHAPE(RGB(0xC0, 0x84, 0x48), RGB(0x8C, 0x58, 0x2C), BO, BW, 8, 6, 22, 10, 22, 42, 8, 44);
    oval(s, x, y, size, 19, 26, 1.6f, 1.6f, RGB(0xFF, 0xD8, 0x40), RGB(0xC0, 0x90, 0x10), NONE, 0);
    SHAPE(RGB(0xF0, 0x40, 0x30), RGB(0xB0, 0x20, 0x14), BO, 1.2f, 30, 22, 38, 22, 38, 17, 46, 25, 38, 33, 38, 28, 30, 28);
}

static void be_disk(struct surface *s, int x, int y, int size)
{
    SHAPE(RGB(0xF8, 0xF8, 0xF8), RGB(0xD8, 0xD8, 0xD8), BO, BW, 4, 22, 14, 12, 44, 12, 34, 22);
    SHAPE(RGB(0x90, 0x90, 0x90), RGB(0x68, 0x68, 0x68), BO, BW, 34, 22, 44, 12, 44, 26, 34, 36);
    SHAPE(RGB(0xC8, 0xC8, 0xC8), RGB(0xA0, 0xA0, 0xA0), BO, BW, 4, 22, 34, 22, 34, 36, 4, 36);
    SHAPE(RGB(0x60, 0xF0, 0x60), RGB(0x20, 0xA0, 0x20), NONE, 0, 8, 28, 13, 28, 13, 31, 8, 31);
}

static void be_network(struct surface *s, int x, int y, int size)
{
    oval(s, x, y, size, 24, 24, 18, 18, RGB(0x60, 0xA8, 0xFF), RGB(0x1E, 0x50, 0xB8), BO, BW);
    SHAPE(RGB(0x60, 0xD0, 0x60), RGB(0x28, 0x90, 0x30), NONE, 0, 13, 14, 22, 11, 26, 18, 20, 24, 14, 22);
    SHAPE(RGB(0x60, 0xD0, 0x60), RGB(0x28, 0x90, 0x30), NONE, 0, 27, 26, 36, 24, 37, 33, 30, 37, 26, 32);
    float ring[2 * 40];
    for (int i = 0; i < 40; i++) {
        float t = i * 6.2831853f / 40;
        ring[2 * i] = x + (24 + 22 * cosf(t)) * size / 48.0f;
        ring[2 * i + 1] = y + (26 + 6 * sinf(t)) * size / 48.0f;
    }
    gfx_stroke(s, ring, 40, true, size >= 32 ? 1.6f : 1.0f, RGB(0xFF, 0xCB, 0x00));
}

/* ================= IRIX style ================= */

#define IO RGB(0x2E, 0x34, 0x44)                   /* the thin outline */
#define IW 0.9f

static void ix_folder(struct surface *s, int x, int y, int size, bool home)
{
    SHAPE(RGB(0xA6, 0xB2, 0xD8), RGB(0x7C, 0x88, 0xB4), IO, IW, 5, 12, 18, 12, 22, 16, 42, 16, 42, 38, 5, 38);
    SHAPE(RGB(0xD4, 0xDC, 0xF4), RGB(0xA0, 0xAE, 0xDA), IO, IW, 5, 20, 42, 20, 42, 38, 5, 38);
    line(s, x, y, size, 8, 24, 38, 24, 1.0f, RGB(0xEE, 0xF2, 0xFF));
    if (home) {
        SHAPE(RGB(0xC0, 0x70, 0xB0), RGB(0x8C, 0x40, 0x80), IO, IW, 16, 30, 24, 23, 32, 30);
        SHAPE(RGB(0xF4, 0xF0, 0xFA), RGB(0xD0, 0xCC, 0xE0), IO, IW, 18, 30, 30, 30, 30, 36, 18, 36);
    }
}

static void ix_terminal(struct surface *s, int x, int y, int size)
{
    SHAPE(RGB(0xC8, 0xCE, 0xDA), RGB(0x98, 0xA0, 0xB2), IO, IW, 6, 6, 38, 6, 38, 34, 6, 34);
    SHAPE(RGB(0x8A, 0x92, 0xA6), RGB(0x6A, 0x72, 0x86), IO, IW, 38, 6, 44, 10, 44, 36, 38, 34);
    SHAPE(RGB(0x1E, 0x4A, 0x58), RGB(0x0C, 0x24, 0x2E), IO, IW, 10, 10, 34, 10, 34, 30, 10, 30);
    SHAPE(RGB(0xA8, 0xB0, 0xC2), RGB(0x80, 0x88, 0x9C), IO, IW, 12, 38, 40, 38, 44, 43, 8, 43);
    line(s, x, y, size, 13, 15, 22, 15, 1.4f, RGB(0x9C, 0xF0, 0xE0));
    line(s, x, y, size, 13, 20, 28, 20, 1.4f, RGB(0x9C, 0xF0, 0xE0));
    line(s, x, y, size, 13, 25, 18, 25, 1.4f, RGB(0x9C, 0xF0, 0xE0));
}

static void ix_file(struct surface *s, int x, int y, int size, bool program)
{
    if (program) {                                 /* a pastel block */
        SHAPE(RGB(0xE0, 0xC8, 0xF0), RGB(0xC0, 0xA0, 0xE0), IO, IW, 24, 7, 42, 15, 24, 23, 6, 15);
        SHAPE(RGB(0x86, 0x64, 0xB0), RGB(0x60, 0x44, 0x88), IO, IW, 6, 15, 24, 23, 24, 41, 6, 33);
        SHAPE(RGB(0xA8, 0x88, 0xCE), RGB(0x82, 0x62, 0xAC), IO, IW, 24, 23, 42, 15, 42, 33, 24, 41);
        line(s, x, y, size, 15, 12, 33, 19, 1.0f, RGB(0x70, 0xD0, 0xC8));
        return;
    }
    SHAPE(RGB(0xFA, 0xF8, 0xFF), RGB(0xDA, 0xD8, 0xEA), IO, IW, 11, 5, 30, 5, 38, 13, 38, 43, 11, 43);
    SHAPE(RGB(0xC4, 0xC4, 0xDA), RGB(0xA8, 0xA8, 0xC4), IO, IW, 30, 5, 30, 13, 38, 13);
    for (int i = 0; i < 5; i++)
        line(s, x, y, size, 15, 19 + i * 5, 34, 19 + i * 5, 1.0f, RGB(0x90, 0xA0, 0xD0));
}

static void ix_monitor(struct surface *s, int x, int y, int size)
{
    SHAPE(RGB(0xD4, 0xD8, 0xE2), RGB(0xA4, 0xAA, 0xBA), IO, IW, 4, 8, 44, 8, 44, 40, 4, 40);
    SHAPE(RGB(0x26, 0x2E, 0x40), RGB(0x16, 0x1C, 0x2A), IO, IW, 8, 12, 40, 12, 40, 36, 8, 36);
    static const float h[] = { 10, 18, 13, 20 };
    static const color_t c[] = { RGB(0xF0, 0xA0, 0x50), RGB(0x70, 0xD8, 0xC8), RGB(0xC0, 0x90, 0xE8),
                                 RGB(0xE8, 0xE0, 0x80) };
    for (int i = 0; i < 4; i++) {
        float x0 = 11 + i * 7.5f, y0 = 34 - h[i];
        float d[] = { x0, y0, x0 + 5, y0, x0 + 5, 34, x0, 34 };
        shape(s, x, y, size, c[i], color_shade(c[i], -40), NONE, 0, 4, d);
    }
}

static void ix_clock(struct surface *s, int x, int y, int size)
{
    oval(s, x, y, size, 24, 24, 19, 19, RGB(0xB8, 0xC0, 0xD8), RGB(0x78, 0x82, 0xA4), IO, IW);
    oval(s, x, y, size, 24, 24, 15, 15, RGB(0xFA, 0xFA, 0xFF), RGB(0xDC, 0xDE, 0xEA), IO, IW);
    for (int i = 0; i < 12; i++) {
        float t = i * 6.2831853f / 12;
        line(s, x, y, size, 24 + 12.5f * cosf(t), 24 + 12.5f * sinf(t), 24 + 14 * cosf(t), 24 + 14 * sinf(t), 1.0f, IO);
    }
    line(s, x, y, size, 24, 24, 24, 14, 2.0f, IO);
    line(s, x, y, size, 24, 24, 31, 27, 2.0f, IO);
    oval(s, x, y, size, 24, 24, 1.8f, 1.8f, RGB(0xA8, 0x2E, 0x2E), RGB(0xA8, 0x2E, 0x2E), NONE, 0);
}

static void ix_info(struct surface *s, int x, int y, int size)
{
    oval(s, x, y, size, 24, 24, 19, 19, RGB(0xB0, 0xA0, 0xE8), RGB(0x6C, 0x5C, 0xB0), IO, IW);
    oval(s, x, y, size, 24, 14, 3, 3, RGB(0xFF, 0xFF, 0xFF), RGB(0xE8, 0xE8, 0xF4), NONE, 0);
    SHAPE(RGB(0xFF, 0xFF, 0xFF), RGB(0xE0, 0xE0, 0xF0), NONE, 0, 21.5f, 20, 26.5f, 20, 26.5f, 35, 21.5f, 35);
}

static void ix_logout(struct surface *s, int x, int y, int size)
{
    SHAPE(RGB(0x3E, 0x44, 0x58), RGB(0x2A, 0x2E, 0x3E), IO, IW, 8, 6, 30, 6, 30, 44, 8, 44);
    SHAPE(RGB(0xB8, 0xC0, 0xD8), RGB(0x88, 0x90, 0xAC), IO, IW, 8, 6, 22, 10, 22, 42, 8, 44);
    oval(s, x, y, size, 19, 26, 1.5f, 1.5f, RGB(0xF0, 0xD0, 0x80), RGB(0xC0, 0x9C, 0x50), NONE, 0);
    SHAPE(RGB(0xE0, 0x70, 0x70), RGB(0xA8, 0x2E, 0x2E), IO, IW, 30, 22, 38, 22, 38, 17, 46, 25, 38, 33, 38, 28, 30, 28);
}

static void ix_disk(struct surface *s, int x, int y, int size)
{
    SHAPE(RGB(0xE4, 0xE8, 0xF2), RGB(0xC4, 0xCA, 0xDA), IO, IW, 4, 22, 14, 12, 44, 12, 34, 22);
    SHAPE(RGB(0x8C, 0x94, 0xAA), RGB(0x6C, 0x74, 0x8A), IO, IW, 34, 22, 44, 12, 44, 26, 34, 36);
    SHAPE(RGB(0xB8, 0xBE, 0xD0), RGB(0x94, 0x9A, 0xB0), IO, IW, 4, 22, 34, 22, 34, 36, 4, 36);
    SHAPE(RGB(0x80, 0xF0, 0xD8), RGB(0x30, 0xB0, 0x98), NONE, 0, 8, 28, 13, 28, 13, 31, 8, 31);
}

static void ix_network(struct surface *s, int x, int y, int size)
{
    oval(s, x, y, size, 24, 24, 18, 18, RGB(0x90, 0xC8, 0xE8), RGB(0x4C, 0x78, 0xB0), IO, IW);
    SHAPE(RGB(0xB8, 0xE8, 0xC8), RGB(0x70, 0xB8, 0x98), NONE, 0, 13, 14, 22, 11, 26, 18, 20, 24, 14, 22);
    SHAPE(RGB(0xB8, 0xE8, 0xC8), RGB(0x70, 0xB8, 0x98), NONE, 0, 27, 26, 36, 24, 37, 33, 30, 37, 26, 32);
    float ring[2 * 40];
    for (int i = 0; i < 40; i++) {
        float t = i * 6.2831853f / 40;
        ring[2 * i] = x + (24 + 22 * cosf(t)) * size / 48.0f;
        ring[2 * i + 1] = y + (26 + 6 * sinf(t)) * size / 48.0f;
    }
    gfx_stroke(s, ring, 40, true, size >= 32 ? 1.4f : 1.0f, RGB(0xC0, 0x70, 0xB0));
}

void skin_icon_draw(int skin, struct surface *s, int kind, int x, int y, int size)
{
    bool be = skin == FCT_SKIN_BEOS;
    switch (kind) {
    case ICON_TERMINAL: be ? be_terminal(s, x, y, size) : ix_terminal(s, x, y, size); break;
    case ICON_FOLDER:   be ? be_folder(s, x, y, size, false) : ix_folder(s, x, y, size, false); break;
    case ICON_HOME:     be ? be_folder(s, x, y, size, true) : ix_folder(s, x, y, size, true); break;
    case ICON_FILE:     be ? be_file(s, x, y, size, false) : ix_file(s, x, y, size, false); break;
    case ICON_PROGRAM:  be ? be_file(s, x, y, size, true) : ix_file(s, x, y, size, true); break;
    case ICON_MONITOR:  be ? be_monitor(s, x, y, size) : ix_monitor(s, x, y, size); break;
    case ICON_CLOCK:    be ? be_clock(s, x, y, size) : ix_clock(s, x, y, size); break;
    case ICON_INFO:     be ? be_info(s, x, y, size) : ix_info(s, x, y, size); break;
    case ICON_LOGOUT:   be ? be_logout(s, x, y, size) : ix_logout(s, x, y, size); break;
    case ICON_DISK:     be ? be_disk(s, x, y, size) : ix_disk(s, x, y, size); break;
    case ICON_NETWORK:  be ? be_network(s, x, y, size) : ix_network(s, x, y, size); break;
    case ICON_BROWSER:
        if (be)
            icon_browser_paint(s, x, y, size, RGB(0x78, 0xC8, 0xFF), RGB(0x18, 0x60, 0xD0), BO, BW, RGB(0xFF, 0xFF, 0xFF));
        else
            icon_browser_paint(s, x, y, size, RGB(0xA8, 0xD8, 0xF0), RGB(0x4C, 0x80, 0xB8), IO, IW, RGB(0xF4, 0xF4, 0xF0));
        break;
    }
}
