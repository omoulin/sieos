/*
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 *
 * nsfb-facet.c - libnsfb's surface for the Facet desktop: a Facet window.
 *
 * Copied into libnsfb (src/surface/facet.c) when it is built for SIEOS
 * (Makefile: netsurf-libs).  The window's buffer, shared with the desktop,
 * is the frame buffer itself: Facet's pixels are 0x00RRGGBB words, libnsfb's
 * NSFB_FMT_XRGB8888 (NetSurf's 32 bpp), so the plotters draw straight into
 * it and an update only tells the desktop which part changed.
 *
 * Input: one Facet event can make several libnsfb events (a pointer move
 * before a click; a synthetic Shift around a key), queued here and handed
 * out one per call.
 *   - Keys: Facet gives the character typed; NetSurf maps key codes through
 *     its own tables (fbtk/event.c, a UK layout under Shift).  A character
 *     is sent as the code NetSurf's unshifted table turns back into it, or,
 *     where that table has none (capitals, % ^ { | }), as the key NetSurf's
 *     Shift table turns into it, between a Shift press and release.  The
 *     real Shift keys are not passed on: the character already has them.
 *     Ctrl+letter goes with a Ctrl press (NetSurf's copy, paste, select all).
 *   - Mouse: the window asks for all the pointer's events (FCT_WIN_POINTER:
 *     moves without a button, so NetSurf shows links under the pointer; the
 *     right and middle buttons; the wheel).  Facet gives the buttons' state
 *     after each change; the buttons that changed become presses and
 *     releases, each after a move to where it happened.  A wheel notch is
 *     a click of button 4 (up) or 5 (down), NetSurf's scroll.
 *   - The title: nsfb_set_parameters(nsfb, "title=...") (NetSurf's page
 *     title) sets the window's, as "NetSurf: title" (the Spine's Web
 *     Browser finds its windows by that prefix).
 *   - A resize: the desktop has already resized the buffer; the surface
 *     takes the new one at once, then NetSurf redraws (NSFB_EVENT_RESIZE).
 *   - Closing the window, or the desktop going away: NSFB_CONTROL_QUIT.
 * The pointer is the desktop's: libnsfb's own (a copy drawn into the
 * buffer) is never drawn.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <facet/facet.h>

#include "libnsfb.h"
#include "libnsfb_event.h"
#include "libnsfb_plot.h"

#include "nsfb.h"
#include "surface.h"
#include "plot.h"

#define QMAX 16

struct facet_priv {
    fct_display *d;
    fct_window *w;
    unsigned buttons;                   /* Facet's button state at its last mouse event */
    nsfb_event_t q[QMAX];               /* made from one Facet event, handed out in order */
    int qhead, qlen;
};

static void push(struct facet_priv *p, enum nsfb_event_type_e type, int a, int b)
{
    if (p->qlen == QMAX)
        return;
    nsfb_event_t *e = &p->q[(p->qhead + p->qlen++) % QMAX];
    e->type = type;
    if (type == NSFB_EVENT_MOVE_ABSOLUTE) {
        e->value.vector.x = a;
        e->value.vector.y = b;
        e->value.vector.z = 0;
    } else if (type == NSFB_EVENT_RESIZE) {
        e->value.resize.w = a;
        e->value.resize.h = b;
    } else if (type == NSFB_EVENT_CONTROL) {
        e->value.controlcode = a;
    } else {
        e->value.keycode = a;
    }
}

static void press(struct facet_priv *p, int code)
{
    push(p, NSFB_EVENT_KEY_DOWN, code, 0);
    push(p, NSFB_EVENT_KEY_UP, code, 0);
}

/* The window's buffer as the frame buffer (again after a resize), with the
 * plotters' clip reset to all of it (as select_plotters() does for the others). */
static void adopt(nsfb_t *nsfb, struct facet_priv *p)
{
    struct surface *s = fct_window_surface(p->w);
    nsfb->ptr = (uint8_t *)s->px;
    nsfb->width = s->w;
    nsfb->height = s->h;
    nsfb->linelen = s->stride * 4;
    if (nsfb->plotter_fns)
        nsfb->plotter_fns->set_clip(nsfb, NULL);
}

/* ---------------------------------------------------------------- keys */

/* Facet's special keys (scan codes; 0x100: an E0 prefix) */
static int special_key(unsigned code)
{
    switch (code) {
    case FCT_KEY_UP:     return NSFB_KEY_UP;
    case FCT_KEY_DOWN:   return NSFB_KEY_DOWN;
    case FCT_KEY_LEFT:   return NSFB_KEY_LEFT;
    case FCT_KEY_RIGHT:  return NSFB_KEY_RIGHT;
    case FCT_KEY_HOME:   return NSFB_KEY_HOME;
    case FCT_KEY_END:    return NSFB_KEY_END;
    case FCT_KEY_PGUP:   return NSFB_KEY_PAGEUP;
    case FCT_KEY_PGDN:   return NSFB_KEY_PAGEDOWN;
    case FCT_KEY_DELETE: return NSFB_KEY_DELETE;
    case 0x152:          return NSFB_KEY_INSERT;
    case 0x57:           return NSFB_KEY_F11;
    case 0x58:           return NSFB_KEY_F12;
    }
    if (code >= FCT_KEY_F1 && code < FCT_KEY_F1 + 10)       /* F1..F10: 0x3B..0x44 */
        return NSFB_KEY_F1 + (int)(code - FCT_KEY_F1);
    return NSFB_KEY_UNKNOWN;
}

/* The code NetSurf turns into character c, and whether it needs Shift for it
 * (NetSurf's tables: fbtk/event.c keymap and sh_keymap); 0 for none. */
static int char_key(unsigned c, bool *shift)
{
    *shift = false;
    if (c >= 'A' && c <= 'Z') {
        *shift = true;
        return c - 'A' + 'a';
    }
    switch (c) {
    case '%': *shift = true; return '5';
    case '^': *shift = true; return '6';
    case '{': *shift = true; return '[';
    case '|': *shift = true; return '\\';
    case '}': *shift = true; return ']';
    case '~': return NSFB_KEY_CARET;             /* (unshifted, NetSurf's 94 is '~') */
    }
    if (c >= ' ' && c < 127)
        return c;
    return 0;
}

static void key(struct facet_priv *p, const struct fct_key *k)
{
    if (!k->value)                               /* releases: each press is sent whole */
        return;
    int code = special_key(k->code);
    if (code != NSFB_KEY_UNKNOWN) {
        press(p, code);
        return;
    }
    unsigned c = k->ascii;
    switch (c) {
    case '\n': case '\r': press(p, NSFB_KEY_RETURN); return;
    case '\b': case 127:  press(p, NSFB_KEY_BACKSPACE); return;
    case '\t':            press(p, NSFB_KEY_TAB); return;
    case 27:              press(p, NSFB_KEY_ESCAPE); return;
    }
    if (k->mods & FCT_MOD_CTRL) {                /* Ctrl+letter: the letter (or its control code) */
        int letter = c >= 1 && c <= 26 ? (int)('a' + c - 1)
                   : c >= 'a' && c <= 'z' ? (int)c
                   : c >= 'A' && c <= 'Z' ? (int)(c - 'A' + 'a') : 0;
        if (letter) {
            push(p, NSFB_EVENT_KEY_DOWN, NSFB_KEY_LCTRL, 0);
            press(p, letter);
            push(p, NSFB_EVENT_KEY_UP, NSFB_KEY_LCTRL, 0);
        }
        return;
    }
    bool shift;
    code = char_key(c, &shift);
    if (!code)
        return;
    if (shift)
        push(p, NSFB_EVENT_KEY_DOWN, NSFB_KEY_LSHIFT, 0);
    press(p, code);
    if (shift)
        push(p, NSFB_EVENT_KEY_UP, NSFB_KEY_LSHIFT, 0);
}

/* ---------------------------------------------------------------- mouse */

static void mouse(struct facet_priv *p, int x, int y, unsigned buttons)
{
    static const struct { unsigned bit; int code; } map[] = {
        { 1, NSFB_KEY_MOUSE_1 },                 /* left */
        { 4, NSFB_KEY_MOUSE_2 },                 /* middle */
        { 2, NSFB_KEY_MOUSE_3 },                 /* right */
    };
    push(p, NSFB_EVENT_MOVE_ABSOLUTE, x, y);
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        unsigned was = p->buttons & map[i].bit, now = buttons & map[i].bit;
        if (now && !was)
            push(p, NSFB_EVENT_KEY_DOWN, map[i].code, 0);
        else if (was && !now)
            push(p, NSFB_EVENT_KEY_UP, map[i].code, 0);
    }
    p->buttons = buttons;
}

static void wheel(struct facet_priv *p, int x, int y, int notches)
{
    push(p, NSFB_EVENT_MOVE_ABSOLUTE, x, y);
    int code = notches > 0 ? NSFB_KEY_MOUSE_5 : NSFB_KEY_MOUSE_4;
    for (int n = notches > 0 ? notches : -notches; n > 0 && p->qlen < QMAX - 1; n--)
        press(p, code);
}

/* ---------------------------------------------------------------- surface */

static int facet_defaults(nsfb_t *nsfb)
{
    nsfb->width = 800;
    nsfb->height = 600;
    nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);
    return 0;
}

static int facet_set_geometry(nsfb_t *nsfb, int width, int height, enum nsfb_format_e format)
{
    struct facet_priv *p = nsfb->surface_priv;
    if (format != NSFB_FMT_ANY && format != NSFB_FMT_XRGB8888)
        return -1;                               /* Facet's buffers are 0x00RRGGBB */
    if (p) {                                     /* the window has it: the user sizes it */
        adopt(nsfb, p);
        return 0;
    }
    if (width > 0)
        nsfb->width = width;
    if (height > 0)
        nsfb->height = height;
    nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);
    return 0;
}

static int facet_initialise(nsfb_t *nsfb)
{
    struct facet_priv *p = calloc(1, sizeof(*p));
    if (!p)
        return -1;
    p->d = fct_open();
    if (!p->d) {
        free(p);
        return -1;
    }
    struct fct_window_attr a = {
        .title = "NetSurf", .x = FCT_POS_AUTO, .y = FCT_POS_AUTO,
        .w = nsfb->width, .h = nsfb->height, .min_w = 320, .min_h = 240, .flags = FCT_WIN_POINTER,
    };
    p->w = fct_window_create(p->d, &a);
    if (!p->w) {
        fct_disconnect(p->d);
        free(p);
        return -1;
    }
    nsfb->surface_priv = p;
    adopt(nsfb, p);
    return 0;
}

static int facet_finalise(nsfb_t *nsfb)
{
    struct facet_priv *p = nsfb->surface_priv;
    if (p) {
        fct_window_destroy(p->w);
        fct_disconnect(p->d);
        free(p);
        nsfb->surface_priv = NULL;
    }
    nsfb->ptr = NULL;                            /* (the desktop's memory: not libnsfb's to free) */
    return 0;
}

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

/* The next event: false if none came within timeout ms (-1 waits, 0 polls). */
static bool facet_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    struct facet_priv *p = nsfb->surface_priv;
    if (!p)
        return false;
    long deadline = timeout > 0 ? now_ms() + timeout : 0;
    while (p->qlen == 0) {
        int wait = timeout;
        if (timeout > 0) {
            long left = deadline - now_ms();
            wait = left > 0 ? (int)left : 0;
        }
        struct fct_event ev;
        int r = fct_next_event(p->d, &ev, wait);
        if (r == 0)
            return false;
        if (r < 0) {                             /* the desktop is gone */
            push(p, NSFB_EVENT_CONTROL, NSFB_CONTROL_QUIT, 0);
            break;
        }
        if (ev.window != p->w)
            continue;
        switch (ev.type) {
        case FCT_KEY:
            key(p, &ev.key);
            break;
        case FCT_MOUSE:
            if (ev.kind == FCT_MOUSE_WHEEL)
                wheel(p, ev.x, ev.y, ev.wheel);
            else
                mouse(p, ev.x, ev.y, (unsigned)ev.buttons);
            break;
        case FCT_RESIZE:
            adopt(nsfb, p);
            push(p, NSFB_EVENT_RESIZE, nsfb->width, nsfb->height);
            break;
        case FCT_CLOSE:
            push(p, NSFB_EVENT_CONTROL, NSFB_CONTROL_QUIT, 0);
            break;
        }
        if (p->qlen == 0 && timeout == 0)
            return false;
    }
    *event = p->q[p->qhead];
    p->qhead = (p->qhead + 1) % QMAX;
    p->qlen--;
    return true;
}

/* "title=TEXT": the window's title, "NetSurf: TEXT" (cut to Facet's length on
 * a character boundary: TEXT is UTF-8). */
static int facet_parameters(nsfb_t *nsfb, const char *parameters)
{
    struct facet_priv *p = nsfb->surface_priv;
    if (!p || strncmp(parameters, "title=", 6) != 0)
        return 0;
    const char *t = parameters + 6;
    char title[FCT_TITLE_MAX];
    int n = snprintf(title, sizeof(title), *t ? "NetSurf: %s" : "NetSurf", t);
    if (n >= (int)sizeof(title)) {
        n = sizeof(title) - 1;
        while (n > 0 && ((unsigned char)title[n] & 0xC0) == 0x80)
            n--;                                 /* (the cut fell inside a character) */
        title[n] = 0;
    }
    fct_window_set_title(p->w, title);
    return 0;
}

static int facet_claim(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    (void)nsfb;
    (void)box;
    return 0;
}

static int facet_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct facet_priv *p = nsfb->surface_priv;
    if (p)
        fct_window_damage(p->w, (struct rect){ box->x0, box->y0, box->x1 - box->x0, box->y1 - box->y0 });
    return 0;
}

static int facet_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor)
{
    (void)nsfb;
    (void)cursor;
    return true;                                 /* the desktop draws the pointer */
}

const nsfb_surface_rtns_t facet_rtns = {
    .defaults = facet_defaults,
    .initialise = facet_initialise,
    .finalise = facet_finalise,
    .input = facet_input,
    .parameters = facet_parameters,
    .claim = facet_claim,
    .update = facet_update,
    .cursor = facet_cursor,
    .geometry = facet_set_geometry,
};

NSFB_SURFACE_DEF(facet, NSFB_SURFACE_FACET, &facet_rtns)
