/*
 * files.c - The Files window (docs/desktop.md): a folder's contents, one
 * row each (icon, name, size), folders first. Double-click (or Enter): a
 * folder opens in place, a file in the editor, a launcher (.app) starts.
 * Up (or Backspace) goes to the parent. It starts in the home folder; it
 * shows only what the logged-in user may read.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"

#define TOP 48                           /* the insides start this far below their top */
#define ROW 34
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
    if (l > 4 && !strcmp(path + l - 4, ".app")) atlas_run_app(path);
    else atlas_open_file(path);
}

static int X0, Y0, X1, Y1;
static int rows(void) { return (Y1 - (Y0 + TOP + 58) - 8) / ROW; }

void files_draw(int x0, int y0, int x1, int y1, int focused)
{
    (void)focused;
    X0 = x0; Y0 = y0; X1 = x1; Y1 = y1;
    hitbox(x0, y0 + TOP, x1, y1, H_FILE, -1);
    int ty = y0 + TOP;                                   /* the bar: Up, the folder */
    int ux = pill(x0 + 14, ty, "Up", 0, H_FILE, -2);
    if (atlas_debug) say("atlas: dbg fileup at %d %d\n", x0 + 40, ty + 21);
    dl_text(ux + 16, ty + 10, cwd, strlen(cwd), S(2), C_DIM, ux, ty, x1 - 16, ty + 42);
    int ly = ty + 54;
    dl_rect(x0 + 2, ly, x1 - 2, y1 - 2, 0, C_TERM);
    int n = rows();
    if (scroll > nent - n) scroll = nent - n;
    if (scroll < 0) scroll = 0;
    if (!nent) label(x0 + 24, ly + 16, atlas_may(cwd, 0) ? "(empty)" : "(not allowed)", 2, C_DIM);
    for (int r = 0; r < n && scroll + r < nent; r++) {
        int i = scroll + r, y = ly + 4 + r * ROW;
        fent_t *e = &ent[i];
        if (i == sel) dl_rect(x0 + 6, y, x1 - 6, y + ROW - 2, 6, C_SEL);
        size_t l = strlen(e->name);
        int app = !e->dir && l > 4 && !strcmp(e->name + l - 4, ".app");
        icon(e->dir ? I_FOLDER : app ? I_APP : I_FILE, x0 + 16, y + 4, 2);
        dl_text(x0 + 52, y + 6, e->name, l, S(2), C_TXT, x0, y, x1 - 150, y + ROW);
        char sz[24], *p = sz;
        if (e->dir) strlcpy(sz, "folder", sizeof sz);
        else { uint64_t s = e->size; p += num(p, s >= 1024 ? (long)(s / 1024) : (long)s); strlcpy(p, s >= 1024 ? " KB" : " B", 6); }
        label(x1 - 24 - tw(strlen(sz), 2), y + 6, sz, 2, C_DIM);
        hitbox(x0 + 6, y, x1 - 6, y + ROW - 2, H_FILE, i);
        if (atlas_debug) say("atlas: dbg file %s at %d %d\n", e->name, x0 + 120, y + ROW / 2);
    }
}

int files_click(int kind, int arg)
{
    if (kind != H_FILE) return 0;
    if (arg == -2) { up(); return 1; }
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
