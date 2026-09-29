/*
 * facet-message title line [line] - a message box with an OK button (Enter, Escape or Space also close it).
 * A Facet application (libfacet).
 */
#include "common.h"

struct message {
    char text[2][96];
};

static struct rect message_ok(struct rect c)
{
    return rect_make(c.x + c.w - 90, c.y + c.h - 36, 76, 26);
}

static void message_draw(struct fct_view *w, struct surface *s, struct rect c)
{
    struct message *m = w->app;
    gfx_fill(s, c.x, c.y, c.w, c.h, C_FACE);
    icon_draw(s, ICON_INFO, c.x + 14, c.y + 14, 40);
    gfx_text(s, c.x + 70, c.y + 20, m->text[0], C_TEXT);
    gfx_text(s, c.x + 70, c.y + 40, m->text[1], C_DIM);
    ui_button(s, message_ok(c), "OK", false);
}

static void message_mouse(struct fct_view *w, int x, int y, int kind, int buttons)
{
    (void)buttons;
    struct rect c = fct_view_content(w);
    if (kind == FCT_MOUSE_UP && rect_contains(message_ok(c), c.x + x, c.y + y))
        fct_view_close(w);
}

static void message_key(struct fct_view *w, const struct fct_key *ev)
{
    if (ev->value && (ev->ascii == '\n' || ev->ascii == 27 || ev->ascii == ' '))
        fct_view_close(w);
}

static void message_destroy(struct fct_view *w)
{
    free(w->app);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: facet-message title line [line]\n");
        return 2;
    }
    if (fct_app_init() < 0)
        return 1;
    struct message *m = calloc(1, sizeof(*m));
    strlcpy(m->text[0], argv[2], sizeof(m->text[0]));
    strlcpy(m->text[1], argc > 3 ? argv[3] : "", sizeof(m->text[1]));
    int w0 = MAX(text_width(m->text[0]), text_width(m->text[1])) + 100;
    struct fct_window_attr a = { argv[1], FCT_POS_CENTER, FCT_POS_CENTER, MAX(300, w0), 110, 0, 0, 0 };
    struct fct_view *w = fct_view_create(&a);
    if (!w)
        return 1;
    w->app = m;
    w->draw = message_draw;
    w->mouse = message_mouse;
    w->key = message_key;
    w->destroy = message_destroy;
    return fct_main();
}
