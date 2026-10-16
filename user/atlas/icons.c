/*
 * icons.c - The desktop's icons: small pixel maps, drawn at any whole
 * scale (12 x 12 at 2: 24 pixels on the Deskbar and in menus). The SIEOS
 * cube is the older desktop's (16 x 16); the folder keeps BeOS's yellow.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"

static const char *const logo_map[16] = {
    "................", ".......tt.......", ".....tttttt.....", "...tttttttttt...",
    "..tttttttttttt..", "..kkttttttttkk..", "..llkktkktkkrr..", "..llllkaakrrrr..",
    "..llllkaakrrrr..", "..lllllkkrrrrr..", "..lllllkkrrrrr..", "..lllllkkrrrrr..",
    "...llllkkrrrr...", ".....llkkrr.....", ".......kk.......", "................",
};
static const uint32_t logo_cols[] = { 0x8fb8e6, 0x6a95d2, 0x4a78bc, 0x1c1c1c, 0xd9a35f };

void logo(int x, int y, int scale) { dl_pixmap(x, y, logo_map, 16, 16, "tlrka", logo_cols, scale); }

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

/* The outline and the marks only (no fills), in the ink on the accent:
 * an icon on a window's tab. */
static const char ink_keys[] = "kgabywsK-";
static uint32_t ink_cols[9];                             /* (C_INK: the look's; set when drawn) */
void icon_ink(int kind, int x, int y, int scale)
{
    if (kind < 0 || kind >= I_N) return;
    for (int i = 0; i < 9; i++) ink_cols[i] = C_INK;
    dl_pixmap(x, y, maps[kind], 12, 12, ink_keys, ink_cols, scale);
}
