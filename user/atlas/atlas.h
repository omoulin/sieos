/*
 * atlas.h - What the desktop's files share: colours, the drawing helpers
 * and click boxes (atlas.c), the windows (wm.c), the assistant (panel.c),
 * the file editor (edit.c), Files (files.c), Settings (settings.c), the
 * icons (icons.c) and the first start (setup.c). The desktop is laid out
 * as BeOS was (title tabs, the Deskbar, workspaces) in SIEOS's colours
 * (docs/desktop.md); the program is /bin/atlas.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "mk.h"
#include "draw.h"

/* Colours: deep navy, "glass" panels a little lighter, one accent. */
#define C_BG     0x0a0f1c
#define C_GLASS  0x141a28             /* a panel, a window, a card */
#define C_LINE   0x2a3245             /* their edge */
#define C_ACC    0x4be3c1             /* focus, the current workspace, buttons that matter */
#define C_TXT    0xe6ecf5
#define C_DIM    0x8fa0b8
#define C_BAR    0x262e40             /* the chosen row, separators */
#define C_TERM   0x070b14
#define C_TERMFG 0x9fe8c8
#define C_ERR    0xff9b8a
#define C_SEL    0x1d5a52             /* selected text's background */
#define C_CARD   C_GLASS              /* (the names setup.c and panel.c use) */
#define C_ISLB   C_LINE

#define K 2                           /* text scale of the desktop: 12 x 22 pixels a character */
#define KW (CW * K)
#define KH (CH * K)
/* The type scale, for dl_text (16.16): titles, the body (S(K)), small text
 * (hints, sizes, paths). At 100% each lands on a native font (draw.c):
 * 11 x 20, 9 x 16 and 8 x 15 screen pixels a character. */
#define ST 160200
#define SS 116510

void redraw(void);
void redraw_box(int x0, int y0, int x1, int y1);

/* ---- Click boxes: what a click at (x, y) means; searched from the last. */
enum { H_BTN = 1, H_WIN, H_STRIP, H_RES, H_MENU, H_PR, H_FIELD, H_LOGIN, H_ED,
       H_PANEL, H_PIN, H_PSEND, H_PSTOP, H_PSUG, H_PACT, H_PCLOSE, H_PMODE, H_SETUP,
       H_WTAB, H_WEDGE, H_WMBTN, H_SET, H_FILE, H_DBMENU, H_DBAPP, H_DBWS, H_DESK, H_PINS };
void hitbox(int x0, int y0, int x1, int y1, int kind, int arg);
int  pill(int x, int y, const char *s, int on, int kind, int arg);   /* -> its right edge */
void label(int x, int y, const char *s, int k, uint32_t c);
static inline int tw(int n, int k) { return text_w(n, S(k)); }       /* n characters at scale k */
void say(const char *fmt, ...);
int  num(char *p, long v);
static inline const char *base_name(const char *p)   /* after the last '/' */
{ const char *b = p; for (; *p; p++) if (*p == '/') b = p + 1; return b; }

/* ---- The desktop (atlas.c), for the assistant's context and actions. */
int  atlas_may(const char *path, int write);           /* may the logged-in user read/write it? */
int  atlas_files(char *out, size_t cap);              /* "path\n"..., -> length */
int  atlas_open_file(const char *path);               /* in the editor: 0, or -ENOENT */
int  atlas_run_app(const char *path);                 /* a launcher (.app): 0, or -error */
int  atlas_find(const char *key, const char *value);  /* the files with that attribute, under the sia strip -> count */
extern int  atlas_debug;                              /* tests: log where things are drawn */
extern char clip[4096];                               /* the clipboard (copied text) */
extern int  clip_n;

/* ---- The login screen (atlas.c) and the first start on the screen (setup.c). */
long atlas_auth(msg_t *m);                            /* a request to the accounts server */
void greeter_check(void);                             /* first start or not; tell auth */
void greeter_login(const char *name, const char *pw); /* log in (wipes nothing: the caller does) */
void setup_draw(void);
void setup_key(int c);
void setup_click(int kind, int arg);

/* ---- The assistant (panel.c): a window, talking to port "sia". */
void panel_init(long atlas_port);
void panel_ask(const char *question);                 /* asks (the desktop shows the window) */
void panel_draw(int x0, int y0, int x1, int y1, int focused);
int  panel_click(int kind, int arg);                  /* 1 if it was the panel's */
int  panel_key(int c);                                /* 1 if the panel took the key */
void panel_wheel(int w);
void panel_event(msg_t *m, const char *data);         /* a message from the panel's thread */
void panel_status(char *out, size_t cap);             /* "sia: local", "sia: idle"... */
const char *panel_last(int k);                        /* the k-th line from the end of the conversation, or 0 */
#define ATLAS_SIA 101                                 /* message: panel thread -> atlas */

/* ---- The editor (edit.c): a window: the tree of files, the open files as tabs. */
int  edit_open(const char *path);                     /* in a tab (or its tab shown): 0 or -error */
int  edit_active(void);                               /* a file is open */
const char *edit_path(void);                          /* the file shown */
int  edit_dirty(void);                                /* ... has changes */
void edit_draw(int x0, int y0, int x1, int y1, int focused);
int  edit_key(int c);                                 /* 1 if taken; the last tab closed: no file open */
void edit_click(int arg, int x, int y);               /* (H_ED, arg) */
void edit_wheel(int arg, int w);
void edit_close(void);                                /* every tab, no question */
int  edit_close_window(void);                         /* the window's close: 0 if changes held it (asked) */
const char *atlas_home(void);                         /* the logged-in user's home folder ("" before) */

/* ---- Pins (pins.c): the GPIO pins as the tracks of a sequencer (docs/gpio.md). */
#define ATLAS_PINS 105                                /* message: its ticker -> atlas, ten times a second */
void pins_init(long atlas_port);
void pins_open(void);
void pins_close(void);
void pins_logout(void);
void pins_draw(int x0, int y0, int x1, int y1, int focused);
int  pins_click(int kind, int arg);
int  pins_key(int c);
void pins_wheel(int w);
int  pins_tick(void);                                 /* ATLAS_PINS: 1 if the window must be drawn again */

/* ---- Settings (settings.c): a window of the desktop: the screen's size,
 * the mouse, the assistant's model. */
#define ATLAS_REVERT 104                              /* message: a new screen size was not kept */
void settings_init(long atlas_port);
void settings_open(void);                             /* (shown: what is set now) */
void settings_close(void);
void settings_draw(int x0, int y0, int x1, int y1, int focused);
int  settings_click(int kind, int arg);               /* 1 if it was the window's */
int  settings_key(int c);                             /* 1 if taken (a field has the keys) */
void settings_wheel(int w);
void settings_revert(void);
/* (atlas.c) */
int  atlas_screen(int *w, int *h);                    /* the screen's size -> 1 if it can change */
int  atlas_set_size(int w, int h);                    /* 0, or -error (the size stays) */
void atlas_mouse(int speed, int swap, int reverse);   /* the user's mouse: applied and kept (~/.mouse) */
void atlas_mouse_get(int *speed, int *swap, int *reverse);
void atlas_look(int look);                            /* the user's look (draw.h LOOK_...): applied and kept (~/.look) */
int  atlas_look_get(void);
void atlas_zoom(int pct);                             /* the size of everything: 50..400 (%), kept with the look */
int  atlas_zoom_get(void);
int  atlas_uid(void);                                 /* the logged-in user */

/* ---- Files (files.c): a folder's contents, a window. */
void files_open(const char *dir);
const char *files_dir(void);
void files_draw(int x0, int y0, int x1, int y1, int focused);
int  files_click(int kind, int arg);                  /* 1 if it was the window's */
int  files_key(int c);
void files_wheel(int w);

/* ---- Icons (icons.c): 12 x 12 pixel maps; the SIEOS cube, 16 x 16. */
enum { I_TERM, I_SIA, I_FOLDER, I_FILE, I_SETTINGS, I_APP, I_WINDOW, I_PINS, I_N };
void icon(int kind, int x, int y, int scale);
void icon_ink(int kind, int x, int y, int scale);     /* its outline only, in C_BG (on the accent: a tab) */
void logo(int x, int y, int scale);

/* ---- The windows (wm.c): a title tab as wide as the title (close, zoom),
 * moved by its tab, resized by its grip or edges; 4 workspaces. Window ids
 * are atlas's: 0..7 terminals, 8 the editor, 9 sia, 10 Settings, 11 Files. */
#define WM_MAX 13
#define WM_EDIT 8                                     /* (atlas's W_EDIT: opens bigger) */
#define WM_SIA 9
#define WM_SET 10
#define WM_FILES 11                                   /* (atlas's W_FILES: a list, opens smaller) */
#define WM_PINS 12                                    /* (atlas's W_PINS: opens bigger) */
#define WM_NWS 4                                      /* workspaces */
#define TAB_H 34                                      /* a window's tab */
#define WM_STRIP TAB_H                                /* the strip at a window's top (title when its tab sank) */
#define WM_BODY_DY (WM_STRIP - 48)                    /* wm_body's y0 from the window's top (insides: TBAR + 8 below it) */
#define ATLAS_ANIM 103                                /* message: the tabs' animation thread -> atlas */
void wm_init(long atlas_port);                        /* starts the animation's thread */
void wm_tick(void);                                   /* ATLAS_ANIM: the next frame */
void wm_reset(void);                                  /* logout: no windows */
void wm_area(int x0, int y0, int x1, int y1);         /* where windows may go */
void wm_show(int w);                                  /* on the screen, in front (a new one is placed) */
void wm_min(int w);
void wm_toggle_max(int w);                            /* zoom: the whole desk, or back */
void wm_gone(int w);                                  /* closed */
int  wm_minimized(int w);
int  wm_box(int w, int *x0, int *y0, int *x1, int *y1);   /* 1: on the screen at that box */
int  wm_top(void);                                    /* the front window of this workspace, or -1 */
int  wm_workspace(void);                              /* 0..3 */
int  wm_ws_of(int w);
void wm_switch(int ws);
void wm_send(int w, int ws);
int  wm_grab(int kind, int arg, int x, int y);        /* a press on a tab or an edge: 1 if it starts a move */
void wm_motion(int x, int y);
void wm_drop(void);
int  wm_grabbed(void);                                /* the window being moved or resized */
void wm_ungrab(void);                                 /* the press was only a click */
void wm_draw(int focus, int keys);
/* (atlas.c) */
int  wm_state(int w);                                 /* 1 if window w exists */
void wm_title(int w, char *t, size_t cap);
int  wm_icon(int w);                                  /* its app's icon (I_...), on its tab */
void wm_info(int w, char *t, size_t cap);             /* the strip's right side ("terminal 80 x 25") */
void wm_body(int w, int x0, int y0, int x1, int y1, int keys);
