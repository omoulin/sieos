/*
 * wm.c - The Standard desktop's windows (docs/desktop.md): free windows
 * that move, resize, maximize, snap, minimize to the dock and close, in
 * Stage's colours. The desktop is chosen at login: Focused (the stage,
 * atlas.c) or Standard (this).
 *
 * Each window (atlas's ids: the terminals, the editor, sia) has a box, a
 * place in the stacking order, and a few animations, all in integers:
 *   - the focused window's title bar rises out of its frame (220 ms, a
 *     slight overshoot) and an accent line runs along its top edge; the
 *     others' bars sink back in (170 ms), leaving their title in a slot;
 *   - opening unfolds a window from a dot to a line to its box (260 ms),
 *     closing folds it back (240 ms); minimizing and restoring fly it to and
 *     from its chip in the dock, snapping and maximizing glide it to its new
 *     box (220 ms). While a box moves only its glass and frame are drawn:
 *     the body follows when it lands;
 *   - the project rail rolls up to its title and its bottom line, and
 *     unrolls while the pointer is over it (200 ms).
 * The bar is drawn before the window's glass, so only what rose above the
 * frame shows: no clipping and no transparency needed. The glow is two
 * solid lines.
 *
 * At rest nothing runs: the animation thread ticks every 16 ms only while
 * something moves, and each tick redraws only what the moving things cover.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"

#define BARH  44                         /* the title bar: how far it rises (WM_RISE) */
#define MINW  420                        /* the smallest window */
#define MINH  240
#define C_MID 0x2f9e88                   /* between C_SEL and C_ACC: the glow's inner line */
enum { TW_NONE, TW_FLY, TW_OPEN };

typedef struct {
    int known, min, max;                 /* placed once; minimized; 1 maximized, 2 snapped to a half */
    int x0, y0, x1, y1;                  /* its box */
    int rx0, ry0, rx1, ry1;              /* max: the box to go back to */
    int cx0, cy0, cx1, cy1;              /* its chip in the dock (atlas tells us) */
    int r_from, r_to;                    /* the bar: rising (to WM_RISE) or sinking (to 0) */
    uint64_t r_t0, scan_t0;
    int tw, tw_ms, f[4], t[4];           /* the box flying or unfolding: from f to t */
    uint64_t tw_t0;
    int ex0, ey0, ex1, ey1, ex;          /* what it covered when last drawn (ex: valid) */
    int was;                             /* it moved at the last tick: one more redraw */
} wwin_t;
static wwin_t ww[WM_MAX];
static int z[WM_MAX], nz;                /* the stacking order, back to front */
static struct { int on, b[4]; uint64_t t0; } ghost[4];   /* closed windows, folding away */
static int AX0, AY0, AX1, AY1;           /* where windows may go */
static int focused = -1;                 /* whose bar is up */
static struct { int on, w, mask, px, py, b[4], snap, s[4]; } dg;   /* a move or a resize */
static struct { int open, from, to, was; uint64_t t0; int b[4]; } rail = { 0, 0, 0, 0, 0, { 0 } };
static long port, anim_tid;
static volatile int ticking;

/* ---- Time and easing: progress p in 0..1024. */
static int prog(uint64_t t0, int ms)
{
    if (!t0) return 1024;
    uint64_t e = (sys_clock() - t0) / 1000000;
    return e >= (uint64_t)ms ? 1024 : (int)(e * 1024 / ms);
}
static int ease_out(int p) { int64_t q = 1024 - p; return 1024 - (int)(q * q * q >> 20); }
static int ease_in(int p)  { return (int)((int64_t)p * p * p >> 20); }
static int ease_io(int p)  { return p < 512 ? ease_in(p * 2) / 2 : 512 + ease_out(p * 2 - 1024) / 2; }
static int back_out(int p)                       /* past the end a little, then back */
{
    int64_t t = p - 1024;
    return 1024 + (int)((2253 * t * t * t >> 30) + (1229 * t * t >> 20));
}

/* ---- The animation thread: one message to atlas every 16 ms while ticking. */
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
static void kick(void)
{
    if (ticking) return;
    ticking = 1;
    if (anim_tid > 0) sys_wake(anim_tid);
}
void wm_init(long atlas_port)
{
    port = atlas_port;
    anim_tid = thread_start(anim_thread, 0, 4096);
}

/* ---- Boxes. */
static void unite(int *d, int x0, int y0, int x1, int y1)   /* d: the union so far (d[0] > d[2]: empty) */
{
    if (x0 >= x1 || y0 >= y1) return;
    if (d[0] > d[2]) { d[0] = x0; d[1] = y0; d[2] = x1; d[3] = y1; return; }
    if (x0 < d[0]) d[0] = x0;
    if (y0 < d[1]) d[1] = y0;
    if (x1 > d[2]) d[2] = x1;
    if (y1 > d[3]) d[3] = y1;
}
static void flush(const int *d) { if (d[0] < d[2]) redraw_box(d[0], d[1], d[2], d[3]); }
static void ext(int *d, int x0, int y0, int x1, int y1)  /* a box with its glow, brackets and bar */
{
    unite(d, x0 - 12, y0 - BARH - 12, x1 + 12, y1 + 14);
}
static void clamp(wwin_t *v)
{
    int w = v->x1 - v->x0, h = v->y1 - v->y0;
    if (w > AX1 - AX0) w = AX1 - AX0;
    if (h > AY1 - AY0) h = AY1 - AY0;
    if (w < MINW) w = MINW;
    if (h < MINH) h = MINH;
    if (v->x0 + w > AX1) v->x0 = AX1 - w;
    if (v->y0 + h > AY1) v->y0 = AY1 - h;
    if (v->x0 < AX0) v->x0 = AX0;
    if (v->y0 < AY0) v->y0 = AY0;
    v->x1 = v->x0 + w; v->y1 = v->y0 + h;
}
void wm_area(int x0, int y0, int x1, int y1) { AX0 = x0; AY0 = y0; AX1 = x1; AY1 = y1; }

/* The box as drawn now: flying or unfolding (-> 1), or where it is (-> 0). */
static int now_box(wwin_t *v, int *b)
{
    b[0] = v->x0; b[1] = v->y0; b[2] = v->x1; b[3] = v->y1;
    if (v->tw == TW_NONE) return 0;
    int p = prog(v->tw_t0, v->tw_ms);
    if (p >= 1024) { v->tw = TW_NONE; return 0; }
    if (v->tw == TW_FLY) {
        int e = ease_io(p);
        for (int i = 0; i < 4; i++) b[i] = v->f[i] + (v->t[i] - v->f[i]) * e / 1024;
        return 1;
    }
    /* TW_OPEN: a dot grows into a line (the first 45 %), the line into the box */
    int cx = (v->t[0] + v->t[2]) / 2, cy = (v->t[1] + v->t[3]) / 2;
    if (p < 460) {
        int half = (v->t[2] - v->t[0]) / 2 * ease_out(p * 1024 / 460) / 1024;
        b[0] = cx - half; b[2] = cx + half + 1; b[1] = cy - 1; b[3] = cy + 1;
    } else {
        int hh = (v->t[3] - v->t[1]) / 2 * ease_out((p - 460) * 1024 / 564) / 1024;
        if (hh < 1) hh = 1;
        b[0] = v->t[0]; b[2] = v->t[2]; b[1] = cy - hh; b[3] = cy + hh;
    }
    return 1;
}
static void fly(wwin_t *v, const int *from, const int *to, int kind, int ms)
{
    memcpy(v->f, from, sizeof v->f);
    memcpy(v->t, to, sizeof v->t);
    v->tw = kind; v->tw_ms = ms; v->tw_t0 = sys_clock();
    kick();
}
static int ghost_box(int g, int *b)                       /* a closed window folding: the opening backwards */
{
    int p = prog(ghost[g].t0, 240);
    if (p >= 1024) { ghost[g].on = 0; return 0; }
    wwin_t tmp = { 0 };
    memcpy(tmp.t, ghost[g].b, sizeof tmp.t);
    memcpy(&tmp.x0, ghost[g].b, sizeof ghost[g].b);
    tmp.tw = TW_OPEN; tmp.tw_ms = 1024; tmp.tw_t0 = sys_clock() - (uint64_t)(1024 - p) * 1000000;
    now_box(&tmp, b);
    return 1;
}

/* The bar's height above the frame now. */
static int rise(wwin_t *v)
{
    int up = v->r_to > v->r_from;
    int p = prog(v->r_t0, up ? 220 : 170);
    if (p >= 1024) { v->r_t0 = 0; return v->r_to; }
    int e = up ? back_out(p) : ease_in(p);
    return v->r_from + (v->r_to - v->r_from) * e / 1024;
}
static int moving(wwin_t *v)
{
    int b[4];
    return now_box(v, b) || v->r_t0 || prog(v->scan_t0, 280) < 1024;
}

/* ---- The stacking order. */
static void z_remove(int w) { for (int i = 0; i < nz; i++) if (z[i] == w) { memmove(z + i, z + i + 1, (nz - i - 1) * sizeof *z); nz--; i--; } }
static void raise(int w) { z_remove(w); z[nz++] = w; }

/* A new window's place: down and to the right of the front one (the
 * others' titles stay in view), back to the top left when it would not
 * fit; sia on the right. */
static void place(int w)
{
    wwin_t *v = &ww[w];
    int aw = AX1 - AX0, ah = AY1 - AY0, top = -1;
    for (int i = nz - 1; i >= 0 && top < 0; i--) if (z[i] != w && wm_state(z[i]) == 2 && !ww[z[i]].min) top = z[i];
    if (w == WM_SIA) { v->x1 = AX1 - 20; v->x0 = v->x1 - aw * 40 / 100; v->y0 = AY0 + 20; v->y1 = v->y0 + ah * 80 / 100; }
    else {
        int ww_ = aw * 58 / 100, hh = ah * 74 / 100;
        v->x0 = AX0 + 40; v->y0 = AY0 + 16;
        if (top >= 0 && ww[top].x0 + 64 + ww_ <= AX1 && ww[top].y0 + 64 + hh <= AY1) { v->x0 = ww[top].x0 + 64; v->y0 = ww[top].y0 + 64; }
        v->x1 = v->x0 + ww_; v->y1 = v->y0 + hh;
    }
    clamp(v);
    v->known = 1; v->min = v->max = 0; v->tw = TW_NONE; v->r_from = v->r_to = 0; v->r_t0 = v->scan_t0 = 0;
    raise(w);
}

/* ---- What atlas asks for. */
void wm_show(int w)                                    /* on screen, in front (new: it unfolds) */
{
    if (w < 0 || w >= WM_MAX) return;
    wwin_t *v = &ww[w];
    if (!v->known) {
        place(w);
        int t[4] = { v->x0, v->y0, v->x1, v->y1 };
        fly(v, t, t, TW_OPEN, 260);
    } else if (v->min) {
        v->min = 0;
        int f[4] = { v->cx0, v->cy0, v->cx1, v->cy1 }, t[4] = { v->x0, v->y0, v->x1, v->y1 };
        if (f[2] <= f[0]) { f[0] = (AX0 + AX1) / 2 - 60; f[2] = f[0] + 120; f[1] = AY1; f[3] = AY1 + 40; }
        fly(v, f, t, TW_FLY, 220);
    }
    raise(w);
}
void wm_min(int w)                                     /* into its chip in the dock */
{
    wwin_t *v = &ww[w];
    if (!v->known || v->min) return;
    int f[4], t[4] = { v->cx0, v->cy0, v->cx1, v->cy1 };
    now_box(v, f);
    if (t[2] <= t[0]) { t[0] = (AX0 + AX1) / 2 - 60; t[2] = t[0] + 120; t[1] = AY1; t[3] = AY1 + 40; }
    v->min = 1;
    fly(v, f, t, TW_FLY, 220);
}
static void go_to(wwin_t *v, int x0, int y0, int x1, int y1)   /* glide to a new box */
{
    int f[4];
    now_box(v, f);
    v->x0 = x0; v->y0 = y0; v->x1 = x1; v->y1 = y1;
    clamp(v);
    int t[4] = { v->x0, v->y0, v->x1, v->y1 };
    fly(v, f, t, TW_FLY, 220);
}
static void save(wwin_t *v, int kind)   /* maximized or snapped: remember the box to go back to */
{
    if (!v->max) { v->rx0 = v->x0; v->ry0 = v->y0; v->rx1 = v->x1; v->ry1 = v->y1; }
    v->max = kind;
}
void wm_toggle_max(int w)
{
    wwin_t *v = &ww[w];
    if (!v->known) return;
    if (v->max == 1) { v->max = 0; go_to(v, v->rx0, v->ry0, v->rx1, v->ry1); }
    else { save(v, 1); go_to(v, AX0, AY0, AX1, AY1); }
    say("atlas: window %d %s\n", w, v->max ? "maximized" : "restored");
}
void wm_snap(int w, int side)                          /* -1 left half, 1 right half */
{
    wwin_t *v = &ww[w];
    if (!v->known) return;
    save(v, 2);
    int mid = (AX0 + AX1) / 2;
    if (side < 0) go_to(v, AX0, AY0, mid - 8, AY1); else go_to(v, mid + 8, AY0, AX1, AY1);
}
void wm_gone(int w)                                    /* closed: it folds away; the id is free */
{
    if (w < 0 || w >= WM_MAX) return;
    wwin_t *v = &ww[w];
    if (v->known && !v->min && wm_state(w) == 2)
        for (int g = 0; g < 4; g++)
            if (!ghost[g].on) { ghost[g].on = 1; now_box(v, ghost[g].b); ghost[g].t0 = sys_clock(); kick(); break; }
    if (dg.on && dg.w == w) dg.on = 0;
    if (focused == w) focused = -1;
    memset(v, 0, sizeof *v);
    z_remove(w);
}
void wm_reset(void)
{
    memset(ww, 0, sizeof ww); memset(ghost, 0, sizeof ghost);
    nz = 0; focused = -1; dg.on = 0; rail.open = rail.from = rail.to = 0; rail.t0 = 0;
}
void wm_chip(int w, int x0, int y0, int x1, int y1) { wwin_t *v = &ww[w]; v->cx0 = x0; v->cy0 = y0; v->cx1 = x1; v->cy1 = y1; }
int wm_minimized(int w) { return w >= 0 && w < WM_MAX && ww[w].known && ww[w].min; }
int wm_box(int w, int *x0, int *y0, int *x1, int *y1)   /* -> 1: on screen, at that box */
{
    if (w < 0 || w >= WM_MAX || !ww[w].known || ww[w].min) return 0;
    *x0 = ww[w].x0; *y0 = ww[w].y0; *x1 = ww[w].x1; *y1 = ww[w].y1;
    return 1;
}
int wm_top(void)                                       /* the front window of this project, or -1 */
{
    for (int i = nz - 1; i >= 0; i--) if (wm_state(z[i]) == 2 && !ww[z[i]].min) return z[i];
    return -1;
}

/* ---- The rail: rolled up (0) to open (1024). */
void wm_rail(int open, int x0, int y0, int x1, int y1)
{
    rail.b[0] = x0; rail.b[1] = y0; rail.b[2] = x1; rail.b[3] = y1;
    if (open == rail.open) return;
    int now = wm_rail_open();
    rail.open = open; rail.from = now; rail.to = open ? 1024 : 0; rail.t0 = sys_clock();
    kick();
}
int wm_rail_open(void)
{
    int p = prog(rail.t0, 200);
    if (p >= 1024) { rail.t0 = 0; return rail.to; }
    return rail.from + (rail.to - rail.from) * ease_out(p) / 1024;
}

/* ---- Moving and resizing with the pointer. */
int wm_grab(int kind, int arg, int x, int y)
{
    int w = kind == H_WBAR ? arg : kind == H_WEDGE ? arg / 16 : -1;
    if (w < 0 || w >= WM_MAX || !ww[w].known) return 0;
    wwin_t *v = &ww[w];
    dg.on = 1; dg.w = w; dg.mask = kind == H_WEDGE ? arg % 16 : 0;
    dg.px = x; dg.py = y; dg.snap = 0;
    dg.b[0] = v->x0; dg.b[1] = v->y0; dg.b[2] = v->x1; dg.b[3] = v->y1;
    return 1;
}
static int snap_at(int x, int y, int *s)               /* the pointer at a screen edge: where it would snap */
{
    int mid = (AX0 + AX1) / 2;
    if (y <= 2) { s[0] = AX0; s[1] = AY0; s[2] = AX1; s[3] = AY1; return 3; }
    if (x <= 2) { s[0] = AX0; s[1] = AY0; s[2] = mid - 8; s[3] = AY1; return 1; }
    if (x >= SW - 3) { s[0] = mid + 8; s[1] = AY0; s[2] = AX1; s[3] = AY1; return 2; }
    return 0;
}
void wm_motion(int x, int y)
{
    if (!dg.on) return;
    wwin_t *v = &ww[dg.w];
    int d[4] = { 1, 0, 0, 0 };
    ext(d, v->x0, v->y0, v->x1, v->y1);
    if (dg.snap) unite(d, dg.s[0] - 4, dg.s[1] - 4, dg.s[2] + 4, dg.s[3] + 4);
    if (!dg.mask) {
        if (v->max) {                                    /* pulled out of maximized: its old size, under the pointer */
            int fx = (dg.px - v->x0) * 1024 / (v->x1 - v->x0 ? v->x1 - v->x0 : 1), w = v->rx1 - v->rx0, h = v->ry1 - v->ry0;
            v->max = 0;
            v->x0 = x - fx * w / 1024; v->x1 = v->x0 + w; v->y1 = v->y0 + h;
            clamp(v);
            dg.px = x; dg.py = y;
            dg.b[0] = v->x0; dg.b[1] = v->y0; dg.b[2] = v->x1; dg.b[3] = v->y1;
        }
        int dx = x - dg.px, dy = y - dg.py;
        v->x0 = dg.b[0] + dx; v->y0 = dg.b[1] + dy; v->x1 = dg.b[2] + dx; v->y1 = dg.b[3] + dy;
        clamp(v);
        dg.snap = snap_at(x, y, dg.s);
    } else {
        int dx = x - dg.px, dy = y - dg.py;
        v->x0 = dg.b[0]; v->y0 = dg.b[1]; v->x1 = dg.b[2]; v->y1 = dg.b[3];
        if (dg.mask & 1) { v->x0 += dx; if (v->x0 > v->x1 - MINW) v->x0 = v->x1 - MINW; if (v->x0 < AX0) v->x0 = AX0; }
        if (dg.mask & 2) { v->x1 += dx; if (v->x1 < v->x0 + MINW) v->x1 = v->x0 + MINW; if (v->x1 > AX1) v->x1 = AX1; }
        if (dg.mask & 4) { v->y0 += dy; if (v->y0 > v->y1 - MINH) v->y0 = v->y1 - MINH; if (v->y0 < AY0) v->y0 = AY0; }
        if (dg.mask & 8) { v->y1 += dy; if (v->y1 < v->y0 + MINH) v->y1 = v->y0 + MINH; if (v->y1 > AY1) v->y1 = AY1; }
        v->max = 0;
    }
    ext(d, v->x0, v->y0, v->x1, v->y1);
    if (dg.snap) unite(d, dg.s[0] - 4, dg.s[1] - 4, dg.s[2] + 4, dg.s[3] + 4);
    flush(d);
}
int wm_grabbed(void) { return dg.w; }
void wm_ungrab(void) { dg.on = 0; }
void wm_drop(void)
{
    if (!dg.on) return;
    wwin_t *v = &ww[dg.w];
    int d[4] = { 1, 0, 0, 0 };
    if (dg.snap) {
        unite(d, dg.s[0] - 4, dg.s[1] - 4, dg.s[2] + 4, dg.s[3] + 4);
        v->x0 = dg.b[0]; v->y0 = dg.b[1]; v->x1 = dg.b[2]; v->y1 = dg.b[3];   /* (back to go back to) */
        clamp(v);
        save(v, dg.snap == 3 ? 1 : 2);
        int f[4] = { v->x0, v->y0, v->x1, v->y1 };
        memcpy(&v->x0, dg.s, sizeof dg.s);
        clamp(v);
        int t[4] = { v->x0, v->y0, v->x1, v->y1 };
        fly(v, f, t, TW_FLY, 220);
        say("atlas: window %d snapped\n", dg.w);
    } else say("atlas: window %d at %d %d %d %d\n", dg.w, v->x0, v->y0, v->x1, v->y1);
    dg.on = 0;
    ext(d, v->x0, v->y0, v->x1, v->y1);
    flush(d);
}

/* ---- Drawing. */
static void button(int x, int y, const char *s, int kind, int arg)
{
    dl_rect(x, y, x + 34, y + 30, 6, C_GLASS);
    dl_frame(x, y, x + 34, y + 30, 6, 1, C_LINE);
    int n = strlen(s);
    dl_text(x + 17 - tw(n, 2) / 2, y + 4, s, n, S(2), C_TXT, x, y, x + 34, y + 30);
    hitbox(x, y, x + 34, y + 30, kind, arg);
}

/* The title bar, r pixels above the frame (drawn before the glass: the rest is hidden). */
static void bar(int w, int x0, int y0, int x1, int r)
{
    char t[64];
    wm_title(w, t, sizeof t);
    int n = strlen(t), bw = 32 + tw(n, 2) + 3 * 40 + 8;
    if (bw > x1 - x0 - 40) bw = x1 - x0 - 40;
    int bx0 = x0 + 22, bx1 = bx0 + bw, by0 = y0 - r;
    dl_rect(bx0, by0, bx1, y0 + 20, 10, C_BG);
    dl_frame(bx0, by0, bx1, y0 + 20, 10, 1, C_ACC);
    dl_text(bx0 + 16, by0 + 11, t, n, S(2), C_ACC, bx0, by0, bx1 - 3 * 40 - 8, y0);
    if (r < BARH / 2) return;                          /* (half sunk: not a target) */
    hitbox(bx0, by0, bx1, y0, H_WBAR, w);
    static const char *bl[3] = { "_", "[]", "x" };
    for (int b = 0; b < 3; b++) {
        int x = bx1 - 8 - (3 - b) * 40 + 4;
        button(x, by0 + 7, b == 1 && ww[w].max == 1 ? "][" : bl[b], H_WMBTN, w * 4 + b);
        if (atlas_debug) say("atlas: dbg wbtn %d %d at %d %d\n", w, b, x + 17, by0 + 22);
    }
    if (atlas_debug) say("atlas: dbg wbar %s at %d %d\n", t, bx0 + 30, by0 + 20);
}

static void draw_one(int w, int f, int keys)
{
    wwin_t *v = &ww[w];
    int b[4], flying = now_box(v, b);
    v->ex = 0;
    if (v->min && !flying) return;
    int x0 = b[0], y0 = b[1], x1 = b[2], y1 = b[3];
    v->ex0 = x0 - 12; v->ey0 = y0 - BARH - 12; v->ex1 = x1 + 12; v->ey1 = y1 + 14; v->ex = 1;
    if (flying) {                                      /* only its glass while it moves */
        int rr = (x1 - x0) / 2 < 16 ? (x1 - x0) / 2 : (y1 - y0) / 2 < 16 ? (y1 - y0) / 2 : 16;
        dl_rect(x0, y0, x1, y1, rr, C_GLASS);
        dl_frame(x0, y0, x1, y1, rr, 1, f ? C_ACC : C_LINE);
        return;
    }
    int r = rise(v);
    if (f) {                                           /* the glow: two solid lines */
        dl_frame(x0 - 5, y0 - 5, x1 + 5, y1 + 5, 21, 1, C_SEL);
        dl_frame(x0 - 3, y0 - 3, x1 + 3, y1 + 3, 19, 1, C_MID);
    }
    if (r > 0) bar(w, x0, y0, x1, r);
    dl_rect(x0, y0, x1, y1, 16, C_GLASS);
    dl_frame(x0, y0, x1, y1, 16, f ? 2 : 1, f ? C_ACC : C_LINE);
    hitbox(x0, y0, x1, y1, H_WIN, w);
    if (r < BARH / 2) {                                /* the slot: the title, sunk */
        char t[64];
        wm_title(w, t, sizeof t);
        dl_text(x0 + 22, y0 + 14, t, strlen(t), S(2), C_DIM, x0, y0, x1 - 20, y0 + 48);
        hitbox(x0 + 8, y0 + 4, x1 - 8, y0 + 46, H_WBAR, w);
        if (atlas_debug) say("atlas: dbg wslot %s at %d %d\n", t, x1 - 60, y0 + 24);
    }
    else {                                             /* the bar is up: the strip says more */
        char t[64];
        wm_info(w, t, sizeof t);
        int n = strlen(t);
        if (n) dl_text(x1 - 22 - tw(n, 2), y0 + 14, t, n, S(2), C_DIM, x0 + 20, y0, x1 - 20, y0 + 48);
    }
    dl_rect(x0 + 1, y0 + 47, x1 - 1, y0 + 48, 0, C_LINE);
    int p = prog(v->scan_t0, 280);                     /* the scan along the top edge */
    if (p < 1024) dl_rect(x0 + 16, y0, x0 + 16 + (x1 - x0 - 32) * ease_out(p) / 1024, y0 + 2, 0, C_ACC);
    wm_body(w, x0, y0, x1, y1, keys);
    if (f) {                                           /* brackets at the bottom corners: resize here */
        dl_rect(x0 - 8, y1 + 6, x0 + 8, y1 + 8, 0, C_ACC); dl_rect(x0 - 8, y1 - 8, x0 - 6, y1 + 8, 0, C_ACC);
        dl_rect(x1 - 8, y1 + 6, x1 + 8, y1 + 8, 0, C_ACC); dl_rect(x1 + 6, y1 - 8, x1 + 8, y1 + 8, 0, C_ACC);
    }
    /* the edges and corners, last: they win near the frame */
    hitbox(x0 - 6, y0 + 16, x0 + 5, y1 - 16, H_WEDGE, w * 16 + 1);
    hitbox(x1 - 5, y0 + 16, x1 + 6, y1 - 16, H_WEDGE, w * 16 + 2);
    hitbox(x0 + 16, y0 - 4, x1 - 16, y0 + 3, H_WEDGE, w * 16 + 4);
    hitbox(x0 + 16, y1 - 5, x1 - 16, y1 + 6, H_WEDGE, w * 16 + 8);
    hitbox(x0 - 8, y0 - 8, x0 + 14, y0 + 14, H_WEDGE, w * 16 + 5);
    hitbox(x1 - 14, y0 - 8, x1 + 8, y0 + 14, H_WEDGE, w * 16 + 6);
    hitbox(x0 - 10, y1 - 14, x0 + 14, y1 + 10, H_WEDGE, w * 16 + 9);
    hitbox(x1 - 14, y1 - 14, x1 + 10, y1 + 10, H_WEDGE, w * 16 + 10);
    if (atlas_debug) say("atlas: dbg window %d at %d %d %d %d\n", w, x0, y0, x1, y1);
}

/* All of this project's windows, back to front, then what is folding away
 * and where a dragged window would snap. focus: whose bar is up; keys: who
 * gets the keys (focus, unless the Lens has them). */
void wm_draw(int focus, int keys)
{
    for (int w = 0; w < WM_MAX; w++) {                 /* windows atlas made elsewhere: placed quietly */
        int s = wm_state(w);
        if (!s && ww[w].known) { memset(&ww[w], 0, sizeof ww[w]); z_remove(w); }
        else if (s == 2 && !ww[w].known) place(w);
    }
    if (focus != focused) {                            /* the bars: old one down, new one up */
        if (focused >= 0 && focused < WM_MAX && ww[focused].known) {
            wwin_t *o = &ww[focused];
            o->r_from = rise(o); o->r_to = 0; o->r_t0 = sys_clock();
        }
        if (focus >= 0 && focus < WM_MAX && ww[focus].known) {
            wwin_t *n = &ww[focus];
            n->r_from = rise(n); n->r_to = BARH; n->r_t0 = n->scan_t0 = sys_clock();
            raise(focus);
        }
        focused = focus;
        kick();
    }
    for (int i = 0; i < nz; i++) if (wm_state(z[i]) == 2) draw_one(z[i], z[i] == focus, z[i] == keys);
    for (int g = 0; g < 4; g++) {
        int b[4];
        if (!ghost[g].on || !ghost_box(g, b)) continue;
        dl_rect(b[0], b[1], b[2], b[3], (b[3] - b[1]) / 2 < 16 ? (b[3] - b[1]) / 2 : 16, C_GLASS);
        dl_frame(b[0], b[1], b[2], b[3], (b[3] - b[1]) / 2 < 16 ? (b[3] - b[1]) / 2 : 16, 1, C_ACC);
    }
    if (dg.on && dg.snap) {
        dl_frame(dg.s[0], dg.s[1], dg.s[2], dg.s[3], 16, 2, C_ACC);
        dl_frame(dg.s[0] + 4, dg.s[1] + 4, dg.s[2] - 4, dg.s[3] - 4, 13, 1, C_SEL);
    }
}

/* A tick: redraw what moves (where it was, where it may go), stop when all rest. */
void wm_tick(void)
{
    int d[4] = { 1, 0, 0, 0 }, any = 0;
    for (int w = 0; w < WM_MAX; w++) {
        wwin_t *v = &ww[w];
        if (!v->known) continue;
        int m = moving(v);
        if (!m && !v->was) continue;
        if (v->ex) unite(d, v->ex0, v->ey0, v->ex1, v->ey1);
        if (v->tw != TW_NONE || v->was == 2) {          /* the box moves: all of it */
            ext(d, v->x0, v->y0, v->x1, v->y1);
            if (v->tw == TW_FLY) { ext(d, v->f[0], v->f[1], v->f[2], v->f[3]); ext(d, v->t[0], v->t[1], v->t[2], v->t[3]); }
        } else unite(d, v->x0 - 12, v->y0 - BARH - 12, v->x1 + 12, v->y0 + 50);   /* only the bar moves */
        v->was = m ? (v->tw != TW_NONE ? 2 : 1) : 0;
        any |= m;
    }
    for (int g = 0; g < 4; g++)
        if (ghost[g].on) { ext(d, ghost[g].b[0], ghost[g].b[1], ghost[g].b[2], ghost[g].b[3]); any = 1; }
    int r = rail.t0 != 0;
    if (r || rail.was) unite(d, rail.b[0] - 2, rail.b[1] - 2, rail.b[2] + 2, rail.b[3] + 2);
    rail.was = r; any |= r;
    flush(d);
    if (!any) ticking = 0;
}
