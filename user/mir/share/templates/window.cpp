/*
 * @TITLE@ - made with MiR (Make it Real) on SIEOS.
 * A Facet window application in C++: libfacet draws it and gives it the keys and the mouse.
 */
#include <facet/facet.h>
#include <string>

class App {
public:
    void draw(struct surface *s, struct rect c)
    {
        gfx_fill(s, c.x, c.y, c.w, c.h, C_CONTENT);
        std::string line = "Clicked " + std::to_string(clicks) + " times";
        gfx_text(s, (c.w - text_width(line.c_str())) / 2, c.h / 2 - 20, line.c_str(), C_TEXT);
        ui_button(s, button_rect(c), "Click me", false);
    }
    bool click(struct rect c, int x, int y)
    {
        if (!rect_contains(button_rect(c), x, y))
            return false;
        clicks++;
        return true;
    }

private:
    static struct rect button_rect(struct rect c) { return rect_make(c.w / 2 - 60, c.h / 2 + 10, 120, 28); }
    int clicks = 0;
};

static App *app(struct fct_view *v) { return static_cast<App *>(v->app); }

int main()
{
    if (fct_app_init() < 0)
        return 1;
    struct fct_view *v = fct_view_new("@TITLE@", 360, 240);
    if (!v)
        return 1;
    v->app = new App;
    v->draw = [](struct fct_view *v, struct surface *s, struct rect c) { app(v)->draw(s, c); };
    v->mouse = [](struct fct_view *v, int x, int y, int kind, int) {
        if (kind == FCT_MOUSE_DOWN && app(v)->click(fct_view_content(v), x, y))
            fct_view_invalidate(v);
    };
    v->key = [](struct fct_view *v, const struct fct_key *k) {
        if (k->value && k->ascii == 27)      /* Escape closes */
            fct_view_close(v);
    };
    v->destroy = [](struct fct_view *v) { delete app(v); };
    return fct_main();
}
