/*
 * ui_menu.c - Menus and dialogs of the UI toolkit (docs/ui.md). A menu is
 * a column of items shown in front (an overlay) under its title in a menu
 * bar, or where the user right-clicked; the keyboard opens the bar's first
 * one (F10), moves in it (arrows, the next menu with Left and Right),
 * picks (Enter) and closes it (Escape); items' shortcuts work whenever no
 * dialog is in front. A dialog is a column of widgets in front of the
 * window, centred, that has the keys until it is answered (Enter: OK,
 * Escape: Cancel): a message, a question, a file to open or save (its
 * folders listed from the file server).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "ui_int.h"

#define TH (&w->app->th)
#define A  (w->app)
static int len(const char *s) { return s ? (int)strlen(s) : 0; }

/* ---- An item: text, its shortcut's name on the right; a = its key; b 1: a separator. */
static void key_name(int k, char *d)
{
    d[0] = 0;
    if (k >= 1 && k <= 26) { memcpy(d, "Ctrl+", 5); d[5] = (char)('A' + k - 1); d[6] = 0; }
    else if (k >= UI_KEY_F1 && k < UI_KEY_F1 + 12) ui_fmt(d, "F", k - UI_KEY_F1 + 1);
}
static int index_of(ui_widget *w) { int i = 0; for (ui_widget *c = w->parent ? w->parent->first : 0; c && c != w; c = c->next) i++; return i; }
static void item_measure(ui_widget *w)
{
    char k[16];
    key_name(w->a, k);
    w->mw = ui_tw(A, len(w->text) + (k[0] ? len(k) + 4 : 0)) + ui_sc(A, 40);
    w->mh = w->b ? ui_sc(A, 10) : A->chh + ui_sc(A, 18);
}
static void item_draw(ui_widget *w)
{
    char k[16];
    if (w->b) { ui_fill(A, w->x + ui_sc(A, 8), w->y + w->h / 2, w->x + w->w - ui_sc(A, 8), w->y + w->h / 2 + 1, TH->line); return; }
    int on = w->parent && w->parent->value == index_of(w) && !(w->flags & F_OFF);
    if (on) ui_fill(A, w->x, w->y, w->x + w->w, w->y + w->h, TH->accent);
    uint32_t fg = w->flags & F_OFF ? TH->dim : on ? TH->ink : TH->text;
    ui_ytext(w, w->text, fg, w->x + ui_sc(A, 20), w->x + w->w);
    key_name(w->a, k);
    if (k[0]) ui_ytext(w, k, on ? TH->ink : TH->dim, w->x + w->w - ui_sc(A, 20) - ui_tw(A, len(k)), w->x + w->w);
}
static void pick(ui_widget *w)                          /* an item chosen: the menus close, then its action */
{
    if (w->b || (w->flags & F_OFF)) return;
    ui_menus_close(A);
    ui_activate(w);
}
static int item_event(ui_widget *w, const ui_event *e)
{
    if (e->type != UI_EV_POINTER || w->b) return e->type == UI_EV_POINTER;
    if (w->parent && w->parent->value != index_of(w)) { w->parent->value = index_of(w); ui_dirty(w->parent); }
    if (!e->buttons && (A->buttons & (UI_MB_LEFT | UI_MB_RIGHT))) pick(w);
    return 1;
}
static const ui_class item_class = { item_measure, 0, item_draw, item_event, 0, 0 };

/* ---- A menu: a column (an overlay when open); value = the item lit. */
static const ui_class menu_class = { ui_box_measure, ui_box_arrange, ui_box_draw, 0, 0, 0 };
ui_widget *ui_menu(ui_app *app, ui_widget *bar, const char *title)
{
    ui_widget *w = ui_new(app, 0, &menu_class, title);
    if (!w) return 0;
    w->dir = UI_COL; w->pad = 4; w->is_menu = 1; w->value = -1;
    if (app->nmenus < 16) app->menus[app->nmenus++] = w;
    if (bar) {                                           /* its title in the bar */
        extern const ui_class ui_mtitle_class;
        ui_widget *t = ui_new(0, bar, &ui_mtitle_class, title);
        if (t) t->saved = w;
    }
    return w;
}
ui_widget *ui_item(ui_widget *menu, const char *text, int key, ui_fn fn, void *data)
{
    ui_widget *w = ui_new(0, menu, &item_class, text);
    if (w) { w->a = key; ui_on(w, fn, data); }
    return w;
}
void ui_separator(ui_widget *menu) { ui_widget *w = ui_new(0, menu, &item_class, 0); if (w) w->b = 1; }

static ui_widget *open_menu(ui_app *a) { for (int i = a->nover - 1; i >= 0; i--) if (a->over[i]->is_menu) return a->over[i]; return 0; }
void ui_menus_close(ui_app *a)
{
    ui_widget *m;
    while ((m = open_menu(a))) ui_hide(m);
    if (a->bar) ui_dirty(a->bar);
    a->bar = 0;
}
static void show(ui_widget *menu, int x, int y, ui_widget *bar)
{
    ui_app *a = menu->app;
    ui_menus_close(a);
    menu->value = -1;
    a->bar = bar;
    ui_show(menu, x, y);
    if (bar) ui_dirty(bar);
}
void ui_popup(ui_widget *menu, int x, int y) { show(menu, x, y, 0); }

/* ---- The bar: titles (each holds its menu in `saved`). */
static void bar_measure(ui_widget *w) { ui_box_measure(w); if (w->mh < A->chh + ui_sc(A, 18)) w->mh = A->chh + ui_sc(A, 18); }
static void bar_draw(ui_widget *w)
{
    ui_fill(A, w->x, w->y, w->x + w->w, w->y + w->h, TH->panel);
    ui_fill(A, w->x, w->y + w->h - 1, w->x + w->w, w->y + w->h, TH->line);
}
static const ui_class bar_class = { bar_measure, ui_box_arrange, bar_draw, 0, 0, 0 };
ui_widget *ui_menubar(ui_widget *parent)
{
    ui_widget *w = ui_new(0, parent, &bar_class, 0);
    if (!w) return 0;
    w->dir = UI_ROW; w->pad = 0;
    if (!w->app->menubar) w->app->menubar = w;
    return w;
}
static void title_measure(ui_widget *w) { w->mw = ui_tw(A, len(w->text)) + ui_sc(A, 32); w->mh = A->chh + ui_sc(A, 18); }
static void title_draw(ui_widget *w)
{
    int on = A->bar == w->parent && open_menu(A) == w->saved;
    if (on) ui_fill(A, w->x, w->y, w->x + w->w, w->y + w->h - 1, TH->accent);
    else if (w->flags & F_HOVER) ui_fill(A, w->x, w->y, w->x + w->w, w->y + w->h - 1, ui_mix(TH->panel, TH->accent, 48));
    ui_ytext(w, w->text, on ? TH->ink : TH->text, w->x + ui_sc(A, 16), w->x + w->w);
}
static int title_event(ui_widget *w, const ui_event *e)
{
    if (e->type != UI_EV_POINTER) return 0;
    int open = open_menu(A) != 0;
    if ((e->buttons & ~A->buttons & UI_MB_LEFT) || (open && A->bar == w->parent && open_menu(A) != w->saved)) {
        if (open && open_menu(A) == w->saved && (e->buttons & ~A->buttons)) ui_menus_close(A);   /* (its title again: closed) */
        else show(w->saved, w->x, w->y + w->h, w->parent);
        A->pressed = 0;                                   /* (the release goes to the menu's items) */
    }
    return 1;
}
const ui_class ui_mtitle_class = { title_measure, 0, title_draw, title_event, 0, 0 };

ui_widget *ui_menu_over(ui_app *a, int x, int y)
{
    if (!a->bar) return 0;
    for (ui_widget *t = a->bar->first; t; t = t->next) if (ui_hit(t, x, y)) return t;
    return 0;
}

static void move(ui_widget *m, int d)                   /* the lit item: the next one that can be picked */
{
    int n = 0;
    for (ui_widget *c = m->first; c; c = c->next) n++;
    for (int k = 0, i = m->value; k < n; k++) {
        i = (i + d + n) % n;
        if (i < 0) i = n - 1;
        ui_widget *c = ui_child(m, i);
        if (c && !c->b && !(c->flags & (F_OFF | F_HIDDEN))) { m->value = i; break; }
    }
    ui_dirty(m);
}
static ui_widget *title_of(ui_app *a, ui_widget *menu)
{
    for (ui_widget *t = a->bar ? a->bar->first : 0; t; t = t->next) if (t->saved == menu) return t;
    return 0;
}

/* Keys: in an open menu, or a shortcut, or F10 (the bar). -> 1: taken. */
int ui_menu_key(ui_app *a, int key)
{
    ui_widget *m = open_menu(a);
    if (m) {
        ui_widget *t = title_of(a, m);
        if (key == UI_KEY_DOWN || key == UI_KEY_UP) move(m, key == UI_KEY_DOWN ? 1 : -1);
        else if ((key == UI_KEY_LEFT || key == UI_KEY_RIGHT) && t) {
            ui_widget *n = key == UI_KEY_RIGHT ? (t->next ? t->next : a->bar->first) : 0;
            if (key == UI_KEY_LEFT) { n = a->bar->first; for (ui_widget *c = a->bar->first; c; c = c->next) if (c->next == t) n = c; if (n == t) n = a->bar->last; }
            show(n->saved, n->x, n->y + n->h, a->bar);
            move(n->saved, 1);
        } else if (key == '\n' || key == ' ') { ui_widget *c = ui_child(m, m->value); if (c) pick(c); }
        else if (key == 27) ui_menus_close(a);
        else return 0;
        return 1;
    }
    if (key == UI_KEY_F10 && a->menubar && a->menubar->first && !a->nover) {
        ui_widget *t = a->menubar->first;
        show(t->saved, t->x, t->y + t->h, a->menubar);
        move(t->saved, 1);
        return 1;
    }
    if (!key || key == '\n' || key == '\t' || key == 27 || key == '\b' || (key >= 32 && key < 256)) return 0;   /* (typing is not a shortcut) */
    for (int i = 0; i < a->nmenus; i++)
        for (ui_widget *c = a->menus[i]->first; c; c = c->next)
            if (c->a == key && !c->b && !(c->flags & F_OFF)) { ui_activate(c); return 1; }
    return 0;
}

/* ---- Dialogs: a column in front; its fn gets it (value 1: OK). */
typedef struct { char dir[512]; char **names; int *dirs; int n, cap, save; ui_widget *list, *name, *where; } file_t;
static void done(ui_widget *d, int ok)
{
    d->value = ok;
    ui_trash(d);                                          /* (freed at the next flush: after its fn) */
    if (d->fn) d->fn(d, d->data);
}
static int dialog_event(ui_widget *w, const ui_event *e)
{
    if (e->type != UI_EV_KEY) return e->type == UI_EV_POINTER;
    if (e->key == 27) { done(w, 0); return 1; }
    if (e->key == '\n') { if (w->ext) { extern void ui_file_accept(ui_widget *); ui_file_accept(w); } else done(w, 1); return 1; }
    return 0;
}
static void dialog_free(ui_widget *w)
{
    file_t *f = w->ext;
    if (!f) return;
    for (int i = 0; i < f->n; i++) free(f->names[i]);
    free(f->names); free(f->dirs);
}
static const ui_class dialog_class = { ui_box_measure, ui_box_arrange, ui_box_draw, dialog_event, dialog_free, 0 };

static void ok_fn(ui_widget *b, void *d) { (void)b; done(d, 1); }
static void cancel_fn(ui_widget *b, void *d) { (void)b; done(d, 0); }

/* Text wrapped at about 52 characters a line. */
static char *wrap(const char *s)
{
    int n = len(s), col = 0, last = -1;
    char *t = ui_strdup(s);
    for (int i = 0; i < n; i++) {
        if (t[i] == '\n') { col = 0; last = -1; continue; }
        if (t[i] == ' ') last = i;
        if (++col > 52 && last >= 0) { t[last] = '\n'; col = i - last; last = -1; }
    }
    return t;
}
static ui_widget *dialog(ui_app *a, const char *title, const char *text, int w, ui_fn fn, void *data)
{
    ui_widget *d = ui_new(a, 0, &dialog_class, title);
    if (!d) return 0;
    d->dir = UI_COL; d->gap = 12; d->pad = 20; d->uw = w;
    ui_on(d, fn, data);
    ui_widget *t = ui_label(d, title);
    if (t) t->c = 1;                                      /* (bold, the accent) */
    if (text) { char *wt = wrap(text); ui_label(d, wt); free(wt); }
    return d;
}
static ui_widget *buttons(ui_widget *d, const char *ok, const char *cancel)
{
    ui_widget *row = ui_box(d, UI_ROW, 10, 0), *sp = ui_box(row, UI_ROW, 0, 0), *b = 0;
    ui_set_stretch(sp, 1);
    if (cancel) ui_button(row, cancel, cancel_fn, d);
    if (ok) b = ui_button(row, ok, ok_fn, d);
    return b;
}
void ui_message(ui_app *a, const char *title, const char *text)
{
    ui_widget *d = dialog(a, title, text, 420, 0, 0);
    if (!d) return;
    ui_widget *b = buttons(d, "OK", 0);
    ui_show(d, -1, -1);
    ui_focus(b);
}
void ui_confirm(ui_app *a, const char *title, const char *text, ui_fn fn, void *data)
{
    ui_widget *d = dialog(a, title, text, 420, fn, data);
    if (!d) return;
    ui_widget *b = buttons(d, "Yes", "No");
    ui_show(d, -1, -1);
    ui_focus(b);
}
void ui_close_dialog(ui_app *a)
{
    for (int i = a->nover - 1; i >= 0; i--) if (!a->over[i]->is_menu) { done(a->over[i], 0); return; }
}

/* ---- The file dialog: a folder's entries (folders first, ".." to go up), a name. */
static void add(void *ctx, const char *name, int dir)
{
    file_t *f = ctx;
    if (f->n == f->cap) {
        int c = f->cap ? f->cap * 2 : 64;
        char **nn = realloc(f->names, (size_t)c * sizeof *nn);
        int *nd = nn ? realloc(f->dirs, (size_t)c * sizeof *nd) : 0;
        if (nn) f->names = nn;
        if (!nd) return;
        f->dirs = nd; f->cap = c;
    }
    f->names[f->n] = ui_strdup(name); f->dirs[f->n++] = dir;
}
static int before(file_t *f, int i, int j)             /* folders first, then by name */
{
    if (f->dirs[i] != f->dirs[j]) return f->dirs[i] > f->dirs[j];
    return strcmp(f->names[i], f->names[j]) < 0;
}
static const char *cell(void *ctx, int row, int col)
{
    file_t *f = ctx;
    static char b[260];
    (void)col;
    if (row < 0 || row >= f->n) return "";
    int n = len(f->names[row]);
    if (n > 250) n = 250;
    memcpy(b, f->names[row], (size_t)n);
    b[n] = 0;
    if (f->dirs[row]) { b[n] = '/'; b[n + 1] = 0; }
    return b;
}
static void load(file_t *f)
{
    for (int i = 0; i < f->n; i++) free(f->names[i]);
    f->n = 0;
    if (strcmp(f->dir, "/")) add(f, "..", 1);
    ui_sys_list(f->dir, add, f);
    for (int i = 1; i < f->n; i++)                        /* (insertion sort: a folder's few entries) */
        for (int j = i; j > (strcmp(f->dir, "/") ? 1 : 0) && before(f, j, j - 1); j--) {
            char *t = f->names[j]; f->names[j] = f->names[j - 1]; f->names[j - 1] = t;
            int d = f->dirs[j]; f->dirs[j] = f->dirs[j - 1]; f->dirs[j - 1] = d;
        }
    ui_list_model(f->list, f->n, cell, f);
    ui_set_value(f->list, -1);
    ui_set_text(f->where, f->dir);
}
static void enter(file_t *f, const char *name)
{
    int n = len(f->dir);
    if (!strcmp(name, "..")) {
        while (n > 1 && f->dir[n - 1] != '/') n--;
        f->dir[n > 1 ? n - 1 : 1] = 0;
    } else if (n + len(name) + 2 < (int)sizeof f->dir) {
        if (n > 1) f->dir[n++] = '/';
        memcpy(f->dir + n, name, (size_t)len(name) + 1);
    }
    load(f);
}
void ui_file_accept(ui_widget *d)
{
    file_t *f = d->ext;
    int r = ui_value(f->list);
    if (d->app->focus == f->list && r >= 0 && f->dirs[r]) { enter(f, f->names[r]); return; }
    const char *name = ui_text(f->name);
    if (!name[0]) return;
    if (name[0] == '/') { ui_set_text(d, name); done(d, 1); return; }
    char path[768];
    int n = len(f->dir), k = len(name);
    if (n + k + 2 > (int)sizeof path) return;
    memcpy(path, f->dir, (size_t)n);
    if (n > 1) path[n++] = '/';
    memcpy(path + n, name, (size_t)k + 1);
    for (int i = 0; i < f->n; i++)
        if (f->dirs[i] && !strcmp(f->names[i], name)) { enter(f, name); ui_set_text(f->name, ""); return; }   /* (a folder typed: go in) */
    ui_set_text(d, path);
    done(d, 1);
}
static void file_chosen(ui_widget *l, void *ctx)        /* a row chosen: a file's name in the field */
{
    ui_widget *d = ctx;
    file_t *f = d->ext;
    int r = ui_value(l);
    if (r >= 0 && !f->dirs[r]) { ui_set_text(f->name, f->names[r]); ui_field_select(f->name, -1, -1); }
}
static void file_open(ui_widget *l, void *ctx)          /* Enter or a double click on a row */
{
    ui_widget *d = ctx;
    file_t *f = d->ext;
    int r = ui_value(l);
    if (r >= 0 && f->dirs[r]) enter(f, f->names[r]); else ui_file_accept(d);
}
static void file_ok(ui_widget *b, void *ctx) { (void)b; ui_widget *d = ctx; file_t *f = d->ext; ui_focus(f->name); ui_file_accept(d); }
static void file_field(ui_widget *b, void *ctx) { (void)b; ui_file_accept(ctx); }
void ui_file_dialog(ui_app *a, int save, const char *dir, const char *name, ui_fn fn, void *data)
{
    ui_widget *d = dialog(a, save ? "Save as" : "Open", 0, 640, fn, data);
    file_t *f = d ? calloc(1, sizeof *f) : 0;
    if (!f) { if (d) ui_destroy(d); return; }
    d->ext = f;
    f->save = save;
    const char *s = dir && dir[0] ? dir : ui_sys_home();
    int n = len(s) < (int)sizeof f->dir - 1 ? len(s) : (int)sizeof f->dir - 1;
    memcpy(f->dir, s, (size_t)n); f->dir[n] = 0;
    while (n > 1 && f->dir[n - 1] == '/') f->dir[--n] = 0;
    f->where = ui_label(d, f->dir);
    static const char *const title[] = { "Name" };
    f->list = ui_list(d, 1, title, file_open, d);
    ui_set_size(f->list, 0, 420);
    ui_list_on_select(f->list, file_chosen, d);
    f->name = ui_field(d, name ? name : "", file_field, d);
    ui_widget *row = ui_box(d, UI_ROW, 10, 0), *sp = ui_box(row, UI_ROW, 0, 0);
    ui_set_stretch(sp, 1);
    ui_button(row, "Cancel", cancel_fn, d);
    ui_button(row, save ? "Save" : "Open", file_ok, d);
    load(f);
    ui_show(d, -1, -1);
    ui_focus(save ? f->name : f->list);
}
