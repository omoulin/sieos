/*
 * facet/protocol.h - The Facet window protocol (version 1).
 *
 * A client connects to the AF_UNIX stream socket named by $FACET_DISPLAY
 * (Facet sets it for every program it starts; by default
 * /tmp/.facet-<uid>).  Every message is a struct fct_msg, a fixed-size
 * record (112 bytes).  A client draws into shared memory: with FCT_CREATE and
 * FCT_BUFFER it passes (SCM_RIGHTS) the descriptor of a file of
 * width * height * 4 bytes (0x00RRGGBB pixels, stride = width), which
 * Facet maps; FCT_DAMAGE asks Facet to show a changed rectangle.
 *
 *   client -> Facet                      Facet -> client
 *   FCT_HELLO    code: version, x: pid   FCT_WELCOME  code: version, w, h: screen
 *   FCT_CREATE   window, x, y: position   FCT_EV_*     events for a window
 *                (FCT_POS_*), w, h: content size, code, value: minimum
 *                content size, kind: FCT_WIN_* flags, title; +fd
 *   FCT_BUFFER   window, w, h; +fd
 *   FCT_DAMAGE   window, x, y, w, h
 *   FCT_TITLE    window, title
 *   FCT_DESTROY  window
 *
 * Windows are numbered by the client (nonzero, unique per connection).
 * When the user resizes a window Facet sends FCT_EV_RESIZE with the new
 * content size and keeps showing the old buffer, clipped or padded, until a
 * buffer of that size arrives.  FCT_EV_CLOSE means the window is gone (the
 * user closed it); the client should drop it.  All integers are native
 * (little-endian).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef FACET_PROTOCOL_H
#define FACET_PROTOCOL_H

#include <stdint.h>

#define FCT_PROTOCOL_VERSION 1
#define FCT_TITLE_MAX 64

enum {
    /* client -> Facet */
    FCT_HELLO = 1,
    FCT_CREATE,
    FCT_BUFFER,
    FCT_DAMAGE,
    FCT_TITLE,
    FCT_DESTROY,
    /* Facet -> client */
    FCT_WELCOME = 64,
    FCT_EV_KEY,          /* code, value (1 press, 0 release), ascii, mods */
    FCT_EV_MOUSE,        /* x, y (content coordinates), kind (FCT_MOUSE_*), buttons;
                            FCT_MOUSE_WHEEL: value, the notches (int32, > 0 down) */
    FCT_EV_RESIZE,       /* w, h: the new content size */
    FCT_EV_CLOSE,        /* the window was closed */
    FCT_EV_TEXT,         /* title: text to type (the desktop's commands for an
                            FCT_WIN_ASSISTANT window); value 1 ends a line */
    FCT_EV_SKIN,         /* window 0, title: the desktop's new skin (facet/skin.h) */
};

/* FCT_CREATE: where to put the window (or screen coordinates) */
#define FCT_POS_AUTO   (-1)      /* cascade */
#define FCT_POS_CENTER (-2)      /* centred (dialogs) */

/* FCT_CREATE flags */
#define FCT_WIN_ASSISTANT 1      /* a terminal running the sia assistant */
#define FCT_WIN_POINTER   2      /* all the pointer's events over the content: moves without a
                                    button (hover), the right and middle buttons (the window
                                    menu stays on the title bar), the wheel (FCT_MOUSE_WHEEL) */

struct fct_msg {
    uint32_t type;
    uint32_t window;
    int32_t x, y, w, h;          /* geometry, or the mouse position */
    uint32_t code, value, ascii, mods, buttons, kind;
    char title[FCT_TITLE_MAX];   /* FCT_CREATE, FCT_TITLE */
};

#ifdef __cplusplus
static_assert(sizeof(struct fct_msg) == 112, "fct_msg size");
#else
_Static_assert(sizeof(struct fct_msg) == 112, "fct_msg size");
#endif

#endif
