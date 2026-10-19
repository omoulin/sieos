/*
 * icons.c - The desktop's icons: small pixel maps, drawn at any whole
 * scale (12 x 12 at 2: 24 pixels on the Deskbar and in menus); the folder
 * keeps BeOS's yellow. The SIEOS logo is a picture (logo.c, made from
 * logo-glass.svg by tools/mklogo.py), smoothly scaled.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"

extern const uint32_t logo_pal[2][64];                  /* logo.c (tools/mklogo.py) */
extern const uint8_t logo32[32 * 32], logo64[64 * 64];
void logo(int x, int y, int scale)                       /* (16 x scale wide: the picture nearest that on the screen) */
{
    int n = 16 * scale, big = (int)(((int64_t)n * draw_zoom_get()) >> 16) > 40;
    dl_pic(x, y, x + n, y + n, big ? logo64 : logo32, logo_pal[big], big ? 64 : 32, big ? 64 : 32);
}

static const char *const maps[I_N][12] = {
    [I_TERM] = {
        "............", ".kkkkkkkkkk.", ".kddddddddk.", ".kdgddddddk.", ".kddgdddddk.", ".kdgdgggddk.",
        ".kddddddddk.", ".kddddddddk.", ".kkkkkkkkkk.", "....kkkk....", "..kkkkkkkk..", "............" },
    [I_SIA] = {
        "............", "..aaaaaaaa..", ".aAAAAAAAAa.", ".aAAAAAAAAa.", ".aAAbAAbAAa.", ".aAAAAAAAAa.",
        ".aAAAAAAAAa.", "..aaaaAAaa..", "......aAa...", ".......aa...", "............", "............" },
    [I_FOLDER] = {
        "............", ".yyyy.......", "yYYYYy......", "yyyyyyyyyyy.", "yYYYYYYYYYy.", "yYYYYYYYYYy.",
        "yYYYYYYYYYy.", "yYYYYYYYYYy.", "yYYYYYYYYYy.", "yyyyyyyyyyy.", "............", "............" },
    [I_FILE] = {
        ".wwwwww.....", ".wppppww....", ".wpppppww...", ".wppppppw...", ".wpsssppw...", ".wppppppw...",
        ".wpssssspw..", ".wppppppw...", ".wpsssppw...", ".wppppppw...", ".wwwwwwww...", "............" },
    [I_SETTINGS] = {
        "............", "..K.........", "-K-K--------", "..K.........", "............", "........K...",
        "-------K-K--", "........K...", "............", ".....K......", "----K-K-----", ".....K......" },
    [I_APP] = {
        "....tttt....", "..tttttttt..", "tttttttttttt", "lllllrrrrrrr", "lllllrrrrrrr", "lllllrrrrrrr",
        "lllllrrrrrrr", "lllllrrrrrrr", "lllllrrrrrrr", "..lllrrrrr..", "....lrrr....", "............" },
    [I_PINS] = {
        "............", ".gggg..gggg.", ".g..g..g..g.", ".g..g..g..g.", ".g..g..g..g.", "gg..gggg..gg",
        "............", "kkkkkkkkkkkk", "kddddddddddk", "kkkkkkkkkkkk", "k.k.k.k.k.k.", "............" },
    [I_GIT] = {                                         /* (a branch leaving the main line) */
        "............", "..aa....aa..", ".aaaa..aaaa.", "..aa....aa..", "..g.....g...", "..g....g....",
        "..g...g.....", "..g..g......", "..g.g.......", "..aa........", ".aaaa.......", "..aa........" },
    [I_MAIL] = {
        "............", "kkkkkkkkkkkk", "kkppppppppkk", "kpkppppppkpk", "kppkppppkppk", "kpppkppkpppk",
        "kppppkkppppk", "kppppppppppk", "kppppppppppk", "kkkkkkkkkkkk", "............", "............" },
    [I_CAM] = {
        "............", "....kkkk....", "kkkkkkkkkkkk", "kddddddddddk", "kdddaaaaddgk", "kddaAAAAaddk",
        "kddaAbAAaddk", "kddaAAAAaddk", "kdddaaaadddk", "kddddddddddk", "kkkkkkkkkkkk", "............" },
    [I_WINDOW] = {
        "............", "aaaaaa......", "aAAAAa......", "kkkkkkkkkkk.", "kGGGGGGGGGk.", "kGGGGGGGGGk.",
        "kGGGGGGGGGk.", "kGGGGGGGGGk.", "kGGGGGGGGGk.", "kkkkkkkkkkk.", "............", "............" },
};
/* the palette of all of them (each map uses some of these characters) */
static const char keys[] = "kdgaAbyYwpsK-tlrG";
static const uint32_t cols[] = {
    0x8fa0b8, 0x070b14, 0x4be3c1, 0x2f9e88, 0x4be3c1, 0x0a0f1c, 0xc89a2c, 0xffcb4a, 0x8fa0b8, 0xe6ecf5,
    0x6bb8ff, 0x4be3c1, 0x8fa0b8, 0xaed0ff, 0x3a6ed8, 0x5c8ee6, 0x141a28 };

void icon(int kind, int x, int y, int scale)
{
    if (kind < 0 || kind >= I_N) return;
    dl_pixmap(x, y, maps[kind], 12, 12, keys, cols, scale);
}

/* The outline and the marks only (no fills), in one ink: the ink on the
 * accent (an icon on a window's tab), or white (on clear glass). */
static const char ink_keys[] = "kgabywsK-";
static uint32_t ink_cols[2][9];                          /* (C_INK, C_GTXT: the look's; set when drawn) */
void icon_ink(int kind, int x, int y, int scale, int white)
{
    if (kind < 0 || kind >= I_N) return;
    for (int i = 0; i < 9; i++) ink_cols[white != 0][i] = white ? C_GTXT : C_INK;
    dl_pixmap(x, y, maps[kind], 12, 12, ink_keys, ink_cols[white != 0], scale);
}
