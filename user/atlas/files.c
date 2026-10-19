/*
 * files.c - The Files window (docs/desktop.md): a folder's contents, one
 * row each (icon, name, kind, size) under a header, folders first, every
 * other row tinted. Double-click (or Enter): a folder opens in place, a
 * file in the editor, a launcher (.app) starts. Up (or Backspace) goes to
 * the parent; a click on a part of the path goes there. Snapshots opens
 * the hidden .snapshots folder of the volume this folder is in
 * (docs/siefs.md): old versions of everything, read only. It starts in the
 * home folder; it shows only what the logged-in user may read.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"

/* Where "/.snapshots" starts in a path, or 0. */
static char *strstr_snap(char *p)
{
    for (; *p; p++) if (!memcmp(p, "/.snapshots", 11) && (p[11] == '/' || !p[11])) return p;
    return 0;
}

#define TOP 48                           /* the insides start this far below their top */
#define ROW 34
#define HEAD 30                          /* the columns' header */
#define KINDW 130                        /* the kind column, then the size's */
#define SIZEW 110
#define MAXF 256
typedef struct { char name[64]; int dir; uint64_t size; } fent_t;
static fent_t ent[MAXF];
static int nent, sel = -1, scroll;
static char cwd[160];
static uint64_t last_t;                  /* (a double click: the same row twice within 450 ms) */
static int last_row = -1;

static void path_of(int i, char *out, size_t cap)
{
    size_t k = strlcpy(out, cwd, cap);
    if (k && out[k - 1] != '/' && k + 1 < cap) out[k++] = '/';
    strlcpy(out + k, ent[i].name, cap - k);
}

/* Read the folder again: folders first, then files, each by name. */
static void load(void)
{
    nent = 0; sel = -1; scroll = 0;
    if (!atlas_may(cwd, 0)) { say("atlas: files %s: not allowed\n", cwd); return; }
    long h = fs_open(cwd, FS_RDONLY | FS_DIRECTORY, 0);
    if (h < 0) return;
    static char buf[4096];
    char name[256];
    uint64_t cs = 0, ino;
    long n;
    int type;
    while ((n = fs_readdir(h, &cs, buf, sizeof buf)) > 0)
        for (const char *p = buf; fs_dirent(&p, buf + n, &ino, &type, name) && nent < MAXF; ) {
            if (name[0] == '.' || (type != 4 && type != 8)) continue;
            fent_t *e = &ent[nent++];
            strlcpy(e->name, name, sizeof e->name);
            e->dir = type == 4;
            char path[224];
            path_of(nent - 1, path, sizeof path);
            siefs_stat_t st;
            e->size = e->dir || fs_stat(path, &st, 1) ? 0 : st.size;
        }
    fs_close(h);
    for (int i = 1; i < nent; i++)
        for (int j = i; j > 0; j--) {
            fent_t *a = &ent[j - 1], *b = &ent[j];
            if (a->dir > b->dir || (a->dir == b->dir && strcmp(a->name, b->name) <= 0)) break;
            fent_t t = *a; *a = *b; *b = t;
        }
    say("atlas: files %s: %d entries\n", cwd, nent);
}

void files_open(const char *dir) { strlcpy(cwd, dir, sizeof cwd); load(); }

/* The snapshots of this folder's volume: the nearest folder at or above it
 * that has a ".snapshots" (a volume's root). */
static void snapshots(void)
{
    char p[sizeof cwd + 16], *s;
    siefs_stat_t st;
    strlcpy(p, cwd, sizeof cwd);
    if ((s = strstr_snap(p))) *s = 0;                    /* (inside one already: from its volume) */
    if (!p[0]) strlcpy(p, "/", sizeof p);
    for (;;) {
        size_t n = strlen(p);
        strlcpy(p + n, n > 1 ? "/.snapshots" : ".snapshots", sizeof p - n);
        if (!fs_stat(p, &st, 0)) { strlcpy(cwd, p, sizeof cwd); load(); return; }
        p[n] = 0;
        s = 0;
        for (char *q = p; *q; q++) if (*q == '/') s = q;
        if (!s || n <= 1) return;
        if (s == p) p[1] = 0; else *s = 0;
    }
}
const char *files_dir(void) { return cwd; }

static void up(void)
{
    char *s = 0;
    for (char *p = cwd; *p; p++) if (*p == '/') s = p;  /* (the last '/') */
    if (!s) return;
    if (s == cwd) cwd[1] = 0; else *s = 0;
    load();
}

static void activate(int i)
{
    if (i < 0 || i >= nent) return;
    char path[224];
    path_of(i, path, sizeof path);
    if (ent[i].dir) { strlcpy(cwd, path, sizeof cwd); load(); return; }
    size_t l = strlen(path);
    if (mail_pick(path)) return;                         /* (Mail's Attach) */
    if (l > 4 && !strcmp(path + l - 4, ".app")) atlas_run_app(path);
    else atlas_open_file(path);
}

/* What a file is, by its name. */
static const char *kind_of(const fent_t *e)
{
    if (e->dir) return "folder";
    const char *x = 0;
    for (const char *p = e->name; *p; p++) if (*p == '.') x = p + 1;
    if (!x) return "file";
    static const struct { const char *ext, *kind; } k[] = {
        { "app", "app" }, { "c", "C source" }, { "h", "C header" }, { "py", "Python" }, { "sh", "shell" },
        { "txt", "text" }, { "md", "Markdown" }, { "conf", "settings" }, { "json", "JSON" }, { "gguf", "model" },
    };
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) if (!strcmp(x, k[i].ext)) return k[i].kind;
    return "file";
}

/* The path, a part at a time: a click on part k goes to that folder. */
#define CRUMB (-10)                      /* hit box args: CRUMB - 1 - k (k -1: the top folder) */
static void go_part(int k)
{
    int n = 0;
    for (char *p = cwd + 1; *p; p++)
        if (*p == '/' && ++n > k) { *p = 0; break; }
    if (k < 0) cwd[1] = 0;
    load();
}

static int X0, Y0, X1, Y1;
static int in_snapshots(void) { return strstr_snap(cwd) != 0; }
static int rows(void) { return (Y1 - (Y0 + TOP + 54 + HEAD) - 8) / ROW; }

void files_draw(int x0, int y0, int x1, int y1, int focused)
{
    (void)focused;
    X0 = x0; Y0 = y0; X1 = x1; Y1 = y1;
    hitbox(x0, y0 + TOP, x1, y1, H_FILE, -1);
    int ty = y0 + TOP;                                   /* the bar: Up, the folder */
    int ux = pill(x0 + 14, ty, "Up", 0, H_FILE, -2);
    if (atlas_debug) say("atlas: dbg fileup at %d %d\n", x0 + 40, ty + 21);
    int sx = x1 - 16 - (tw(9, 2) + 28);                 /* Snapshots, at the right end */
    pill(sx, ty, "Snapshots", in_snapshots(), H_FILE, -3);
    if (atlas_debug) say("atlas: dbg filesnap at %d %d\n", sx + 60, ty + 21);
    {                                                    /* the path: each part a link, the last one bold */
        int x = ux + 16, parts = 0;
        for (const char *q = cwd; *q; q++) parts += *q == '/' && q[1];   /* ("/home/alice": 2, "/": 0) */
        const char *p = cwd + 1;
        for (int k = -1; k < parts && x < x1 - 40; k++) {
            const char *s = k < 0 ? "/" : p;
            int l = k < 0 ? 1 : 0;
            if (k >= 0) { while (s[l] && s[l] != '/') l++; p = s + l + (s[l] == '/'); }
            int last = k == parts - 1, w = tw(l, 2);
            dl_text(x, ty + 10, s, l, S(2), last ? C_TXT : C_DIM, x0, ty, sx - 12, ty + 42);
            if (last) dl_text(x + 1, ty + 10, s, l, S(2), C_TXT, x0, ty, sx - 12, ty + 42);
            else hitbox(x - 4, ty + 4, x + w + 4, ty + 38, H_FILE, CRUMB - 1 - k);
            if (!last && atlas_debug) say("atlas: dbg crumb %d at %d %d\n", k, x + w / 2, ty + 21);
            x += w;
            if (!last) { dl_text(x + 9, ty + 12, ">", 1, SS, C_LINE, x0, ty, sx - 12, ty + 42); x += 30; }
        }
    }
    int hy = ty + 54, ly = hy + HEAD, kx = x1 - 24 - SIZEW - KINDW;   /* the columns' header */
    dl_rect(x0 + 2, hy, x1 - 2, y1 - 2, 0, C_TERM);
    dl_text(x0 + 52, hy + 8, "NAME", 4, SS, C_DIM, 0, 0, SW, SH);
    dl_text(kx, hy + 8, "KIND", 4, SS, C_DIM, 0, 0, SW, SH);
    dl_text(x1 - 24 - text_w(4, SS), hy + 8, "SIZE", 4, SS, C_DIM, 0, 0, SW, SH);
    dl_rect(x0 + 10, ly - 1, x1 - 10, ly, 0, C_LINE);
    int n = rows();
    if (scroll > nent - n) scroll = nent - n;
    if (scroll < 0) scroll = 0;
    if (!nent) {                                         /* nothing: say why, in the middle */
        const char *m = atlas_may(cwd, 0) ? "This folder is empty" : "You may not read this folder";
        label((x0 + x1) / 2 - tw(strlen(m), 2) / 2, (ly + y1) / 2 - KH, m, 2, C_DIM);
    }
    int wide = kx > x0 + 52 + tw(12, 2);                 /* (room for the kind column) */
    for (int r = 0; r < n && scroll + r < nent; r++) {
        int i = scroll + r, y = ly + 4 + r * ROW;
        fent_t *e = &ent[i];
        if (i == sel) dl_rect(x0 + 6, y, x1 - 6, y + ROW - 2, 6, C_SEL);
        else if (r & 1) dl_rect(x0 + 6, y, x1 - 6, y + ROW - 2, 6, C_PANEL);
        size_t l = strlen(e->name);
        int app = !e->dir && l > 4 && !strcmp(e->name + l - 4, ".app");
        icon(e->dir ? I_FOLDER : app ? I_APP : I_FILE, x0 + 16, y + 4, 2);
        dl_text(x0 + 52, y + 6, e->name, l, S(2), C_TXT, x0, y, (wide ? kx : x1 - SIZEW) - 16, y + ROW);
        if (e->dir) dl_text(x0 + 53, y + 6, e->name, l, S(2), C_TXT, x0, y, (wide ? kx : x1 - SIZEW) - 16, y + ROW);   /* (folders bold) */
        const char *kd = kind_of(e);
        if (wide) dl_text(kx, y + 8, kd, strlen(kd), SS, C_DIM, 0, 0, x1 - SIZEW - 24, SH);
        char sz[24], *p = sz;
        if (e->dir) strlcpy(sz, "-", sizeof sz);
        else { uint64_t s = e->size; p += num(p, s >= 1024 ? (long)(s / 1024) : (long)s); strlcpy(p, s >= 1024 ? " KB" : " B", 6); }
        dl_text(x1 - 24 - text_w(strlen(sz), SS), y + 8, sz, strlen(sz), SS, C_DIM, 0, 0, SW, SH);
        hitbox(x0 + 6, y, x1 - 6, y + ROW - 2, H_FILE, i);
        if (atlas_debug) say("atlas: dbg file %s at %d %d\n", e->name, x0 + 120, y + ROW / 2);
    }
}

int files_click(int kind, int arg)
{
    if (kind != H_FILE) return 0;
    if (arg == -2) { up(); return 1; }
    if (arg == -3) { snapshots(); return 1; }
    if (arg <= CRUMB) { go_part(CRUMB - 1 - arg); return 1; }
    if (arg < 0) return 1;
    uint64_t t = sys_clock();
    sel = arg;
    if (arg == last_row && t - last_t < 450000000) { last_row = -1; activate(arg); }
    else { last_row = arg; last_t = t; }
    return 1;
}

void files_wheel(int w) { scroll += w * 3; if (scroll < 0) scroll = 0; }

int files_key(int c)
{
    int n = rows() > 0 ? rows() : 1;
    if (c == KEY_DOWN) { if (sel + 1 < nent) sel++; }
    else if (c == KEY_UP) { if (sel > 0) sel--; else sel = 0; }
    else if (c == '\n') activate(sel);
    else if (c == '\b') up();
    else return 0;
    if (sel >= 0 && sel < scroll) scroll = sel;
    if (sel >= scroll + n) scroll = sel - n + 1;
    return 1;
}
