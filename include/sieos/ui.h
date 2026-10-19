/*
 * sieos/ui.h - SIEOS's UI toolkit (user/lib/ui, docs/ui.md): a window on
 * the desktop (mk/atlas.h) filled with a retained tree of widgets drawn in
 * the desktop's theme (its look's colours, its size), redrawn only where
 * something changed, driven by the window's input. For programs built by
 * SIEOS's own compiler (libui.a, with user/lib) and for libposix programs
 * (libui-posix.a), in C or C++.
 *
 *   ui_app *app = ui_app_new("Hello");
 *   ui_widget *col = ui_root(app);
 *   ui_label(col, "Hello");
 *   ui_button(col, "Quit", quit, app);
 *   return ui_run(app);
 *
 * Sizes are the desktop's picture pixels (as atlas lays out: a button is 42
 * high), turned into the window's pixels by the zoom; positions given to and
 * by the program (events, canvas pixels) are the window's own pixels.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_app ui_app;
typedef struct ui_widget ui_widget;

/* Input, as the window gets it (mk/atlas.h's events): a key (the
 * character, Ctrl applied: Ctrl+C is 3, or a UI_KEY_...), the pointer (x, y
 * in the widget, buttons held UI_MB_..., the wheel: + towards the user),
 * a new size (x, y; a canvas: its new size), focus (key 1 in, 0 out), the
 * window closed, the theme changed. */
typedef struct { int type, key, x, y, buttons, wheel; } ui_event;
enum { UI_EV_KEY = 1, UI_EV_POINTER, UI_EV_SIZE, UI_EV_FOCUS, UI_EV_CLOSE, UI_EV_THEME };
enum { UI_MB_LEFT = 1, UI_MB_RIGHT = 2, UI_MB_MIDDLE = 4 };
enum { UI_KEY_UP = 0x101, UI_KEY_DOWN, UI_KEY_LEFT, UI_KEY_RIGHT, UI_KEY_PGUP, UI_KEY_PGDN, UI_KEY_HOME, UI_KEY_END,
       UI_KEY_DEL, UI_KEY_BTAB, UI_KEY_F1 = 0x111, UI_KEY_F10 = 0x111 + 9 };
#define UI_CTRL(c) ((c) & 31)                  /* Ctrl+letter: UI_CTRL('o') */

/* The theme: the desktop's look (atlas's palette as it shows on the screen,
 * its corners, its zoom), sent by atlas and again when the user changes the zoom. */
typedef struct {
    int look, zoom;                            /* UI_LOOK_FROST; the desktop's size in % (100: normal) */
    uint32_t bg, panel, face, line, accent, ink, text, dim, field, sel, err, acctext, list, chosen;
                                               /* the window's ground, bars, a button, edges, the accent, text on it,
                                                  text, dim text, where you type, selected text, an error, the accent
                                                  as text, a list's ground, its chosen row */
    int radius;                                /* how round corners are */
} ui_theme;
enum { UI_LOOK_FROST };

typedef void (*ui_fn)(ui_widget *w, void *data);                        /* an action */
typedef void (*ui_event_fn)(ui_widget *w, const ui_event *e, void *data);   /* a canvas's input */
typedef const char *(*ui_cell_fn)(void *data, int row, int col);        /* a list's cell */

/* ---- The application: one window, its widgets, its loop. */
ui_app     *ui_app_new(const char *title);     /* the window opened (0: no desktop, or not allowed) */
ui_widget  *ui_root(ui_app *app);              /* a column filling the window */
int         ui_run(ui_app *app);               /* until ui_quit or the window closes -> the code */
void        ui_quit(ui_app *app, int code);
void        ui_set_title(ui_app *app, const char *title);
const ui_theme *ui_theme_of(ui_app *app);
void        ui_on_theme(ui_app *app, ui_fn fn, void *data);   /* after a change (widgets restyled already) */
void        ui_on_close(ui_app *app, ui_fn fn, void *data);   /* the window was closed */
int         ui_scale(ui_app *app, int v);      /* picture pixels -> the window's, at the zoom */
int         ui_clip_get(ui_app *app, char *buf, int cap);   /* the clipboard's text -> its length */
void        ui_clip_set(ui_app *app, const char *s, int n);
/* Another main loop (GLib's): a thread waits, the UI thread dispatches and flushes. */
int         ui_wait(ui_app *app, ui_event *ev, int max);   /* blocks (any thread) -> events, < 0: window gone */
void        ui_dispatch(ui_app *app, const ui_event *ev, int n);
void        ui_flush(ui_app *app);             /* paint what changed, tell the desktop */
int         ui_fmt(char *d, const char *s, long v);   /* d = s then v in decimal -> its length (a small helper) */
int         ui_done(ui_app *app);              /* ui_quit was called, or the window closed */
void        ui_app_close(ui_app *app);         /* the window closed (ui_run does it) */

/* ---- Any widget. */
void        ui_set_text(ui_widget *w, const char *text);
const char *ui_text(ui_widget *w);
void        ui_set_value(ui_widget *w, int v); /* check/toggle 0 1; progress 0..1000 (-1: busy); tabs: the page;
                                                  list: the chosen row (-1 none); scroll bar, split: the position */
int         ui_value(ui_widget *w);
void        ui_set_enabled(ui_widget *w, int on);
void        ui_set_visible(ui_widget *w, int on);
void        ui_set_stretch(ui_widget *w, int share);   /* its share of a box's room to spare (0: its own size) */
void        ui_set_size(ui_widget *w, int width, int height);   /* at least this (picture pixels; 0: its own) */
void        ui_on(ui_widget *w, ui_fn fn, void *data);   /* its action (click, toggle, Enter, choice...) */
void        ui_set_menu(ui_widget *w, ui_widget *menu);  /* its right-click menu */
void        ui_focus(ui_widget *w);
int         ui_has_focus(ui_widget *w);        /* it has the window's keys */
void        ui_dirty(ui_widget *w);            /* redraw it */
void        ui_bounds(ui_widget *w, int *x, int *y, int *width, int *height);   /* in the window's pixels */
ui_app     *ui_app_of(ui_widget *w);
void        ui_destroy(ui_widget *w);          /* and its children */

/* ---- Layout. */
enum { UI_ROW, UI_COL };
ui_widget  *ui_box(ui_widget *parent, int dir, int gap, int pad);   /* children side by side or one under the other */
ui_widget  *ui_split(ui_widget *parent, int dir, int pos);          /* two children, a divider the user drags */
ui_widget  *ui_tabs(ui_widget *parent);
ui_widget  *ui_tab(ui_widget *tabs, const char *title);             /* -> its page (a column) */

/* ---- Widgets. */
ui_widget  *ui_label(ui_widget *parent, const char *text);
ui_widget  *ui_button(ui_widget *parent, const char *text, ui_fn fn, void *data);
ui_widget  *ui_check(ui_widget *parent, const char *text, int on, ui_fn fn, void *data);
ui_widget  *ui_toggle(ui_widget *parent, const char *text, int on, ui_fn fn, void *data);   /* a button that stays in */
ui_widget  *ui_field(ui_widget *parent, const char *text, ui_fn fn, void *data);   /* one line; fn: Enter */
void        ui_field_select(ui_widget *w, int from, int to);   /* (characters; -1: the end) */
ui_widget  *ui_progress(ui_widget *parent);
ui_widget  *ui_scrollbar(ui_widget *parent, int dir, ui_fn fn, void *data);
void        ui_scroll_range(ui_widget *w, int total, int page);
ui_widget  *ui_list(ui_widget *parent, int ncol, const char *const *titles, ui_fn fn, void *data);   /* fn: Enter or a double click */
void        ui_list_model(ui_widget *w, int rows, ui_cell_fn cell, void *data);   /* (again when the rows change) */
void        ui_list_widths(ui_widget *w, const int *widths);   /* columns' widths in characters (0: an equal share) */
void        ui_list_on_select(ui_widget *w, ui_fn fn, void *data);   /* the chosen row changed (ui_value) */
ui_widget  *ui_toolbar(ui_widget *parent);
enum { UI_ICON_NONE, UI_ICON_BACK, UI_ICON_FORWARD, UI_ICON_RELOAD, UI_ICON_STOP, UI_ICON_MENU, UI_ICON_PLUS, UI_ICON_MINUS };
ui_widget  *ui_tool(ui_widget *toolbar, int icon, const char *text, ui_fn fn, void *data);
void        ui_set_icon(ui_widget *tool, int icon);
/* A canvas: pixels of the program's own, shown exactly as it draws them
 * (0x00RRGGBB, `w` to a row, never themed); its input comes to fn. A new
 * size (UI_EV_SIZE) gives new pixels: ask for them again. */
ui_widget  *ui_canvas(ui_widget *parent, ui_event_fn fn, void *data);
uint32_t   *ui_canvas_pixels(ui_widget *w, int *width, int *height);
void        ui_canvas_damage(ui_widget *w, int x, int y, int width, int height);

/* ---- Menus: a bar of menus, or a menu of its own shown at a place (right click). */
ui_widget  *ui_menubar(ui_widget *parent);
ui_widget  *ui_menu(ui_app *app, ui_widget *bar, const char *title);   /* bar 0: a context menu */
ui_widget  *ui_item(ui_widget *menu, const char *text, int key, ui_fn fn, void *data);   /* key: its shortcut (UI_CTRL('o'), UI_KEY_F1...) */
void        ui_separator(ui_widget *menu);
void        ui_popup(ui_widget *menu, int x, int y);   /* (the window's pixels) */

/* ---- Dialogs: in the window, in front of everything; fn gets the dialog
 * (ui_value 1: OK or Yes, 0: Cancel; a file dialog's ui_text: the path). */
void        ui_message(ui_app *app, const char *title, const char *text);
void        ui_confirm(ui_app *app, const char *title, const char *text, ui_fn fn, void *data);
void        ui_file_dialog(ui_app *app, int save, const char *dir, const char *name, ui_fn fn, void *data);
void        ui_close_dialog(ui_app *app);      /* the front one, as if cancelled */

#ifdef __cplusplus
}
#endif
