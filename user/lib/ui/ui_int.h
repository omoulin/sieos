/*
 * ui_int.h - The UI toolkit's insides (user/lib/ui): widgets, the
 * application, drawing, and the few things the system gives (ui_sys.c:
 * SIEOS's own library, or libposix's with UI_POSIX).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef UI_POSIX
#include <stdlib.h>
#include <string.h>
#else
#include "mk.h"
static inline void *ui_zalloc(size_t n) { void *p = malloc(n); if (p) memset(p, 0, n); return p; }
#define calloc(n, s) ui_zalloc((n) * (s))
#endif
#include "mk/atlas.h"
#include "sieos/ui.h"

/* ---- ui_sys.c */
long  ui_sys_call(uint64_t op, uint64_t a, uint64_t b, uint64_t c, const void *s, size_t slen, void *r, size_t rlen);
void *ui_sys_shm(size_t size, uint64_t *id);          /* a new shared memory object, mapped */
void  ui_sys_shm_close(uint64_t id);
void  ui_sys_unmap(void *p, size_t size);
long  ui_sys_ms(void);
/* A folder's entries: fn(name, is_folder) for each -> how many, or < 0. */
int   ui_sys_list(const char *dir, void (*fn)(void *ctx, const char *name, int dir), void *ctx);
const char *ui_sys_home(void);

/* ---- A widget: one struct for every kind, its class says what it does. */
typedef struct ui_class {
    void (*measure)(ui_widget *w);                     /* -> w->mw, w->mh (window pixels) */
    void (*arrange)(ui_widget *w);                     /* place its children in x, y, w, h */
    void (*draw)(ui_widget *w);
    int  (*event)(ui_widget *w, const ui_event *e);    /* -> 1: taken (x, y: the window's) */
    void (*free)(ui_widget *w);
    int  focusable;                    /* 1: takes the keys; 2: Tab too (a canvas); 3: a text field */
} ui_class;

enum { F_HIDDEN = 1, F_OFF = 2, F_HOVER = 4, F_DOWN = 8 };

struct ui_widget {
    const ui_class *cls;
    ui_app *app;
    ui_widget *parent, *first, *last, *next;
    int x, y, w, h;                    /* where it is (window pixels) */
    int mw, mh;                        /* the room it wants (measure) */
    int uw, uh;                        /* at least this (ui_set_size, picture pixels) */
    int flags, stretch;
    int dir, gap, pad;                 /* a box */
    int value, a, b, c, d;             /* its state (each kind its own meaning) */
    char *text;
    ui_fn fn; void *data;
    ui_widget *menu;                   /* its right-click menu */
    int ox, oy, is_menu;               /* an overlay: where it was asked for (-1: centred); a menu */
    ui_widget *saved;                  /* an overlay: the focus before it */
    void *ext;                         /* more state of its own kind (free'd with it) */
};

#define NDMG 8
#define NOVER 4
struct ui_app {
    long win;
    uint32_t *px; int W, H; size_t psize;   /* the window's pixels */
    int zoom, Z;                       /* the zoom: % and 16.16 screen pixels per picture pixel */
    int ts, cw, chh;                   /* text: its scale (16.16 of the 6 x 11 font), a cell's width and height */
    ui_theme th;
    ui_widget *root, *focus, *hover, *pressed;
    ui_widget *over[NOVER]; int nover; /* in front: menus, dialogs (the last gets the input) */
    ui_widget *menus[16]; int nmenus;  /* every menu (their shortcuts) */
    ui_widget *bar;                    /* the menu bar open (keyboard), or 0 */
    int dmg[NDMG][4], ndmg, layout;    /* to redraw; the layout to do again */
    int buttons, focused, quit, code, running;
    ui_fn on_theme, on_close; void *theme_data, *close_data;
    int cx0, cy0, cx1, cy1;            /* drawing's clip */
    long click_ms; ui_widget *click_w; /* (double clicks) */
    ui_widget *trash[8]; int ntrash;   /* to free at the next flush (a dialog closed from its own button) */
    ui_widget *menubar;                /* the first menu bar (F10) */
};

/* ---- ui.c: the core */
ui_widget *ui_new(ui_app *app, ui_widget *parent, const ui_class *cls, const char *text);
void ui_relayout(ui_app *app);
int  ui_sc(ui_app *app, int v);        /* picture pixels -> window pixels */
void ui_show(ui_widget *w, int x, int y);   /* in front (an overlay), at x, y (-1: centred) */
void ui_hide(ui_widget *w);
int  ui_hit(ui_widget *w, int x, int y);
void ui_measure(ui_widget *w);
void ui_arrange(ui_widget *w, int x, int y, int ww, int hh);
void ui_draw_tree(ui_widget *w);
int  ui_visible(ui_widget *w);
ui_widget *ui_child(ui_widget *w, int i);
char *ui_strdup(const char *s);
void ui_activate(ui_widget *w);        /* its fn */
extern const ui_class ui_box_class;
void ui_box_measure(ui_widget *w);
void ui_box_arrange(ui_widget *w);
void ui_box_draw(ui_widget *w);
void ui_trash(ui_widget *w);
int  ui_double(ui_widget *w);          /* a press now: the second of a double click? */

/* drawing, in the window's pixels, clipped to app->cx0.. */
void ui_fill(ui_app *a, int x0, int y0, int x1, int y1, uint32_t c);
void ui_rrect(ui_app *a, int x0, int y0, int x1, int y1, int r, uint32_t c);
void ui_frame(ui_app *a, int x0, int y0, int x1, int y1, int r, int t, uint32_t c);
int  ui_text_draw(ui_app *a, int x, int y, const char *s, int n, uint32_t c, int x1);   /* -> its right end */
int  ui_tw(ui_app *a, int n);          /* room for n characters (an average one's width) */
int  ui_sw(ui_app *a, const char *s, int n);   /* width of s's n characters, as drawn */
int  ui_sfit(ui_app *a, const char *s, int n, int w);   /* how many of them fit in w */
void ui_icon(ui_app *a, int icon, int cx, int cy, uint32_t c);
void ui_ytext(ui_widget *w, const char *s, uint32_t c, int x, int x1);   /* text centred in w's height */
uint32_t ui_mix(uint32_t a, uint32_t b, int k);   /* k/256 of b over a */

#define CH_UI 11                      /* the 6 x 11 font's cell height */

/* ---- ui_menu.c */
ui_widget *ui_menu_over(ui_app *app, int x, int y);   /* a menu bar's title under (x, y) while a menu is open */
int  ui_menu_key(ui_app *app, int key);   /* a shortcut, F10 -> 1: taken */
void ui_menus_close(ui_app *app);
int  ui_scroll_event(ui_widget *w, const ui_event *e, int *pos, int total, int page, int x0, int y0, int x1, int y1, int vertical);
void ui_scroll_draw(ui_widget *w, int pos, int total, int page, int x0, int y0, int x1, int y1, int vertical);
