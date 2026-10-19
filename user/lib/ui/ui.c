/*
 * ui.c - The UI toolkit's core (docs/ui.md): the application and its
 * window (mk/atlas.h), the widget tree, layout, damage and painting, input
 * (the pointer, keys, focus and Tab), the theme, and drawing into the
 * window's pixels: boxes with round corners, outlines, text (the desktop's
 * 9 x 16 font at 100% and its multiples, its 6 x 11 font smoothly scaled
 * at other sizes) and a few icons. Integers only (SIEOS's programs are
 * built without floating point).
 *
 * Painting: what changed is a list of rectangles (ui_dirty); ui_flush draws
 * the tree clipped to each (only the widgets they touch), the overlays
 * (menus, dialogs) over it, then tells atlas those rectangles.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "ui_int.h"

extern const uint8_t ui_font15[95][16][9];   /* user/atlas/font15.c: 9 x 16, 8-bit coverage */
extern const uint8_t ui_font[95][9];         /* user/atlas/font.c: 5 x 9 in a 6 x 11 cell */

/* ---- Small helpers (no C library needed beyond memory and strings). */
char *ui_strdup(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    char *d = malloc(n + 1);
    if (d) { if (n) memcpy(d, s, n); d[n] = 0; }
    return d;
}
int ui_fmt(char *d, const char *s, long v)
{
    int n = 0;
    char t[24];
    int k = 0;
    while (s && *s) d[n++] = *s++;
    if (v < 0) { d[n++] = '-'; v = -v; }
    do t[k++] = (char)('0' + v % 10); while (v /= 10);
    while (k) d[n++] = t[--k];
    d[n] = 0;
    return n;
}
uint32_t ui_mix(uint32_t a, uint32_t b, int k)
{
    return ((((a >> 16 & 255) * (256 - k) + (b >> 16 & 255) * k) >> 8) << 16) |
           ((((a >> 8 & 255) * (256 - k) + (b >> 8 & 255) * k) >> 8) << 8) | (((a & 255) * (256 - k) + (b & 255) * k) >> 8);
}
int ui_sc(ui_app *a, int v) { return v <= 0 ? 0 : (int)(((int64_t)v * a->Z + 32768) >> 16); }
int ui_scale(ui_app *a, int v) { return ui_sc(a, v); }

/* ---- Drawing, clipped to a->cx0..cx1, cy0..cy1. */
void ui_fill(ui_app *a, int x0, int y0, int x1, int y1, uint32_t c)
{
    if (x0 < a->cx0) x0 = a->cx0;
    if (y0 < a->cy0) y0 = a->cy0;
    if (x1 > a->cx1) x1 = a->cx1;
    if (y1 > a->cy1) y1 = a->cy1;
    for (int y = y0; y < y1; y++) {
        uint32_t *d = a->px + (size_t)y * a->W;
        for (int x = x0; x < x1; x++) d[x] = c;
    }
}
static int isqrt(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }
static int inset(int y, int y0, int y1, int r)   /* how far row y of a box with corner radius r starts inside */
{
    if (r <= 0) return 0;
    int d = y < y0 + r ? y0 + r - y : y >= y1 - r ? y - (y1 - r) + 1 : 0;
    return d ? r - isqrt(r * r - (d - 1) * (d - 1)) : 0;
}
static int radius(ui_app *a, int r, int x0, int y0, int x1, int y1)
{
    if (r > a->th.radius) r = a->th.radius;
    if (r > (x1 - x0) / 2) r = (x1 - x0) / 2;
    if (r > (y1 - y0) / 2) r = (y1 - y0) / 2;
    return r;
}
void ui_rrect(ui_app *a, int x0, int y0, int x1, int y1, int r, uint32_t c)
{
    r = radius(a, r, x0, y0, x1, y1);
    int ya = y0 > a->cy0 ? y0 : a->cy0, yb = y1 < a->cy1 ? y1 : a->cy1;
    for (int y = ya; y < yb; y++) { int i = inset(y, y0, y1, r); ui_fill(a, x0 + i, y, x1 - i, y + 1, c); }
}
void ui_frame(ui_app *a, int x0, int y0, int x1, int y1, int r, int t, uint32_t c)
{
    r = radius(a, r, x0, y0, x1, y1);
    int ir = r > t ? r - t : 0, ya = y0 > a->cy0 ? y0 : a->cy0, yb = y1 < a->cy1 ? y1 : a->cy1;
    for (int y = ya; y < yb; y++) {
        int i = inset(y, y0, y1, r);
        if (y < y0 + t || y >= y1 - t) { ui_fill(a, x0 + i, y, x1 - i, y + 1, c); continue; }
        int j = inset(y, y0 + t, y1 - t, ir);
        ui_fill(a, x0 + i, y, x0 + t + j, y + 1, c);
        ui_fill(a, x1 - t - j, y, x1 - i, y + 1, c);
    }
}

int ui_tw(ui_app *a, int n) { return n * a->cw; }

static void blend(ui_app *a, int x, int y, int k, uint32_t c)
{
    if (x < a->cx0 || x >= a->cx1 || y < a->cy0 || y >= a->cy1 || k <= 0) return;
    uint32_t *d = a->px + (size_t)y * a->W + x;
    *d = k >= 256 ? c : ui_mix(*d, c, k);
}

/* Text at (x, y) (its cell's top left) up to x1. The 9 x 16 font when the
 * scale is 1.5 times n (100%, 200%...: each of its pixels n x n), else the
 * 6 x 11 one, each screen pixel as much ink as the glyph covers. */
int ui_text_draw(ui_app *a, int x, int y, const char *s, int n, uint32_t c, int x1)
{
    int ts = a->ts, k = ts % 98304 ? 0 : ts / 98304, cw = a->cw;
    if (x1 > a->cx1) x1 = a->cx1;
    for (int i = 0; i < n; i++, x += cw) {
        unsigned ch = (uint8_t)s[i];
        int g = ch >= 32 && ch < 127 ? (int)ch - 32 : '?' - 32;
        if (x >= x1) break;
        if (ch == ' ' || x + cw <= a->cx0) continue;
        if (k) {
            for (int r = 0; r < 16; r++)
                for (int col = 0; col < 9; col++) {
                    int v = ui_font15[g][r][col];
                    if (!v) continue;
                    v += v >> 7;
                    for (int dy = 0; dy < k; dy++)
                        for (int dx = 0; dx < k; dx++)
                            if (x + col * k + dx < x1) blend(a, x + col * k + dx, y + r * k + dy, v, c);
                }
            continue;
        }
        int64_t step = ((int64_t)1 << 32) / ts;         /* a screen pixel in font pixels (16.16) */
        int hh = (int)((int64_t)CH_UI * ts >> 16);
        for (int py = 0; py < hh; py++) {
            int64_t v0 = ((int64_t)py << 32) / ts, v1 = v0 + step;
            int r0 = (int)(v0 >> 16), r1 = (int)((v1 - 1) >> 16);
            for (int px = 0; px < cw && x + px < x1; px++) {
                int64_t u0 = ((int64_t)px << 32) / ts, u1 = u0 + step, cov = 0;
                int c0 = (int)(u0 >> 16), c1 = (int)((u1 - 1) >> 16);
                for (int j = r0; j <= r1; j++) {
                    if (j < 1 || j > 9) continue;
                    int64_t oy = (v1 < ((int64_t)(j + 1) << 16) ? v1 : (int64_t)(j + 1) << 16) - (v0 > ((int64_t)j << 16) ? v0 : (int64_t)j << 16);
                    for (int b = c0; b <= c1; b++) {
                        if (b >= 5 || !(ui_font[g][j - 1] & 0x10 >> b)) continue;
                        int64_t ox = (u1 < ((int64_t)(b + 1) << 16) ? u1 : (int64_t)(b + 1) << 16) - (u0 > ((int64_t)b << 16) ? u0 : (int64_t)b << 16);
                        cov += ox * oy >> 16;
                    }
                }
                if (cov) blend(a, x + px, y + py, (int)(cov * 256 / ((u1 - u0) * (v1 - v0) >> 16)), c);
            }
        }
    }
    return x;
}
void ui_ytext(ui_widget *w, const char *s, uint32_t c, int x, int x1)
{
    if (s) ui_text_draw(w->app, x, w->y + (w->h - w->app->chh) / 2, s, (int)strlen(s), c, x1);
}

/* Icons: 9 x 9, a row's bits from the left, scaled to the text's height. */
static const uint16_t icons[][9] = {
    { 0 },
    { 0x010, 0x020, 0x040, 0x080, 0x1ff, 0x080, 0x040, 0x020, 0x010 },   /* back */
    { 0x010, 0x008, 0x004, 0x002, 0x1ff, 0x002, 0x004, 0x008, 0x010 },   /* forward */
    { 0x07a, 0x086, 0x10e, 0x100, 0x100, 0x101, 0x082, 0x07c, 0x000 },   /* reload */
    { 0x101, 0x082, 0x044, 0x028, 0x010, 0x028, 0x044, 0x082, 0x101 },   /* stop */
    { 0x000, 0x1ff, 0x000, 0x000, 0x1ff, 0x000, 0x000, 0x1ff, 0x000 },   /* menu */
    { 0x010, 0x010, 0x010, 0x010, 0x1ff, 0x010, 0x010, 0x010, 0x010 },   /* plus */
    { 0x000, 0x000, 0x000, 0x000, 0x1ff, 0x000, 0x000, 0x000, 0x000 },   /* minus */
};
void ui_icon(ui_app *a, int icon, int cx, int cy, uint32_t c)
{
    if (icon <= 0 || icon >= (int)(sizeof icons / sizeof *icons)) return;
    int s = a->chh / 9 < 1 ? 1 : a->chh / 9, x0 = cx - 9 * s / 2, y0 = cy - 9 * s / 2;
    for (int r = 0; r < 9; r++)
        for (int b = 0; b < 9; b++)
            if (icons[icon][r] & 0x100 >> b) ui_fill(a, x0 + b * s, y0 + r * s, x0 + b * s + s, y0 + r * s + s, c);
}

/* ---- The theme: atlas's, or its colours when there is no desktop to ask. */
static void theme_load(ui_app *a)
{
    atlas_theme_t t;
    static const uint32_t frost[ATT_N] = { 0x34478f, 0xbfc6de, 0xb9c1d6, 0xffc93c, 0x1a2238, 0x4a5470, 0xd9e0f2, 0xfbfcff, 0x1a2238,
                                           0xc8322a, 0xbcd0f7, 0x3a4a8c, 0x2a1d00, 0xf3f5fa, 0xffffff, 0x2a3350, 0x8a5200 };   /* (draw.c's, as shown) */
    memset(&t, 0, sizeof t);
    if (ui_sys_call(ATLAS_WIN_THEME, (uint64_t)a->win, 0, 0, 0, 0, &t, sizeof t) < 0 || t.size < sizeof t) {
        t.look = UI_LOOK_FROST; t.zoom = (uint16_t)(a->zoom ? a->zoom : 100); t.radius = 12; t.text = 98304 * t.zoom / 100;
        memcpy(t.pal, frost, sizeof frost);
    }
    ui_theme *h = &a->th;
    h->look = t.look; h->zoom = t.zoom; h->radius = t.radius;
    h->bg = t.pal[ATT_TERM]; h->panel = t.pal[ATT_PANEL]; h->face = t.pal[ATT_BAR]; h->line = t.pal[ATT_LINE];
    h->accent = t.pal[ATT_ACC]; h->ink = t.pal[ATT_INK]; h->text = t.pal[ATT_TXT]; h->dim = t.pal[ATT_DIM];
    h->field = t.pal[ATT_FIELD]; h->sel = t.pal[ATT_SEL]; h->err = t.pal[ATT_ERR]; h->acctext = t.pal[ATT_ACCT];
    h->list = t.pal[ATT_TERM]; h->chosen = t.pal[ATT_BAR];
    a->zoom = t.zoom;
    a->Z = 49152 * a->zoom / 100;
    a->ts = t.text > 16384 ? t.text : 98304;
    a->cw = (int)((int64_t)6 * a->ts >> 16);
    a->chh = (int)((int64_t)CH_UI * a->ts >> 16);
}

/* ---- Damage. */
static void damage(ui_app *a, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > a->W) x1 = a->W;
    if (y1 > a->H) y1 = a->H;
    if (x0 >= x1 || y0 >= y1) return;
    for (int i = 0; i < a->ndmg; i++) {
        int *d = a->dmg[i];
        if (x0 <= d[2] && x1 >= d[0] && y0 <= d[3] && y1 >= d[1]) {   /* touching one: grow it */
            if (x0 < d[0]) d[0] = x0;
            if (y0 < d[1]) d[1] = y0;
            if (x1 > d[2]) d[2] = x1;
            if (y1 > d[3]) d[3] = y1;
            return;
        }
    }
    if (a->ndmg == NDMG) {                               /* too many: one around them all */
        for (int i = 1; i < a->ndmg; i++) {
            int *d = a->dmg[i];
            if (d[0] < a->dmg[0][0]) a->dmg[0][0] = d[0];
            if (d[1] < a->dmg[0][1]) a->dmg[0][1] = d[1];
            if (d[2] > a->dmg[0][2]) a->dmg[0][2] = d[2];
            if (d[3] > a->dmg[0][3]) a->dmg[0][3] = d[3];
        }
        a->ndmg = 1;
        damage(a, x0, y0, x1, y1);
        return;
    }
    int *d = a->dmg[a->ndmg++];
    d[0] = x0; d[1] = y0; d[2] = x1; d[3] = y1;
}
void ui_dirty(ui_widget *w) { if (w) damage(w->app, w->x, w->y, w->x + w->w, w->y + w->h); }

/* ---- The tree. */
ui_widget *ui_new(ui_app *app, ui_widget *parent, const ui_class *cls, const char *text)
{
    ui_widget *w = calloc(1, sizeof *w);
    if (!w) return 0;
    w->cls = cls; w->app = app ? app : parent->app;
    w->text = ui_strdup(text ? text : "");
    w->a = w->b = w->c = w->d = 0;
    if (parent) {
        w->parent = parent;
        if (parent->last) parent->last->next = w; else parent->first = w;
        parent->last = w;
    }
    w->app->layout = 1;
    return w;
}
ui_widget *ui_child(ui_widget *w, int i) { ui_widget *c = w->first; while (c && i--) c = c->next; return c; }
ui_app *ui_app_of(ui_widget *w) { return w->app; }
int ui_visible(ui_widget *w)
{
    for (; w; w = w->parent) if (w->flags & F_HIDDEN) return 0;
    return 1;
}
static int inside(ui_widget *w, ui_widget *top) { while (w && w != top) w = w->parent; return w != 0; }

static void forget(ui_app *a, ui_widget *w)            /* (a widget going away: nothing points at it) */
{
    if (inside(a->focus, w)) a->focus = 0;
    if (inside(a->hover, w)) a->hover = 0;
    if (inside(a->pressed, w)) a->pressed = 0;
    if (inside(a->click_w, w)) a->click_w = 0;
}
void ui_destroy(ui_widget *w)
{
    if (!w) return;
    ui_app *a = w->app;
    while (w->first) ui_destroy(w->first);
    forget(a, w);
    for (int i = 0; i < a->nover; i++) if (a->over[i] == w) ui_hide(w);
    if (w->parent) {
        ui_widget **p = &w->parent->first, *prev = 0;
        while (*p && *p != w) { prev = *p; p = &(*p)->next; }
        if (*p) *p = w->next;
        if (w->parent->last == w) w->parent->last = prev;
    }
    if (w->cls->free) w->cls->free(w);
    free(w->ext); free(w->text); free(w);
    a->layout = 1;
}

void ui_set_text(ui_widget *w, const char *t)
{
    if (!strcmp(w->text, t ? t : "")) return;
    free(w->text);
    w->text = ui_strdup(t);
    w->app->layout = 1;                                   /* (its size may change) */
}
const char *ui_text(ui_widget *w) { return w->text; }
int  ui_value(ui_widget *w) { return w->value; }
void ui_set_value(ui_widget *w, int v)
{
    if (w->value == v) return;
    w->value = v;
    if (w->cls->event) { ui_event e = { 0, 0, 0, 0, 0, 0 }; w->cls->event(w, &e); }   /* (type 0: "the value changed") */
    ui_dirty(w);
}
void ui_set_enabled(ui_widget *w, int on) { int f = on ? w->flags & ~F_OFF : w->flags | F_OFF; if (f != w->flags) { w->flags = f; ui_dirty(w); } }
void ui_set_visible(ui_widget *w, int on) { int f = on ? w->flags & ~F_HIDDEN : w->flags | F_HIDDEN; if (f != w->flags) { w->flags = f; w->app->layout = 1; } }
void ui_set_stretch(ui_widget *w, int s) { w->stretch = s; w->app->layout = 1; }
void ui_set_size(ui_widget *w, int width, int height) { w->uw = width; w->uh = height; w->app->layout = 1; }
void ui_on(ui_widget *w, ui_fn fn, void *data) { w->fn = fn; w->data = data; }
void ui_set_menu(ui_widget *w, ui_widget *menu) { w->menu = menu; }
void ui_activate(ui_widget *w) { if (w->fn) w->fn(w, w->data); }
void ui_bounds(ui_widget *w, int *x, int *y, int *width, int *height) { *x = w->x; *y = w->y; *width = w->w; *height = w->h; }

/* ---- Layout: what each wants (measure), then where each goes (arrange). */
void ui_measure(ui_widget *w)
{
    w->mw = w->mh = 0;
    if (w->cls->measure) w->cls->measure(w);
    else for (ui_widget *c = w->first; c; c = c->next) ui_measure(c);
    int uw = ui_sc(w->app, w->uw), uh = ui_sc(w->app, w->uh);
    if (w->mw < uw) w->mw = uw;
    if (w->mh < uh) w->mh = uh;
}
void ui_arrange(ui_widget *w, int x, int y, int ww, int hh)
{
    w->x = x; w->y = y; w->w = ww; w->h = hh;
    if (w->cls->arrange) w->cls->arrange(w);
}

/* A box: children in a row or a column; the room to spare goes to those
 * that stretch; across, each takes the whole box. */
void ui_box_measure(ui_widget *w)
{
    int gap = ui_sc(w->app, w->gap), pad = ui_sc(w->app, w->pad), n = 0, main = 0, cross = 0;
    for (ui_widget *c = w->first; c; c = c->next) {
        ui_measure(c);
        if (c->flags & F_HIDDEN) continue;
        int m = w->dir == UI_ROW ? c->mw : c->mh, k = w->dir == UI_ROW ? c->mh : c->mw;
        main += m + (n++ ? gap : 0);
        if (k > cross) cross = k;
    }
    if (w->dir == UI_ROW) { w->mw = main + 2 * pad; w->mh = cross + 2 * pad; }
    else { w->mw = cross + 2 * pad; w->mh = main + 2 * pad; }
}
void ui_box_arrange(ui_widget *w)
{
    int gap = ui_sc(w->app, w->gap), pad = ui_sc(w->app, w->pad), n = 0, used = 0, shares = 0;
    for (ui_widget *c = w->first; c; c = c->next) {
        if (c->flags & F_HIDDEN) continue;
        used += (w->dir == UI_ROW ? c->mw : c->mh) + (n++ ? gap : 0);
        shares += c->stretch;
    }
    int room = (w->dir == UI_ROW ? w->w : w->h) - 2 * pad, spare = room - used, at = pad, given = 0, s = 0;
    for (ui_widget *c = w->first; c; c = c->next) {
        if (c->flags & F_HIDDEN) continue;
        int m = w->dir == UI_ROW ? c->mw : c->mh;
        if (shares && c->stretch) {                       /* (the last share takes what rounding left) */
            s += c->stretch;
            int to = (int)((int64_t)spare * s / shares);
            m += to - given; given = to;
        } else if (!shares && spare < 0) m += spare * m / (used ? used : 1);   /* (too small: each gives up its part) */
        if (m < 0) m = 0;
        if (w->dir == UI_ROW) ui_arrange(c, w->x + at, w->y + pad, m, w->h - 2 * pad);
        else ui_arrange(c, w->x + pad, w->y + at, w->w - 2 * pad, m);
        at += m + gap;
    }
}
void ui_box_draw(ui_widget *w) { if (!w->parent || w->d) ui_fill(w->app, w->x, w->y, w->x + w->w, w->y + w->h, w->d ? w->app->th.panel : w->app->th.bg); }
const ui_class ui_box_class = { ui_box_measure, ui_box_arrange, ui_box_draw, 0, 0, 0 };

ui_widget *ui_box(ui_widget *parent, int dir, int gap, int pad)
{
    ui_widget *w = ui_new(0, parent, &ui_box_class, 0);
    if (w) { w->dir = dir; w->gap = gap; w->pad = pad; }
    return w;
}
ui_widget *ui_root(ui_app *a) { return a->root; }

void ui_relayout(ui_app *a)
{
    a->layout = 0;
    ui_measure(a->root);
    ui_arrange(a->root, 0, 0, a->W, a->H);
    for (int i = 0; i < a->nover; i++) {
        ui_widget *o = a->over[i];
        int x = o->ox, y = o->oy;
        ui_measure(o);
        if (o->mw > a->W) o->mw = a->W;
        if (o->mh > a->H) o->mh = a->H;
        if (x < 0) { x = (a->W - o->mw) / 2; y = (a->H - o->mh) / 3; }
        if (x + o->mw > a->W) x = a->W - o->mw;
        if (y + o->mh > a->H) y = a->H - o->mh;
        ui_arrange(o, x < 0 ? 0 : x, y < 0 ? 0 : y, o->mw, o->mh);
    }
    damage(a, 0, 0, a->W, a->H);
}

/* ---- Overlays: menus and dialogs in front of everything; the last one gets the input. */
void ui_show(ui_widget *w, int x, int y)
{
    ui_app *a = w->app;
    if (a->nover == NOVER) return;
    w->ox = x; w->oy = y;
    a->over[a->nover++] = w;
    w->flags &= ~F_HIDDEN;
    w->saved = a->focus;                                  /* (the focus to give back) */
    a->focus = 0;
    ui_relayout(a);
}
void ui_hide(ui_widget *w)
{
    ui_app *a = w->app;
    int i = 0;
    while (i < a->nover && a->over[i] != w) i++;
    if (i == a->nover) return;
    damage(a, w->x - ui_sc(a, 8), w->y - ui_sc(a, 8), w->x + w->w + ui_sc(a, 8), w->y + w->h + ui_sc(a, 8));
    memmove(a->over + i, a->over + i + 1, (size_t)(a->nover - i - 1) * sizeof *a->over);
    a->nover--;
    if (inside(a->focus, w) || !a->focus) a->focus = w->saved;
    if (inside(a->hover, w)) a->hover = 0;
    if (inside(a->pressed, w)) a->pressed = 0;
    w->saved = 0;
}

/* ---- Painting. */
void ui_draw_tree(ui_widget *w)
{
    ui_app *a = w->app;
    if ((w->flags & F_HIDDEN) || w->x >= a->cx1 || w->y >= a->cy1 || w->x + w->w <= a->cx0 || w->y + w->h <= a->cy0) return;
    if (w->cls->draw) w->cls->draw(w);
    int cx0 = a->cx0, cy0 = a->cy0, cx1 = a->cx1, cy1 = a->cy1;   /* children stay inside */
    if (w->x > a->cx0) a->cx0 = w->x;
    if (w->y > a->cy0) a->cy0 = w->y;
    if (w->x + w->w < a->cx1) a->cx1 = w->x + w->w;
    if (w->y + w->h < a->cy1) a->cy1 = w->y + w->h;
    if (a->cx0 < a->cx1 && a->cy0 < a->cy1) for (ui_widget *c = w->first; c; c = c->next) ui_draw_tree(c);
    a->cx0 = cx0; a->cy0 = cy0; a->cx1 = cx1; a->cy1 = cy1;
}
static void draw_over(ui_widget *o)
{
    ui_app *a = o->app;
    int s = ui_sc(a, 6);
    ui_fill(a, o->x + s, o->y + o->h, o->x + o->w + s, o->y + o->h + s, ui_mix(a->th.bg, 0, 160));   /* (a hard shadow, as atlas's) */
    ui_fill(a, o->x + o->w, o->y + s, o->x + o->w + s, o->y + o->h + s, ui_mix(a->th.bg, 0, 160));
    ui_draw_tree(o);
    a->cx0 = 0; a->cy0 = 0; a->cx1 = a->W; a->cy1 = a->H;
    ui_frame(a, o->x, o->y, o->x + o->w, o->y + o->h, 0, ui_sc(a, 2) ? ui_sc(a, 2) : 1, a->th.line);
}
void ui_flush(ui_app *a)
{
    while (a->ntrash) ui_destroy(a->trash[--a->ntrash]);
    if (!a->px) return;
    if (a->layout) ui_relayout(a);
    if (!a->ndmg) return;
    atlas_rect_t r[NDMG];
    int n = a->ndmg;
    for (int i = 0; i < n; i++) {
        int *d = a->dmg[i];
        r[i].x0 = d[0]; r[i].y0 = d[1]; r[i].x1 = d[2]; r[i].y1 = d[3];
        a->cx0 = d[0]; a->cy0 = d[1]; a->cx1 = d[2]; a->cy1 = d[3];
        ui_draw_tree(a->root);
        for (int k = 0; k < a->nover; k++) { a->cx0 = d[0]; a->cy0 = d[1]; a->cx1 = d[2]; a->cy1 = d[3]; draw_over(a->over[k]); }
    }
    a->ndmg = 0;
    ui_sys_call(ATLAS_WIN_DAMAGE, (uint64_t)a->win, 0, 0, r, (size_t)n * sizeof *r, 0, 0);
}

/* ---- The window. */
static void resize(ui_app *a, int w, int h)
{
    uint64_t id;
    size_t size = ((size_t)w * h * 4 + 4095) & ~(size_t)4095;
    uint32_t *px = w > 0 && h > 0 ? ui_sys_shm(size, &id) : 0;
    if (!px) return;
    ui_sys_call(ATLAS_WIN_BUFFER, (uint64_t)a->win, id, (uint64_t)w | (uint64_t)h << 16, 0, 0, 0, 0);
    ui_sys_shm_close(id);
    if (a->px) ui_sys_unmap(a->px, a->psize);
    a->px = px; a->W = w; a->H = h; a->psize = size;
    a->layout = 1;
}

ui_app *ui_app_new(const char *title)
{
    ui_app *a = calloc(1, sizeof *a);
    if (!a) return 0;
    a->win = ui_sys_call(ATLAS_WIN_OPEN, 0, 0, 0, title, strlen(title), 0, 0);
    if (a->win < 0) { free(a); return 0; }
    a->focused = 1;
    theme_load(a);
    a->root = ui_new(a, 0, &ui_box_class, 0);
    a->root->dir = UI_COL;
    return a;
}
void ui_quit(ui_app *a, int code) { a->quit = 1; a->code = code; }
void ui_set_title(ui_app *a, const char *t) { ui_sys_call(ATLAS_WIN_TITLE, (uint64_t)a->win, 0, 0, t, strlen(t), 0, 0); }
const ui_theme *ui_theme_of(ui_app *a) { return &a->th; }
void ui_on_theme(ui_app *a, ui_fn fn, void *data) { a->on_theme = fn; a->theme_data = data; }
void ui_on_close(ui_app *a, ui_fn fn, void *data) { a->on_close = fn; a->close_data = data; }
int ui_clip_get(ui_app *a, char *buf, int cap)
{
    long n = ui_sys_call(ATLAS_WIN_CLIP_GET, (uint64_t)a->win, 0, 0, 0, 0, buf, (size_t)cap);
    return n < 0 ? 0 : n > cap ? cap : (int)n;
}
void ui_clip_set(ui_app *a, const char *s, int n) { ui_sys_call(ATLAS_WIN_CLIP_SET, (uint64_t)a->win, 0, 0, s, (size_t)n, 0, 0); }

/* ---- Input. */
static ui_widget *hit(ui_widget *w, int x, int y)    /* the deepest visible widget under (x, y) */
{
    if ((w->flags & F_HIDDEN) || x < w->x || y < w->y || x >= w->x + w->w || y >= w->y + w->h) return 0;
    ui_widget *found = w;
    for (ui_widget *c = w->first; c; c = c->next) { ui_widget *h = hit(c, x, y); if (h) found = h; }
    return found;
}
int ui_hit(ui_widget *w, int x, int y) { return x >= w->x && y >= w->y && x < w->x + w->w && y < w->y + w->h; }

int ui_has_focus(ui_widget *w) { return w && w->app->focus == w; }
void ui_focus(ui_widget *w)
{
    ui_app *a = w ? w->app : 0;
    if (!w || a->focus == w) return;
    ui_widget *old = a->focus;
    a->focus = w;
    if (old) { ui_event e = { UI_EV_FOCUS, 0, 0, 0, 0, 0 }; if (old->cls->event) old->cls->event(old, &e); ui_dirty(old); }
    ui_event e = { UI_EV_FOCUS, 1, 0, 0, 0, 0 };
    if (w->cls->event) w->cls->event(w, &e);
    ui_dirty(w);
}

/* Tab and Shift+Tab: the next widget that takes keys, in the tree's order,
 * inside the front dialog if there is one. */
static void walk(ui_widget *w, ui_widget **list, int *n, int max)
{
    if (w->flags & (F_HIDDEN | F_OFF)) return;
    if (w->cls->focusable && *n < max) list[(*n)++] = w;
    for (ui_widget *c = w->first; c; c = c->next) walk(c, list, n, max);
}
static void tab(ui_app *a, int back)
{
    ui_widget *list[256];
    int n = 0, i = 0;
    walk(a->nover ? a->over[a->nover - 1] : a->root, list, &n, 256);
    if (!n) return;
    while (i < n && list[i] != a->focus) i++;
    i = i == n ? (back ? n - 1 : 0) : (i + (back ? n - 1 : 1)) % n;
    ui_focus(list[i]);
}

static int deliver(ui_widget *w, const ui_event *e)   /* to w, then its parents, until one takes it */
{
    for (; w; w = w->parent) if (!(w->flags & F_OFF) && w->cls->event && w->cls->event(w, e)) return 1;
    return 0;
}

static void key(ui_app *a, const ui_event *e)
{
    ui_widget *top = a->nover ? a->over[a->nover - 1] : 0;
    int k = e->key, edit = a->focus && a->focus->cls->focusable == 3 &&
        (k == UI_CTRL('a') || k == UI_CTRL('c') || k == UI_CTRL('v') || k == UI_CTRL('x'));   /* (a field's own) */
    if (!top && !edit && ui_menu_key(a, k)) return;
    if (top && top->is_menu && ui_menu_key(a, e->key)) return;   /* (a menu open: its keys) */
    if ((e->key == '\t' || e->key == UI_KEY_BTAB) && !(a->focus && a->focus->cls->focusable == 2)) { tab(a, e->key == UI_KEY_BTAB); return; }
    if (deliver(a->focus ? a->focus : top ? top : a->root, e)) return;
    if (top && !inside(a->focus, top)) deliver(top, e);
}

static void pointer(ui_app *a, const ui_event *e)
{
    ui_widget *top = a->nover ? a->over[a->nover - 1] : 0, *h;
    int press = e->buttons & ~a->buttons;
    if (a->pressed) h = a->pressed;
    else if (top) {
        h = hit(top, e->x, e->y);
        if (!h && top->is_menu && press) { ui_menus_close(a); a->buttons = e->buttons; return; }   /* (a click outside a menu closes it) */
        if (!h && top->is_menu) h = ui_menu_over(a, e->x, e->y);   /* (another menu of the bar under it) */
    } else h = hit(a->root, e->x, e->y);
    if (h != a->hover) {
        if (a->hover) { a->hover->flags &= ~F_HOVER; ui_dirty(a->hover); }
        a->hover = h;
        if (h) { h->flags |= F_HOVER; ui_dirty(h); }
    }
    if (press && h) {
        ui_widget *m = h;
        while (m && !m->menu) m = m->parent;
        if ((press & UI_MB_RIGHT) && m && !top) { ui_popup(m->menu, e->x, e->y); a->buttons = e->buttons; return; }
        a->pressed = h;
        ui_widget *f = h;
        while (f && !f->cls->focusable) f = f->parent;
        if (f && !(f->flags & F_OFF)) ui_focus(f);
    }
    if (h) deliver(h, e);
    a->buttons = e->buttons;
    if (!e->buttons) a->pressed = 0;
}

void ui_dispatch(ui_app *a, const ui_event *ev, int n)
{
    for (int i = 0; i < n; i++) {
        const ui_event *e = &ev[i];
        switch (e->type) {
        case UI_EV_SIZE:
            if (e->key && e->key != a->zoom) { a->zoom = e->key; theme_load(a); }
            if (e->x != a->W || e->y != a->H) resize(a, e->x, e->y);
            break;
        case UI_EV_KEY: key(a, e); break;
        case UI_EV_POINTER: pointer(a, e); break;
        case UI_EV_FOCUS:
            a->focused = e->key;
            if (a->focus) ui_dirty(a->focus);
            break;
        case UI_EV_CLOSE:
            a->quit = 1; a->win = -1;
            if (a->on_close) a->on_close(a->root, a->close_data);
            break;
        case UI_EV_THEME:
            theme_load(a);
            a->layout = 1;
            if (a->on_theme) a->on_theme(a->root, a->theme_data);
            break;
        }
    }
}

int ui_wait(ui_app *a, ui_event *ev, int max)
{
    atlas_event_t in[64];
    if (max > 64) max = 64;
    long n = a->win < 0 ? -1 : ui_sys_call(ATLAS_WIN_EVENT, (uint64_t)a->win, 0, 0, 0, 0, in, (size_t)max * sizeof *in);
    for (long i = 0; i < n; i++) {
        ev[i].type = in[i].type; ev[i].key = in[i].code; ev[i].x = in[i].x; ev[i].y = in[i].y;
        ev[i].buttons = in[i].buttons; ev[i].wheel = in[i].wheel;
    }
    return (int)n;
}

int ui_run(ui_app *a)
{
    ui_event ev[64];
    a->running = 1;
    while (!a->quit) {
        ui_flush(a);
        int n = ui_wait(a, ev, 64);
        if (n < 0) break;
        ui_dispatch(a, ev, n);
    }
    ui_app_close(a);
    return a->code;
}
int ui_done(ui_app *a) { return a->quit; }
void ui_app_close(ui_app *a)
{
    if (a->win >= 0) ui_sys_call(ATLAS_WIN_CLOSE, (uint64_t)a->win, 0, 0, 0, 0, 0, 0);
    a->win = -1;
}

void ui_trash(ui_widget *w)
{
    ui_app *a = w->app;
    ui_hide(w);
    if (a->ntrash < 8) a->trash[a->ntrash++] = w;
}
int ui_double(ui_widget *w)
{
    ui_app *a = w->app;
    long now = ui_sys_ms();
    int d = a->click_w == w && now - a->click_ms < 450;
    a->click_w = d ? 0 : w; a->click_ms = now;
    return d;
}
