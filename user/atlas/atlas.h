/*
 * atlas.h - What the desktop's files share: colours, the drawing helpers
 * and click boxes (atlas.c), the assistant (panel.c), the file editor
 * (edit.c) and the first start (setup.c). The desktop is "Stage"
 * (docs/desktop.md); the program keeps its old name, /bin/atlas.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "mk.h"
#include "draw.h"

/* Colours: deep navy, "glass" panels a little lighter, one accent. */
#define C_BG     0x0a0f1c
#define C_GLASS  0x141a28             /* a panel, a window, a card */
#define C_LINE   0x2a3245             /* their edge */
#define C_ACC    0x4be3c1             /* focus, the current project, buttons that matter */
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

void redraw(void);
void redraw_box(int x0, int y0, int x1, int y1);

/* ---- Click boxes: what a click at (x, y) means; searched from the last. */
enum { H_BTN = 1, H_PROJ, H_CARD, H_WIN, H_WBTN, H_LENS, H_RES, H_MENU, H_PR, H_FIELD, H_LOGIN, H_ED,
       H_PANEL, H_PIN, H_PSEND, H_PSTOP, H_PSUG, H_PACT, H_PCLOSE, H_PMODE, H_SETUP };
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
int  atlas_files(char *out, size_t cap);              /* "path (project X)\n"..., -> length */
int  atlas_open_file(const char *path);               /* 0, or -ENOENT */
int  atlas_open_project(const char *name);            /* 0, or -ENOENT */
int  atlas_find(const char *key, const char *value);  /* the files with that attribute, in the Lens -> count */
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

/* ---- The assistant (panel.c): a window of the stage, talking to port "sia". */
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

/* ---- The file editor (edit.c): one file at a time, a window of the stage. */
int  edit_open(const char *path);                     /* 0 or -error */
int  edit_active(void);                               /* a file is open */
const char *edit_path(void);
int  edit_dirty(void);
void edit_draw(int x0, int y0, int x1, int y1, int focused);
int  edit_key(int c);                                 /* 1 if taken; Esc may close it */
void edit_click(int x, int y);
void edit_wheel(int w);
void edit_close(void);
