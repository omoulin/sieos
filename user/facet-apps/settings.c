/*
 * facet-settings - Settings: the desktop's settings, one page per section
 * in a sidebar (Display: the screen's resolution; Appearance: the skin).
 * Facet owns the display and the look, so changes go through the desktop
 * channel ({"op":"display"}, {"op":"skin"}); Facet applies them and keeps
 * them in ~/.facet/settings for the next session (facet/settings.h).
 * "facet-settings SECTION" opens on that section.  A Facet application
 * (libfacet).
 */
#include "common.h"
#include <facet/settings.h>

#define SIDE_W 150
#define PAD 14
#define ROW 24
#define CARD_H 118
#define MAXMODES 32

struct page {
    const char *name, *title;
    int icon;
    void (*enter)(void);
    void (*draw)(struct surface *s, struct rect c);
    void (*click)(struct rect c, int x, int y);
    void (*move)(struct rect c, int x, int y);
};

static int cur_page, side_hover = -1;
static char status[200];

/* ---------------- Display ---------------- */

static struct {
    char header[128];
    struct { int w, h; bool current, preferred; } mode[MAXMODES];
    int n, hover;
    bool fixed;
} dsp = { .hover = -1 };

static void display_enter(void)
{
    char r[4096];
    dsp.n = 0;
    dsp.fixed = false;
    if (!desktop_request("{\"op\":\"display\"}", r, sizeof(r))) {
        snprintf(dsp.header, sizeof(dsp.header), "%s", r[0] ? r : "no display information");
        return;
    }
    char *save, *line = strtok_r(r, "\n", &save);
    snprintf(dsp.header, sizeof(dsp.header), "%s", line ? line : "");
    char *fixed = strstr(dsp.header, ", its mode cannot be changed");
    if (fixed) {
        dsp.fixed = true;
        *fixed = 0;
    }
    while ((line = strtok_r(NULL, "\n", &save)) && dsp.n < MAXMODES) {
        int w, h;
        if (sscanf(line, "%dx%d", &w, &h) != 2)
            continue;
        dsp.mode[dsp.n].w = w;
        dsp.mode[dsp.n].h = h;
        dsp.mode[dsp.n].current = strstr(line, "(current)") != NULL;
        dsp.mode[dsp.n].preferred = strstr(line, "(preferred)") != NULL;
        dsp.n++;
    }
}

/* The modes in as many columns as the page needs. */
static struct rect mode_rect(struct rect c, int i)
{
    int rows = MAX(1, (c.h - 62) / ROW), cols = (dsp.n + rows - 1) / rows;
    if (cols < 2)
        return rect_make(c.x + PAD, c.y + 62 + i * ROW, c.w - 2 * PAD, ROW - 4);
    rows = (dsp.n + 1) / 2;                          /* (two even columns) */
    int cw = (c.w - 2 * PAD - 10) / 2;
    return rect_make(c.x + PAD + i / rows * (cw + 10), c.y + 62 + i % rows * ROW, cw, ROW - 4);
}

static void display_draw(struct surface *s, struct rect c)
{
    icon_draw(s, ICON_MONITOR, c.x + PAD, c.y + 10, 32);
    gfx_text_bold(s, c.x + PAD + 42, c.y + 10, "Screen resolution", C_TEXT);
    gfx_text(s, c.x + PAD + 42, c.y + 28, dsp.header, C_DIM);
    for (int i = 0; i < dsp.n; i++) {
        struct rect r = mode_rect(c, i);
        char label[64];
        snprintf(label, sizeof(label), "%d x %d%s", dsp.mode[i].w, dsp.mode[i].h,
                 dsp.mode[i].preferred ? "   (native)" : "");
        if (dsp.mode[i].current)
            gfx_fill(s, r.x, r.y, r.w, r.h, C_SELECT);
        else if (i == dsp.hover && !dsp.fixed)
            gfx_fill(s, r.x, r.y, r.w, r.h, C_CONTENT_ALT);
        gfx_text(s, r.x + 10, r.y + (r.h - FONT_H) / 2, label, C_TEXT);
        if (dsp.mode[i].current)
            gfx_text_bold(s, r.x + r.w - 10 - text_width_bold("current"), r.y + (r.h - FONT_H) / 2, "current", C_ACCENT);
    }
    if (!status[0])
        snprintf(status, sizeof(status), "%s", dsp.fixed ? "This display's mode is fixed (the firmware set it)."
                                                         : "Click a resolution to use it; it is kept for the next session.");
}

static void display_move(struct rect c, int x, int y)
{
    int hit = -1;
    for (int i = 0; i < dsp.n; i++)
        if (rect_contains(mode_rect(c, i), x, y))
            hit = i;
    dsp.hover = hit;
}

static void display_click(struct rect c, int x, int y)
{
    for (int i = 0; i < dsp.n; i++) {
        if (!rect_contains(mode_rect(c, i), x, y) || dsp.fixed || dsp.mode[i].current)
            continue;
        char req[96], r[256];
        snprintf(req, sizeof(req), "{\"op\":\"display\",\"width\":%d,\"height\":%d}", dsp.mode[i].w, dsp.mode[i].h);
        bool ok = desktop_request(req, r, sizeof(r));
        snprintf(status, sizeof(status), "%s%s", ok ? "" : "Could not change: ", r);
        display_enter();
        return;
    }
}

/* ---------------- Appearance ---------------- */

static struct rect card_rect(struct rect c, int i)
{
    return rect_make(c.x + PAD, c.y + 36 + i * (CARD_H + 10), c.w - 2 * PAD, CARD_H);
}

/* A miniature desktop in skin k: its background, a small window, some icons. */
static void preview(struct surface *s, struct rect r, const struct fct_skin *k)
{
    const struct fct_skin *cur = fct_skin;
    fct_skin_use(k);
    gfx_vgradient(s, r.x, r.y, r.w, r.h, C_DESK_TOP, C_DESK_BOT);
    struct rect w = rect_make(r.x + 12, r.y + 14, r.w - 24, r.h - 22);
    if (k->id == FCT_SKIN_BEOS) {                   /* a yellow tab over a grey frame */
        gfx_fill(s, w.x, w.y, 80, 14, RGB(0x40, 0x40, 0x40));
        gfx_vgradient(s, w.x + 1, w.y + 1, 78, 13, color_shade(C_TITLEBAR, 40), C_TITLEBAR);
        gfx_fill(s, w.x, w.y + 14, w.w, w.h - 14, C_FACE);
        gfx_frame(s, w.x, w.y + 14, w.w, w.h - 14, RGB(0x40, 0x40, 0x40));
        gfx_fill(s, w.x + 4, w.y + 18, w.w - 8, w.h - 22, C_CONTENT);
    } else if (k->id == FCT_SKIN_IRIX) {            /* a thick steel-blue bevelled frame */
        gfx_fill(s, w.x, w.y, w.w, w.h, C_TITLEBAR);
        gfx_bevel(s, w.x, w.y, w.w, w.h, 2, true, color_shade(C_TITLEBAR, 70), color_shade(C_TITLEBAR, -60));
        gfx_fill(s, w.x + 5, w.y + 18, w.w - 10, w.h - 23, C_CONTENT);
    } else {                                        /* Strata: a dark rounded frame */
        gfx_round_rect_top(s, w.x, w.y, w.w, 16, 4, C_TITLEBAR);
        gfx_fill(s, w.x, w.y + 16, w.w, w.h - 16, C_CONTENT);
        gfx_frame(s, w.x, w.y, w.w, w.h, C_FACE_DARK);
        gfx_fill(s, w.x + 1, w.y + 16, 3, w.h - 17, C_ACCENT);
    }
    static const int icons[] = { ICON_FOLDER, ICON_TERMINAL, ICON_MONITOR, ICON_NETWORK, ICON_CLOCK };
    for (int i = 0; i < 5; i++)
        icon_draw(s, icons[i], w.x + 12 + i * 40, w.y + w.h - 42, 32);
    fct_skin_use(cur);
}

static void appearance_draw(struct surface *s, struct rect c)
{
    gfx_text_bold(s, c.x + PAD, c.y + 12, "Desktop skin", C_TEXT);
    for (int i = 0; i < FCT_NSKINS; i++) {
        const struct fct_skin *k = fct_skin_at(i);
        struct rect r = card_rect(c, i);
        bool cur = k == fct_skin;
        gfx_fill(s, r.x, r.y, r.w, r.h, cur ? C_SELECT : C_CONTENT);
        gfx_frame(s, r.x, r.y, r.w, r.h, cur ? C_ACCENT : C_LINE);
        struct rect pv = rect_make(r.x + 8, r.y + 8, 240, r.h - 16);
        preview(s, pv, k);
        gfx_frame(s, pv.x, pv.y, pv.w, pv.h, C_FACE_DARK);
        int tx = pv.x + pv.w + 14, tw = r.x + r.w - 10 - tx;
        gfx_text_bold(s, tx, r.y + 12, k->title, C_TEXT);
        if (cur)
            gfx_text_bold(s, r.x + r.w - 10 - text_width_bold("current"), r.y + 12, "current", C_ACCENT);
        const char *p = k->blurb;                   /* the description, wrapped */
        int y = r.y + 36;
        while (*p && y < r.y + r.h - FONT_H) {
            size_t n = text_fit(p, tw);
            if (p[n]) {
                size_t sp = n;
                while (sp > 0 && p[sp] != ' ')
                    sp--;
                if (sp > 0)
                    n = sp;
            }
            char line[128];
            snprintf(line, sizeof(line), "%.*s", (int)MIN(n, sizeof(line) - 1), p);
            gfx_text(s, tx, y, line, C_DIM);
            p += n;
            while (*p == ' ')
                p++;
            y += FONT_H + 2;
        }
    }
    if (!status[0])
        snprintf(status, sizeof(status), "Click a skin to use it; it is kept for the next session.");
}

static void appearance_click(struct rect c, int x, int y)
{
    for (int i = 0; i < FCT_NSKINS; i++) {
        if (!rect_contains(card_rect(c, i), x, y) || fct_skin_at(i) == fct_skin)
            continue;
        char req[64], r[256];
        snprintf(req, sizeof(req), "{\"op\":\"skin\",\"name\":\"%s\"}", fct_skin_at(i)->name);
        bool ok = desktop_request(req, r, sizeof(r));
        snprintf(status, sizeof(status), "%s%s", ok ? "" : "Could not change: ", r);
        if (ok)
            fct_skin_use(fct_skin_at(i));           /* (the desktop tells us too) */
    }
}

/* ---------------- the window ---------------- */

static const struct page pages[] = {
    { "display", "Display", ICON_MONITOR, display_enter, display_draw, display_click, display_move },
    { "appearance", "Appearance", ICON_PROGRAM, NULL, appearance_draw, appearance_click, NULL },
};
#define NPAGES ((int)(sizeof(pages) / sizeof(pages[0])))

static struct rect side_item(struct rect c, int i)
{
    return rect_make(c.x + 6, c.y + 8 + i * 40, SIDE_W - 12, 36);
}

static struct rect page_rect(struct rect c)
{
    return rect_make(c.x + SIDE_W, c.y, c.w - SIDE_W, c.h - 30);
}

static void open_page(int i)
{
    cur_page = i;
    status[0] = 0;
    if (pages[i].enter)
        pages[i].enter();
}

static void draw(struct fct_view *v, struct surface *s, struct rect c)
{
    (void)v;
    gfx_fill(s, c.x, c.y, c.w, c.h, C_FACE);
    /* the sidebar: the sections */
    gfx_fill(s, c.x, c.y, SIDE_W, c.h, C_CONTENT);
    gfx_vline(s, c.x + SIDE_W - 1, c.y, c.h, C_LINE);
    for (int i = 0; i < NPAGES; i++) {
        struct rect r = side_item(c, i);
        if (i == cur_page)
            gfx_fill(s, r.x, r.y, r.w, r.h, C_SELECT);
        else if (i == side_hover)
            gfx_fill(s, r.x, r.y, r.w, r.h, C_CONTENT_ALT);
        icon_draw(s, pages[i].icon, r.x + 6, r.y + 6, 24);
        if (i == cur_page)
            gfx_text_bold(s, r.x + 38, r.y + (r.h - FONT_H) / 2, pages[i].title, C_TEXT);
        else
            gfx_text(s, r.x + 38, r.y + (r.h - FONT_H) / 2, pages[i].title, C_TEXT);
    }
    pages[cur_page].draw(s, page_rect(c));
    gfx_text(s, c.x + SIDE_W + PAD, c.y + c.h - 24, status, C_DIM);
}

static void mouse(struct fct_view *v, int x, int y, int kind, int buttons)
{
    (void)buttons;
    struct rect c = fct_view_content(v);
    x += c.x;
    y += c.y;
    if (kind == FCT_MOUSE_MOVE) {
        int hit = -1;
        for (int i = 0; i < NPAGES; i++)
            if (rect_contains(side_item(c, i), x, y))
                hit = i;
        side_hover = hit;
        if (pages[cur_page].move)
            pages[cur_page].move(page_rect(c), x, y);
        fct_view_invalidate(v);
        return;
    }
    if (kind != FCT_MOUSE_DOWN)
        return;
    for (int i = 0; i < NPAGES; i++)
        if (rect_contains(side_item(c, i), x, y)) {
            if (i != cur_page)
                open_page(i);
            fct_view_invalidate(v);
            return;
        }
    if (pages[cur_page].click)
        pages[cur_page].click(page_rect(c), x, y);
    fct_view_invalidate(v);
}

static void key(struct fct_view *v, const struct fct_key *k)
{
    if (k->value && (k->code == FCT_KEY_UP || k->code == FCT_KEY_DOWN)) {   /* up and down: the sections */
        open_page((cur_page + (k->code == FCT_KEY_DOWN ? 1 : NPAGES - 1)) % NPAGES);
        fct_view_invalidate(v);
    }
}

int main(int argc, char **argv)
{
    if (fct_app_init() < 0)
        return 1;
    int first = 0;
    for (int i = 0; argc > 1 && i < NPAGES; i++)
        if (!strcmp(argv[1], pages[i].name))
            first = i;
    open_page(first);
    struct fct_window_attr a = { "Settings", FCT_POS_CENTER, FCT_POS_CENTER, SIDE_W + 560,
                                 36 + FCT_NSKINS * (CARD_H + 10) + 34, SIDE_W + 400, 300, 0 };
    struct fct_view *v = fct_view_create(&a);
    if (!v)
        return 1;
    v->draw = draw;
    v->mouse = mouse;
    v->key = key;
    return fct_main();
}
