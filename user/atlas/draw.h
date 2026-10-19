/*
 * draw.h - The drawing layer of the desktop: a display list rasterized
 * straight into video memory, one region at a time.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

extern int PW, PH;                  /* the screen, 32 bits per pixel: 1920 x 1080, or the size the
                                       firmware's screen has (UEFI; screen.c sets them) */
extern int SW, SH;                  /* the picture atlas lays out and draws in: PW x PH / the zoom */
#define CW 6                        /* a character cell of the font, in pixels at scale 1 */
#define CH 11

/* The screen (screen.c): where the SW x SH picture goes, the pitch
 * (pixels from one row to the next) and the colour order (swap: the
 * screen wants 0x00BBGGRR, Atlas draws 0x00RRGGBB). */
typedef struct { uint32_t *fb; int pitch, swap, resizable; const char *name; } screen_t;
int  screen_init(screen_t *s);      /* 0: ready, at SW x SH if the screen can take any size
                                       (resizable: QEMU's VGA and ramfb, the Pi 4's firmware);
                                       a firmware's fixed screen sets SW x SH itself. Called
                                       again with a new SW x SH: the screen changes size.
                                       PW 0: the screen's own size. */
int  screen_native(int *w, int *h); /* a resizable screen's own size (the Intel display's
                                       panel, igfx.c): 1, or 0 if it has none */
int  screen_backlight(int pct);     /* the panel's brightness in %: set it (pct >= 0) -> now; -1: none */
int  screen_wake(void);            /* after a suspend: the Intel display set up again -> 0; -1: not it */
void screen_blank(int off);        /* the panel's backlight off (1: the lid closed) or on again */
int  screen_hpd(void);             /* the Intel display's monitor ports: their hot-plug pins; -1: not it */
void screen_plug(void);            /* each monitor lit or turned off as its pin says */
long rtc_seconds(void);            /* the time of day in seconds (UTC), or -1: no clock */

void draw_init(const screen_t *s);
void dl_reset(void);                /* start a new picture */
void dl_desk(void);                 /* the desk: its colours, the whole screen */
void dl_rect(int x0, int y0, int x1, int y1, int r, uint32_t c);           /* filled, corner radius r (c FROST | tint: glass; GLOSS: glossy) */
void dl_glass(int x0, int y0, int x1, int y1, int r, uint32_t c, int hx0, int hy0, int hx1, int hy1);   /* glass (c FROST),
                                       not worked out in the hole (what will cover it: a window's insides) */
void dl_frame(int x0, int y0, int x1, int y1, int r, int t, uint32_t c);   /* outline, t pixels thick */
void dl_shade(int x0, int y0, int x1, int y1, int r, int k);               /* darken what is under it: k 1 to 3/4, 2 to 1/2, 3 to 7/8 */
void dl_shadow(int x0, int y0, int x1, int y1, int r, int k, int hx0, int hy0, int hx1, int hy1);   /* the same, k 3, but not
                                       in the hole (what will cover it: a window over its own shadow) */
/* Text. Its size is a 16.16 fixed-point scale (S(2): twice the font's
 * 6 x 11 pixels), any value, so text grows smoothly with the zoom. */
#define S(n) ((n) << 16)
void dl_text(int x, int y, const char *s, int n, int scale, uint32_t c,
             int cx0, int cy0, int cx1, int cy1);                         /* clipped to the box */
int  text_w(int n, int scale);      /* width in pixels of n characters */
/* A pixel image: h rows of w characters, each a colour (cols[i] for keys[i];
 * '.' and ' ' leave what is under it), every pixel scale x scale. The rows
 * are not copied: they must stay (static data). */
void dl_pixmap(int x, int y, const char *const *rows, int w, int h, const char *keys, const uint32_t *cols, int scale);
void dl_image(int x0, int y0, int x1, int y1, const uint32_t *px, int w, int h);   /* w x h pixels (0x00RRGGBB, rows
                                       one after the other) stretched into the box; not copied: they must stay */
void dl_client(int x0, int y0, int x1, int y1, const uint32_t *px, int w, int h, int raw);   /* the same; raw: another
                                       program's pixels, shown as they are (a shadow darkens them, nothing tints them) */
void dl_pic(int x0, int y0, int x1, int y1, const uint8_t *px, const uint32_t *pal, int w, int h);   /* w x h pixels,
                                       each an index into pal (0xAARRGGBB: A its opacity), smoothly scaled into the box */
void dl_pointer(int x, int y);      /* where the mouse pointer is drawn (on top of everything) */
void present(int x0, int y0, int x1, int y1);   /* draw this region of the picture on the screen (picture coordinates) */
void present_pointer(int ox, int oy, int x, int y);   /* the pointer moved from (ox, oy): both places */
/* The colours the desktop draws in (atlas.h's C_...), the look's: glass.
 * A colour with FROST is glass of that tint (dl_rect, dl_glass): what is
 * behind it shows through, blurred, the tint laid over it by its ALPHA. A
 * fill or an outline with an ALPHA (and no FROST) is laid over what is
 * there, unblurred: see-through. */
#define FROST 0x02000000u
#define GLOSS 0x04000000u           /* (a solid fill with glass's light: the accent) */
#define ALPHA(k) ((uint32_t)(k) << 27)   /* k/32 of the colour (1 to 31) */
#define ALPHA_M 0xf8000000u
#define ALPHA_OF(c) ((int)((c) >> 27))
enum { PAL_BG, PAL_GLASS, PAL_LINE, PAL_ACC, PAL_TXT, PAL_DIM, PAL_BAR, PAL_TERM, PAL_TERMFG, PAL_ERR,
       PAL_SEL, PAL_GRID, PAL_INK, PAL_PANEL, PAL_FIELD, PAL_GAP, PAL_ACCT,
       PAL_CLEAR, PAL_GTXT, PAL_GDIM, PAL_GLINE, PAL_WASH, PAL_CELL, PAL_SMOKE, PAL_SMOKEFG, NPAL };
extern uint32_t pal[NPAL];
uint32_t draw_shown(uint32_t c);    /* a colour as it shows on the screen (glass: over the desk's middle) */
/* The zoom, 16.16: screen pixels per picture pixel (65536: 1:1). Then
 * redraw everything (SW and SH change). */
void draw_zoom(int z);
int  draw_zoom_get(void);
int  dl_count(void);                /* items in the display list (for the statistics) */
