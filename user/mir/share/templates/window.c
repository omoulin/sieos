/*
 * @TITLE@ - made with MiR (Make it Real) on SIEOS.
 * A Facet window application: libfacet draws it and gives it the keys and the mouse.
 */
#include <facet/facet.h>
#include <stdio.h>
#include <string.h>

struct app {
    int clicks;
};

/* Where the button is, from the window's content size (it can be resized) */
static struct rect button_rect(struct rect c)
{
    return rect_make(c.w / 2 - 60, c.h / 2 + 10, 120, 28);
}

static void draw(struct fct_view *v, struct surface *s, struct rect c)
{
    struct app *a = v->app;
    gfx_fill(s, c.x, c.y, c.w, c.h, C_CONTENT);
    char line[64];
    snprintf(line, sizeof(line), "Clicked %d times", a->clicks);
    gfx_text(s, (c.w - text_width(line)) / 2, c.h / 2 - 20, line, C_TEXT);
    ui_button(s, button_rect(c), "Click me", false);
}

static void mouse(struct fct_view *v, int x, int y, int kind, int buttons)
{
    struct app *a = v->app;
    (void)buttons;
    if (kind == FCT_MOUSE_DOWN && rect_contains(button_rect(fct_view_content(v)), x, y)) {
        a->clicks++;
        fct_view_invalidate(v);
    }
}

static void key(struct fct_view *v, const struct fct_key *k)
{
    if (!k->value)                       /* (releases) */
        return;
    if (k->ascii == 27)                  /* Escape closes */
        fct_view_close(v);
}

int main(void)
{
    static struct app a;
    if (fct_app_init() < 0)
        return 1;
    struct fct_view *v = fct_view_new("@TITLE@", 360, 240);
    if (!v)
        return 1;
    v->app = &a;
    v->draw = draw;
    v->mouse = mouse;
    v->key = key;
    return fct_main();
}
