/*
 * ui_wid.c - The UI toolkit's widgets (docs/ui.md): label, button, check
 * box, toggle, text field, progress bar, scroll bar, list, toolbar and its
 * tools, canvas, split, tabs. Each is a class (ui_int.h): how big it wants
 * to be, where its children go, how it draws, what it does with input.
 * They look like atlas's own: pills 42 high, fields 44 with a frame that
 * lights in the accent when they have the keys, the chosen row in the bar
 * colour.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "ui_int.h"

#define TH (&w->app->th)
#define A  (w->app)
static int len(const char *s) { return s ? (int)strlen(s) : 0; }
static int line_w(ui_app *a) { int t = ui_sc(a, 2); return t ? t : 1; }
static int clicked(ui_widget *w, const ui_event *e)    /* the left button released over it, pressed on it */
{
    return e->type == UI_EV_POINTER && !(e->buttons & UI_MB_LEFT) && (A->buttons & UI_MB_LEFT) && ui_hit(w, e->x, e->y);
}
static int pressed(ui_widget *w, const ui_event *e) { return e->type == UI_EV_POINTER && (e->buttons & ~A->buttons); }
static int focused(ui_widget *w) { return A->focus == w && A->focused; }

/* ---- Label: text, one line per '\n'. */
static void label_measure(ui_widget *w)
{
    int n = 1, l = 0, m = 0;
    for (const char *p = w->text; *p; p++) if (*p == '\n') { n++; l = 0; } else if (++l > m) m = l;
    w->mw = ui_tw(A, m); w->mh = n * (A->chh + ui_sc(A, 6)) + ui_sc(A, 4);
}
static void label_draw(ui_widget *w)
{
    int lh = A->chh + ui_sc(A, 6), n = 1;
    for (const char *q = w->text; *q; q++) n += *q == '\n';
    int y = w->y + (w->h - n * lh) / 2 + ui_sc(A, 3);   /* (centred in its height) */
    const char *p = w->text;
    uint32_t c = w->flags & F_OFF ? TH->dim : w->c ? TH->acctext : TH->text;
    for (;;) {
        const char *e = strchr(p, '\n');
        int n = e ? (int)(e - p) : len(p);
        ui_text_draw(A, w->x, y, p, n, c, w->x + w->w);
        if (w->c) ui_text_draw(A, w->x + 1, y, p, n, c, w->x + w->w);   /* (a title: bold) */
        if (!e) break;
        p = e + 1; y += lh;
    }
}
static const ui_class label_class = { label_measure, 0, label_draw, 0, 0, 0 };
ui_widget *ui_label(ui_widget *parent, const char *text) { return ui_new(0, parent, &label_class, text); }

/* ---- Button, toggle (c 1: stays in, value 1), tool (a toolbar's: flat, an icon). */
static void button_measure(ui_widget *w)
{
    if (w->d) { w->mh = ui_sc(A, 40); w->mw = w->b ? ui_sc(A, 40) : ui_tw(A, len(w->text)) + ui_sc(A, 24); return; }   /* (a tool) */
    w->mw = ui_tw(A, len(w->text)) + ui_sc(A, 28); w->mh = ui_sc(A, 42);
}
static void button_draw(ui_widget *w)
{
    int on = (w->flags & F_DOWN) || (w->c && w->value), off = w->flags & F_OFF, flat = w->d;
    int r = ui_sc(A, flat ? 8 : 21);
    uint32_t bg = on ? TH->accent : (w->flags & F_HOVER) && !off ? ui_mix(TH->face, TH->accent, 48) : flat ? 0 : TH->face;
    if (bg || !flat) ui_rrect(A, w->x, w->y, w->x + w->w, w->y + w->h, r, bg ? bg : TH->face);
    if (!flat || focused(w)) ui_frame(A, w->x, w->y, w->x + w->w, w->y + w->h, r, focused(w) ? 2 * line_w(A) : 1, focused(w) || on ? TH->accent : TH->line);
    uint32_t fg = off ? TH->dim : on ? TH->ink : TH->text;
    if (w->b) ui_icon(A, w->b, w->x + w->w / 2, w->y + w->h / 2, fg);
    else ui_ytext(w, w->text, fg, w->x + (w->w - ui_tw(A, len(w->text))) / 2, w->x + w->w);
}
static int button_event(ui_widget *w, const ui_event *e)
{
    if (e->type == UI_EV_POINTER) {
        if (pressed(w, e) && (e->buttons & UI_MB_LEFT)) { w->flags |= F_DOWN; ui_dirty(w); }
        else if (!(e->buttons & UI_MB_LEFT) && (w->flags & F_DOWN)) {
            w->flags &= ~F_DOWN; ui_dirty(w);
            if (clicked(w, e)) { if (w->c) w->value ^= 1; ui_activate(w); }
        }
        return 1;
    }
    if (e->type == UI_EV_KEY && (e->key == ' ' || e->key == '\n')) { if (w->c) w->value ^= 1; ui_dirty(w); ui_activate(w); return 1; }
    return 0;
}
static const ui_class button_class = { button_measure, 0, button_draw, button_event, 0, 1 };
ui_widget *ui_button(ui_widget *parent, const char *text, ui_fn fn, void *data)
{
    ui_widget *w = ui_new(0, parent, &button_class, text);
    if (w) ui_on(w, fn, data);
    return w;
}
ui_widget *ui_toggle(ui_widget *parent, const char *text, int on, ui_fn fn, void *data)
{
    ui_widget *w = ui_button(parent, text, fn, data);
    if (w) { w->c = 1; w->value = on; }
    return w;
}

/* ---- Check box: a square, ticked (value 1), and its text. */
static void check_measure(ui_widget *w) { w->mw = ui_sc(A, 34) + ui_tw(A, len(w->text)); w->mh = ui_sc(A, 42); }
static void check_draw(ui_widget *w)
{
    int s = ui_sc(A, 24), x = w->x, y = w->y + (w->h - s) / 2;
    ui_rrect(A, x, y, x + s, y + s, ui_sc(A, 5), TH->field);
    ui_frame(A, x, y, x + s, y + s, ui_sc(A, 5), focused(w) ? 2 * line_w(A) : 1, focused(w) ? TH->accent : TH->line);
    if (w->value) { int k = s / 4; ui_rrect(A, x + k, y + k, x + s - k, y + s - k, ui_sc(A, 2), TH->accent); }
    ui_ytext(w, w->text, w->flags & F_OFF ? TH->dim : TH->text, x + ui_sc(A, 34), w->x + w->w);
}
static int check_event(ui_widget *w, const ui_event *e)
{
    if (clicked(w, e) || (e->type == UI_EV_KEY && e->key == ' ')) { w->value ^= 1; ui_dirty(w); ui_activate(w); return 1; }
    return e->type == UI_EV_POINTER;
}
static const ui_class check_class = { check_measure, 0, check_draw, check_event, 0, 1 };
ui_widget *ui_check(ui_widget *parent, const char *text, int on, ui_fn fn, void *data)
{
    ui_widget *w = ui_new(0, parent, &check_class, text);
    if (w) { w->value = on; ui_on(w, fn, data); }
    return w;
}

/* ---- Text field: one line; a = the cursor, b = the selection's other end, c = the first character shown. */
static void field_measure(ui_widget *w) { w->mw = ui_tw(A, 16) + ui_sc(A, 28); w->mh = ui_sc(A, 44); }
static int field_at(ui_widget *w, int x)                /* the character boundary nearest x */
{
    int k = (x - w->x - ui_sc(A, 14) + A->cw / 2) / A->cw + w->c, n = len(w->text);
    return k < 0 ? 0 : k > n ? n : k;
}
static void field_show(ui_widget *w)                    /* scroll so the cursor shows */
{
    int fit = (w->w - ui_sc(A, 28)) / (A->cw ? A->cw : 1);
    if (fit < 1) fit = 1;
    if (w->a < w->c) w->c = w->a;
    if (w->a > w->c + fit) w->c = w->a - fit;
    if (w->c < 0) w->c = 0;
}
static void field_draw(ui_widget *w)
{
    int r = ui_sc(A, 10), px = w->x + ui_sc(A, 14), f = focused(w) || (A->focus == w && w->d);
    int ty = w->y + (w->h - A->chh) / 2, n = len(w->text), s0 = w->a < w->b ? w->a : w->b, s1 = w->a < w->b ? w->b : w->a;
    ui_rrect(A, w->x, w->y, w->x + w->w, w->y + w->h, r, TH->field);
    ui_frame(A, w->x, w->y, w->x + w->w, w->y + w->h, r, f ? 2 * line_w(A) : 1, f ? TH->accent : TH->line);
    int cx0 = A->cx0, cx1 = A->cx1;                      /* (the text stays inside the frame) */
    if (A->cx0 < w->x + ui_sc(A, 8)) A->cx0 = w->x + ui_sc(A, 8);
    if (A->cx1 > w->x + w->w - ui_sc(A, 8)) A->cx1 = w->x + w->w - ui_sc(A, 8);
    if (s1 > s0 && A->focus == w) {
        int a = px + (s0 - w->c) * A->cw, b = px + (s1 - w->c) * A->cw;
        ui_fill(A, a, ty - ui_sc(A, 2), b, ty + A->chh + ui_sc(A, 2), TH->sel);
    }
    if (n) ui_text_draw(A, px, ty, w->text + w->c, n - w->c, w->flags & F_OFF ? TH->dim : TH->text, w->x + w->w);
    if (focused(w)) { int x = px + (w->a - w->c) * A->cw; ui_fill(A, x - 1, ty - ui_sc(A, 2), x - 1 + 2 * line_w(A), ty + A->chh + ui_sc(A, 2), TH->accent); }
    A->cx0 = cx0; A->cx1 = cx1;
}
static void field_replace(ui_widget *w, const char *ins, int k)   /* the selection (or nothing at the cursor) becomes ins */
{
    int n = len(w->text), s0 = w->a < w->b ? w->a : w->b, s1 = w->a < w->b ? w->b : w->a;
    char *t = malloc((size_t)(n - (s1 - s0) + k + 1));
    if (!t) return;
    memcpy(t, w->text, (size_t)s0);
    memcpy(t + s0, ins, (size_t)k);
    memcpy(t + s0 + k, w->text + s1, (size_t)(n - s1 + 1));
    free(w->text);
    w->text = t;
    w->a = w->b = s0 + k;
}
static int field_event(ui_widget *w, const ui_event *e)
{
    int n = len(w->text), sel = w->a != w->b;
    if (e->type == 0) { w->a = w->b = n; w->c = 0; field_show(w); return 0; }   /* (ui_set_value: nothing) */
    if (e->type == UI_EV_POINTER) {
        if (pressed(w, e) && (e->buttons & UI_MB_LEFT)) {
            if (ui_double(w)) { w->b = 0; w->a = n; }
            else w->a = w->b = field_at(w, e->x);
        } else if ((e->buttons & UI_MB_LEFT) && A->pressed == w) w->a = field_at(w, e->x);
        else return 1;
        field_show(w); ui_dirty(w);
        return 1;
    }
    if (e->type != UI_EV_KEY) return 0;
    int k = e->key;
    char buf[1024];
    if (k == '\n') { ui_activate(w); return 1; }
    if ((k >= 32 && k < 127) || (k >= 128 && k < 256)) { char c = (char)k; field_replace(w, &c, 1); }
    else if (k == '\b') { if (!sel && w->a) w->b = w->a - 1; field_replace(w, "", 0); }
    else if (k == UI_KEY_DEL) { if (!sel && w->a < n) w->b = w->a + 1; field_replace(w, "", 0); }
    else if (k == UI_KEY_LEFT) w->a = w->b = sel ? (w->a < w->b ? w->a : w->b) : w->a > 0 ? w->a - 1 : 0;
    else if (k == UI_KEY_RIGHT) w->a = w->b = sel ? (w->a > w->b ? w->a : w->b) : w->a < n ? w->a + 1 : n;
    else if (k == UI_KEY_HOME) w->a = w->b = 0;
    else if (k == UI_KEY_END) w->a = w->b = n;
    else if (k == UI_CTRL('a')) { w->b = 0; w->a = n; }
    else if ((k == UI_CTRL('c') || k == UI_CTRL('x')) && sel) {
        int s0 = w->a < w->b ? w->a : w->b, s1 = w->a < w->b ? w->b : w->a;
        ui_clip_set(A, w->text + s0, s1 - s0);
        if (k == UI_CTRL('x')) field_replace(w, "", 0);
    } else if (k == UI_CTRL('v')) {
        int m = ui_clip_get(A, buf, sizeof buf), j = 0;
        for (int i = 0; i < m; i++) if (buf[i] != '\n' && buf[i] != '\r') buf[j++] = buf[i];   /* (one line) */
        field_replace(w, buf, j);
    } else return 0;
    field_show(w); ui_dirty(w);
    return 1;
}
static const ui_class field_class = { field_measure, 0, field_draw, field_event, 0, 3 };
ui_widget *ui_field(ui_widget *parent, const char *text, ui_fn fn, void *data)
{
    ui_widget *w = ui_new(0, parent, &field_class, text);
    if (w) { ui_on(w, fn, data); w->a = w->b = len(w->text); }
    return w;
}
void ui_field_select(ui_widget *w, int from, int to)
{
    int n = len(w->text);
    w->b = from < 0 || from > n ? n : from; w->a = to < 0 || to > n ? n : to;
    field_show(w); ui_dirty(w);
}

/* ---- Progress bar: value 0..1000, or -1 (busy: stripes). */
static void progress_measure(ui_widget *w) { w->mw = ui_sc(A, 160); w->mh = ui_sc(A, 16); }
static void progress_draw(ui_widget *w)
{
    int r = w->h / 2, x1 = w->x + (int)((int64_t)w->w * (w->value < 0 ? 0 : w->value > 1000 ? 1000 : w->value) / 1000);
    ui_rrect(A, w->x, w->y, w->x + w->w, w->y + w->h, r, TH->field);
    if (w->value < 0)
        for (int x = w->x + r; x < w->x + w->w - r; x += 2 * w->h) ui_fill(A, x, w->y + 2, x + w->h, w->y + w->h - 2, TH->accent);
    else if (x1 > w->x) ui_rrect(A, w->x, w->y, x1 < w->x + 2 * r ? w->x + 2 * r : x1, w->y + w->h, r, TH->accent);
    ui_frame(A, w->x, w->y, w->x + w->w, w->y + w->h, r, 1, TH->line);
}
static const ui_class progress_class = { progress_measure, 0, progress_draw, 0, 0, 0 };
ui_widget *ui_progress(ui_widget *parent) { return ui_new(0, parent, &progress_class, 0); }

/* ---- Scroll bars: drawn and moved for a scroll bar widget and for lists. */
static void thumb(int total, int page, int pos, int len, int min, int *t0, int *t1)
{
    if (total <= page || total <= 0) { *t0 = 0; *t1 = len; return; }
    int s = (int)((int64_t)len * page / total);
    if (s < min) s = min;
    if (s > len) s = len;
    *t0 = (int)((int64_t)(len - s) * pos / (total - page));
    *t1 = *t0 + s;
}
void ui_scroll_draw(ui_widget *w, int pos, int total, int page, int x0, int y0, int x1, int y1, int vertical)
{
    int t0, t1, l = vertical ? y1 - y0 : x1 - x0, k = ui_sc(A, 3);
    thumb(total, page, pos, l, ui_sc(A, 30), &t0, &t1);
    ui_fill(A, x0, y0, x1, y1, TH->panel);
    uint32_t c = A->pressed == w || (w->flags & F_HOVER) ? TH->accent : TH->line;
    if (vertical) ui_rrect(A, x0 + k, y0 + t0 + k, x1 - k, y0 + t1 - k, (x1 - x0) / 2, c);
    else ui_rrect(A, x0 + t0 + k, y0 + k, x0 + t1 - k, y1 - k, (y1 - y0) / 2, c);
}
/* -> 1 if pos changed. grab: where in the thumb it was taken (-1: not held). */
static int scroll_event(ui_widget *w, const ui_event *e, int *pos, int total, int page, int x0, int y0, int x1, int y1, int vertical, int *grab)
{
    int t0, t1, l = vertical ? y1 - y0 : x1 - x0, at = vertical ? e->y - y0 : e->x - x0, old = *pos, max = total - page;
    if (max <= 0) return 0;
    thumb(total, page, *pos, l, ui_sc(A, 30), &t0, &t1);
    if (e->type != UI_EV_POINTER) return 0;
    if (pressed(w, e) && e->x >= x0 && e->x < x1 && e->y >= y0 && e->y < y1) {
        if (at >= t0 && at < t1) *grab = at - t0;
        else *pos += at < t0 ? -page : page;
    } else if ((e->buttons & UI_MB_LEFT) && *grab >= 0 && l > t1 - t0)
        *pos = (int)((int64_t)(at - *grab) * max / (l - (t1 - t0)));
    if (!(e->buttons & UI_MB_LEFT)) *grab = -1;
    if (*pos < 0) *pos = 0;
    if (*pos > max) *pos = max;
    return *pos != old;
}
int ui_scroll_event(ui_widget *w, const ui_event *e, int *pos, int total, int page, int x0, int y0, int x1, int y1, int vertical)
{
    return scroll_event(w, e, pos, total, page, x0, y0, x1, y1, vertical, &w->d);
}
/* the widget: value = the position, b = the total, c = a page; d = the grab */
static void sbar_measure(ui_widget *w) { int t = ui_sc(A, 16); w->mw = w->dir == UI_COL ? t : ui_sc(A, 60); w->mh = w->dir == UI_COL ? ui_sc(A, 60) : t; }
static void sbar_draw(ui_widget *w) { ui_scroll_draw(w, w->value, w->b, w->c, w->x, w->y, w->x + w->w, w->y + w->h, w->dir == UI_COL); }
static int sbar_event(ui_widget *w, const ui_event *e)
{
    int max = w->b - w->c;
    if (e->type == UI_EV_POINTER && e->wheel) w->value += e->wheel * (w->c / 8 > 1 ? w->c / 8 : 1);
    else if (!ui_scroll_event(w, e, &w->value, w->b, w->c, w->x, w->y, w->x + w->w, w->y + w->h, w->dir == UI_COL)) return e->type == UI_EV_POINTER;
    if (w->value > max) w->value = max;
    if (w->value < 0) w->value = 0;
    ui_dirty(w); ui_activate(w);
    return 1;
}
static const ui_class sbar_class = { sbar_measure, 0, sbar_draw, sbar_event, 0, 0 };
ui_widget *ui_scrollbar(ui_widget *parent, int dir, ui_fn fn, void *data)
{
    ui_widget *w = ui_new(0, parent, &sbar_class, 0);
    if (w) { w->dir = dir; w->d = -1; w->b = 100; w->c = 10; ui_on(w, fn, data); }
    return w;
}
void ui_scroll_range(ui_widget *w, int total, int page) { w->b = total; w->c = page; if (w->value > total - page) w->value = total - page > 0 ? total - page : 0; ui_dirty(w); }

/* ---- List: rows from the program (ui_list_model), columns, a header, a
 * scroll bar; value = the chosen row; fn: Enter or a double click. */
typedef struct { int ncol, rows, top, grab, widths[8]; char *titles[8]; ui_cell_fn cell; void *cdata; ui_fn sel; void *sdata; } list_t;
static int row_h(ui_app *a) { return a->chh + ui_sc(a, 12); }
static int list_head(ui_widget *w) { list_t *l = w->ext; return l->titles[0] ? row_h(A) : 0; }
static int list_page(ui_widget *w) { int p = (w->h - list_head(w)) / row_h(A); return p < 1 ? 1 : p; }
static void list_show(ui_widget *w)
{
    list_t *l = w->ext;
    int p = list_page(w);
    if (w->value >= 0 && w->value < l->top) l->top = w->value;
    if (w->value >= l->top + p) l->top = w->value - p + 1;
    if (l->top > l->rows - p) l->top = l->rows - p;
    if (l->top < 0) l->top = 0;
}
static void list_measure(ui_widget *w)
{
    list_t *l = w->ext;
    int cw = 0;
    for (int i = 0; i < l->ncol; i++) cw += l->widths[i] ? l->widths[i] : 12;
    w->mw = ui_tw(A, cw) + ui_sc(A, 40); w->mh = list_head(w) + 4 * row_h(A);
}
static void list_draw(ui_widget *w)
{
    list_t *l = w->ext;
    int rh = row_h(A), hh = list_head(w), sb = l->rows > list_page(w) ? ui_sc(A, 16) : 0, x1 = w->x + w->w - sb, fixed = 0, eq = 0;
    for (int i = 0; i < l->ncol; i++) { if (l->widths[i]) fixed += ui_tw(A, l->widths[i]); else eq++; }
    int share = eq ? (w->w - sb - fixed - ui_sc(A, 12)) / eq : 0, xs[9];
    xs[0] = w->x + ui_sc(A, 12);
    for (int i = 0; i < l->ncol; i++) xs[i + 1] = xs[i] + (l->widths[i] ? ui_tw(A, l->widths[i]) : share);
    ui_fill(A, w->x, w->y, w->x + w->w, w->y + w->h, TH->list);
    if (hh) {
        ui_fill(A, w->x, w->y, x1, w->y + hh, TH->panel);
        for (int i = 0; i < l->ncol; i++) ui_text_draw(A, xs[i], w->y + (hh - A->chh) / 2, l->titles[i], len(l->titles[i]), TH->dim, xs[i + 1] - A->cw);
        ui_fill(A, w->x, w->y + hh - 1, x1, w->y + hh, TH->line);
    }
    for (int r = l->top, y = w->y + hh; r < l->rows && y < w->y + w->h; r++, y += rh) {
        if (r == w->value) {
            ui_fill(A, w->x, y, x1, y + rh, TH->chosen);
            if (A->focus == w) ui_fill(A, w->x, y, w->x + ui_sc(A, 4), y + rh, TH->accent);
        }
        for (int i = 0; i < l->ncol; i++) {
            const char *t = l->cell ? l->cell(l->cdata, r, i) : 0;
            if (t) ui_text_draw(A, xs[i], y + (rh - A->chh) / 2, t, len(t), TH->text, (i + 1 < l->ncol ? xs[i + 1] - A->cw : x1));
        }
    }
    if (sb) ui_scroll_draw(w, l->top, l->rows, list_page(w), x1, w->y + hh, w->x + w->w, w->y + w->h, 1);
    ui_frame(A, w->x, w->y, w->x + w->w, w->y + w->h, 0, 1, A->focus == w ? TH->accent : TH->line);
}
static void list_choose(ui_widget *w, int r)
{
    list_t *l = w->ext;
    if (r >= l->rows) r = l->rows - 1;
    if (r < 0) r = l->rows ? 0 : -1;
    if (r == w->value) return;
    w->value = r;
    list_show(w); ui_dirty(w);
    if (l->sel) l->sel(w, l->sdata);
}
static int list_event(ui_widget *w, const ui_event *e)
{
    list_t *l = w->ext;
    int p = list_page(w), hh = list_head(w), sb = l->rows > p ? ui_sc(A, 16) : 0;
    if (e->type == 0) { list_show(w); return 0; }
    if (e->type == UI_EV_POINTER) {
        if (e->wheel) { l->top += e->wheel * 3; list_show(w); ui_dirty(w); return 1; }
        if (sb && (e->x >= w->x + w->w - sb || l->grab >= 0)) {
            if (scroll_event(w, e, &l->top, l->rows, p, w->x + w->w - sb, w->y + hh, w->x + w->w, w->y + w->h, 1, &l->grab)) ui_dirty(w);
            return 1;
        }
        if (pressed(w, e) && (e->buttons & UI_MB_LEFT) && e->y >= w->y + hh) {
            int r = l->top + (e->y - w->y - hh) / row_h(A), dbl = ui_double(w);
            if (r < l->rows) { list_choose(w, r); if (dbl) ui_activate(w); }
        }
        return 1;
    }
    if (e->type != UI_EV_KEY) return 0;
    switch (e->key) {
    case UI_KEY_UP: list_choose(w, w->value - 1); break;
    case UI_KEY_DOWN: list_choose(w, w->value + 1); break;
    case UI_KEY_PGUP: list_choose(w, w->value - p); break;
    case UI_KEY_PGDN: list_choose(w, w->value + p); break;
    case UI_KEY_HOME: list_choose(w, 0); break;
    case UI_KEY_END: list_choose(w, l->rows - 1); break;
    case '\n': if (w->value >= 0) ui_activate(w); break;
    default: return 0;
    }
    return 1;
}
static void list_free(ui_widget *w) { list_t *l = w->ext; for (int i = 0; i < 8; i++) free(l->titles[i]); }
static const ui_class list_class = { list_measure, 0, list_draw, list_event, list_free, 1 };
ui_widget *ui_list(ui_widget *parent, int ncol, const char *const *titles, ui_fn fn, void *data)
{
    ui_widget *w = ui_new(0, parent, &list_class, 0);
    list_t *l = w ? calloc(1, sizeof *l) : 0;
    if (!l) return w;
    w->ext = l;
    l->ncol = ncol < 1 ? 1 : ncol > 8 ? 8 : ncol;
    l->grab = -1;
    for (int i = 0; titles && i < l->ncol; i++) l->titles[i] = ui_strdup(titles[i]);
    w->value = -1;
    ui_on(w, fn, data);
    return w;
}
void ui_list_model(ui_widget *w, int rows, ui_cell_fn cell, void *data)
{
    list_t *l = w->ext;
    l->rows = rows; l->cell = cell; l->cdata = data;
    if (w->value >= rows) w->value = rows - 1;
    list_show(w); ui_dirty(w);
}
void ui_list_widths(ui_widget *w, const int *widths) { list_t *l = w->ext; for (int i = 0; i < l->ncol; i++) l->widths[i] = widths[i]; w->app->layout = 1; }
void ui_list_on_select(ui_widget *w, ui_fn fn, void *data) { list_t *l = w->ext; l->sel = fn; l->sdata = data; }

/* ---- Toolbar: a row on the bars' colour, a line under it; its tools are flat buttons. */
static void toolbar_draw(ui_widget *w)
{
    ui_fill(A, w->x, w->y, w->x + w->w, w->y + w->h, TH->panel);
    ui_fill(A, w->x, w->y + w->h - 1, w->x + w->w, w->y + w->h, TH->line);
}
static const ui_class toolbar_class = { ui_box_measure, ui_box_arrange, toolbar_draw, 0, 0, 0 };
ui_widget *ui_toolbar(ui_widget *parent)
{
    ui_widget *w = ui_new(0, parent, &toolbar_class, 0);
    if (w) { w->dir = UI_ROW; w->gap = 6; w->pad = 6; }
    return w;
}
ui_widget *ui_tool(ui_widget *toolbar, int icon, const char *text, ui_fn fn, void *data)
{
    ui_widget *w = ui_button(toolbar, text, fn, data);
    if (w) { w->b = icon; w->d = 1; }
    return w;
}

void ui_set_icon(ui_widget *w, int icon) { if (w->b != icon) { w->b = icon; ui_dirty(w); } }

/* ---- Canvas: the program's own pixels (ext), shown exactly; a = width, b = height. */
static void canvas_arrange(ui_widget *w)
{
    if (w->w == w->a && w->h == w->b && w->ext) return;
    free(w->ext);
    w->ext = w->w > 0 && w->h > 0 ? malloc((size_t)w->w * w->h * 4) : 0;
    w->a = w->ext ? w->w : 0; w->b = w->ext ? w->h : 0;
    if (w->ext) memset(w->ext, 0xff, (size_t)w->a * w->b * 4);
    ui_event e = { UI_EV_SIZE, 0, w->a, w->b, 0, 0 };
    if (w->fn) ((ui_event_fn)(void (*)(void))w->fn)(w, &e, w->data);
}
static void canvas_draw(ui_widget *w)
{
    uint32_t *px = w->ext;
    int x0 = w->x > A->cx0 ? w->x : A->cx0, x1 = w->x + w->a < A->cx1 ? w->x + w->a : A->cx1;
    int y0 = w->y > A->cy0 ? w->y : A->cy0, y1 = w->y + w->b < A->cy1 ? w->y + w->b : A->cy1;
    if (!px || x0 >= x1) return;
    for (int y = y0; y < y1; y++)
        memcpy(A->px + (size_t)y * A->W + x0, px + (size_t)(y - w->y) * w->a + (x0 - w->x), (size_t)(x1 - x0) * 4);
}
static int canvas_event(ui_widget *w, const ui_event *e)
{
    if (!w->fn || e->type == 0) return 0;
    ui_event c = *e;
    if (e->type == UI_EV_POINTER) { c.x -= w->x; c.y -= w->y; }
    ((ui_event_fn)(void (*)(void))w->fn)(w, &c, w->data);
    return e->type == UI_EV_POINTER || e->type == UI_EV_KEY || e->type == UI_EV_FOCUS;
}
static const ui_class canvas_class = { 0, canvas_arrange, canvas_draw, canvas_event, 0, 2 };
ui_widget *ui_canvas(ui_widget *parent, ui_event_fn fn, void *data)
{
    ui_widget *w = ui_new(0, parent, &canvas_class, 0);
    if (w) { w->fn = (ui_fn)(void (*)(void))fn; w->data = data; w->stretch = 1; }
    return w;
}
uint32_t *ui_canvas_pixels(ui_widget *w, int *width, int *height) { *width = w->a; *height = w->b; return w->ext; }
void ui_canvas_damage(ui_widget *w, int x, int y, int width, int height)
{
    int x0 = w->x + (x < 0 ? 0 : x), y0 = w->y + (y < 0 ? 0 : y), x1 = w->x + x + width, y1 = w->y + y + height;
    if (x1 > w->x + w->w) x1 = w->x + w->w;
    if (y1 > w->y + w->h) y1 = w->y + w->h;
    ui_widget tmp = *w;                                   /* (damage the box only: ui_dirty takes a widget) */
    tmp.x = x0; tmp.y = y0; tmp.w = x1 - x0; tmp.h = y1 - y0;
    if (tmp.w > 0 && tmp.h > 0) ui_dirty(&tmp);
}

/* ---- Split: two children and a divider; value = the first one's size (window pixels), c = asked (picture). */
static int split_bar(ui_widget *w) { return ui_sc(A, 8); }
static void split_measure(ui_widget *w)
{
    ui_box_measure(w);
    if (w->dir == UI_ROW) w->mw += split_bar(w); else w->mh += split_bar(w);
}
static void split_arrange(ui_widget *w)
{
    ui_widget *a = w->first, *b = a ? a->next : 0;
    int len = w->dir == UI_ROW ? w->w : w->h, d = split_bar(w);
    if (w->value < 0) w->value = ui_sc(A, w->c);
    if (w->value > len - d) w->value = len - d;
    if (w->value < 0) w->value = 0;
    if (w->dir == UI_ROW) {
        if (a) ui_arrange(a, w->x, w->y, w->value, w->h);
        if (b) ui_arrange(b, w->x + w->value + d, w->y, w->w - w->value - d, w->h);
    } else {
        if (a) ui_arrange(a, w->x, w->y, w->w, w->value);
        if (b) ui_arrange(b, w->x, w->y + w->value + d, w->w, w->h - w->value - d);
    }
}
static void split_draw(ui_widget *w)
{
    int d = split_bar(w), t = line_w(A);
    if (w->dir == UI_ROW) { ui_fill(A, w->x + w->value, w->y, w->x + w->value + d, w->y + w->h, TH->bg); ui_fill(A, w->x + w->value + d / 2 - t / 2, w->y, w->x + w->value + d / 2 - t / 2 + t, w->y + w->h, w->b ? TH->accent : TH->line); }
    else { ui_fill(A, w->x, w->y + w->value, w->x + w->w, w->y + w->value + d, TH->bg); ui_fill(A, w->x, w->y + w->value + d / 2 - t / 2, w->x + w->w, w->y + w->value + d / 2 - t / 2 + t, w->b ? TH->accent : TH->line); }
}
static int split_event(ui_widget *w, const ui_event *e)
{
    if (e->type != UI_EV_POINTER) return 0;
    int at = w->dir == UI_ROW ? e->x - w->x : e->y - w->y;
    if (pressed(w, e) && at >= w->value && at < w->value + split_bar(w)) { w->b = 1; ui_dirty(w); return 1; }
    if (!w->b) return 0;
    if (e->buttons & UI_MB_LEFT) { w->value = at - split_bar(w) / 2; A->layout = 1; ui_activate(w); }
    else { w->b = 0; ui_dirty(w); }
    return 1;
}
static const ui_class split_class = { split_measure, split_arrange, split_draw, split_event, 0, 0 };
ui_widget *ui_split(ui_widget *parent, int dir, int pos)
{
    ui_widget *w = ui_new(0, parent, &split_class, 0);
    if (w) { w->dir = dir; w->c = pos; w->value = -1; w->stretch = 1; }
    return w;
}

/* ---- Tabs: a row of titles; its children are the pages (value: the one shown). */
static int tabs_h(ui_widget *w) { return A->chh + ui_sc(A, 22); }
static void tabs_measure(ui_widget *w)
{
    int tw = 0;
    for (ui_widget *c = w->first; c; c = c->next) {
        ui_measure(c);
        tw += ui_tw(A, len(c->text)) + ui_sc(A, 32);
        if (c->mw > w->mw) w->mw = c->mw;
        if (c->mh > w->mh) w->mh = c->mh;
    }
    if (tw > w->mw) w->mw = tw;
    w->mh += tabs_h(w);
}
static void tabs_arrange(ui_widget *w)
{
    int i = 0, h = tabs_h(w);
    for (ui_widget *c = w->first; c; c = c->next, i++) {
        if (i == w->value) c->flags &= ~F_HIDDEN; else c->flags |= F_HIDDEN;
        ui_arrange(c, w->x, w->y + h, w->w, w->h - h);
    }
}
static void tabs_draw(ui_widget *w)
{
    int h = tabs_h(w), x = w->x, i = 0;
    ui_fill(A, w->x, w->y, w->x + w->w, w->y + h, TH->panel);
    ui_fill(A, w->x, w->y + h - 1, w->x + w->w, w->y + h, TH->line);
    for (ui_widget *c = w->first; c; c = c->next, i++) {
        int tw = ui_tw(A, len(c->text)) + ui_sc(A, 32), on = i == w->value;
        if (on) {
            ui_fill(A, x, w->y, x + tw, w->y + h, TH->bg);
            ui_fill(A, x, w->y + h - ui_sc(A, 4), x + tw, w->y + h, TH->accent);
        }
        if (on && focused(w)) ui_frame(A, x, w->y, x + tw, w->y + h, 0, 1, TH->accent);
        ui_text_draw(A, x + ui_sc(A, 16), w->y + (h - A->chh) / 2, c->text, len(c->text), on ? TH->text : TH->dim, x + tw);
        x += tw;
    }
}
static int tabs_count(ui_widget *w) { int n = 0; for (ui_widget *c = w->first; c; c = c->next) n++; return n; }
static void tabs_pick(ui_widget *w, int i)
{
    if (i < 0 || i >= tabs_count(w) || i == w->value) return;
    w->value = i; A->layout = 1; ui_activate(w);
}
static int tabs_event(ui_widget *w, const ui_event *e)
{
    if (e->type == 0) { A->layout = 1; return 0; }
    if (e->type == UI_EV_POINTER && pressed(w, e) && e->y < w->y + tabs_h(w)) {
        int x = w->x, i = 0;
        for (ui_widget *c = w->first; c; c = c->next, i++) {
            int tw = ui_tw(A, len(c->text)) + ui_sc(A, 32);
            if (e->x >= x && e->x < x + tw) { tabs_pick(w, i); break; }
            x += tw;
        }
        return 1;
    }
    if (e->type == UI_EV_KEY && A->focus == w && (e->key == UI_KEY_LEFT || e->key == UI_KEY_RIGHT)) { tabs_pick(w, w->value + (e->key == UI_KEY_LEFT ? -1 : 1)); return 1; }
    return 0;
}
static const ui_class tabs_class = { tabs_measure, tabs_arrange, tabs_draw, tabs_event, 0, 1 };
ui_widget *ui_tabs(ui_widget *parent) { ui_widget *w = ui_new(0, parent, &tabs_class, 0); if (w) w->stretch = 1; return w; }
ui_widget *ui_tab(ui_widget *tabs, const char *title)
{
    ui_widget *w = ui_new(0, tabs, &ui_box_class, title);
    if (w) { w->dir = UI_COL; w->gap = 10; w->pad = 16; }
    return w;
}
