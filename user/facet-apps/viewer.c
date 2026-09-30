/*
 * facet-viewer file - a text viewer with line numbers (up to 256 KB): arrows, Page Up/Down, Home/End, Space.
 * A Facet application (libfacet).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "common.h"

struct viewer {
    char *text;
    int *lines;          /* offsets of line starts */
    int nlines, first;
};

static void viewer_draw(struct fct_view *w, struct surface *s, struct rect c)
{
    struct viewer *v = w->app;
    struct rect t = rect_make(c.x, c.y, c.w - SB_W, c.h);
    gfx_fill(s, t.x, t.y, t.w, t.h, C_CONTENT);
    int vis = MAX(1, (c.h - 8) / FONT_H);
    struct rect clip = s->clip;
    gfx_set_clip(s, rect_intersect(clip, t));
    for (int i = v->first; i < v->nlines && i < v->first + vis + 1; i++) {
        int y = c.y + 4 + (i - v->first) * FONT_H;
        char num[8];
        snprintf(num, sizeof(num), "%4d", i + 1);
        gfx_text_mono(s, c.x + 4, y, num, C_FACE_SHADOW);
        const char *p = v->text + v->lines[i];
        int x = c.x + 44;
        for (; *p && *p != '\n' && x < t.x + t.w; p++) {
            unsigned char ch = *p;
            if (ch == '\t') {
                x += gfx_cell_w() * 4;
                continue;
            }
            gfx_char(s, x, y, ch >= 32 && ch < 127 ? ch : '.', C_TEXT, 0, false);
            x += gfx_cell_w();
        }
    }
    gfx_vline(s, c.x + 40, c.y, c.h, C_LINE);
    gfx_set_clip(s, clip);
    draw_scrollbar(s, rect_make(c.x + c.w - SB_W, c.y, SB_W, c.h), v->first, vis, v->nlines);
}

static void viewer_scroll(struct fct_view *w, int delta)
{
    struct viewer *v = w->app;
    int vis = MAX(1, (fct_view_content(w).h - 8) / FONT_H);
    v->first = MAX(0, MIN(MAX(0, v->nlines - vis), v->first + delta));
    fct_view_invalidate(w);
}

static void viewer_key(struct fct_view *w, const struct fct_key *ev)
{
    if (!ev->value)
        return;
    int vis = MAX(1, (fct_view_content(w).h - 8) / FONT_H);
    switch (ev->code) {
    case FCT_KEY_UP:   viewer_scroll(w, -1); break;
    case FCT_KEY_DOWN: viewer_scroll(w, 1); break;
    case FCT_KEY_PGUP: viewer_scroll(w, -vis); break;
    case FCT_KEY_PGDN: viewer_scroll(w, vis); break;
    case FCT_KEY_HOME: viewer_scroll(w, -1000000); break;
    case FCT_KEY_END:  viewer_scroll(w, 1000000); break;
    default:
        if (ev->ascii == ' ')
            viewer_scroll(w, vis);
    }
}

static void viewer_mouse(struct fct_view *w, int x, int y, int kind, int buttons)
{
    (void)buttons;
    if (kind != FCT_MOUSE_DOWN)
        return;
    struct viewer *v = w->app;
    struct rect c = fct_view_content(w);
    if (x >= c.w - SB_W) {
        int vis = MAX(1, (c.h - 8) / FONT_H);
        v->first = scrollbar_click(rect_make(c.x + c.w - SB_W, c.y, SB_W, c.h), c.y + y, v->first, vis, v->nlines);
        fct_view_invalidate(w);
    }
}

static void viewer_destroy(struct fct_view *w)
{
    struct viewer *v = w->app;
    free(v->text);
    free(v->lines);
    free(v);
}

static struct viewer *viewer_load(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        char line[96];
        snprintf(line, sizeof(line), "Cannot open %s", path);
        execl("/bin/facet-message", "facet-message", "Viewer", line, strerror(errno), (char *)NULL);
        return NULL;
    }
    struct viewer *v = calloc(1, sizeof(*v));
    size_t cap = 256 * 1024, len = 0;
    v->text = malloc(cap + 1);
    long n;
    while (len < cap && (n = read(fd, v->text + len, cap - len)) > 0)
        len += n;
    close(fd);
    v->text[len] = 0;
    int lcap = 256;
    v->lines = malloc(lcap * sizeof(int));
    v->lines[v->nlines++] = 0;
    for (size_t i = 0; i < len; i++) {
        if (v->text[i] == 0)
            v->text[i] = ' ';
        if (v->text[i] == '\n' && i + 1 < len) {
            if (v->nlines == lcap) {
                lcap *= 2;
                v->lines = realloc(v->lines, lcap * sizeof(int));
            }
            v->lines[v->nlines++] = i + 1;
        }
    }
    return v;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: facet-viewer file\n");
        return 2;
    }
    if (fct_app_init() < 0)
        return 1;
    const char *path = argv[1];
    struct viewer *v = viewer_load(path);
    if (!v)
        return 1;
    const char *base = strrchr(path, '/');
    char title[64];
    snprintf(title, sizeof(title), "Viewer - %s", base ? base + 1 : path);
    struct fct_view *w = fct_view_new(title, 600, 420);
    if (!w)
        return 1;
    w->app = v;
    w->draw = viewer_draw;
    w->key = viewer_key;
    w->mouse = viewer_mouse;
    w->destroy = viewer_destroy;
    return fct_main();
}
