/*
 * edit.c - The desktop's file editor: one file at a time, shown in a
 * window of the stage (main or beside, whatever room it gets).
 *
 * The whole file sits in one buffer (at most 64 KiB, only while it is
 * open); typing moves the rest of the buffer by one byte, which for files
 * this size costs nothing noticeable and keeps the code small. Keys:
 * arrows, Home/End, PgUp/PgDn, Backspace, Del, Enter, Tab (4 spaces),
 * Ctrl+S saves, Esc closes (twice if there are unsaved changes), Ctrl+V
 * pastes the clipboard (atlas.c types it in).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"

#define EMAX (64 << 10)
#define TOP 48                       /* the title strip above the text */

static struct {
    int open, dirty, ro, warn, want;
    int len, cap, cur, top, left;   /* text; cursor (byte); first line and column shown */
    int x0, y0, x1, y1, cols, rows; /* where it was last drawn, and how much text fits */
    char *b, path[160], name[48], msg[64];
} e;

int edit_active(void) { return e.open; }
int edit_dirty(void) { return e.open && e.dirty; }
const char *edit_path(void) { return e.open ? e.path : ""; }

static int bol(int p) { while (p > 0 && e.b[p - 1] != '\n') p--; return p; }   /* line start */
static int eol(int p) { while (p < e.len && e.b[p] != '\n') p++; return p; }   /* line end */
static int line_of(int p) { int n = 0; for (int i = 0; i < p; i++) n += e.b[i] == '\n'; return n; }
static int line_pos(int n)                               /* where line n starts */
{
    int p = 0;
    while (n > 0 && p < e.len) if (e.b[p++] == '\n') n--;
    return p;
}

void edit_close(void)
{
    if (!e.open) return;
    free(e.b);
    say("atlas: editor closed %s\n", e.name);
    memset(&e, 0, sizeof e);
}

int edit_open(const char *path)
{
    if (e.open && !strcmp(e.path, path)) return 0;      /* (already open) */
    if (!atlas_may(path, 0)) { say("atlas: %s: not allowed\n", path); return -EACCES; }
    size_t n = 0;
    char *f = file_get(path, &n);
    if (!f) { say("atlas: cannot open %s\n", path); return -ENOENT; }
    edit_close();
    int bad = 0;
    for (size_t i = 0; i < n && i < 4096; i++) bad += (uint8_t)f[i] < 9 || ((uint8_t)f[i] > 13 && (uint8_t)f[i] < 32);
    e.ro = n > EMAX || bad * 10 > (int)(n < 4096 ? n : 4096) || !atlas_may(path, 1);
    e.len = n > EMAX ? EMAX : (int)n;
    e.cap = e.len + 4096 > EMAX ? EMAX : e.len + 4096;
    if (!(e.b = realloc(f, e.cap + 1))) { free(f); return -ENOMEM; }
    e.open = 1;
    e.cols = 80; e.rows = 25;
    strlcpy(e.path, path, sizeof e.path);
    strlcpy(e.name, base_name(path), sizeof e.name);
    strlcpy(e.msg, e.ro ? "read only (not yours to change, binary, or over 64 KiB)" : "Ctrl+S saves, Esc closes", sizeof e.msg);
    say("atlas: editing %s (%d bytes)\n", e.name, e.len);
    return 0;
}

static void save(void)
{
    if (e.ro) return;
    long h = fs_open(e.path, FS_WRONLY | FS_TRUNC, 0), w = 0;
    for (int off = 0; h >= 0 && off < e.len && w >= 0; off += w)
        w = fs_write(h, off, e.b + off, e.len - off < FS_MAX ? e.len - off : FS_MAX);
    if (h >= 0) fs_close(h);
    if (h < 0 || w < 0) {
        strlcpy(e.msg, "cannot save: ", sizeof e.msg);
        num(e.msg + strlen(e.msg), h < 0 ? h : w);
        say("atlas: save %s failed\n", e.name);
        return;
    }
    e.dirty = e.warn = 0;
    char *p = e.msg + strlcpy(e.msg, "saved, ", sizeof e.msg);
    strlcpy(p + num(p, e.len), " bytes", 8);
    say("atlas: saved %s %d bytes\n", e.name, e.len);
}

static void insert(char c)
{
    if (e.ro || e.len >= EMAX) return;
    if (e.len == e.cap) {
        int cap = e.cap + 4096 > EMAX ? EMAX : e.cap + 4096;
        char *b = realloc(e.b, cap + 1);
        if (!b) return;
        e.b = b; e.cap = cap;
    }
    memmove(e.b + e.cur + 1, e.b + e.cur, e.len - e.cur);
    e.b[e.cur++] = c;
    e.len++;
    e.dirty = 1;
}

static void erase(int p)                                 /* the character at p */
{
    if (e.ro || p < 0 || p >= e.len) return;
    memmove(e.b + p, e.b + p + 1, e.len - p - 1);
    e.len--;
    if (e.cur > p) e.cur--;
    e.dirty = 1;
}

static void vertical(int lines)                          /* up/down, keeping the column */
{
    int l = line_of(e.cur) + lines;
    if (l < 0) l = 0;
    int p = line_pos(l), end = eol(p);
    e.cur = p + e.want < end ? p + e.want : end;
}

static void in_view(void)                                /* keep the cursor on the screen */
{
    int l = line_of(e.cur), col = e.cur - bol(e.cur);
    if (l < e.top) e.top = l;
    if (l >= e.top + e.rows) e.top = l - e.rows + 1;
    if (col < e.left) e.left = col;
    if (col >= e.left + e.cols) e.left = col - e.cols + 1;
}

int edit_key(int c)
{
    if (!e.open) return 0;
    int col = e.cur - bol(e.cur);
    switch (c) {
    case 27:                                             /* Esc */
        if (e.dirty && !e.warn) { e.warn = 1; strlcpy(e.msg, "not saved: Ctrl+S saves, Esc again closes", sizeof e.msg); break; }
        edit_close();
        return 1;
    case 19: save(); break;                              /* Ctrl+S */
    case KEY_LEFT:  if (e.cur) e.cur--; e.want = e.cur - bol(e.cur); break;
    case KEY_RIGHT: if (e.cur < e.len) e.cur++; e.want = e.cur - bol(e.cur); break;
    case KEY_HOME:  e.cur = bol(e.cur); e.want = 0; break;
    case KEY_END:   e.cur = eol(e.cur); e.want = e.cur - bol(e.cur); break;
    case KEY_UP:    vertical(-1); break;
    case KEY_DOWN:  vertical(1); break;
    case KEY_PGUP:  vertical(-(e.rows - 2)); break;
    case KEY_PGDN:  vertical(e.rows - 2); break;
    case KEY_DEL:   erase(e.cur); break;
    case '\b':      erase(e.cur - 1); e.want = e.cur - bol(e.cur); break;
    case '\t':      for (int i = 0; i < 4; i++) insert(' '); e.want = col + 4; break;
    default:
        if (c == '\n' || (c >= 32 && c < 127)) { insert(c); e.want = e.cur - bol(e.cur); }
        else return 1;
    }
    if (c != 27 && c != 19) e.warn = 0;
    in_view();
    return 1;
}

void edit_wheel(int w)
{
    e.top -= 3 * w;
    int max = line_of(e.len);
    e.top = e.top < 0 ? 0 : e.top > max ? max : e.top;
}

/* A click in the editor: the cursor where the click was. */
void edit_click(int x, int y)
{
    int r = (y - e.y0 - TOP) / KH, k = (x - e.x0 - 14) / KW;
    if (r < 0) return;
    int p = line_pos(e.top + r), end = eol(p);
    k = (k < 0 ? 0 : k) + e.left;
    e.cur = p + k < end ? p + k : end;
    e.want = e.cur - p;
}

void edit_draw(int x0, int y0, int x1, int y1, int focused)
{
    e.x0 = x0; e.y0 = y0; e.x1 = x1; e.y1 = y1;
    e.cols = (x1 - x0 - 28) / KW; e.rows = (y1 - y0 - TOP - 40) / KH;
    if (e.cols < 10) e.cols = 10;
    if (e.rows < 3) e.rows = 3;
    in_view();
    dl_rect(x0 + 2, y0 + TOP, x1 - 2, y1 - 2, 0, C_TERM);
    hitbox(x0, y0 + TOP, x1, y1, H_ED, 0);
    int p = line_pos(e.top);
    for (int r = 0; r < e.rows && p <= e.len; r++) {
        int end = eol(p), s = p + e.left, ty = y0 + TOP + 8 + r * KH;
        if (s < end) dl_text(x0 + 14, ty, e.b + s, end - s < e.cols ? end - s : e.cols, S(K), C_TXT, x0, y0, x1 - 10, y1);
        if (focused && e.cur >= p && e.cur <= end) {     /* the cursor */
            int c = e.cur - p - e.left;
            dl_rect(x0 + 14 + c * KW, ty, x0 + 16 + c * KW, ty + KH - 2, 0, C_ACC);
        }
        if (end >= e.len) break;
        p = end + 1;
    }
    char t[96];
    size_t n = strlcpy(t, e.msg, sizeof t);
    if (e.dirty) strlcpy(t + n, "  (changed)", sizeof t - n);
    label(x0 + 14, y1 - 32, t, K, e.msg[0] == 'c' || e.msg[0] == 'n' ? C_ERR : C_DIM);
}
