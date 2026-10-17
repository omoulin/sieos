/*
 * ui.h - The few widgets the app draws: clickable zones, buttons, check
 * boxes, text fields. Immediate mode: each frame draws them again and
 * records where they are (zones), so the next click finds them.
 */
#ifndef UI_H
#define UI_H

#include "plat.h"
#include <stdint.h>

extern int UI_S;                          /* the size, in % (the screen's scale x the user's size) */
#define U(v) ((v) * UI_S / 100)
extern int mouse_x, mouse_y;

/* ---- zones ---- */
typedef struct { int x0, y0, x1, y1, id, a; const void *p; int cursor; } zone_t;
void zones_reset(void);
void zone(int x0, int y0, int x1, int y1, int id, int a, const void *p, int cursor);
const zone_t *zone_at(int x, int y);
int  hovered(int x0, int y0, int x1, int y1);

/* ---- text ---- */
int  ui_px(void);                         /* the interface's text size */
int  ui_small(void);
int  ui_title(void);
int  ui_text(int x, int y, const char *s, int face, int px, uint32_t c);   /* y: the top; the end x */
int  ui_wrap(int x, int y, int w, const char *s, int face, int px, uint32_t c, int maxlines);   /* the height */
int  ui_wrap_h(int w, const char *s, int face, int px, int maxlines);                          /* (measured only) */

/* ---- buttons ---- */
enum { BTN_PLAIN, BTN_ACCENT, BTN_FLAT, BTN_DANGER, BTN_ON };
int  ui_button(int x, int y, int h, const char *label, int style, int id, int a, const void *p);   /* its right edge */
int  ui_button_w(const char *label, int h);
void ui_check(int x, int y, int on, const char *label, const char *sub, int id, int a, const void *p, int w);

/* ---- text fields ---- */
typedef struct {
    char *s; int len, cap;
    int cur, anchor;                      /* the caret and the selection's other end (bytes) */
    int multiline, password;
    int scroll;                           /* (pixels) */
    int x0, y0, x1, y1;                   /* where it was drawn */
    int *ls; int nls, cls;                /* multi-line: the line starts, as drawn */
    int want_x;                           /* up/down: the column kept */
} field_t;

void field_set(field_t *f, const char *s);
const char *field_str(field_t *f);
void field_draw(field_t *f, int x0, int y0, int x1, int y1, const char *placeholder, int focused, int id);
int  field_key(field_t *f, const ev_t *e);   /* 1 handled, 2: Enter in a one-line field */
void field_insert(field_t *f, const char *s);
void field_click(field_t *f, int x, int y, int extend);
void field_select_all(field_t *f);

#endif
