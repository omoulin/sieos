/*
 * apps.c - Facet desktop applications: Files, Viewer, System Monitor,
 * Clock and About.
 */
#include "facet.h"

/* ------------------------------------------------------------------ */
/* Shared: vertical scroll bar                                         */
/* ------------------------------------------------------------------ */

#define SB_W 16

static void draw_scrollbar(struct surface *s, struct rect r, int first, int visible, int total)
{
    gfx_fill(s, r.x, r.y, r.w, r.h, color_shade(C_FACE, -20));
    ui_button(s, rect_make(r.x, r.y, r.w, SB_W), NULL, false);
    ui_button(s, rect_make(r.x, r.y + r.h - SB_W, r.w, SB_W), NULL, false);
    int cx = r.x + r.w / 2;
    gfx_triangle(s, cx - 4, r.y + 10, cx, r.y + 5, cx + 4, r.y + 10, C_TEXT);
    gfx_triangle(s, cx - 4, r.y + r.h - 10, cx, r.y + r.h - 5, cx + 4, r.y + r.h - 10, C_TEXT);
    int track = r.h - 2 * SB_W;
    if (total > visible && track > 10) {
        int th = MAX(12, track * visible / total);
        int ty = r.y + SB_W + (track - th) * first / MAX(1, total - visible);
        ui_button(s, rect_make(r.x + 1, ty, r.w - 2, th), NULL, false);
        gfx_hline(s, r.x + 4, ty + th / 2, r.w - 8, C_ACCENT);
    }
}

/* Returns the new first line after a click at y inside scroll bar r. */
static int scrollbar_click(struct rect r, int y, int first, int visible, int total)
{
    if (y < r.y + SB_W)
        return MAX(0, first - 3);
    if (y >= r.y + r.h - SB_W)
        return MAX(0, MIN(total - visible, first + 3));
    if (y < r.y + r.h / 2)
        return MAX(0, first - visible);
    return MAX(0, MIN(total - visible, first + visible));
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

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

static void files_title(struct window *w)
{
    struct files *f = w->app;
    snprintf(w->title, sizeof(w->title), "Files - %s", f->path);
}

static void files_go(struct window *w, const char *name)
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
    wm_invalidate(w);
}

static struct rect files_list_rect(struct rect c)
{
    return rect_make(c.x + 4, c.y + TOOLBAR_H + 2, c.w - 8 - SB_W, c.h - TOOLBAR_H - 24);
}

static int files_visible(struct rect c)
{
    return MAX(1, files_list_rect(c).h / ROW_H);
}

static void files_draw(struct window *w, struct surface *s, struct rect c)
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

static void files_open(struct window *w, int i)
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
        app_viewer(full);
    }
}

static void files_ensure_visible(struct window *w)
{
    struct files *f = w->app;
    int vis = files_visible(wm_content(w));
    if (f->sel < f->first)
        f->first = f->sel;
    if (f->sel >= f->first + vis)
        f->first = f->sel - vis + 1;
}

static void files_mouse(struct window *w, int x, int y, int kind, int buttons)
{
    (void)buttons;
    struct files *f = w->app;
    if (kind != MOUSE_DOWN && kind != MOUSE_DOUBLE)
        return;
    struct rect c = wm_content(w);
    int sx = c.x + x, sy = c.y + y;
    if (y < TOOLBAR_H) {
        if (x >= 4 && x < 56)
            files_go(w, "..");
        else if (x >= 60 && x < 120)
            files_go(w, getenv("HOME") ? getenv("HOME") : "/");
        else if (x >= 124 && x < 188) {
            files_load(f);
            wm_invalidate(w);
        }
        return;
    }
    struct rect l = files_list_rect(c);
    struct rect sb = rect_make(l.x + l.w + 1, l.y - 1, SB_W, l.h + 2);
    if (rect_contains(sb, sx, sy)) {
        f->first = scrollbar_click(sb, sy, f->first, files_visible(c), f->n);
        wm_invalidate(w);
        return;
    }
    if (!rect_contains(l, sx, sy))
        return;
    int i = f->first + (sy - l.y) / ROW_H;
    if (i >= f->n)
        return;
    f->sel = i;
    wm_invalidate(w);
    if (kind == MOUSE_DOUBLE)
        files_open(w, i);
}

static void files_key(struct window *w, const struct input_event *ev)
{
    struct files *f = w->app;
    if (ev->type != EV_KEY || !ev->value)
        return;
    if (ev->code == KEY_UP && f->sel > 0)
        f->sel--;
    else if (ev->code == KEY_DOWN && f->sel + 1 < f->n)
        f->sel++;
    else if (ev->ascii == '\n')
        files_open(w, f->sel);
    else if (ev->ascii == '\b')
        files_go(w, "..");
    else
        return;
    files_ensure_visible(w);
    wm_invalidate(w);
}

static void files_destroy(struct window *w)
{
    struct files *f = w->app;
    free(f->e);
    free(f);
}

void app_files(const char *path)
{
    struct files *f = calloc(1, sizeof(*f));
    if (!f)
        return;
    strlcpy(f->path, path, sizeof(f->path));
    files_load(f);
    struct window *w = wm_create("Files", -1, -1, 460, 380);
    if (!w) {
        files_destroy(&(struct window){ .app = f });
        return;
    }
    w->app = f;
    w->draw = files_draw;
    w->mouse = files_mouse;
    w->key = files_key;
    w->destroy = files_destroy;
    w->min_w = 360;
    w->min_h = 200;
    files_title(w);
}

/* ------------------------------------------------------------------ */
/* Viewer                                                              */
/* ------------------------------------------------------------------ */

struct viewer {
    char *text;
    int *lines;          /* offsets of line starts */
    int nlines, first;
};

static void viewer_draw(struct window *w, struct surface *s, struct rect c)
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
        gfx_text(s, c.x + 4, y, num, C_FACE_SHADOW);
        const char *p = v->text + v->lines[i];
        int x = c.x + 44;
        for (; *p && *p != '\n' && x < t.x + t.w; p++) {
            unsigned char ch = *p;
            if (ch == '\t') {
                x += FONT_W * 4;
                continue;
            }
            gfx_char(s, x, y, ch >= 32 && ch < 127 ? ch : '.', C_TEXT, 0, false);
            x += FONT_W;
        }
    }
    gfx_vline(s, c.x + 40, c.y, c.h, C_LINE);
    gfx_set_clip(s, clip);
    draw_scrollbar(s, rect_make(c.x + c.w - SB_W, c.y, SB_W, c.h), v->first, vis, v->nlines);
}

static void viewer_scroll(struct window *w, int delta)
{
    struct viewer *v = w->app;
    int vis = MAX(1, (wm_content(w).h - 8) / FONT_H);
    v->first = MAX(0, MIN(MAX(0, v->nlines - vis), v->first + delta));
    wm_invalidate(w);
}

static void viewer_key(struct window *w, const struct input_event *ev)
{
    if (ev->type != EV_KEY || !ev->value)
        return;
    int vis = MAX(1, (wm_content(w).h - 8) / FONT_H);
    switch (ev->code) {
    case KEY_UP:   viewer_scroll(w, -1); break;
    case KEY_DOWN: viewer_scroll(w, 1); break;
    case KEY_PGUP: viewer_scroll(w, -vis); break;
    case KEY_PGDN: viewer_scroll(w, vis); break;
    case KEY_HOME: viewer_scroll(w, -1000000); break;
    case KEY_END:  viewer_scroll(w, 1000000); break;
    default:
        if (ev->ascii == ' ')
            viewer_scroll(w, vis);
    }
}

static void viewer_mouse(struct window *w, int x, int y, int kind, int buttons)
{
    (void)buttons;
    if (kind != MOUSE_DOWN)
        return;
    struct viewer *v = w->app;
    struct rect c = wm_content(w);
    if (x >= c.w - SB_W) {
        int vis = MAX(1, (c.h - 8) / FONT_H);
        v->first = scrollbar_click(rect_make(c.x + c.w - SB_W, c.y, SB_W, c.h), c.y + y, v->first, vis, v->nlines);
        wm_invalidate(w);
    }
}

static void viewer_destroy(struct window *w)
{
    struct viewer *v = w->app;
    free(v->text);
    free(v->lines);
    free(v);
}

/* ------------------------------------------------------------------ */
/* Message dialog                                                      */
/* ------------------------------------------------------------------ */

struct message {
    char text[2][96];
};

static struct rect message_ok(struct rect c)
{
    return rect_make(c.x + c.w - 90, c.y + c.h - 36, 76, 26);
}

static void message_draw(struct window *w, struct surface *s, struct rect c)
{
    struct message *m = w->app;
    gfx_fill(s, c.x, c.y, c.w, c.h, C_FACE);
    icon_draw(s, ICON_INFO, c.x + 14, c.y + 14, 40);
    gfx_text(s, c.x + 70, c.y + 20, m->text[0], C_TEXT);
    gfx_text(s, c.x + 70, c.y + 40, m->text[1], C_DIM);
    ui_button(s, message_ok(c), "OK", false);
}

static void message_mouse(struct window *w, int x, int y, int kind, int buttons)
{
    (void)buttons;
    struct rect c = wm_content(w);
    if (kind == MOUSE_UP && rect_contains(message_ok(c), c.x + x, c.y + y))
        wm_close(w);
}

static void message_key(struct window *w, const struct input_event *ev)
{
    if (ev->type == EV_KEY && ev->value && (ev->ascii == '\n' || ev->ascii == 27 || ev->ascii == ' '))
        wm_close(w);
}

static void message_destroy(struct window *w)
{
    free(w->app);
}

void app_message(const char *title, const char *line1, const char *line2)
{
    struct message *m = calloc(1, sizeof(*m));
    strlcpy(m->text[0], line1, sizeof(m->text[0]));
    strlcpy(m->text[1], line2 ? line2 : "", sizeof(m->text[1]));
    int w0 = MAX(text_width(m->text[0]), text_width(m->text[1])) + 100;
    struct window *w = wm_create(title, (screen_w - w0) / 2, screen_h / 3, MAX(300, w0), 110);
    if (!w) {
        free(m);
        return;
    }
    w->app = m;
    w->draw = message_draw;
    w->mouse = message_mouse;
    w->key = message_key;
    w->destroy = message_destroy;
}

void app_viewer(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        char line[96];
        snprintf(line, sizeof(line), "Cannot open %s", path);
        app_message("Viewer", line, strerror(errno));
        return;
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
    const char *base = strrchr(path, '/');
    char title[64];
    snprintf(title, sizeof(title), "Viewer - %s", base ? base + 1 : path);
    struct window *w = wm_create(title, -1, -1, 600, 420);
    if (!w) {
        viewer_destroy(&(struct window){ .app = v });
        return;
    }
    w->app = v;
    w->draw = viewer_draw;
    w->key = viewer_key;
    w->mouse = viewer_mouse;
    w->destroy = viewer_destroy;
}

/* ------------------------------------------------------------------ */
/* System Monitor                                                      */
/* ------------------------------------------------------------------ */

#define HIST 60

struct monitor {
    int hist[16][HIST];
    int pos, ticks;
    struct meminfo mem;
    struct procinfo procs[64];
    int nprocs;
};

static void monitor_sample(struct monitor *m)
{
    int n = wm_ncpus();
    for (int i = 0; i < n && i < 16; i++)
        m->hist[i][m->pos] = wm_cpu_percent(i);
    m->pos = (m->pos + 1) % HIST;
    meminfo(&m->mem);
    m->nprocs = procinfo(m->procs, 64);
}

static void monitor_draw(struct window *w, struct surface *s, struct rect c)
{
    struct monitor *m = w->app;
    gfx_fill(s, c.x, c.y, c.w, c.h, C_FACE);
    int n = wm_ncpus(), y = c.y + 8;
    gfx_text(s, c.x + 10, y, "Processors", C_TEXT);
    y += 20;
    int gw = c.w - 190;
    for (int i = 0; i < n; i++) {
        char label[16];
        int pct = m->hist[i][(m->pos + HIST - 1) % HIST];
        snprintf(label, sizeof(label), "CPU %d", i);
        gfx_text(s, c.x + 10, y + 4, label, C_TEXT);
        ui_meter(s, rect_make(c.x + 60, y + 4, 70, 16), pct, pct > 80 ? C_ACCENT : RGB(0x6C, 0xC0, 0x8A));
        snprintf(label, sizeof(label), "%3d%%", pct);
        gfx_text(s, c.x + 136, y + 4, label, C_TEXT);
        /* history graph */
        struct rect g = rect_make(c.x + 176, y, gw, 24);
        gfx_fill(s, g.x, g.y, g.w, g.h, RGB(0x1A, 0x22, 0x26));
        gfx_bevel(s, g.x, g.y, g.w, g.h, 1, false, C_LINE, C_FACE_DARK);
        for (int k = 1; k < 4; k++)
            gfx_hline(s, g.x + 1, g.y + k * g.h / 4, g.w - 2, RGB(0x28, 0x34, 0x38));
        int px = -1, py = 0;
        for (int k = 0; k < HIST; k++) {
            int v = m->hist[i][(m->pos + k) % HIST];
            int xx = g.x + 2 + k * (g.w - 4) / (HIST - 1), yy = g.y + g.h - 3 - v * (g.h - 5) / 100;
            if (px >= 0)
                gfx_line(s, px, py, xx, yy, RGB(0x7C, 0xE0, 0x9A));
            px = xx;
            py = yy;
        }
        y += 30;
    }
    /* memory */
    y += 6;
    unsigned long used = m->mem.total_kb - m->mem.free_kb;
    char line[96];
    snprintf(line, sizeof(line), "Memory  %lu / %lu MiB", used / 1024, m->mem.total_kb / 1024);
    gfx_text(s, c.x + 10, y, line, C_TEXT);
    ui_meter(s, rect_make(c.x + 230, y, c.w - 240, 16),
             m->mem.total_kb ? (int)(used * 100 / m->mem.total_kb) : 0, C_TITLE_A1);
    y += 28;
    /* process table */
    struct rect tbl = rect_make(c.x + 8, y, c.w - 16, c.y + c.h - y - 8);
    gfx_fill(s, tbl.x, tbl.y, tbl.w, tbl.h, C_CONTENT);
    gfx_bevel(s, tbl.x, tbl.y, tbl.w, tbl.h, 1, false, C_LINE, C_FACE_DARK);
    gfx_fill(s, tbl.x + 1, tbl.y + 1, tbl.w - 2, 18, color_shade(C_FACE, 10));
    gfx_text(s, tbl.x + 6, tbl.y + 2, "  PID USER     STATE   CPU  MEM(KB) COMMAND", C_TEXT);
    struct rect clip = s->clip;
    gfx_set_clip(s, rect_intersect(clip, tbl));
    static const char *states[] = { "?", "new", "ready", "run", "sleep", "zombie", "stopped" };
    int row = 0;
    for (int i = 0; i < m->nprocs; i++) {
        struct procinfo *p = &m->procs[i];
        if (p->pid == 0)
            continue;
        struct passwd *pw = getpwuid(p->euid);
        char cpu[6];
        if (p->cpu >= 0)
            snprintf(cpu, sizeof(cpu), "%d", p->cpu);
        else
            strcpy(cpu, "-");
        snprintf(line, sizeof(line), "%5d %-8s %-7s %3s %8lu %s", p->pid, pw ? pw->pw_name : "?",
                 p->state < 7 ? states[p->state] : "?", cpu, p->mem_kb, p->name);
        int ry = tbl.y + 20 + row * 17;
        if (p->state == PSTATE_RUNNING)
            gfx_fill(s, tbl.x + 1, ry, tbl.w - 2, 17, C_SELECT);
        gfx_text(s, tbl.x + 6, ry, line, C_TEXT);
        row++;
    }
    gfx_set_clip(s, clip);
}

static void monitor_tick(struct window *w)
{
    struct monitor *m = w->app;
    if (++m->ticks % 4)
        return;
    monitor_sample(m);
    wm_invalidate_rect(wm_content(w));
}

static void monitor_destroy(struct window *w)
{
    free(w->app);
}

void app_monitor(void)
{
    struct monitor *m = calloc(1, sizeof(*m));
    if (!m)
        return;
    monitor_sample(m);
    int n = wm_ncpus();
    struct window *w = wm_create("System Monitor", -1, -1, 560, 36 + n * 30 + 40 + 220);
    if (!w) {
        free(m);
        return;
    }
    w->app = m;
    w->draw = monitor_draw;
    w->tick = monitor_tick;
    w->destroy = monitor_destroy;
    w->min_w = 420;
    w->min_h = 240;
}

/* ------------------------------------------------------------------ */
/* Clock                                                               */
/* ------------------------------------------------------------------ */

/* sin(6k degrees) * 1000 for k = 0..15 */
static const int sin6[16] = { 0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995, 1000 };

static int isin60(int k)                 /* k in 0..59 : sin(k*6deg)*1000 */
{
    k = ((k % 60) + 60) % 60;
    if (k <= 15) return sin6[k];
    if (k <= 30) return sin6[30 - k];
    if (k <= 45) return -sin6[k - 30];
    return -sin6[60 - k];
}

static int icos60(int k) { return isin60(k + 15); }

struct clockapp {
    long last;
};

static void hand(struct surface *s, int cx, int cy, int pos60, int len, int thick, color_t c)
{
    int x = cx + isin60(pos60) * len / 1000, y = cy - icos60(pos60) * len / 1000;
    gfx_thick_line(s, cx, cy, x, y, thick, c);
}

static void clock_draw(struct window *w, struct surface *s, struct rect c)
{
    (void)w;
    gfx_vgradient(s, c.x, c.y, c.w, c.h, C_CONTENT_ALT, C_CONTENT);
    int r = MIN(c.w, c.h - 30) / 2 - 12;
    int cx = c.x + c.w / 2, cy = c.y + 12 + r;
    gfx_disc(s, cx + 3, cy + 3, r + 4, RGB(0x0C, 0x0D, 0x0F));
    gfx_disc(s, cx, cy, r + 4, C_TITLE_A2);
    gfx_disc(s, cx, cy, r, RGB(0x22, 0x25, 0x2A));
    for (int k = 0; k < 60; k++) {
        int outer = r - 4, inner = k % 5 ? r - 8 : r - 16;
        int x0 = cx + isin60(k) * inner / 1000, y0 = cy - icos60(k) * inner / 1000;
        int x1 = cx + isin60(k) * outer / 1000, y1 = cy - icos60(k) * outer / 1000;
        gfx_thick_line(s, x0, y0, x1, y1, k % 5 ? 1 : 3, k % 15 ? C_TEXT : C_ACCENT);
    }
    long now = time(NULL);
    struct tm tm;
    time_t now_t = now;
    gmtime_r(&now_t, &tm);
    int hpos = (tm.tm_hour % 12) * 5 + tm.tm_min / 12;
    hand(s, cx, cy, hpos, r * 50 / 100, 5, C_TEXT);
    hand(s, cx, cy, tm.tm_min, r * 75 / 100, 3, C_TEXT);
    hand(s, cx, cy, tm.tm_sec, r * 85 / 100, 1, RGB(0xC9, 0x4B, 0x3F));
    gfx_disc(s, cx, cy, 5, C_ACCENT);
    static const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    char date[64];
    snprintf(date, sizeof(date), "%s %04d-%02d-%02d  %02d:%02d:%02d UTC", days[tm.tm_wday], tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    gfx_text(s, c.x + (c.w - text_width(date)) / 2, c.y + c.h - 22, date, C_TEXT);
}

static void clock_tick(struct window *w)
{
    struct clockapp *ca = w->app;
    long now = time(NULL);
    if (now != ca->last) {
        ca->last = now;
        wm_invalidate_rect(wm_content(w));
    }
}

static void clock_destroy(struct window *w)
{
    free(w->app);
}

void app_clock(void)
{
    struct clockapp *ca = calloc(1, sizeof(*ca));
    struct window *w = wm_create("Clock", -1, -1, 300, 330);
    if (!w) {
        free(ca);
        return;
    }
    w->app = ca;
    w->draw = clock_draw;
    w->tick = clock_tick;
    w->destroy = clock_destroy;
    w->min_w = 200;
    w->min_h = 220;
}

/* ------------------------------------------------------------------ */
/* About                                                               */
/* ------------------------------------------------------------------ */

static void about_draw(struct window *w, struct surface *s, struct rect c)
{
    (void)w;
    gfx_vgradient(s, c.x, c.y, c.w, c.h, C_CONTENT_ALT, C_CONTENT);
    logo_draw(s, c.x + 72, c.y + 66, 100);
    gfx_text_scaled(s, c.x + 140, c.y + 22, "SIEOS", 4, C_TITLE_A2);
    gfx_text(s, c.x + 142, c.y + 84, "Synthetic Intelligence", C_TEXT);
    gfx_text(s, c.x + 142, c.y + 102, "Enhanced Operating System", C_TEXT);
    struct utsname u;
    uname(&u);
    struct meminfo mi;
    meminfo(&mi);
    char line[96];
    int y = c.y + 140;
    snprintf(line, sizeof(line), "Facet Desktop 2.0 on %s %s (%s)", u.sysname, u.release, u.machine);
    gfx_text(s, c.x + 24, y, line, C_TEXT);
    snprintf(line, sizeof(line), "Processors: %d    Memory: %lu MiB", wm_ncpus(), mi.total_kb / 1024);
    gfx_text(s, c.x + 24, y + 20, line, C_TEXT);
    snprintf(line, sizeof(line), "Logged in as: %s", desktop_user);
    gfx_text(s, c.x + 24, y + 40, line, C_TEXT);
    gfx_hline(s, c.x + 24, y + 66, c.w - 48, C_FACE_SHADOW);
    gfx_text(s, c.x + 24, y + 76, "Start programs from the dock on the left, or type in", C_DIM);
    gfx_text(s, c.x + 24, y + 94, "the sia strip at the top (Ctrl+Space).", C_DIM);
    gfx_text(s, c.x + 24, y + 112, "Ctrl+Alt+1..4 switch workspaces; Alt+Tab windows.", C_DIM);
}

void app_about(void)
{
    struct window *w = wm_create("About SIEOS", screen_w - 580, screen_h - 390, 440, 290);
    if (!w)
        return;
    w->draw = about_draw;
}

/* ------------------------------------------------------------------ */
/* Network Status                                                      */
/* ------------------------------------------------------------------ */

#define NET_HIST 60

struct netapp {
    struct netinfo ni;
    bool ok;
    unsigned long prev_rx, prev_tx;
    int rx_rate[NET_HIST], tx_rate[NET_HIST];   /* bytes per second */
    int pos, ticks;
    struct sockinfo socks[32];
    int nsocks;
};

static void netapp_sample(struct netapp *a)
{
    a->ok = netinfo(&a->ni) == 0;
    if (a->ok) {
        if (a->prev_rx || a->prev_tx) {
            a->rx_rate[a->pos] = (int)(a->ni.rx_bytes - a->prev_rx);
            a->tx_rate[a->pos] = (int)(a->ni.tx_bytes - a->prev_tx);
            a->pos = (a->pos + 1) % NET_HIST;
        }
        a->prev_rx = a->ni.rx_bytes;
        a->prev_tx = a->ni.tx_bytes;
    }
    a->nsocks = netstat(a->socks, 32);
    if (a->nsocks < 0)
        a->nsocks = 0;
}

static void netapp_draw(struct window *w, struct surface *s, struct rect c)
{
    struct netapp *a = w->app;
    gfx_fill(s, c.x, c.y, c.w, c.h, C_FACE);
    icon_draw(s, ICON_NETWORK, c.x + 12, c.y + 10, 40);
    char line[96], b1[16], b2[16], b3[16];
    if (!a->ok) {
        gfx_text(s, c.x + 64, c.y + 20, "No network interface.", C_TEXT);
        return;
    }
    snprintf(line, sizeof(line), "eth0  %s  (%s, %s)", ip_to_str(a->ni.ip, b1), a->ni.driver,
             a->ni.dhcp ? "DHCP" : "static");
    gfx_text(s, c.x + 64, c.y + 10, line, C_TEXT);
    snprintf(line, sizeof(line), "netmask %s   gateway %s", ip_to_str(a->ni.netmask, b1), ip_to_str(a->ni.gateway, b2));
    gfx_text(s, c.x + 64, c.y + 28, line, C_DIM);
    snprintf(line, sizeof(line), "dns %s   mac %02x:%02x:%02x:%02x:%02x:%02x", ip_to_str(a->ni.dns, b3),
             a->ni.mac[0], a->ni.mac[1], a->ni.mac[2], a->ni.mac[3], a->ni.mac[4], a->ni.mac[5]);
    gfx_text(s, c.x + 64, c.y + 46, line, C_DIM);

    /* traffic graph */
    int gy = c.y + 72, gh = 90;
    struct rect g = rect_make(c.x + 10, gy, c.w - 20, gh);
    gfx_fill(s, g.x, g.y, g.w, g.h, RGB(0x10, 0x18, 0x20));
    gfx_bevel(s, g.x, g.y, g.w, g.h, 1, false, C_LINE, C_FACE_DARK);
    int peak = 1024;
    for (int k = 0; k < NET_HIST; k++)
        peak = MAX(peak, MAX(a->rx_rate[k], a->tx_rate[k]));
    for (int pass = 0; pass < 2; pass++) {
        int *rate = pass ? a->tx_rate : a->rx_rate;
        color_t col = pass ? RGB(0xFF, 0xB0, 0x30) : RGB(0x60, 0xE0, 0x70);
        int px = -1, py = 0;
        for (int k = 0; k < NET_HIST; k++) {
            int v = rate[(a->pos + k) % NET_HIST];
            int xx = g.x + 2 + k * (g.w - 4) / (NET_HIST - 1), yy = g.y + g.h - 3 - (int)((long)v * (g.h - 6) / peak);
            if (px >= 0)
                gfx_line(s, px, py, xx, yy, col);
            px = xx;
            py = yy;
        }
    }
    int last = (a->pos + NET_HIST - 1) % NET_HIST;
    snprintf(line, sizeof(line), "in %d B/s", a->rx_rate[last]);
    gfx_text(s, g.x + 6, g.y + 4, line, RGB(0x60, 0xE0, 0x70));
    snprintf(line, sizeof(line), "out %d B/s", a->tx_rate[last]);
    gfx_text(s, g.x + 130, g.y + 4, line, RGB(0xFF, 0xB0, 0x30));

    snprintf(line, sizeof(line), "RX %lu packets, %lu KB     TX %lu packets, %lu KB", a->ni.rx_packets,
             a->ni.rx_bytes / 1024, a->ni.tx_packets, a->ni.tx_bytes / 1024);
    gfx_text(s, c.x + 10, gy + gh + 8, line, C_TEXT);

    /* sockets */
    static const char *states[] = { "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT_1",
                                    "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT" };
    struct rect t = rect_make(c.x + 10, gy + gh + 30, c.w - 20, c.y + c.h - (gy + gh + 30) - 10);
    gfx_fill(s, t.x, t.y, t.w, t.h, C_CONTENT);
    gfx_bevel(s, t.x, t.y, t.w, t.h, 1, false, C_LINE, C_FACE_DARK);
    gfx_fill(s, t.x + 1, t.y + 1, t.w - 2, 18, RGB(0x26, 0x29, 0x2E));
    gfx_text(s, t.x + 6, t.y + 2, "Proto Local                 Remote                State", C_TEXT);
    struct rect clip = s->clip;
    gfx_set_clip(s, rect_intersect(clip, t));
    for (int i = 0; i < a->nsocks; i++) {
        struct sockinfo *si = &a->socks[i];
        char l[28], r[28];
        snprintf(l, sizeof(l), "%s:%u", si->lip ? ip_to_str(si->lip, b1) : "*", si->lport);
        if (si->rip || si->rport)
            snprintf(r, sizeof(r), "%s:%u", ip_to_str(si->rip, b2), si->rport);
        else
            strcpy(r, "*:*");
        snprintf(line, sizeof(line), "%-5s %-21s %-21s %s",
                 si->proto == IPPROTO_TCP ? "tcp" : si->proto == IPPROTO_UDP ? "udp" : "raw", l, r,
                 si->proto == IPPROTO_TCP && si->state <= 10 ? states[si->state] : "");
        gfx_text(s, t.x + 6, t.y + 21 + i * 17, line, C_TEXT);
    }
    gfx_set_clip(s, clip);
}

static void netapp_tick(struct window *w)
{
    struct netapp *a = w->app;
    if (++a->ticks % 4)
        return;
    netapp_sample(a);
    wm_invalidate_rect(wm_content(w));
}

static void netapp_destroy(struct window *w)
{
    free(w->app);
}

void app_network(void)
{
    struct netapp *a = calloc(1, sizeof(*a));
    if (!a)
        return;
    netapp_sample(a);
    struct window *w = wm_create("Network Status", -1, -1, 520, 400);
    if (!w) {
        free(a);
        return;
    }
    w->app = a;
    w->draw = netapp_draw;
    w->tick = netapp_tick;
    w->destroy = netapp_destroy;
    w->min_w = 460;
    w->min_h = 300;
}
