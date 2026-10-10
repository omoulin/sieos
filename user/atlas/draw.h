/*
 * draw.h - The drawing layer of the desktop: a display list rasterized
 * straight into video memory, one region at a time.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

extern int SW, SH;                  /* the screen, 32 bits per pixel: 1920 x 1080, or the size the
                                       firmware's screen has (UEFI; screen.c sets them) */
#define CW 6                        /* a character cell of the font, in pixels at scale 1 */
#define CH 11

/* The screen (screen.c): where the SW x SH picture goes, the pitch
 * (pixels from one row to the next) and the colour order (swap: the
 * screen wants 0x00BBGGRR, Atlas draws 0x00RRGGBB). */
typedef struct { uint32_t *fb; int pitch, swap; const char *name; } screen_t;
int  screen_init(screen_t *s);      /* 0: ready */
long rtc_seconds(void);             /* the time of day in seconds (UTC), or -1: no clock */

void draw_init(const screen_t *s);
void dl_reset(void);                /* start a new picture */
void dl_grid(int ox, int oy, int step, uint32_t bg, uint32_t line);
void dl_rect(int x0, int y0, int x1, int y1, int r, uint32_t c);           /* filled, corner radius r */
void dl_frame(int x0, int y0, int x1, int y1, int r, int t, uint32_t c);   /* outline, t pixels thick */
/* Text. Its size is a 16.16 fixed-point scale (S(2): twice the font's
 * 6 x 11 pixels), any value, so text grows smoothly with the zoom. */
#define S(n) ((n) << 16)
void dl_text(int x, int y, const char *s, int n, int scale, uint32_t c,
             int cx0, int cy0, int cx1, int cy1);                         /* clipped to the box */
int  text_w(int n, int scale);      /* width in pixels of n characters */
void dl_pointer(int x, int y);      /* where the mouse pointer is drawn (on top of everything) */
void present(int x0, int y0, int x1, int y1);   /* draw this region of the picture on the screen */
int  dl_count(void);                /* items in the display list (for the statistics) */
