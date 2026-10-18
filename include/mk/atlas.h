/*
 * mk/atlas.h - Windows of other programs on the desktop (port "atlas"): a
 * program draws its window's insides into shared memory, atlas shows them
 * in a window of its own kind (tab, frame, workspaces) and hands the
 * program the input that window gets. (The web browser's WPE platform,
 * user/web/platform, and any program that draws itself.) Only the user
 * logged in, or root, may open one.
 *
 *   ATLAS_WIN_OPEN    sbuf = the title -> a window id; it shows at once,
 *                     its first event is ATW_SIZE (the size its insides have)
 *   ATLAS_WIN_BUFFER  w[1] = window, w[2] = a shared memory object (SYS_SHM_*)
 *                     holding its pixels, w[3] = width | height << 16: 32 bits
 *                     a pixel (0x00RRGGBB, as a uint32_t), rows one after the
 *                     other -> 0. Atlas maps it; the program may close its
 *                     hold. A new buffer replaces the last (after ATW_SIZE)
 *   ATLAS_WIN_DAMAGE  w[1] = window, sbuf = atlas_rect_t[]: what changed
 *                     -> 0 once it is on the screen (pace frames by it)
 *   ATLAS_WIN_EVENT   w[1] = window, rbuf <- atlas_event_t[]: waits for
 *                     input (one waiting call per window) -> how many
 *   ATLAS_WIN_TITLE   w[1] = window, sbuf = the title -> 0
 *   ATLAS_WIN_CLOSE   w[1] = window -> 0
 *   ATLAS_WIN_CLIP_GET rbuf <- the clipboard's text (a terminal's selection,
 *                     or what a program put there) -> its length
 *   ATLAS_WIN_CLIP_SET sbuf = text (at most 16 KiB) -> 0: the clipboard
 *   ATLAS_WIN_THEME   rbuf <- atlas_theme_t: the desktop's look, as a program
 *                     draws to match it (the UI toolkit, sieos/ui.h) -> 0;
 *                     ATW_THEME comes to every window when it changes
 *
 * Keys: the character (Ctrl applied) or a KEY_...: Ctrl+F (6), Ctrl+V (22),
 * Ctrl+Enter and Ctrl+Shift+letter come to the window too (the desktop
 * keeps Ctrl+Space, Ctrl+T, Ctrl+W, Ctrl+Tab and Ctrl+1..4).
 *
 * Errors: -EPERM (not the user's desktop), -EBADF (no such window: it
 * was closed, by the user then ATW_CLOSE came), -EMFILE (too many).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

enum { ATLAS_WIN_OPEN = 130, ATLAS_WIN_BUFFER, ATLAS_WIN_DAMAGE, ATLAS_WIN_EVENT, ATLAS_WIN_TITLE, ATLAS_WIN_CLOSE,
       ATLAS_WIN_CLIP_GET, ATLAS_WIN_CLIP_SET, ATLAS_WIN_THEME };
#define ATLAS_WIN_MAX 8                  /* windows of other programs at once */

typedef struct { int32_t x0, y0, x1, y1; } atlas_rect_t;
/* An event. ATW_KEY: code = the character (Ctrl applied) or a KEY_... (mk/proto.h);
 * ATW_POINTER: x, y in the window's insides, buttons = MB_... held (a press or
 * release is a change of them), wheel = its movement (+: towards the user);
 * ATW_SIZE: x, y = the insides' new width and height, code = the desktop's
 * zoom in % (its text size: 100 normal, 200 twice as big); ATW_FOCUS: code 1 in,
 * 0 out; ATW_CLOSE: the user closed it (it is gone); ATW_THEME: the look or
 * the zoom changed (ATLAS_WIN_THEME again), code = the look. */
typedef struct { uint16_t type, code; int16_t x, y; uint8_t buttons; int8_t wheel; uint16_t _pad; } atlas_event_t;
enum { ATW_KEY = 1, ATW_POINTER, ATW_SIZE, ATW_FOCUS, ATW_CLOSE, ATW_THEME };
/* The theme: atlas's palette as it shows on the screen in the look (Phosphor's
 * greens, Amber's ambers; a program's pixels are shown as they are, so it
 * draws these to look like the desktop), the look (0 Phosphor, 1 Amber 1984,
 * 2 Glass, 3 BeOS), the zoom in %, the corners' radius at 100% (0: square),
 * text's scale (16.16 of a 6 x 11 font, in the window's pixels). */
enum { ATT_BG, ATT_GLASS, ATT_LINE, ATT_ACC, ATT_TXT, ATT_DIM, ATT_BAR, ATT_TERM, ATT_TERMFG, ATT_ERR,
       ATT_SEL, ATT_GRID, ATT_INK, ATT_PANEL, ATT_FIELD, ATT_GAP, ATT_ACCT, ATT_N };
typedef struct { uint16_t size, look, zoom, radius; int32_t text; uint32_t pal[ATT_N]; } atlas_theme_t;
