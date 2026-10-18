/*
 * uidemo - Every widget of the UI toolkit (sieos/ui.h, docs/ui.md) in one
 * window: a menu bar (File, Edit, View, Help, with shortcuts), a toolbar,
 * tabs: Widgets (label, buttons, a toggle, a check box, a text field, a
 * progress bar driven by a scroll bar, a message and a question), List (a
 * list of 500 rows beside a split), Canvas (pixels of its own, exact
 * colours, never themed); a right-click menu; the file dialogs.
 *
 *   uidemo [-v]     -v: says what happens on the serial console (tests)
 *
 * Its own loop (ui_wait, ui_dispatch, ui_flush), to tell the test where
 * its widgets are each time they move.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "sieos/ui.h"

static ui_app *app;
static ui_widget *status, *field, *bar, *list, *canvas, *button, *tabs, *menu_ctx;
static int verbose;

static void say(const char *a, const char *b)
{
    if (!verbose) return;
    char line[200];
    size_t n = strlcpy(line, "uidemo: ", sizeof line);
    n += strlcpy(line + n, a, sizeof line - n);
    if (b && n < sizeof line) n += strlcpy(line + n, b, sizeof line - n);
    if (n > sizeof line - 2) n = sizeof line - 2;
    line[n++] = '\n';
    sys_debug(line, n);
}
static void set_status(const char *a, const char *b)
{
    char t[160];
    size_t n = strlcpy(t, a, sizeof t);
    if (b && n < sizeof t) strlcpy(t + n, b, sizeof t - n);
    ui_set_text(status, t);
    say(a, b);
}

static void clicked(ui_widget *w, void *d) { (void)d; set_status("clicked ", ui_text(w)); }
static void toggled(ui_widget *w, void *d) { (void)d; set_status(ui_value(w) ? "on: " : "off: ", ui_text(w)); }
static void entered(ui_widget *w, void *d) { (void)d; set_status("field ", ui_text(w)); }
static void scrolled(ui_widget *w, void *d) { (void)d; ui_set_value(bar, ui_value(w) * 10); }
static void answered(ui_widget *w, void *d) { (void)d; set_status("answer ", ui_value(w) ? "yes" : "no"); }
static void ask(ui_widget *w, void *d) { (void)w; (void)d; ui_confirm(app, "A question", "Do you like this toolkit? It is small, and it follows the desktop's look.", answered, 0); }
static void tell(ui_widget *w, void *d) { (void)w; (void)d; ui_message(app, "About uidemo", "Every widget of SIEOS's UI toolkit (sieos/ui.h), drawn in the desktop's look."); say("menu About", 0); }
static void file_done(ui_widget *w, void *d) { (void)d; set_status(ui_value(w) ? "file " : "no file", ui_value(w) ? ui_text(w) : 0); }
static void open_file(ui_widget *w, void *d) { (void)w; (void)d; say("menu Open", 0); ui_file_dialog(app, 0, 0, 0, file_done, 0); }
static void save_file(ui_widget *w, void *d) { (void)w; (void)d; say("menu Save", 0); ui_file_dialog(app, 1, 0, "untitled.txt", file_done, 0); }
static void quit(ui_widget *w, void *d) { (void)w; (void)d; say("menu Quit", 0); ui_quit(app, 0); }
static void menu_pick(ui_widget *w, void *d) { (void)d; set_status("menu ", ui_text(w)); }
static void clear(ui_widget *w, void *d) { (void)w; (void)d; ui_set_text(field, ""); set_status("menu ", "Clear"); }
static void more(ui_widget *w, void *d) { int v = ui_value(bar) + (d ? 100 : -100); (void)w; ui_set_value(bar, v < 0 ? 0 : v > 1000 ? 1000 : v); }
static void show_list(ui_widget *w, void *d) { (void)w; (void)d; ui_set_value(tabs, 1); say("menu List", 0); }
static void show_canvas(ui_widget *w, void *d) { (void)w; (void)d; ui_set_value(tabs, 2); say("menu Canvas", 0); }
static void tab_shown(ui_widget *w, void *d) { char t[16]; (void)d; ui_fmt(t, "", ui_value(w)); say("tab ", t); }

static const char *cell(void *d, int row, int col)
{
    static char t[32];
    (void)d;
    if (col == 0) ui_fmt(t, "Row ", row + 1);
    else ui_fmt(t, "", (long)row * row % 9973);
    return t;
}
static void row_chosen(ui_widget *w, void *d) { char t[16]; (void)d; ui_fmt(t, "", ui_value(w) + 1); set_status("row ", t); }
static void row_open(ui_widget *w, void *d) { char t[16]; (void)d; ui_fmt(t, "", ui_value(w) + 1); set_status("opened row ", t); }

/* The canvas: exact colours (ten patches), a gradient, a dot where clicked. */
static const uint32_t patch[10] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff, 0x000000, 0x808080, 0x123456, 0xfedcba, 0x4be3c1, 0x0a0f1c };
static void paint(ui_widget *c, int dx, int dy)
{
    int w, h;
    uint32_t *px = ui_canvas_pixels(c, &w, &h);
    if (!px) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) px[y * w + x] = (uint32_t)(x * 255 / (w ? w : 1)) << 16 | (uint32_t)(y * 255 / (h ? h : 1)) << 8 | 0x40;
    for (int k = 0; k < 10; k++)
        for (int y = 20 + k / 5 * 70; y < 80 + k / 5 * 70 && y < h; y++)
            for (int x = 20 + k % 5 * 70; x < 80 + k % 5 * 70 && x < w; x++) px[y * w + x] = patch[k];
    for (int y = dy - 6; y < dy + 6; y++)
        for (int x = dx - 6; x < dx + 6; x++) if (x >= 0 && y >= 0 && x < w && y < h) px[y * w + x] = 0xffffff;
    ui_canvas_damage(c, 0, 0, w, h);
}
static void canvas_event(ui_widget *c, const ui_event *e, void *d)
{
    char t[32];
    (void)d;
    if (e->type == UI_EV_SIZE) paint(c, -100, -100);
    if (e->type == UI_EV_POINTER && (e->buttons & UI_MB_LEFT)) {
        paint(c, e->x, e->y);
        int n = ui_fmt(t, "", e->x);
        t[n++] = ' ';
        ui_fmt(t + n, "", e->y);
        say("canvas click ", t);
    }
    if (e->type == UI_EV_KEY && e->key >= 32 && e->key < 127) { t[0] = (char)e->key; t[1] = 0; say("canvas key ", t); }
}

static void where(const char *name, ui_widget *w)        /* (the test clicks there) */
{
    char t[64];
    int x, y, ww, hh, n;
    ui_bounds(w, &x, &y, &ww, &hh);
    n = ui_fmt(t, " ", x + ww / 2);
    ui_fmt(t + n, " ", y + hh / 2);
    say(name, t);
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    app = ui_app_new("UI demo");
    if (!app) { printf("uidemo: no desktop\n"); return 1; }
    ui_widget *root = ui_root(app);

    ui_widget *mb = ui_menubar(root);
    ui_widget *m = ui_menu(app, mb, "File");
    ui_item(m, "Open...", UI_CTRL('o'), open_file, 0);
    ui_item(m, "Save as...", UI_CTRL('s'), save_file, 0);
    ui_separator(m);
    ui_item(m, "Quit", UI_CTRL('q'), quit, 0);
    m = ui_menu(app, mb, "Edit");
    ui_item(m, "Clear the field", UI_CTRL('k'), clear, 0);
    ui_item(m, "Pick me", 0, menu_pick, 0);
    m = ui_menu(app, mb, "View");
    ui_item(m, "More progress", UI_CTRL('p'), more, (void *)1);
    ui_item(m, "Less progress", 0, more, 0);
    ui_separator(m);
    ui_item(m, "The list", UI_CTRL('e'), show_list, 0);
    ui_item(m, "The canvas", UI_CTRL('g'), show_canvas, 0);
    m = ui_menu(app, mb, "Help");
    ui_item(m, "About", UI_KEY_F1, tell, 0);

    ui_widget *tb = ui_toolbar(root);
    ui_tool(tb, UI_ICON_BACK, "Back", clicked, 0);
    ui_tool(tb, UI_ICON_FORWARD, "Forward", clicked, 0);
    ui_tool(tb, UI_ICON_RELOAD, "Reload", clicked, 0);
    ui_tool(tb, UI_ICON_STOP, "Stop", clicked, 0);
    ui_tool(tb, UI_ICON_NONE, "Text tool", clicked, 0);
    ui_widget *sp = ui_box(tb, UI_ROW, 0, 0);
    ui_set_stretch(sp, 1);
    ui_tool(tb, UI_ICON_MENU, "Menu", clicked, 0);

    tabs = ui_tabs(root);
    ui_on(tabs, tab_shown, 0);
    ui_widget *p = ui_tab(tabs, "Widgets");
    ui_label(p, "A label. Buttons, a toggle, a check box:");
    ui_widget *row = ui_box(p, UI_ROW, 10, 0);
    button = ui_button(row, "Button", clicked, 0);
    ui_toggle(row, "Toggle", 0, toggled, 0);
    ui_check(row, "Check box", 1, toggled, 0);
    ui_label(p, "A text field (Enter, Ctrl+A, Ctrl+C, Ctrl+V):");
    field = ui_field(p, "", entered, 0);
    ui_label(p, "A progress bar, moved by a scroll bar:");
    bar = ui_progress(p);
    ui_set_value(bar, 300);
    ui_widget *sb = ui_scrollbar(p, UI_ROW, scrolled, 0);
    ui_scroll_range(sb, 110, 10);
    ui_set_value(sb, 30);
    row = ui_box(p, UI_ROW, 10, 0);
    ui_button(row, "A question...", ask, 0);
    ui_button(row, "A message...", tell, 0);
    ui_button(row, "Open a file...", open_file, 0);

    p = ui_tab(tabs, "List");
    ui_widget *split = ui_split(p, UI_ROW, 520);
    static const char *const titles[] = { "Name", "Value" };
    list = ui_list(split, 2, titles, row_open, 0);
    ui_list_model(list, 500, cell, 0);
    ui_list_on_select(list, row_chosen, 0);
    ui_widget *right = ui_box(split, UI_COL, 10, 12);
    ui_label(right, "Drag the divider.\nRight-click the list\nfor its menu.");
    menu_ctx = ui_menu(app, 0, "List");
    ui_item(menu_ctx, "First row", 0, menu_pick, 0);
    ui_item(menu_ctx, "Last row", 0, menu_pick, 0);
    ui_set_menu(list, menu_ctx);

    p = ui_tab(tabs, "Canvas");
    ui_label(p, "The program's own pixels, never themed:");
    canvas = ui_canvas(p, canvas_event, 0);

    status = ui_label(ui_box(root, UI_ROW, 0, 8), "Ready.");

    say("ready", 0);
    int px = -1, py = -1, pv = -1;
    for (;;) {
        ui_event ev[64];
        ui_flush(app);
        int x, y, w, h;
        ui_bounds(button, &x, &y, &w, &h);
        if (x != px || y != py || ui_value(tabs) != pv) {   /* (moved: where things are, for the test) */
            px = x; py = y; pv = ui_value(tabs);
            where("at button", button); where("at field", field); where("at tabs", tabs);
            where("at list", list); where("at canvas", canvas); where("at bar", bar);
        }
        int n = ui_wait(app, ev, 64);
        if (n < 0) break;
        ui_dispatch(app, ev, n);
        for (int i = 0; i < n; i++) if (ev[i].type == UI_EV_THEME) { char t[16]; ui_fmt(t, "", ui_theme_of(app)->look); say("theme ", t); }
        if (ui_done(app)) break;
    }
    ui_app_close(app);
    return 0;
}
