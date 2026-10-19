/*
 * draw.h - The drawing layer, after SIEOS Atlas's (user/atlas/draw.c): the
 * window is drawn in a palette of colours by role, Atlas's look (frosted
 * glass: light panels, dark text, one amber accent) as a window shows it.
 * Integers only (as Atlas): the same code runs on SIEOS.
 */
#ifndef DRAW_H
#define DRAW_H

#include <stdint.h>

enum { PAL_BG, PAL_GLASS, PAL_LINE, PAL_ACC, PAL_TXT, PAL_DIM, PAL_BAR, PAL_TERM, PAL_TERMFG, PAL_ERR,
       PAL_SEL, PAL_GRID, PAL_INK, PAL_PANEL, PAL_FIELD, PAL_GAP, PAL_ACCT, NPAL };
extern uint32_t pal[NPAL];
#define C_BG     pal[PAL_BG]          /* the desk */
#define C_GLASS  pal[PAL_GLASS]       /* a panel, a card */
#define C_LINE   pal[PAL_LINE]        /* their edge */
#define C_ACC    pal[PAL_ACC]         /* focus, the current tab, buttons that matter */
#define C_TXT    pal[PAL_TXT]
#define C_DIM    pal[PAL_DIM]
#define C_BAR    pal[PAL_BAR]         /* the chosen row, separators */
#define C_TERM   pal[PAL_TERM]        /* the reading ground, lists */
#define C_TERMFG pal[PAL_TERMFG]
#define C_ERR    pal[PAL_ERR]
#define C_SEL    pal[PAL_SEL]         /* selected text's background */
#define C_INK    pal[PAL_INK]         /* text on the accent */
#define C_PANEL  pal[PAL_PANEL]
#define C_FIELD  pal[PAL_FIELD]       /* where you type */
#define C_ACCT   pal[PAL_ACCT]        /* the accent as text */

uint32_t hl_color(int i);             /* highlight colour i (0..7) */

void draw_target(uint32_t *px, int w, int h);   /* the picture: w x h, drawn in palette colours */
void draw_clip(int x0, int y0, int x1, int y1); /* (intersected with the current one) */
void draw_unclip(void);
void draw_desk(int x0, int y0, int x1, int y1);   /* the ground, lighter towards the top left */
void draw_rect(int x0, int y0, int x1, int y1, int r, uint32_t c);
void draw_frame(int x0, int y0, int x1, int y1, int r, int t, uint32_t c);
void draw_shade(int x0, int y0, int x1, int y1, int r, int k);   /* darken: 3/4 (1), 1/2 (2) */
void draw_top_round(int x0, int y0, int x1, int y1, int r, uint32_t c);   /* a tab: round top corners only */
int  draw_text(int x, int y, const char *s, int n, int face, int px, uint32_t c);   /* y: baseline; the end x */
int  text_w(const char *s, int n, int face, int px);
int  text_fit(const char *s, int n, int face, int px, int w);    /* bytes of s that fit in w */
void draw_text_clipped(int x, int y, int maxw, const char *s, int face, int px, uint32_t c);   /* "…" if cut */

/* The picture to the screen (out: 0xRRGGBB pixels). */
void draw_present(uint32_t *out, int out_pitch);

#endif
