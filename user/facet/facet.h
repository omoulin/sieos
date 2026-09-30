/*
 * facet.h - The Facet desktop: windows, theme and applications.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef FACET_H
#define FACET_H

#include "sieos.h"
#include "facet/gfx.h"
#include "facet/theme.h"
#include "facet/ui.h"
#include "facet/settings.h"

/* Workspace colours: the band down a window's left edge. */
#define NWORKSPACES 4
extern const color_t ws_color[NWORKSPACES];

/* The current skin's window frame: the content's insets from the frame's
 * outer edge (the title bar is in top). */
struct frame_insets {
    int top, left, right, bottom;
};
struct frame_insets wm_frame(void);
bool wm_set_skin(const char *name, char *err, size_t n);

/* ---------------- windows ---------------- */
enum { MOUSE_DOWN, MOUSE_UP, MOUSE_MOVE, MOUSE_DOUBLE, MOUSE_WHEEL };     /* (= FCT_MOUSE_*) */

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
    bool pointer;               /* all pointer events over the content (FCT_WIN_POINTER): moves
                                   without a button, right and middle buttons, the wheel */
    void (*wheel)(struct window *w, int x, int y, int notches);   /* (> 0 down) */
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

/* ---------------- applications (launch.c: separate programs, /bin/facet-*) ---------------- */
pid_t app_launch(const char *app, const char *arg);   /* terminal, shell, files, viewer, monitor,
                                                         network, clock, about: the pid, or -1 */
void app_reaped(pid_t pid);       /* a child exited */
void app_terminal(void);          /* the sia assistant (or the shell) */
void app_shell_terminal(void);    /* always the plain shell */
struct window *term_open(bool plain);   /* started, and its window up (or NULL) */

/* ---------------- server.c: windows of other programs (libfacet clients) ---------------- */
struct pollfd;
int  server_start(void);          /* listen; sets FACET_DISPLAY */
void server_stop(void);
void server_broadcast_skin(const char *name);
int  server_poll_fds(struct pollfd *p, int max, int *ids);
void server_ready(int id, short revents);
bool server_window(struct window *w);
pid_t server_window_pid(struct window *w);
bool server_is_assistant(struct window *w);
void server_type_line(struct window *w, const char *text);
struct window *server_wait_window(pid_t pid, int timeout_ms);

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
bool wm_set_resolution(int w, int h, char *err, size_t n);
int  wm_display_modes(char *buf, size_t n);          /* the modes as text lines; their number, -1 */
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
void app_browser(void);
void app_sipm(void);
void app_installer(void);
void app_power(void);             /* Power and Temperature */         /* Install SIEOS (on a disk) */
void wm_reboot(void);             /* end the session: reboot */
bool wm_set_pointer(int speed, int accel, char *msg, size_t n);   /* -1: unchanged; msg: the settings */
/* saver.c: the screen saver and the lock */
void saver_load_settings(void);
bool saver_configure(const char *kind, int timeout, int lock, char *msg, size_t n);   /* -1/NULL: unchanged */
void saver_preview(void);
void saver_lock_now(void);
bool saver_active(void);
bool saver_input(const struct input_event *ev);   /* true: the saver took it */
int  saver_step(struct surface *back);            /* the poll's timeout it wants (ms) */
void app_settings(void);

#endif
