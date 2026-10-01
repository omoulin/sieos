/*
 * facet/facet.h - libfacet: writing applications for the Facet desktop.
 *
 *   #include <facet/facet.h>          cc app.c -lfacet
 *
 * Two levels:
 *
 * Views (the usual way).  fct_app_init() connects to the desktop;
 * fct_view_new() opens a window; set its callbacks; fct_main() runs the
 * event loop until the last view is closed.  A view is redrawn (its draw
 * callback into the window's buffer, then shown) after fct_view_invalidate(),
 * a resize, and when it first appears.  Content coordinates start at (0, 0).
 *
 *     static void draw(struct fct_view *v, struct surface *s, struct rect c)
 *     {
 *         gfx_fill(s, c.x, c.y, c.w, c.h, C_CONTENT);
 *         gfx_text(s, c.x + 10, c.y + 10, "Hello, Facet", C_TEXT);
 *     }
 *     int main(void)
 *     {
 *         if (fct_app_init() < 0)
 *             return 1;
 *         struct fct_view *v = fct_view_new("Hello", 300, 120);
 *         v->draw = draw;
 *         return fct_main();
 *     }
 *
 * Windows and events (fct_open, fct_window_create, fct_next_event): the
 * protocol (facet/protocol.h) with buffers and resizing handled, for
 * programs with their own loop.
 *
 * Drawing is facet/gfx.h, widgets and icons facet/ui.h, the desktop's
 * colours facet/theme.h (all included here).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef FACET_FACET_H
#define FACET_FACET_H

#include "facet/gfx.h"
#include "facet/font.h"
#include "facet/protocol.h"
#include "facet/theme.h"
#include "facet/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- input ---------------- */

/* keys: code is the scan code (FCT_KEY_* for the special ones), ascii the
 * character typed (0 for none), value 1 for a press and 0 for a release */
struct fct_key {
    unsigned code;
    int value;
    unsigned ascii;
    unsigned mods;                  /* FCT_MOD_* */
};
#define FCT_MOD_SHIFT 1
#define FCT_MOD_CTRL  2
#define FCT_MOD_ALT   4
#define FCT_KEY_UP     0x148
#define FCT_KEY_DOWN   0x150
#define FCT_KEY_LEFT   0x14B
#define FCT_KEY_RIGHT  0x14D
#define FCT_KEY_HOME   0x147
#define FCT_KEY_END    0x14F
#define FCT_KEY_PGUP   0x149
#define FCT_KEY_PGDN   0x151
#define FCT_KEY_DELETE 0x153
#define FCT_KEY_F1     0x03B        /* F2.. follow */

/* mouse event kinds; buttons: bit 0 left, 1 right, 2 middle.  Without
 * FCT_WIN_POINTER a window gets the left button's presses and releases, and
 * moves while it is held; with it, every move over the content, the right and
 * middle buttons, and FCT_MOUSE_WHEEL (wheel: notches, > 0 down). */
enum { FCT_MOUSE_DOWN, FCT_MOUSE_UP, FCT_MOUSE_MOVE, FCT_MOUSE_DOUBLE, FCT_MOUSE_WHEEL };

/* ---------------- windows and events ---------------- */

typedef struct fct_display fct_display;
typedef struct fct_window fct_window;

struct fct_window_attr {
    const char *title;
    int x, y;                       /* FCT_POS_AUTO, FCT_POS_CENTER, or screen coordinates */
    int w, h;                       /* content size */
    int min_w, min_h;               /* smallest content size (0: the desktop's default) */
    unsigned flags;                 /* FCT_WIN_* */
};

enum { FCT_KEY = 1, FCT_MOUSE, FCT_RESIZE, FCT_CLOSE, FCT_TEXT,
       FCT_SKIN };                      /* the desktop changed skin (window NULL): redraw */

struct fct_event {
    int type;                       /* FCT_KEY ... */
    fct_window *window;
    struct fct_key key;             /* FCT_KEY */
    int x, y, kind, buttons;        /* FCT_MOUSE (content coordinates, FCT_MOUSE_*) */
    union {
        struct { int w, h; };       /* FCT_RESIZE: the new size (the buffer already has it) */
        int wheel;                  /* FCT_MOUSE_WHEEL: notches, > 0 down */
    };
    const char *text;               /* FCT_TEXT: a line, valid until the next call */
};

/* Connect to the desktop ($FACET_DISPLAY, else /tmp/.facet-<uid>); NULL with errno. */
fct_display *fct_open(void);
void fct_disconnect(fct_display *d);
int  fct_fd(fct_display *d);                       /* to poll() for events */
void fct_screen_size(fct_display *d, int *w, int *h);

fct_window *fct_window_create(fct_display *d, const struct fct_window_attr *a);
void fct_window_destroy(fct_window *w);            /* (also after FCT_CLOSE) */
struct surface *fct_window_surface(fct_window *w); /* its buffer: w x h pixels */
void fct_window_size(fct_window *w, int *width, int *height);
void fct_window_damage(fct_window *w, struct rect r);   /* show changed pixels */
void fct_window_set_title(fct_window *w, const char *title);
void fct_window_set_user(fct_window *w, void *p);
void *fct_window_user(fct_window *w);

/* Next event: 1, 0 after timeout_ms (-1 waits, 0 polls), -1 if the desktop is gone. */
int  fct_next_event(fct_display *d, struct fct_event *ev, int timeout_ms);

/* ---------------- the clipboard ---------------- */

/* The desktop's clipboard (text, UTF-8), shared by the session's programs:
 * set replaces it (true if done); get returns a copy, NUL-terminated, to
 * free (NULL when empty), its length in *len. */
bool  fct_clipboard_set(const char *text, size_t len);
char *fct_clipboard_get(size_t *len);

/* ---------------- the text field ---------------- */

/* One line of text to type in (field.c): a click places the cursor, a drag
 * selects, a double-click selects the word; Left, Right, Home, End (Shift
 * extends), Backspace, Delete, Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V (the clipboard).
 * masked: a password (dots, never copied).  The application draws it where
 * it wants, gives it the keys and the mouse events, and keeps Enter, Tab,
 * Escape, Up and Down (fct_field_key returns FCT_FIELD_NONE for them). */
struct fct_field {
    char text[512];
    int cur, anchor;                /* byte offsets: the selection is between them */
    int scroll;                     /* the first byte shown */
    bool masked;
    bool dragging;
};
enum { FCT_FIELD_NONE, FCT_FIELD_MOVED, FCT_FIELD_CHANGED };   /* fct_field_key: not its key, the cursor, the text */
void fct_field_set(struct fct_field *f, const char *text);
void fct_field_draw(struct surface *s, struct rect r, struct fct_field *f, bool focus, const char *hint);
int  fct_field_key(struct fct_field *f, const struct fct_key *k);
bool fct_field_mouse(struct fct_field *f, struct rect r, int x, int y, int kind);   /* true: it was the field's */

/* ---------------- views ---------------- */

struct fct_view {
    fct_window *win;
    char title[FCT_TITLE_MAX];
    void *app;                                  /* the application's data */
    void (*draw)(struct fct_view *v, struct surface *s, struct rect content);
    void (*key)(struct fct_view *v, const struct fct_key *k);
    void (*mouse)(struct fct_view *v, int x, int y, int kind, int buttons);
    void (*tick)(struct fct_view *v);           /* about 4 times a second */
    void (*resized)(struct fct_view *v);        /* before the redraw */
    void (*destroy)(struct fct_view *v);        /* the view is going away: free app */
    int  (*pollfd)(struct fct_view *v);         /* a descriptor to watch, -1 none */
    void (*readable)(struct fct_view *v);       /* ... it is readable */
    void (*text)(struct fct_view *v, const char *line);   /* FCT_TEXT */
    /* private */
    bool dirty, closing;
    struct fct_view *next;
};

int  fct_app_init(void);                        /* connect: 0, or -1 with a message */
fct_display *fct_app_display(void);
struct fct_view *fct_view_new(const char *title, int w, int h);
struct fct_view *fct_view_create(const struct fct_window_attr *a);
void fct_view_invalidate(struct fct_view *v);   /* redraw it */
void fct_view_set_title(struct fct_view *v, const char *title);
struct rect fct_view_content(struct fct_view *v);   /* (0, 0, width, height) */
void fct_view_close(struct fct_view *v);        /* destroy callback, then gone */
int  fct_main(void);                            /* 0 when the last view closed, 1 if the desktop went away */
void fct_quit(void);                            /* leave fct_main after this round */
void fct_screen(int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif
