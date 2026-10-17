/*
 * plat.h - What the app needs from the system: a window to put pixels in,
 * input, the clipboard. Linux: plat_sdl.c (SDL2). SIEOS: an Atlas window.
 */
#ifndef PLAT_H
#define PLAT_H

#include <stdint.h>

enum { EV_NONE, EV_QUIT, EV_RESIZE, EV_DOWN, EV_UP, EV_MOVE, EV_WHEEL, EV_KEY, EV_TEXT, EV_WAKE, EV_LEAVE };
enum { M_SHIFT = 1, M_CTRL = 2, M_ALT = 4 };
enum {                               /* keys (others: their ASCII, lower case) */
    K_ESC = 0x1b, K_ENTER = '\r', K_TAB = '\t', K_BACKSPACE = 8, K_DELETE = 127,
    K_LEFT = 0x100, K_RIGHT, K_UP, K_DOWN, K_HOME, K_END, K_PGUP, K_PGDN, K_F1,
};
enum { CUR_ARROW, CUR_HAND, CUR_TEXT, NCUR };

typedef struct {
    int type;
    int x, y;                        /* the pointer, in window pixels */
    int button, clicks;              /* EV_DOWN, EV_UP: 1 left, 2 middle, 3 right */
    int key, mod;                    /* EV_KEY */
    int wheel;                       /* EV_WHEEL: + up, - down (in 1/120 notches... here: lines * 120) */
    char text[32];                   /* EV_TEXT: UTF-8 */
} ev_t;

int   plat_open(const char *title, int w, int h);   /* (decorated by the window manager) */
void  plat_size(int *w, int *h);
uint32_t *plat_begin(int *pitch);    /* the window's pixels, to fill (0xRRGGBB) */
void  plat_end(void);                /* ...shown */
int   plat_wait(ev_t *e, int timeout_ms);   /* 0: nothing before the timeout */
void  plat_wake(void);               /* from any thread: plat_wait returns EV_WAKE */
void  plat_cursor(int kind);
void  plat_clip_set(const char *utf8);
char *plat_clip_get(void);           /* malloc'd, or NULL */
void  plat_open_url(const char *url);
void  plat_title(const char *t);
void  plat_resize(int w, int h);
uint32_t plat_ms(void);
int   plat_scale(void);              /* the screen's scale, in % (100: normal, 200: HiDPI) */

#endif
