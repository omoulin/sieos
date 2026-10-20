/*
 * icons.c - The desktop's icons: one family of pixel maps, 16 x 16, in a
 * box 12 x scale wide (24 at 2: the Deskbar, menus, tabs), each map pixel
 * whole screen pixels, so they stay sharp at every zoom (draw.c). The SIEOS logo is a picture (logo.c, made from
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

/* One family on a 16 x 16 grid: an ink outline (#), white faces (w), one
 * amber (a) or blue (b) fill. */
static const char *const maps[I_N][16] = {
    [I_TERM] = {
        "................", ".##############.", ".#wwwwwwwwwwww#.", ".##############.",
        ".#bbbbbbbbbbbb#.", ".#bwbbbbbbbbbb#.", ".#bbwbbbbbbbbb#.", ".#bbbwbbbbbbbb#.",
        ".#bbwbbbbbbbbb#.", ".#bwbbbaaaabbb#.", ".#bbbbbbbbbbbb#.", ".#bbbbbbbbbbbb#.",
        ".##############.", "................", "................", "................" },
    [I_SIA] = {
        "................", "...##########...", "..#wwwwwwwwww#..", ".#wwwwwwwwwwww#.",
        ".#wwwwwwawwwww#.", ".#wwwwwaaawwww#.", ".#wwwwaaaaawww#.", ".#wwwwwaaawwww#.",
        ".#wwwwwwawwwww#.", "..#wwwwwwwwww#..", "...####ww####...", "......#w#.......",
        ".....#w#........", ".....##.........", "................", "................" },
    [I_FOLDER] = {
        "................", "................", "..#####.........", ".#aaaaa#........",
        ".#aaaaaa######..", ".#aaaaaaaaaaaa#.", ".##############.", ".#wwwwwwwwwwww#.",
        ".#aaaaaaaaaaaa#.", ".#aaaaaaaaaaaa#.", ".#aaaaaaaaaaaa#.", ".#aaaaaaaaaaaa#.",
        ".#aaaaaaaaaaaa#.", ".##############.", "................", "................" },
    [I_FILE] = {
        "................", "..#########.....", "..#wwwwwww##....", "..#wwwwwww#w#...",
        "..#wwwwwww####..", "..#wwwwwwwwww#..", "..#w#####wwww#..", "..#wwwwwwwwww#..",
        "..#w#######ww#..", "..#wwwwwwwwww#..", "..#w####aaaww#..", "..#wwwwwwwwww#..",
        "..#wwwwwwwwww#..", "..############..", "................", "................" },
    [I_SETTINGS] = {
        "................", "................", "....###.........", ".###aaa########.",
        "....###.........", "................", ".........###....", ".########aaa###.",
        ".........###....", "................", "......###.......", ".#####bbb######.",
        "......###.......", "................", "................", "................" },
    [I_APP] = {
        "................", ".......##.......", ".....##bb##.....", "...##bbbbbb##...",
        ".##wwbbbbbbww##.", ".#wwwwbbbbwwww#.", ".#wwwwwwwwwwww#.", ".#bbwwwaawwwbb#.",
        ".#bbbwwaawwbbb#.", ".#bbbbb##bbbbb#.", ".#bbbbb##bbbbb#.", ".##bbbb##bbbb##.",
        "...##bb##bb##...", ".....######.....", ".......##.......", "................" },
    [I_WINDOW] = {
        "................", ".#######........", ".#aaaaa#........", ".##############.",
        ".#wwwwwwwwwwww#.", ".#wwwwwwwwwwww#.", ".#wwwwwwwwwwww#.", ".#wwwwwwwwwwww#.",
        ".#wwwwwwwwwwww#.", ".#wwwwwwwwwwww#.", ".#wwwwwwwwwwww#.", ".#wwwwwwwwwwww#.",
        ".#wwwwwwwwwwww#.", ".##############.", "................", "................" },
    [I_PINS] = {
        "................", "...#.#.#.#.#....", "...#.#.#.#.#....", ".#############..",
        ".#bbbbbbbbbbb#..", ".#bbbbbbbbbbb#..", ".#bbaabbbbbbb#..", ".#bbaabbbbbbb#..",
        ".#bbbbbbbbbbb#..", ".#bbbbbbbbbbb#..", ".#############..", "...#.#.#.#.#....",
        "...#.#.#.#.#....", "................", "................", "................" },
    [I_GIT] = {
        "................", "...##...........", "..#aa#..........", "..#aa#....##....",
        "...##....#bb#...", "...#.....#bb#...", "...#......##....", "...#.......#....",
        "...#......#.....", "...#....##......", "...#..##........", "...###..........",
        "...##...........", "..#aa#..........", "..#aa#..........", "...##..........." },
    [I_MAIL] = {
        "................", "................", "................", ".##############.",
        ".##wwwwwwwwww##.", ".#w#wwwwwwww#w#.", ".#ww#wwwwww#ww#.", ".#www#wwww#www#.",
        ".#wwww#aa#wwww#.", ".#wwwwwaawwwww#.", ".#wwwwwwwwwwww#.", ".#wwwwwwwwwwww#.",
        ".##############.", "................", "................", "................" },
    [I_CAM] = {
        "................", "................", "....#####.......", ".##############.",
        ".#wwwwwwwwwwaa#.", ".#wwww####wwww#.", ".#www#bbbb#www#.", ".#ww#bbwbbb#ww#.",
        ".#ww#bbbbbb#ww#.", ".#ww#bbbbbb#ww#.", ".#www#bbbb#www#.", ".#wwww####wwww#.",
        ".#wwwwwwwwwwww#.", ".##############.", "................", "................" },
};
/* the palette of all of them */
static const char keys[] = "#wab";
static const uint32_t cols[] = { 0x1b2240, 0xffffff, 0xf6b93b, 0x5b7ef2 };

void icon(int kind, int x, int y, int scale)             /* (in a box 12 x scale wide) */
{
    if (kind < 0 || kind >= I_N) return;
    dl_pixmap(x, y, maps[kind], 16, 16, keys, cols, 12 * scale);
}

/* Its shape only, in one ink (the faces left out): the ink on the accent
 * (an icon on a window's tab), or white (on clear glass). */
static const char ink_keys[] = "#ab";
static uint32_t ink_cols[2][3];                          /* (C_INK, C_GTXT: the look's; set when drawn) */
void icon_ink(int kind, int x, int y, int scale, int white)
{
    if (kind < 0 || kind >= I_N) return;
    for (int i = 0; i < 3; i++) ink_cols[white != 0][i] = white ? C_GTXT : C_INK;
    dl_pixmap(x, y, maps[kind], 16, 16, ink_keys, ink_cols[white != 0], 12 * scale);
}
