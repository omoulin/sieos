/*
 * hello.c - The smallest useful Facet application: a window with a
 * greeting, a button that counts clicks, and a key that closes it.
 *
 *   cc hello.c -lfacet -o hello && ./hello        (in a Facet terminal)
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <stdio.h>
#include <facet/facet.h>

static int clicks;

static struct rect button(struct rect c)
{
    return rect_make(c.x + (c.w - 120) / 2, c.y + c.h - 50, 120, 30);
}

static void draw(struct fct_view *v, struct surface *s, struct rect c)
{
    (void)v;
    gfx_vgradient(s, c.x, c.y, c.w, c.h, C_CONTENT_ALT, C_CONTENT);
    gfx_text_scaled(s, c.x + 20, c.y + 20, "Hello, Facet", 2, C_ACCENT);
    char line[64];
    snprintf(line, sizeof(line), "The button was clicked %d time%s.", clicks, clicks == 1 ? "" : "s");
    gfx_text(s, c.x + 20, c.y + 64, line, C_TEXT);
    gfx_text(s, c.x + 20, c.y + 84, "Press q to quit.", C_DIM);
    ui_button(s, button(c), "Click me", false);
}

static void mouse(struct fct_view *v, int x, int y, int kind, int buttons)
{
    (void)buttons;
    if (kind == FCT_MOUSE_UP && rect_contains(button(fct_view_content(v)), x, y)) {
        clicks++;
        fct_view_invalidate(v);
    }
}

static void key(struct fct_view *v, const struct fct_key *k)
{
    if (k->value && k->ascii == 'q')
        fct_view_close(v);
}

int main(void)
{
    if (fct_app_init() < 0)
        return 1;
    struct fct_view *v = fct_view_new("Hello", 360, 170);
    if (!v)
        return 1;
    v->draw = draw;
    v->mouse = mouse;
    v->key = key;
    return fct_main();
}
