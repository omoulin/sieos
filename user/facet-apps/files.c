/*
 * facet-files [directory] - the Files browser: folders first, double-click or Enter opens (files in facet-viewer), Backspace goes up.
 * A Facet application (libfacet).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "common.h"

struct fentry {
    char name[64];
    unsigned mode;
    unsigned long size;
};

struct files {
    char path[256];
    struct fentry *e;
    int n, sel, first;
    char status[80];
};

#define TOOLBAR_H 32
#define ROW_H 20

static int fentry_cmp(const struct fentry *a, const struct fentry *b)
{
    bool ad = S_ISDIR(a->mode), bd = S_ISDIR(b->mode);
    if (ad != bd)
        return ad ? -1 : 1;
    return strcmp(a->name, b->name);
}

static void join_path(char *out, size_t n, const char *dir, const char *name)
{
    if (!strcmp(dir, "/"))
        snprintf(out, n, "/%s", name);
    else
        snprintf(out, n, "%s/%s", dir, name);
}

static void files_load(struct files *f)
{
    free(f->e);
    f->e = NULL;
    f->n = f->sel = f->first = 0;
    int fd = open(f->path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        snprintf(f->status, sizeof(f->status), "%s", strerror(errno));
        return;
    }
    int cap = 64;
    f->e = malloc(cap * sizeof(struct fentry));
    char buf[2048];
    long got;
    while ((got = getdents(fd, (struct dirent *)buf, sizeof(buf))) > 0) {
        for (long off = 0; off < got;) {
            struct dirent *d = (struct dirent *)(buf + off);
            off += d->d_reclen;
            if (!strcmp(d->d_name, ".") || (!strcmp(d->d_name, "..") && !strcmp(f->path, "/")))
                continue;
            if (f->n == cap) {
                cap *= 2;
                f->e = realloc(f->e, cap * sizeof(struct fentry));
            }
            struct fentry *e = &f->e[f->n++];
            strlcpy(e->name, d->d_name, sizeof(e->name));
            char full[320];
            join_path(full, sizeof(full), f->path, d->d_name);
            struct stat st;
            if (stat(full, &st) == 0) {
                e->mode = st.st_mode;
                e->size = st.st_size;
            } else {
                e->mode = d->d_type == DT_DIR ? S_IFDIR : S_IFREG;
                e->size = 0;
            }
        }
    }
    close(fd);
    /* insertion sort: directories first, then by name */
    for (int i = 1; i < f->n; i++) {
        struct fentry tmp = f->e[i];
        int j = i - 1;
        while (j >= 0 && fentry_cmp(&f->e[j], &tmp) > 0) {
            f->e[j + 1] = f->e[j];
            j--;
        }
        f->e[j + 1] = tmp;
    }
    snprintf(f->status, sizeof(f->status), "%d item%s", f->n, f->n == 1 ? "" : "s");
}

static void files_title(struct fct_view *w)
{
    struct files *f = w->app;
    char t[FCT_TITLE_MAX];
    snprintf(t, sizeof(t), "Files - %s", f->path);
    fct_view_set_title(w, t);
}

static void files_go(struct fct_view *w, const char *name)
{
    struct files *f = w->app;
    char np[256];
    if (!strcmp(name, "..")) {
        strlcpy(np, f->path, sizeof(np));
        char *slash = strrchr(np, '/');
        if (slash && slash != np)
            *slash = 0;
        else
            strcpy(np, "/");
    } else if (name[0] == '/') {
        strlcpy(np, name, sizeof(np));
    } else {
        join_path(np, sizeof(np), f->path, name);
    }
    strlcpy(f->path, np, sizeof(f->path));
    files_load(f);
    files_title(w);
    fct_view_invalidate(w);
}

static struct rect files_list_rect(struct rect c)
{
    return rect_make(c.x + 4, c.y + TOOLBAR_H + 2, c.w - 8 - SB_W, c.h - TOOLBAR_H - 24);
}

static int files_visible(struct rect c)
{
    return MAX(1, files_list_rect(c).h / ROW_H);
}

static void files_draw(struct fct_view *w, struct surface *s, struct rect c)
{
    struct files *f = w->app;
    gfx_fill(s, c.x, c.y, c.w, c.h, C_FACE);
    /* toolbar */
    ui_button(s, rect_make(c.x + 4, c.y + 4, 52, 24), "Up", false);
    ui_button(s, rect_make(c.x + 60, c.y + 4, 60, 24), "Home", false);
    ui_button(s, rect_make(c.x + 124, c.y + 4, 64, 24), "Reload", false);
    struct rect pathbox = rect_make(c.x + 194, c.y + 5, c.w - 198, 22);
    ui_panel(s, pathbox, true);
    struct rect clip = s->clip;
    gfx_set_clip(s, rect_intersect(clip, pathbox));
    gfx_text(s, pathbox.x + 6, pathbox.y + 3, f->path, C_TEXT);
    gfx_set_clip(s, clip);

    /* list */
    struct rect l = files_list_rect(c);
    gfx_fill(s, l.x, l.y, l.w, l.h, C_CONTENT);
    gfx_bevel(s, l.x - 1, l.y - 1, l.w + 2, l.h + 2, 1, false, C_LINE, C_FACE_DARK);
    gfx_set_clip(s, rect_intersect(clip, l));
    int vis = files_visible(c);
    for (int i = f->first; i < f->n && i < f->first + vis + 1; i++) {
        struct fentry *e = &f->e[i];
        int y = l.y + (i - f->first) * ROW_H;
        bool sel = i == f->sel;
        if (sel)
            gfx_fill(s, l.x, y, l.w, ROW_H, C_SELECT);
        else if (i & 1)
            gfx_fill(s, l.x, y, l.w, ROW_H, C_CONTENT_ALT);
        int kind = S_ISDIR(e->mode) ? ICON_FOLDER : (e->mode & 0111) ? ICON_PROGRAM : ICON_FILE;
        icon_draw(s, kind, l.x + 4, y + 1, 18);
        color_t ink = sel ? RGB(255, 255, 255) : C_TEXT;
        gfx_text(s, l.x + 28, y + 2, e->name, ink);
        char info[24];
        if (S_ISDIR(e->mode))
            strcpy(info, "folder");
        else if (S_ISCHR(e->mode))
            strcpy(info, "device");
        else if (e->size >= 10240)
            snprintf(info, sizeof(info), "%lu KB", e->size / 1024);
        else
            snprintf(info, sizeof(info), "%lu B", e->size);
        gfx_text(s, l.x + l.w - text_width(info) - 8, y + 2, info, sel ? RGB(255, 240, 200) : C_FACE_SHADOW);
    }
    gfx_set_clip(s, clip);
    draw_scrollbar(s, rect_make(l.x + l.w + 1, l.y - 1, SB_W, l.h + 2), f->first, vis, f->n);
    /* status line */
    gfx_text(s, c.x + 8, c.y + c.h - 19, f->status, C_TEXT);
}

static void files_open(struct fct_view *w, int i)
{
    struct files *f = w->app;
    if (i < 0 || i >= f->n)
        return;
    struct fentry *e = &f->e[i];
    if (S_ISDIR(e->mode)) {
        char name[64];
        strlcpy(name, e->name, sizeof(name));
        files_go(w, name);
    } else if (S_ISREG(e->mode)) {
        char full[320];
        join_path(full, sizeof(full), f->path, e->name);
        if (fork() == 0) {                    /* the viewer is a program too */
            execl("/bin/facet-viewer", "facet-viewer", full, (char *)NULL);
            _exit(127);
        }
    }
}

static void files_ensure_visible(struct fct_view *w)
{
    struct files *f = w->app;
    int vis = files_visible(fct_view_content(w));
    if (f->sel < f->first)
        f->first = f->sel;
    if (f->sel >= f->first + vis)
        f->first = f->sel - vis + 1;
}

static void files_mouse(struct fct_view *w, int x, int y, int kind, int buttons)
{
    (void)buttons;
    struct files *f = w->app;
    if (kind != FCT_MOUSE_DOWN && kind != FCT_MOUSE_DOUBLE)
        return;
    struct rect c = fct_view_content(w);
    int sx = c.x + x, sy = c.y + y;
    if (y < TOOLBAR_H) {
        if (x >= 4 && x < 56)
            files_go(w, "..");
        else if (x >= 60 && x < 120)
            files_go(w, getenv("HOME") ? getenv("HOME") : "/");
        else if (x >= 124 && x < 188) {
            files_load(f);
            fct_view_invalidate(w);
        }
        return;
    }
    struct rect l = files_list_rect(c);
    struct rect sb = rect_make(l.x + l.w + 1, l.y - 1, SB_W, l.h + 2);
    if (rect_contains(sb, sx, sy)) {
        f->first = scrollbar_click(sb, sy, f->first, files_visible(c), f->n);
        fct_view_invalidate(w);
        return;
    }
    if (!rect_contains(l, sx, sy))
        return;
    int i = f->first + (sy - l.y) / ROW_H;
    if (i >= f->n)
        return;
    f->sel = i;
    fct_view_invalidate(w);
    if (kind == FCT_MOUSE_DOUBLE)
        files_open(w, i);
}

static void files_key(struct fct_view *w, const struct fct_key *ev)
{
    struct files *f = w->app;
    if (!ev->value)
        return;
    if (ev->code == FCT_KEY_UP && f->sel > 0)
        f->sel--;
    else if (ev->code == FCT_KEY_DOWN && f->sel + 1 < f->n)
        f->sel++;
    else if (ev->ascii == '\n')
        files_open(w, f->sel);
    else if (ev->ascii == '\b')
        files_go(w, "..");
    else
        return;
    files_ensure_visible(w);
    fct_view_invalidate(w);
}

static void files_destroy(struct fct_view *w)
{
    struct files *f = w->app;
    free(f->e);
    free(f);
}

int main(int argc, char **argv)
{
    signal(SIGCHLD, SIG_IGN);                    /* viewers need no waiting for */
    struct files *f = calloc(1, sizeof(*f));
    if (!f || fct_app_init() < 0)
        return 1;
    strlcpy(f->path, argc > 1 ? argv[1] : getenv("HOME") ? getenv("HOME") : "/", sizeof(f->path));
    files_load(f);
    struct fct_window_attr a = { "Files", FCT_POS_AUTO, FCT_POS_AUTO, 460, 380, 354, 171, 0 };
    struct fct_view *w = fct_view_create(&a);
    if (!w)
        return 1;
    w->app = f;
    w->draw = files_draw;
    w->mouse = files_mouse;
    w->key = files_key;
    w->destroy = files_destroy;
    files_title(w);
    return fct_main();
}
