/*
 * facet.h - The Facet desktop: windows, theme and applications.
 */
#ifndef FACET_H
#define FACET_H

#include "gfx.h"

/* ---------------- theme: Strata ----------------------------------------
 * A dark, layered-stone desktop: graphite surfaces, warm light text, one
 * amber accent, a Mineral-style dock on the left (the spine), workstation
 * window controls, and the sia command strip along the bottom.
 * (Original SIEOS artwork.)
 */
#define C_FACE        RGB(0x28, 0x2B, 0x30)   /* raised surfaces, buttons */
#define C_FACE_LIGHT  RGB(0x3A, 0x3E, 0x45)   /* top highlights, separators */
#define C_FACE_SHADOW RGB(0x8D, 0x88, 0x80)   /* secondary text, subtle marks */
#define C_FACE_DARK   RGB(0x0E, 0x0F, 0x11)   /* outlines */
#define C_TEXT        RGB(0xE4, 0xE0, 0xD8)
#define C_DIM         RGB(0x8D, 0x88, 0x80)
#define C_ACCENT      RGB(0xD9, 0xA1, 0x5F)   /* amber: focus, sia, workspace 1 */
#define C_BLUE        RGB(0x8F, 0xB4, 0xDC)
#define C_TITLE_A1    RGB(0x8F, 0xB4, 0xDC)   /* blue used by icons and graphs */
#define C_TITLE_A2    RGB(0x5F, 0x84, 0xAD)
#define C_DESK_TOP    RGB(0x1D, 0x20, 0x24)
#define C_DESK_BOT    RGB(0x10, 0x11, 0x13)
#define C_CONTENT     RGB(0x1B, 0x1D, 0x21)   /* window content */
#define C_CONTENT_ALT RGB(0x21, 0x24, 0x28)   /* alternate rows */
#define C_LINE        RGB(0x33, 0x37, 0x3D)
#define C_SELECT      RGB(0x3A, 0x44, 0x52)   /* list selection */
#define C_MENU        RGB(0x26, 0x28, 0x2E)
#define C_MENU_HOT    RGB(0x3A, 0x44, 0x52)
#define C_TITLEBAR    RGB(0x27, 0x2A, 0x2F)
#define C_TITLEBAR_I  RGB(0x21, 0x23, 0x27)
#define C_SPINE       RGB(0x28, 0x2B, 0x30)
#define C_STRIP       RGB(0x0C, 0x0D, 0x0F)
#define C_GOOD        RGB(0x7F, 0xD3, 0x9A)
#define C_BAD         RGB(0xC9, 0x6A, 0x5A)

/* Workspace colours: the band down a window's left edge. */
#define NWORKSPACES 4
extern const color_t ws_color[NWORKSPACES];

#define TITLE_H 28        /* window title bar */
#define BAND_W  4         /* workspace band */

/* ---------------- windows ---------------- */
enum { MOUSE_DOWN, MOUSE_UP, MOUSE_MOVE, MOUSE_DOUBLE };

struct window {
    int id;
    struct rect r;              /* outer frame (tab sits above it), screen coordinates */
    int tab_off;                /* horizontal position of the tab along the top edge */
    struct rect restore;        /* for maximise */
    char title[64];
    bool minimized, maximized, dead;
    int ws;                     /* workspace 0..NWORKSPACES-1 */
    int min_w, min_h;
    void *app;
    /* callbacks (content coordinates are relative to the content area) */
    void (*draw)(struct window *w, struct surface *s, struct rect content);
    void (*key)(struct window *w, const struct input_event *ev);
    void (*mouse)(struct window *w, int x, int y, int kind, int buttons);
    void (*tick)(struct window *w);             /* ~4 times per second */
    void (*resized)(struct window *w);
    void (*destroy)(struct window *w);
    int  (*pollfd)(struct window *w);           /* fd to watch, -1 = none */
    void (*readable)(struct window *w);
};

struct window *wm_create(const char *title, int x, int y, int cw, int ch);
void wm_close(struct window *w);
void wm_focus(struct window *w);
struct rect wm_content(struct window *w);
void wm_invalidate(struct window *w);            /* redraw whole window */
void wm_invalidate_rect(struct rect r);          /* redraw a screen region */
struct window *wm_focused(void);
extern int screen_w, screen_h;
int  abs_int(int v);
int  wm_cpu_percent(int cpu);
int  wm_ncpus(void);
extern char desktop_user[32];

/* ---------------- widgets (drawn in content coordinates) ---------------- */
void ui_button(struct surface *s, struct rect r, const char *label, bool pressed);
void ui_panel(struct surface *s, struct rect r, bool sunken);
void ui_meter(struct surface *s, struct rect r, int percent, color_t fill);

/* ---------------- icons ---------------- */
enum { ICON_TERMINAL, ICON_FOLDER, ICON_FILE, ICON_PROGRAM, ICON_MONITOR, ICON_CLOCK,
       ICON_INFO, ICON_LOGOUT, ICON_HOME, ICON_NETWORK, ICON_DISK };
void icon_draw(struct surface *s, int kind, int x, int y, int size);

/* The SIEOS logo (Orbit Node), centred on (cx, cy), size pixels across. */
void logo_draw(struct surface *s, int cx, int cy, int size);
void logo_pixels(struct surface *s, int x, int y, int scale);   /* 16x16 bitmap version */

/* ---------------- applications ---------------- */
void app_terminal(void);          /* the sia assistant (or the shell) */
void app_shell_terminal(void);    /* always the plain shell */
struct window *term_open(bool plain);

/* desktop.c: the control channel of programs Facet starts (libsia desktop tools) */
int  desktop_channel_new(void);             /* -1 if none left */
void desktop_channel_child(int id);         /* in the child before exec: fds 3/4, SIEOS_DESKTOP */
void desktop_channel_parent(int id);        /* in Facet after fork */
void desktop_channel_close(int id);
struct pollfd;
int  desktop_poll_fds(struct pollfd *p, int max, int *ids);
void desktop_readable(int id);

/* wm.c services for the desktop channel */
int  wm_window_list(struct window **out, int max);
struct window *wm_find_window(int id);
int  wm_workspace(void);
void wm_switch_workspace(int n);
void wm_move_to_workspace(struct window *w, int n);
void wm_show_window(struct window *w);
bool term_is_assistant(struct window *w);
void term_type_line(struct window *w, const char *text);
void app_files(const char *path);
void app_viewer(const char *path);
void app_monitor(void);
void app_clock(void);
void app_about(void);
void app_message(const char *title, const char *line1, const char *line2);
void app_network(void);

#endif
