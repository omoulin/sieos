/*
 * wm.c - The desktop's windows (docs/desktop.md): the BeOS way of doing
 * it, in SIEOS's colours.
 *
 *   ┌─────────────────────┐
 *   │ ▣ Terminal 2   □  x │  the tab: as wide as its title, on the window's
 *   ├─────────────────────┴──────┐  top left: the app's icon, the title; □ zooms
 *   │ (title when sunk)   80 x 25 │  (the whole desk, again: back), x closes; drag it to move,
 *   │   the window (dark glass)  │  double-click it to minimize, right-click
 *   │                        ⟋⟋⟋ │  for the window's menu. The grip (or an
 *   └────────────────────────────┘  edge) resizes.
 *
 * The focused window's tab rises out of its frame (220 ms, a slight
 * overshoot), in the accent colour; the others' sink back into it (170 ms),
 * leaving their title in the strip at the window's top. The tab is drawn
 * before the window's glass, so only what rose above the frame shows. A
 * thread ticks every 16 ms only while a tab moves, or while a new window
 * opens: a bright line at its middle that widens, then opens to its full
 * height (160 ms, a CRT powering on; not under Glass, nor at 300% and up).
 *
 * A window may go past the screen's bottom and sides; its tab never leaves
 * the screen (past the left side, the tab slides to stay on it). Each
 * window lives on one of 4 workspaces; minimized ones wait on the Deskbar.
 * Window ids are atlas's (terminals, the editor, sia, Settings, Files): it
 * says whether one exists, its title, and draws its inside.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"

#define MINW 360                         /* the smallest window */
#define OPEN_MS 160                      /* a new window's opening */
#define MINH 200
#define BOX 20                           /* the tab's close and zoom boxes */
#define TAB_L 44                         /* the tab: its icon, before the title */
#define TAB_R (16 + BOX + 8 + BOX + 12)  /* ... zoom and close, after it */

typedef struct {
    int known, ws, min, max;             /* placed once; its workspace; minimized; zoomed */
    int x0, y0, x1, y1;                  /* its box (the tab is above it) */
    int rx0, ry0, rx1, ry1;              /* zoomed: the box to go back to */
    int r_from, r_to;                    /* the tab: rising (to TAB_H) or sinking (to 0) ... */
    uint64_t r_t0;                       /* ... since then (0: at rest) */
    int was;                             /* moved at the last tick: one more redraw */
    uint64_t o_t0;                       /* opening since then (0: open) */
} wwin_t;
static wwin_t ww[WM_MAX];
static int z[WM_MAX], nz;                /* the stacking order, back to front */
static int cws;                          /* the workspace on the screen */
static int AX0, AY0, AX1, AY1;           /* where new and zoomed windows go */
static int focused = -1;                 /* whose tab is up */
static struct { int on, w, mask, px, py, b[4]; } dg;   /* a move (mask 0) or a resize */
static long port, anim_tid;
static volatile int ticking;

/* ---- The tab's animation: progress 0..1024, eased; a thread that ticks
 * (ATLAS_ANIM) only while a tab moves. */
static int prog(uint64_t t0, int ms)
{
    if (!t0) return 1024;
    uint64_t e = (sys_clock() - t0) / 1000000;
    return e >= (uint64_t)ms ? 1024 : (int)(e * 1024 / ms);
}
static int ease_in(int p) { return (int)((int64_t)p * p * p >> 20); }
static int back_out(int p)                       /* past the end a little, then back */
{
    int64_t t = p - 1024;
    return 1024 + (int)((2253 * t * t * t >> 30) + (1229 * t * t >> 20));
}
static int rise(wwin_t *v)                       /* how far the tab is out of the frame now */
{
    int up = v->r_to > v->r_from, p = prog(v->r_t0, up ? 220 : 170);
    if (p >= 1024) { v->r_t0 = 0; return v->r_to; }
    return v->r_from + (v->r_to - v->r_from) * (up ? back_out(p) : ease_in(p)) / 1024;
}
static void anim_thread(void *arg)
{
    (void)arg;
    for (;;) {
        if (!ticking) { sys_sleep(250000000); continue; }   /* (woken early by kick) */
        sys_sleep(16000000);
        msg_t m = { .w = { ATLAS_ANIM } };
        ipc_call(port, &m);
    }
}
static void kick(void) { if (!ticking) { ticking = 1; if (anim_tid > 0) sys_wake(anim_tid); } }
void wm_init(long atlas_port) { port = atlas_port; anim_tid = thread_start(anim_thread, 0, 4096); }

/* ---- Boxes. */
static int tab_w(int w)                  /* the tab: its icon, its title, the two boxes */
{
    char t[64];
    wm_title(w, t, sizeof t);
    int n = TAB_L + tw(strlen(t), 2) + TAB_R, room = ww[w].x1 - ww[w].x0;
    return n < room ? n : room;
}
static int tab_x(int w) { return ww[w].x0 < 0 ? 0 : ww[w].x0; }   /* (past the left side: it slides) */

/* Sizes within limits; the tab wholly on the screen: no further left than
 * the window can still hold it, no further right or down than it fits. */
static void clamp(int w)
{
    wwin_t *v = &ww[w];
    int wd = v->x1 - v->x0, h = v->y1 - v->y0;
    if (wd > SW) wd = SW;
    if (h > SH) h = SH;
    if (wd < MINW) wd = MINW;
    if (h < MINH) h = MINH;
    v->x1 = v->x0 + wd; v->y1 = v->y0 + h;
    int t = tab_w(w);
    if (v->x0 > SW - t) v->x0 = SW - t;
    if (v->x0 < t - wd) v->x0 = t - wd;
    if (v->y0 > SH - 8) v->y0 = SH - 8;
    if (v->y0 < AY0) v->y0 = AY0;
    v->x1 = v->x0 + wd; v->y1 = v->y0 + h;
}
/* Where new and zoomed windows go. A new size of it (the screen's, the
 * zoom): the windows keep their place and size in proportion to it. */
static int scale_x(int v, int ox0, int ow) { return AX0 + (int)((int64_t)(v - ox0) * (AX1 - AX0) / ow); }
static int scale_y(int v, int oy0, int oh) { return AY0 + (int)((int64_t)(v - oy0) * (AY1 - AY0) / oh); }
void wm_area(int x0, int y0, int x1, int y1)
{
    int ox0 = AX0, oy0 = AY0, ow = AX1 - AX0, oh = AY1 - AY0;
    AX0 = x0; AY0 = y0; AX1 = x1; AY1 = y1;
    for (int w = 0; w < WM_MAX; w++) {
        wwin_t *v = &ww[w];
        if (!v->known) continue;
        if (ow > 0 && oh > 0 && (ow != AX1 - AX0 || oh != AY1 - AY0 || ox0 != AX0 || oy0 != AY0)) {
            v->x0 = scale_x(v->x0, ox0, ow); v->x1 = scale_x(v->x1, ox0, ow);
            v->y0 = scale_y(v->y0, oy0, oh); v->y1 = scale_y(v->y1, oy0, oh);
            v->rx0 = scale_x(v->rx0, ox0, ow); v->rx1 = scale_x(v->rx1, ox0, ow);
            v->ry0 = scale_y(v->ry0, oy0, oh); v->ry1 = scale_y(v->ry1, oy0, oh);
        }
        if (v->max) { v->x0 = AX0; v->y0 = AY0; v->x1 = AX1; v->y1 = AY1; }
        if (wm_state(w)) clamp(w);
    }
}
static void damage(const wwin_t *v)                   /* redraw what a window covers (its tab too) */
{
    redraw_box((v->x0 < 0 ? 0 : v->x0) - 12, v->y0 - TAB_H - 12, v->x1 + 12, v->y1 + 12);
}

/* ---- The stacking order. */
static void z_remove(int w) { for (int i = 0; i < nz; i++) if (z[i] == w) { memmove(z + i, z + i + 1, (nz - i - 1) * sizeof *z); nz--; i--; } }
static void raise(int w) { z_remove(w); z[nz++] = w; }
static int shown(int w) { return w >= 0 && w < WM_MAX && ww[w].known && !ww[w].min && ww[w].ws == cws && wm_state(w); }

/* A new window: down and to the right of the front one (its tab stays in
 * view), back to the top left when it would not fit; sia on the right. */
static void place(int w)
{
    wwin_t *v = &ww[w];
    int aw = AX1 - AX0, ah = AY1 - AY0, top = wm_top();
    int ww_ = aw * 56 / 100, hh = ah * 70 / 100;
    if (w == WM_SIA) { v->x1 = AX1; v->x0 = v->x1 - aw * 42 / 100; v->y0 = AY0; v->y1 = v->y0 + ah * 86 / 100; }
    else if (w == WM_EDIT || w == WM_REPO) { v->x0 = AX0 + 20; v->y0 = AY0; v->x1 = v->x0 + aw * 76 / 100; v->y1 = v->y0 + ah * 88 / 100; }
    else if (w == WM_PINS) { v->x0 = AX0 + 20; v->y0 = AY0; v->x1 = v->x0 + aw * 76 / 100; v->y1 = v->y0 + ah * 60 / 100; }   /* (its tracks, room for more) */
    else if (w == WM_SET) { v->x0 = AX0 + 60; v->x1 = v->x0 + aw * 54 / 100; v->y0 = AY0; v->y1 = AY1; }   /* (Settings: all its rows) */
    else {
        if (w == WM_FILES) { ww_ = aw * 40 / 100; hh = ah * 56 / 100; }   /* (a list: narrower, shorter) */
        v->x0 = AX0 + 30; v->y0 = AY0 + 10;
        if (top >= 0 && ww[top].x0 + 48 + ww_ <= AX1 && ww[top].y0 + 48 + hh <= AY1) { v->x0 = ww[top].x0 + 48; v->y0 = ww[top].y0 + 48; }
        v->x1 = v->x0 + ww_; v->y1 = v->y0 + hh;
    }
    v->known = 1; v->min = v->max = 0; v->ws = cws;
    v->r_from = v->r_to = 0; v->r_t0 = 0;
    clamp(w);
    if (draw_look_get() != LOOK_GLASS && draw_zoom_get() < 147456) { v->o_t0 = sys_clock(); kick(); }   /* (it opens) */
}

/* ---- What atlas asks for. */
void wm_show(int w)                      /* on the screen and in front (its workspace with it) */
{
    if (w < 0 || w >= WM_MAX) return;
    wwin_t *v = &ww[w];
    if (!v->known) place(w);
    if (v->ws != cws) wm_switch(v->ws);
    v->min = 0;
    raise(w);
}
void wm_min(int w)
{
    if (w < 0 || w >= WM_MAX || !ww[w].known || ww[w].min) return;
    ww[w].min = 1;
    say("atlas: window %d minimized\n", w);
}
void wm_toggle_max(int w)                /* zoom: the whole desk, or back */
{
    if (w < 0 || w >= WM_MAX || !ww[w].known) return;
    wwin_t *v = &ww[w];
    if (v->max) { v->x0 = v->rx0; v->y0 = v->ry0; v->x1 = v->rx1; v->y1 = v->ry1; v->max = 0; }
    else { v->rx0 = v->x0; v->ry0 = v->y0; v->rx1 = v->x1; v->ry1 = v->y1; v->x0 = AX0; v->y0 = AY0; v->x1 = AX1; v->y1 = AY1; v->max = 1; }
    clamp(w);
    say("atlas: window %d %s\n", w, v->max ? "zoomed" : "unzoomed");
}
void wm_gone(int w)
{
    if (w < 0 || w >= WM_MAX) return;
    if (dg.on && dg.w == w) dg.on = 0;
    if (focused == w) focused = -1;
    memset(&ww[w], 0, sizeof ww[w]);
    z_remove(w);
}
void wm_reset(void) { memset(ww, 0, sizeof ww); nz = 0; cws = 0; dg.on = 0; focused = -1; }
int wm_minimized(int w) { return w >= 0 && w < WM_MAX && ww[w].known && ww[w].min; }
int wm_box(int w, int *x0, int *y0, int *x1, int *y1)
{
    if (!shown(w)) return 0;
    *x0 = ww[w].x0; *y0 = ww[w].y0; *x1 = ww[w].x1; *y1 = ww[w].y1;
    return 1;
}
/* Workspace ws's windows as small frames in a cell of the Deskbar, back
 * to front; the front one (on the current workspace) brighter. */
void wm_minimap(int ws, int x0, int y0, int x1, int y1, uint32_t c, uint32_t front)
{
    int aw = AX1 - AX0, ah = AY1 - AY0, top = ws == cws ? wm_top() : -1;
    if (aw <= 0 || ah <= 0) return;
    for (int i = 0; i < nz; i++) {
        int w = z[i];
        wwin_t *v = &ww[w];
        if (!v->known || v->min || v->ws != ws || !wm_state(w)) continue;
        int a = x0 + (int)((int64_t)(v->x0 - AX0) * (x1 - x0) / aw), b = y0 + (int)((int64_t)(v->y0 - AY0) * (y1 - y0) / ah);
        int c_ = x0 + (int)((int64_t)(v->x1 - AX0) * (x1 - x0) / aw), d = y0 + (int)((int64_t)(v->y1 - AY0) * (y1 - y0) / ah);
        if (a < x0) a = x0;
        if (c_ > x1) c_ = x1;
        if (d > y1) d = y1;
        if (c_ - a < 4 || d - b < 3) continue;
        dl_rect(a, b, c_, d, 0, C_PANEL);                /* (a front window hides those behind it) */
        dl_frame(a, b, c_, d, 0, 2, w == top ? front : c);
    }
}

int wm_top(void)                         /* the front window of this workspace, or -1 */
{
    for (int i = nz - 1; i >= 0; i--) if (shown(z[i])) return z[i];
    return -1;
}
int wm_workspace(void) { return cws; }
int wm_ws_of(int w) { return w >= 0 && w < WM_MAX && ww[w].known ? ww[w].ws : -1; }
void wm_switch(int ws)
{
    if (ws < 0 || ws >= WM_NWS || ws == cws) return;
    cws = ws;
    say("atlas: workspace %d\n", ws + 1);
}
void wm_send(int w, int ws)              /* to another workspace (it stays there) */
{
    if (w < 0 || w >= WM_MAX || !ww[w].known || ws < 0 || ws >= WM_NWS) return;
    ww[w].ws = ws;
    say("atlas: window %d to workspace %d\n", w, ws + 1);
}

/* ---- Moving and resizing with the pointer. */
int wm_grab(int kind, int arg, int x, int y)
{
    int w = kind == H_WTAB ? arg : kind == H_WEDGE ? arg / 16 : -1;
    if (!shown(w)) return 0;
    wwin_t *v = &ww[w];
    dg.on = 1; dg.w = w; dg.mask = kind == H_WEDGE ? arg % 16 : 0;
    dg.px = x; dg.py = y;
    dg.b[0] = v->x0; dg.b[1] = v->y0; dg.b[2] = v->x1; dg.b[3] = v->y1;
    return 1;
}
int wm_grabbed(void) { return dg.w; }
void wm_ungrab(void) { dg.on = 0; }
void wm_motion(int x, int y)
{
    if (!dg.on) return;
    wwin_t *v = &ww[dg.w], old = *v;
    int dx = x - dg.px, dy = y - dg.py;
    v->x0 = dg.b[0]; v->y0 = dg.b[1]; v->x1 = dg.b[2]; v->y1 = dg.b[3];
    if (!dg.mask) { v->x0 += dx; v->x1 += dx; v->y0 += dy; v->y1 += dy; }
    else {
        if (dg.mask & 1) { v->x0 += dx; if (v->x0 > v->x1 - MINW) v->x0 = v->x1 - MINW; }
        if (dg.mask & 2) { v->x1 += dx; if (v->x1 < v->x0 + MINW) v->x1 = v->x0 + MINW; }
        if (dg.mask & 4) { v->y0 += dy; if (v->y0 > v->y1 - MINH) v->y0 = v->y1 - MINH; }
        if (dg.mask & 8) { v->y1 += dy; if (v->y1 < v->y0 + MINH) v->y1 = v->y0 + MINH; }
    }
    v->max = 0;
    clamp(dg.w);
    int a = old.x0 < v->x0 ? old.x0 : v->x0, b = old.y0 < v->y0 ? old.y0 : v->y0;
    int c = old.x1 > v->x1 ? old.x1 : v->x1, d = old.y1 > v->y1 ? old.y1 : v->y1;
    redraw_box((a < 0 ? 0 : a) - 12, b - TAB_H - 12, c + 12, d + 12);   /* where it was and where it is */
}
void wm_drop(void)
{
    if (!dg.on) return;
    wwin_t *v = &ww[dg.w];
    say("atlas: window %d at %d %d %d %d\n", dg.w, v->x0, v->y0, v->x1, v->y1);
    dg.on = 0;
    damage(v);
}


/* ---- Drawing. */
static void box(int x, int y, int zoom, uint32_t c, int kind, int arg, int live)
{
    if (zoom) dl_frame(x + 3, y + 3, x + BOX - 3, y + BOX - 3, 2, 2, c);   /* (zoom: a square) */
    else for (int k = 0; k <= 11; k++) {                 /* (close: a plain x, no box; solid strokes at any zoom) */
        dl_rect(x + 3 + k, y + 3 + k, x + 6 + k, y + 6 + k, 0, c);
        dl_rect(x + 14 - k, y + 3 + k, x + 17 - k, y + 6 + k, 0, c);
    }
    if (live) hitbox(x - 4, y - 4, x + BOX + 4, y + BOX + 4, kind, arg);
}

static void draw_one(int w, int f, int keys)
{
    wwin_t *v = &ww[w];
    int x0 = v->x0, y0 = v->y0, x1 = v->x1, y1 = v->y1, t = tab_w(w), tx = tab_x(w), r = rise(v);
    int op = v->o_t0 ? prog(v->o_t0, OPEN_MS) : 1024;
    char title[64];
    wm_title(w, title, sizeof title);
    int n = strlen(title), up = r >= TAB_H / 2;          /* (half out: it is the tab; else the strip) */
    /* the tab, before the glass: only what rose above the frame shows */
    if (r > 0 && op >= 1024) {
        int ty = y0 - r;
        dl_rect(tx, ty, tx + t, y0 + 12, 10, C_ACC);
        if (up) hitbox(tx, ty, tx + t, y0, H_WTAB, w);    /* (before its boxes: they win) */
        int cx = tx + t - 12 - BOX, zx = cx - 8 - BOX;   /* (close at the end, zoom before it) */
        icon_ink(wm_icon(w), tx + 14, ty + 5, 2);
        dl_text(tx + TAB_L, ty + 6, title, n, S(2), C_INK, tx + TAB_L - 2, ty, zx - 12, y0);
        dl_text(tx + TAB_L + 1, ty + 6, title, n, S(2), C_INK, tx + TAB_L - 2, ty, zx - 12, y0);   /* (bold) */
        box(cx, ty + 7, 0, C_INK, H_WMBTN, w * 4 + 2, up);
        if (zx >= tx + TAB_L) box(zx, ty + 7, 1, C_INK, H_WMBTN, w * 4 + 1, up);
    }
    if (atlas_debug) {                                   /* (where they are once risen) */
        say("atlas: dbg wtab %s at %d %d\n", title, tx + TAB_L + tw(n, 2) / 2, y0 - TAB_H / 2);
        say("atlas: dbg wclose %d at %d %d\n", w, tx + t - 12 - BOX / 2, y0 - TAB_H + 7 + BOX / 2);
        say("atlas: dbg wzoom %d at %d %d\n", w, tx + t - 12 - BOX - 8 - BOX / 2, y0 - TAB_H + 7 + BOX / 2);
    }
    if (op < 1024) {                                     /* opening: a line that widens, then opens to full height */
        int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, a = op < 360 ? op * 1024 / 360 : 1024, b = op < 360 ? 0 : (op - 360) * 1024 / 664;
        a = 1024 - ((1024 - a) * (1024 - a) >> 10); b = 1024 - ((1024 - b) * (1024 - b) >> 10);   /* (eased out) */
        int hw = (x1 - x0) / 2 * a / 1024 + 2, hh = (y1 - y0) / 2 * b / 1024 + 2;
        if (b) { dl_rect(cx - hw, cy - hh, cx + hw, cy + hh, 6, C_GLASS); dl_frame(cx - hw, cy - hh, cx + hw, cy + hh, 6, 2, C_ACC); }
        dl_rect(cx - hw, cy - 2, cx + hw, cy + 2, 0, C_TXT);   /* (the beam) */
        hitbox(x0, y0, x1, y1, H_WIN, w);
        if (atlas_debug) say("atlas: dbg window %d at %d %d %d %d\n", w, x0, y0, x1, y1);
        return;
    }
    /* its shadow: two hard steps down and to the right (a printed look, no blur) */
    dl_shade(x0 + 8, y0 + 8, x1 + 8, y1 + 8, 12, 1);
    dl_shade(x0 + 4, y0 + 4, x1 + 4, y1 + 4, 12, 1);
    dl_frame(x0 - 3, y0 - 3, x1 + 3, y1 + 3, 15, 3, C_GAP);   /* (a dark gap around it: overlapping windows stay apart) */
    /* the window: its glass, its frame, the strip at its top */
    dl_rect(x0, y0, x1, y1, 12, C_GLASS);
    dl_frame(x0, y0, x1, y1, 12, f ? 2 : 1, f ? C_ACC : C_LINE);
    if (r > 0) dl_rect(tx + 2, y0, tx + t - 2, y0 + 2, 0, C_ACC);   /* (the tab joins it) */
    hitbox(x0, y0, x1, y1, H_WIN, w);
    if (!up) {                                           /* sunk: the title in the strip, a handle to move it */
        dl_text(tx + 22, y0 + 6, title, n, S(2), C_DIM, x0 + 2, y0, x1 - 20, y0 + WM_STRIP);
        hitbox(x0 + 8, y0 + 3, x1 - 8, y0 + WM_STRIP, H_WTAB, w);
    } else {                                             /* up: the strip says more */
        char info[64];
        wm_info(w, info, sizeof info);
        int k = strlen(info);
        if (k) dl_text(x1 - 22 - text_w(k, SS), y0 + 8, info, k, SS, C_DIM, x0 + 20, y0, x1 - 20, y0 + WM_STRIP);
    }
    dl_rect(x0 + 1, y0 + WM_STRIP - 1, x1 - 1, y0 + WM_STRIP, 0, C_LINE);
    wm_body(w, x0, y0 + WM_BODY_DY, x1, y1, keys);
    if (!f) dl_shade(x0 + 2, y0 + WM_STRIP, x1 - 2, y1 - 2, 10, 1);   /* (behind: its insides dimmed, the focused one stands out) */
    /* the grip: three short diagonals in the bottom-right corner */
    for (int i = 0; i < 3; i++)
        for (int k = 0; k <= 4 + 5 * i; k += 2)
            dl_rect(x1 - 10 - (4 + 5 * i) + k, y1 - 10 - k, x1 - 8 - (4 + 5 * i) + k, y1 - 8 - k, 0, C_DIM);
    /* the edges and the grip, last: they win near the frame */
    hitbox(x0 - 6, y0 + 16, x0 + 5, y1 - 16, H_WEDGE, w * 16 + 1);
    hitbox(x1 - 5, y0 + 16, x1 + 6, y1 - 16, H_WEDGE, w * 16 + 2);
    hitbox(tx + t, y0 - 4, x1 - 16, y0 + 3, H_WEDGE, w * 16 + 4);
    hitbox(x0 + 16, y1 - 5, x1 - 16, y1 + 6, H_WEDGE, w * 16 + 8);
    hitbox(x0 - 8, y1 - 14, x0 + 14, y1 + 8, H_WEDGE, w * 16 + 9);
    hitbox(x1 - 24, y1 - 24, x1 + 8, y1 + 8, H_WEDGE, w * 16 + 10);
    if (atlas_debug) say("atlas: dbg window %d at %d %d %d %d\n", w, x0, y0, x1, y1);
}

/* The workspace's windows, back to front. focus: whose tab is up; keys:
 * who gets the keys (focus, unless the sia strip has them). */
void wm_draw(int focus, int keys)
{
    for (int w = 0; w < WM_MAX; w++) if (ww[w].known && !wm_state(w)) wm_gone(w);   /* (closed by atlas) */
    if (focus != focused) {                              /* the old tab sinks, the new one rises */
        if (focused >= 0 && focused < WM_MAX && ww[focused].known) {
            wwin_t *o = &ww[focused];
            o->r_from = rise(o); o->r_to = 0; o->r_t0 = sys_clock();
        }
        if (focus >= 0 && focus < WM_MAX && ww[focus].known) {
            wwin_t *u = &ww[focus];
            u->r_from = rise(u); u->r_to = TAB_H; u->r_t0 = sys_clock();
        }
        focused = focus;
        kick();
    }
    for (int i = 0; i < nz; i++) {
        if (!shown(z[i])) continue;
        wwin_t *v = &ww[z[i]];
        int hidden = 0;                                  /* (wholly behind a window in front: not drawn) */
        for (int j = i + 1; j < nz && !hidden; j++) {
            wwin_t *u = &ww[z[j]];
            hidden = shown(z[j]) && u->x0 <= v->x0 && u->x1 >= v->x1 && u->y0 <= v->y0 - TAB_H && u->y1 >= v->y1;
        }
        if (!hidden) draw_one(z[i], z[i] == focus, z[i] == keys);
    }
}

/* ATLAS_ANIM: the next frame of the tabs that move (only around their
 * strip); the ticks stop when all rest. */
int wm_tick(void)
{
    int any = 0, opened = 0;
    for (int w = 0; w < WM_MAX; w++) {
        wwin_t *v = &ww[w];
        if (v->known && v->o_t0) {                       /* opening: the whole window, each frame */
            int done = prog(v->o_t0, OPEN_MS) >= 1024;
            if (done) { v->o_t0 = 0; opened = 1; }       /* (open: the caller draws it all, once) */
            else if (shown(w)) damage(v);
            any |= !done || v->r_t0;
            continue;
        }
        if (!v->known || (!v->r_t0 && !v->was)) continue;
        int moving = v->r_t0 != 0;
        if (shown(w)) redraw_box(tab_x(w) - 4, v->y0 - TAB_H - 12, v->x1 + 4, v->y0 + WM_STRIP + 2);
        if (moving) rise(v);                             /* (ends it when its time is up) */
        v->was = moving;
        any |= moving;
    }
    if (!any) ticking = 0;
    return opened;
}
