/*
 * edit.c - The desktop's editor (docs/desktop.md): a window with the tree
 * of the user's files on the left and the open files as tabs on the right,
 * the way code editors lay it out, in the desktop's retro look.
 *
 *   ┌ EXPLORER ────────┬─ note.txt × ─┬─ hello.c ■ ─┬──────────────────┐
 *   │ .. alice         │ home > alice > hello.c                        │
 *   │ [-] src          │   1  #include "mk.h"                          │
 *   │     ▤ hello.c    │   2  int main(void)                           │
 *   │ [+] notes        │   3  {█                                       │
 *   │   ▤ note.txt     │                                               │
 *   ├──────────────────┴───────────────────────────────────────────────┤
 *   │ hello.c  Ln 3, Col 2  C  changed          Ctrl+S save  Ctrl+W close│
 *   └──────────────────────────────────────────────────────────────────┘
 *
 * The tree: a click opens or closes a folder ([+] / [-]), opens a file in
 * a tab (or shows its tab); ".." shows the folder above. It starts at the
 * home folder and opens the folders down to a file opened from elsewhere.
 * The tabs: a click shows one; × closes it (a changed one, marked ■, asks
 * a second click). The text: line numbers, the cursor's line lit, a block
 * cursor, and colours for C (and the like), shell and Python, Markdown.
 * The status bar, inverse like an old terminal's, says where the cursor is.
 *
 * Keys: arrows, Home/End, PgUp/PgDn, Backspace, Del, Enter, Tab (4
 * spaces); Ctrl+S saves; Ctrl+W (or Esc) closes the tab (twice if it has
 * changes); F5 / F6: the tab before / after; Ctrl+B hides or shows the
 * tree. Ctrl+V pastes (atlas.c types it in). The last tab closed closes
 * the window.
 *
 * Each file sits whole in a buffer (at most 64 KiB); typing moves the rest
 * of it by a byte, which for files this size costs nothing noticeable.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"

#define EMAX (64 << 10)
#define TOP 48                           /* the insides start this far below the y0 we are given */
#define NTAB 10
#define TABH 40                          /* the tab bar */
#define CRUMB 30                         /* the path under it */
#define STATUS 32                        /* the status bar */
#define TROW 30                          /* a row of the tree */
#define MAXN 400                         /* rows of the tree */
#define MAXEXP 48                        /* folders open in it */
enum { E_TEXT = 0, E_TREE = 1000, E_TAB = 2000, E_TABX = 3000, E_UP = 4000, E_TREEBG = 5000 };

typedef struct {
    int dirty, ro, warn, want;
    int len, cap, cur, top, left;        /* text; cursor (byte); first line and column shown */
    char *b, path[160], name[48];
} tab_t;
static tab_t tabs[NTAB];
static int ntab, act;                    /* the tabs, in the order opened; the one shown */
static char msg[80];                     /* the status bar's message */
static int msg_err;

static struct { char name[48]; char path[160]; int depth, dir, open; } node[MAXN];
static int nnode, tscroll, hide_tree;
static char root[160], expd[MAXEXP][160];
static int nexp;

static int X0, Y0, X1, Y1, TX1, RX0, TY0, TY1, cols, rows;   /* where things were drawn */

int edit_active(void) { return ntab > 0; }
int edit_dirty(void) { return ntab > 0 && tabs[act].dirty; }
const char *edit_path(void) { return ntab ? tabs[act].path : ""; }

static tab_t *T(void) { return &tabs[act]; }
static int bol(const tab_t *e, int p) { while (p > 0 && e->b[p - 1] != '\n') p--; return p; }
static int eol(const tab_t *e, int p) { while (p < e->len && e->b[p] != '\n') p++; return p; }
static int line_of(const tab_t *e, int p) { int n = 0; for (int i = 0; i < p; i++) n += e->b[i] == '\n'; return n; }
static int line_pos(const tab_t *e, int n)
{
    int p = 0;
    while (n > 0 && p < e->len) if (e->b[p++] == '\n') n--;
    return p;
}
static void set_msg(const char *s, int err) { strlcpy(msg, s, sizeof msg); msg_err = err; }

/* ---- The tree. */
static int expanded(const char *p) { for (int i = 0; i < nexp; i++) if (!strcmp(expd[i], p)) return i; return -1; }
static void join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t k = strlcpy(out, dir, cap);
    if (k && out[k - 1] != '/' && k + 1 < cap) out[k++] = '/';
    strlcpy(out + k, name, cap - k);
}
static void list(const char *dir, int depth)           /* a folder's entries: folders first, by name */
{
    if (depth > 8 || !atlas_may(dir, 0)) return;
    long h = fs_open(dir, FS_RDONLY | FS_DIRECTORY, 0);
    if (h < 0) return;
    static char buf[4096];
    char name[256];
    uint64_t cs = 0, ino;
    long n;
    int type, first = nnode;
    while ((n = fs_readdir(h, &cs, buf, sizeof buf)) > 0)
        for (const char *p = buf; fs_dirent(&p, buf + n, &ino, &type, name) && nnode < MAXN; ) {
            if (name[0] == '.' || (type != 4 && type != 8)) continue;
            strlcpy(node[nnode].name, name, sizeof node[nnode].name);
            join(node[nnode].path, sizeof node[nnode].path, dir, name);
            node[nnode].depth = depth; node[nnode].dir = type == 4; node[nnode].open = 0;
            nnode++;
        }
    fs_close(h);
    for (int i = first + 1; i < nnode; i++)            /* (insertion sort: a few dozen at most) */
        for (int j = i; j > first; j--) {
            if (node[j - 1].dir > node[j].dir || (node[j - 1].dir == node[j].dir && strcmp(node[j - 1].name, node[j].name) <= 0)) break;
            char t[sizeof node[0]];
            memcpy(t, &node[j - 1], sizeof t); memcpy(&node[j - 1], &node[j], sizeof t); memcpy(&node[j], t, sizeof t);
        }
    int last = nnode;                                   /* the open folders' insides, under them */
    for (int i = first; i < last; i++) {
        if (!node[i].dir || expanded(node[i].path) < 0) continue;
        node[i].open = 1;
        int before = nnode;
        list(node[i].path, depth + 1);
        int added = nnode - before;                     /* move them right after their folder */
        if (added && i + 1 < before) {
            static char tmp[MAXN * sizeof node[0] / 4];
            size_t sz = sizeof node[0], move = (size_t)(before - (i + 1)) * sz;
            if ((size_t)added * sz > sizeof tmp) { nnode = before; continue; }
            memcpy(tmp, &node[before], (size_t)added * sz);
            memmove(&node[i + 1 + added], &node[i + 1], move);
            memcpy(&node[i + 1], tmp, (size_t)added * sz);
        }
        last += added; i += added;
    }
}
static void build(void) { nnode = 0; if (root[0]) list(root, 0); }
static void toggle(const char *path)
{
    int k = expanded(path);
    if (k >= 0) { memmove(expd[k], expd[k + 1], (size_t)(nexp - k - 1) * sizeof expd[0]); nexp--; }
    else if (nexp < MAXEXP) strlcpy(expd[nexp++], path, sizeof expd[0]);
    build();
}
static void reveal(const char *path)                   /* open the folders down to it, under the root */
{
    size_t rl = strlen(root);
    if (!rl || memcmp(path, root, rl) || (path[rl] != '/' && rl > 1)) return;
    char p[160];
    for (size_t i = rl + 1; path[i]; i++)
        if (path[i] == '/') {
            memcpy(p, path, i); p[i] = 0;
            if (expanded(p) < 0 && nexp < MAXEXP) strlcpy(expd[nexp++], p, sizeof expd[0]);
        }
    build();
}
static void root_up(void)
{
    char *s = 0;
    for (char *p = root; *p; p++) if (*p == '/') s = p;
    if (!s) return;
    if (s == root) root[1] = 0; else *s = 0;
    tscroll = 0;
    build();
}

/* ---- Tabs. */
static void close_tab(int i)
{
    if (i < 0 || i >= ntab) return;
    say("atlas: editor closed %s\n", tabs[i].name);
    free(tabs[i].b);
    memmove(&tabs[i], &tabs[i + 1], (size_t)(ntab - i - 1) * sizeof tabs[0]);
    ntab--;
    if (act > i || act >= ntab) act = act > 0 ? act - 1 : 0;
    msg[0] = 0;
}
void edit_close(void) { while (ntab) close_tab(ntab - 1); nexp = 0; root[0] = 0; nnode = 0; }
int edit_close_window(void)                            /* the window's ×: 0 if a changed file held it */
{
    for (int i = 0; i < ntab; i++)
        if (tabs[i].dirty && !tabs[i].warn) {
            for (int k = 0; k < ntab; k++) tabs[k].warn = 1;
            act = i;
            set_msg("not saved: close again to discard the changes", 1);
            return 0;
        }
    edit_close();
    return 1;
}

int edit_open(const char *path)
{
    for (int i = 0; i < ntab; i++) if (!strcmp(tabs[i].path, path)) { act = i; return 0; }   /* (open: shown) */
    if (!atlas_may(path, 0)) { say("atlas: %s: not allowed\n", path); return -EACCES; }
    if (ntab == NTAB) {                                 /* full: the oldest unchanged one goes */
        int k = -1;
        for (int i = 0; i < ntab && k < 0; i++) if (!tabs[i].dirty) k = i;
        if (k < 0) { set_msg("too many changed files open: save or close one", 1); return -EBUSY; }
        close_tab(k);
    }
    size_t n = 0;
    char *f = file_get(path, &n);
    if (!f) { say("atlas: cannot open %s\n", path); return -ENOENT; }
    tab_t *e = &tabs[ntab];
    memset(e, 0, sizeof *e);
    int bad = 0;
    for (size_t i = 0; i < n && i < 4096; i++) bad += (uint8_t)f[i] < 9 || ((uint8_t)f[i] > 13 && (uint8_t)f[i] < 32);
    e->ro = n > EMAX || bad * 10 > (int)(n < 4096 ? n : 4096) || !atlas_may(path, 1);
    e->len = n > EMAX ? EMAX : (int)n;
    e->cap = e->len + 4096 > EMAX ? EMAX : e->len + 4096;
    if (!(e->b = realloc(f, e->cap + 1))) { free(f); return -ENOMEM; }
    strlcpy(e->path, path, sizeof e->path);
    strlcpy(e->name, base_name(path), sizeof e->name);
    act = ntab++;
    set_msg(e->ro ? "read only: not yours to change, binary, or over 64 KiB" : "", 0);
    if (!root[0]) { strlcpy(root, atlas_home(), sizeof root); if (!root[0]) strlcpy(root, "/", sizeof root); }
    reveal(path);
    if (!nnode) build();
    say("atlas: editing %s (%d bytes)\n", e->name, e->len);
    return 0;
}

static void save(void)
{
    tab_t *e = T();
    if (e->ro) return;
    long h = fs_open(e->path, FS_WRONLY | FS_TRUNC, 0), w = 0;
    for (int off = 0; h >= 0 && off < e->len && w >= 0; off += w)
        w = fs_write(h, off, e->b + off, e->len - off < FS_MAX ? e->len - off : FS_MAX);
    if (h >= 0) fs_close(h);
    if (h < 0 || w < 0) {
        char t[48], *p = t + strlcpy(t, "cannot save: ", sizeof t);
        p[num(p, h < 0 ? h : w)] = 0;
        set_msg(t, 1);
        say("atlas: save %s failed\n", e->name);
        return;
    }
    e->dirty = e->warn = 0;
    char t[48], *p = t + strlcpy(t, "saved, ", sizeof t);
    strlcpy(p + num(p, e->len), " bytes", 8);
    set_msg(t, 0);
    say("atlas: saved %s %d bytes\n", e->name, e->len);
}

static void insert(char c)
{
    tab_t *e = T();
    if (e->ro || e->len >= EMAX) return;
    if (e->len == e->cap) {
        int cap = e->cap + 4096 > EMAX ? EMAX : e->cap + 4096;
        char *b = realloc(e->b, cap + 1);
        if (!b) return;
        e->b = b; e->cap = cap;
    }
    memmove(e->b + e->cur + 1, e->b + e->cur, e->len - e->cur);
    e->b[e->cur++] = c;
    e->len++;
    e->dirty = 1;
}
static void erase(int p)
{
    tab_t *e = T();
    if (e->ro || p < 0 || p >= e->len) return;
    memmove(e->b + p, e->b + p + 1, e->len - p - 1);
    e->len--;
    if (e->cur > p) e->cur--;
    e->dirty = 1;
}
static void vertical(int lines)
{
    tab_t *e = T();
    int l = line_of(e, e->cur) + lines;
    if (l < 0) l = 0;
    int p = line_pos(e, l), end = eol(e, p);
    e->cur = p + e->want < end ? p + e->want : end;
}
static void in_view(void)
{
    tab_t *e = T();
    int l = line_of(e, e->cur), col = e->cur - bol(e, e->cur);
    if (l < e->top) e->top = l;
    if (rows > 0 && l >= e->top + rows) e->top = l - rows + 1;
    if (col < e->left) e->left = col;
    if (cols > 0 && col >= e->left + cols) e->left = col - cols + 1;
}

static void ask_close(int i)                           /* a tab's ×, Ctrl+W, Esc */
{
    if (tabs[i].dirty && !tabs[i].warn) {
        tabs[i].warn = 1; act = i;
        set_msg("not saved: Ctrl+S saves, close again to discard", 1);
        return;
    }
    close_tab(i);
}

int edit_key(int c)
{
    if (!ntab) return 0;
    tab_t *e = T();
    int col = e->cur - bol(e, e->cur);
    switch (c) {
    case 27: case 23: ask_close(act); return 1;          /* Esc, Ctrl+W */
    case 19: save(); break;                              /* Ctrl+S */
    case 2: hide_tree = !hide_tree; break;               /* Ctrl+B */
    case KEY_F1 + 4: act = (act + ntab - 1) % ntab; return 1;   /* F5, F6 */
    case KEY_F1 + 5: act = (act + 1) % ntab; return 1;
    case KEY_LEFT:  if (e->cur) e->cur--; e->want = e->cur - bol(e, e->cur); break;
    case KEY_RIGHT: if (e->cur < e->len) e->cur++; e->want = e->cur - bol(e, e->cur); break;
    case KEY_HOME:  e->cur = bol(e, e->cur); e->want = 0; break;
    case KEY_END:   e->cur = eol(e, e->cur); e->want = e->cur - bol(e, e->cur); break;
    case KEY_UP:    vertical(-1); break;
    case KEY_DOWN:  vertical(1); break;
    case KEY_PGUP:  vertical(-(rows > 3 ? rows - 2 : 1)); break;
    case KEY_PGDN:  vertical(rows > 3 ? rows - 2 : 1); break;
    case KEY_DEL:   erase(e->cur); break;
    case '\b':      erase(e->cur - 1); e->want = e->cur - bol(e, e->cur); break;
    case '\t':      for (int i = 0; i < 4; i++) insert(' '); e->want = col + 4; break;
    default:
        if (c == '\n' || (c >= 32 && c < 127)) { insert((char)c); e->want = e->cur - bol(e, e->cur); }
        else return 1;
    }
    if (c != 19) { e->warn = 0; if (!msg_err) msg[0] = 0; else if (c != 2) msg[0] = 0, msg_err = 0; }
    in_view();
    return 1;
}

void edit_wheel(int arg, int w)
{
    if (arg >= E_TREE && arg != E_TAB && (arg < E_TAB || arg >= E_UP)) {   /* over the tree */
        tscroll -= 3 * w;
        if (tscroll > nnode - 3) tscroll = nnode - 3;
        if (tscroll < 0) tscroll = 0;
        return;
    }
    if (!ntab) return;
    tab_t *e = T();
    e->top -= 3 * w;
    int max = line_of(e, e->len);
    e->top = e->top < 0 ? 0 : e->top > max ? max : e->top;
}

void edit_click(int arg, int x, int y)
{
    if (arg >= E_TREE && arg < E_TREE + MAXN) {
        int i = arg - E_TREE;
        if (i >= nnode) return;
        if (node[i].dir) toggle(node[i].path);
        else { char p[160]; strlcpy(p, node[i].path, sizeof p); edit_open(p); }
        return;
    }
    if (arg >= E_TAB && arg < E_TAB + NTAB) { if (arg - E_TAB < ntab) act = arg - E_TAB; return; }
    if (arg >= E_TABX && arg < E_TABX + NTAB) { ask_close(arg - E_TABX); return; }
    if (arg == E_UP) { root_up(); return; }
    if (arg != E_TEXT || !ntab) return;
    tab_t *e = T();                                     /* the text: the cursor where the click was */
    int r = (y - TY0) / KH, k = (x - (RX0 + 6 * KW + 8)) / KW;
    if (r < 0) return;
    int p = line_pos(e, e->top + r), end = eol(e, p);
    k = (k < 0 ? 0 : k) + e->left;
    e->cur = p + k < end ? p + k : end;
    e->want = e->cur - p;
}

/* ---- Colours: what each character of a line is. */
enum { K_TXT, K_KEY, K_STR, K_NUM, K_COM, K_PRE };
static const uint32_t kcol[] = { C_TXT, C_ACC, C_TERMFG, C_TERMFG, C_DIM, C_ACC };
enum { L_PLAIN, L_C, L_HASH, L_MD };
static int lang_of(const char *name)
{
    const char *d = 0;
    for (const char *q = name; *q; q++) if (*q == '.') d = q;
    if (!strcmp(name, "Makefile")) return L_HASH;
    if (!d) return L_PLAIN;
    static const char *c[] = { ".c", ".h", ".cc", ".cpp", ".js", ".ts", ".java", ".rs", ".go", ".cs", ".S", 0 };
    static const char *h[] = { ".sh", ".py", ".conf", ".mk", ".toml", ".yaml", ".yml", ".app", 0 };
    for (int i = 0; c[i]; i++) if (!strcmp(d, c[i])) return L_C;
    for (int i = 0; h[i]; i++) if (!strcmp(d, h[i])) return L_HASH;
    return !strcmp(d, ".md") ? L_MD : L_PLAIN;
}
static int keyword(const char *s, int n)
{
    static const char *kw[] = { "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue",
        "return", "goto", "struct", "union", "enum", "typedef", "static", "const", "volatile", "extern", "inline",
        "void", "char", "short", "int", "long", "unsigned", "signed", "float", "double", "sizeof", "def", "class",
        "import", "from", "fn", "let", "var", "func", "true", "false", "null", "None", "True", "False", "elif",
        "then", "fi", "done", "esac", "in", "local", "export", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "int64_t", "int32_t", "size_t", "bool", 0 };
    for (int i = 0; kw[i]; i++) if ((int)strlen(kw[i]) == n && !memcmp(kw[i], s, n)) return 1;
    return 0;
}
static int idch(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* Colour line [p, end) into k[] (at most n characters); *com: inside a
 * C comment, across lines. */
static void colour(const char *b, int p, int end, int lang, int *com, uint8_t *k, int n)
{
    int len = end - p < n ? end - p : n, i = 0;
    const char *s = b + p;
    memset(k, K_TXT, (size_t)len);
    if (lang == L_PLAIN) return;
    if (lang == L_MD) {
        if (len && s[0] == '#') memset(k, K_KEY, (size_t)len);
        else for (; i < len; i++) if (s[i] == '`') { int j = i + 1; while (j < len && s[j] != '`') j++; memset(k + i, K_STR, (size_t)((j < len ? j + 1 : len) - i)); i = j; }
        return;
    }
    int lead = 0;
    while (lead < len && (s[lead] == ' ' || s[lead] == '\t')) lead++;
    if (lang == L_C && lead < len && s[lead] == '#' && !*com) { memset(k, K_PRE, (size_t)len); return; }
    while (i < len) {
        if (*com) {                                      /* inside a C comment */
            k[i] = K_COM;
            if (s[i] == '*' && i + 1 < len && s[i + 1] == '/') { k[i + 1] = K_COM; i += 2; *com = 0; }
            else i++;
            continue;
        }
        char c = s[i];
        if ((lang == L_C && c == '/' && i + 1 < len && s[i + 1] == '/') || (lang == L_HASH && c == '#')) {
            memset(k + i, K_COM, (size_t)(len - i)); return;
        }
        if (lang == L_C && c == '/' && i + 1 < len && s[i + 1] == '*') { *com = 1; k[i] = k[i + 1] = K_COM; i += 2; continue; }
        if (c == '"' || c == '\'') {
            int j = i + 1;
            while (j < len && s[j] != c) j += s[j] == '\\' ? 2 : 1;
            j = j < len ? j + 1 : len;
            memset(k + i, K_STR, (size_t)(j - i)); i = j; continue;
        }
        if (c >= '0' && c <= '9' && (i == 0 || !idch(s[i - 1]))) {
            int j = i; while (j < len && idch(s[j])) j++;
            memset(k + i, K_NUM, (size_t)(j - i)); i = j; continue;
        }
        if (idch(c)) {
            int j = i; while (j < len && idch(s[j])) j++;
            if (keyword(s + i, j - i)) memset(k + i, K_KEY, (size_t)(j - i));
            i = j; continue;
        }
        i++;
    }
    if (len < end - p && lang == L_C && *com) {          /* (the rest of a very long line) */
        for (int j = p + len; j + 1 < end; j++) if (b[j] == '*' && b[j + 1] == '/') { *com = 0; break; }
    }
}
static int comment_at(const tab_t *e, int upto, int lang)   /* inside a C comment where line `upto` starts? */
{
    if (lang != L_C) return 0;
    int com = 0, q = 0;
    for (int i = 0; i < upto; i++) {
        char c = e->b[i];
        if (c == '\n') { q = 0; if (!com) continue; }
        if (com) { if (c == '*' && i + 1 < upto && e->b[i + 1] == '/') { com = 0; i++; } continue; }
        if (q) { if (c == '\\') i++; else if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') q = c;
        else if (c == '/' && i + 1 < upto && e->b[i + 1] == '/') { while (i < upto && e->b[i] != '\n') i++; }
        else if (c == '/' && i + 1 < upto && e->b[i + 1] == '*') { com = 1; i++; }
    }
    return com;
}

/* ---- Drawing. */
static void tree_draw(int x0, int y0, int x1, int y1)
{
    dl_rect(x0, y0, x1, y1, 0, C_BG);
    dl_rect(x1, y0, x1 + 1, y1, 0, C_LINE);
    hitbox(x0, y0, x1, y1, H_ED, E_TREEBG);
    dl_text(x0 + 14, y0 + 12, "EXPLORER", 8, SS, C_DIM, 0, 0, SW, SH);
    int hy = y0 + TABH;                                  /* the root: ".." and its name */
    const char *rn = root[1] ? base_name(root) : "/";
    char t[64], *p = t + strlcpy(t, ".. ", sizeof t);
    strlcpy(p, rn, sizeof t - 3);
    dl_text(x0 + 14, hy + 4, t, strlen(t), S(2), C_TXT, x0, hy, x1 - 6, hy + TROW);
    dl_text(x0 + 15, hy + 4, t, strlen(t), S(2), C_TXT, x0, hy, x1 - 6, hy + TROW);
    hitbox(x0, hy, x1, hy + TROW, H_ED, E_UP);
    if (atlas_debug) say("atlas: dbg etreeup at %d %d\n", x0 + 40, hy + TROW / 2);
    int ry = hy + TROW + 4, n = (y1 - ry - 4) / TROW;
    if (tscroll > nnode - n) tscroll = nnode - n;
    if (tscroll < 0) tscroll = 0;
    const char *cur = ntab ? tabs[act].path : "";
    for (int r = 0; r < n && tscroll + r < nnode; r++) {
        int i = tscroll + r, y = ry + r * TROW, ix = x0 + 14 + node[i].depth * 24;
        if (!strcmp(node[i].path, cur)) dl_rect(x0 + 2, y, x1 - 2, y + TROW, 0, C_SEL);
        for (int d = 0; d < node[i].depth; d++)          /* the guides: where each level starts */
            dl_rect(x0 + 22 + d * 24, y, x0 + 23 + d * 24, y + TROW, 0, C_BAR);
        int tx = ix;
        if (node[i].dir) {
            label(ix, y + 4, node[i].open ? "[-]" : "[+]", 2, C_ACC);
            tx = ix + 3 * KW + 6;
        } else {
            icon(I_FILE, ix + 6, y + 5, 2);
            tx = ix + 36;
        }
        dl_text(tx, y + 4, node[i].name, strlen(node[i].name), S(2), node[i].dir ? C_TXT : C_DIM, x0, y, x1 - 6, y + TROW);
        hitbox(x0, y, x1, y + TROW, H_ED, E_TREE + i);
        if (atlas_debug) say("atlas: dbg etree %s at %d %d\n", node[i].name, tx + 20, y + TROW / 2);
    }
}

static void tabs_draw(int x0, int y0, int x1)
{
    dl_rect(x0, y0, x1, y0 + TABH, 0, C_BG);
    dl_rect(x0, y0 + TABH - 1, x1, y0 + TABH, 0, C_LINE);
    int w[NTAB], total = 0, first = 0;
    for (int i = 0; i < ntab; i++) { w[i] = tw(strlen(tabs[i].name), 2) + 64; total += w[i]; }
    while (total > x1 - x0 && first < act) total -= w[first++];   /* (too many: the shown one stays in view) */
    int x = x0;
    for (int i = first; i < ntab && x < x1; i++) {
        int on = i == act, xe = x + w[i] < x1 ? x + w[i] : x1;
        if (on) {
            dl_rect(x, y0, xe, y0 + TABH, 0, C_TERM);
            dl_rect(x, y0, xe, y0 + 3, 0, C_ACC);          /* the shown one: lit on top */
        }
        dl_rect(xe - 1, y0 + 6, xe, y0 + TABH - 6, 0, C_LINE);
        int n = strlen(tabs[i].name);
        dl_text(x + 16, y0 + 10, tabs[i].name, n, S(2), on ? C_TXT : C_DIM, x, y0, xe - 34, y0 + TABH);
        if (on) dl_text(x + 17, y0 + 10, tabs[i].name, n, S(2), C_TXT, x, y0, xe - 34, y0 + TABH);
        hitbox(x, y0, xe, y0 + TABH, H_ED, E_TAB + i);
        int bx = xe - 30;                                /* the close box: ×, or ■ if changed */
        if (tabs[i].dirty) dl_rect(bx + 6, y0 + 15, bx + 16, y0 + 25, 2, on ? C_ACC : C_DIM);
        else label(bx + 5, y0 + 9, "x", 2, on ? C_TXT : C_DIM);
        hitbox(bx - 2, y0 + 6, bx + 22, y0 + TABH - 6, H_ED, E_TABX + i);
        if (atlas_debug) {
            say("atlas: dbg etab %s at %d %d\n", tabs[i].name, x + 30, y0 + TABH / 2);
            say("atlas: dbg etabx %s at %d %d\n", tabs[i].name, bx + 10, y0 + TABH / 2);
        }
        x = xe;
    }
}

void edit_draw(int x0, int y0, int x1, int y1, int focused)
{
    if (!ntab) return;
    X0 = x0 + 2; Y0 = y0 + TOP; X1 = x1 - 2; Y1 = y1 - 2;
    int tw_ = hide_tree || X1 - X0 < 640 ? 0 : (X1 - X0) * 28 / 100;
    if (tw_ && tw_ < 220) tw_ = 220;
    if (tw_ > 420) tw_ = 420;
    TX1 = X0 + tw_; RX0 = tw_ ? TX1 + 1 : X0;
    int sy = Y1 - STATUS;
    hitbox(X0, Y0, X1, Y1, H_ED, E_TEXT);
    if (tw_) tree_draw(X0, Y0, TX1, sy);
    tabs_draw(RX0, Y0, X1);

    tab_t *e = T();
    int cy = Y0 + TABH;                                  /* the path, as a trail */
    dl_rect(RX0, cy, X1, cy + CRUMB, 0, C_TERM);
    {
        char t[200], *o = t;
        const char *p = e->path[0] == '/' ? e->path + 1 : e->path;
        for (; *p && o < t + sizeof t - 4; p++) {
            if (*p == '/') { memcpy(o, " > ", 3); o += 3; }
            else *o++ = *p;
        }
        *o = 0;
        dl_text(RX0 + 16, cy + 6, t, strlen(t), SS, C_DIM, RX0, cy, X1 - 10, cy + CRUMB);
    }
    TY0 = cy + CRUMB; TY1 = sy;
    int gut = 6 * KW, tx = RX0 + gut + 8;
    cols = (X1 - tx - 16) / KW; rows = (TY1 - TY0 - 4) / KH;
    if (cols < 4) cols = 4;
    if (rows < 1) rows = 1;
    in_view();
    dl_rect(RX0, TY0, X1, TY1, 0, C_TERM);
    dl_rect(RX0, TY0, RX0 + gut, TY1, 0, C_BG);         /* the gutter */
    dl_rect(RX0 + gut, TY0, RX0 + gut + 1, TY1, 0, C_LINE);
    int lang = lang_of(e->name), com = comment_at(e, line_pos(e, e->top), lang);
    int p = line_pos(e, e->top), cl = line_of(e, e->cur);
    static uint8_t k[512];
    for (int r = 0; r < rows && p <= e->len; r++) {
        int end = eol(e, p), ty = TY0 + 2 + r * KH, ln = e->top + r;
        if (ln == cl) dl_rect(RX0 + gut + 1, ty, X1, ty + KH, 0, C_BAR);   /* the cursor's line */
        char nb[12];
        int nl = num(nb, ln + 1);
        nb[nl] = 0;
        label(RX0 + gut - 10 - tw(nl, 2), ty + 1, nb, 2, ln == cl ? C_TXT : C_DIM);
        colour(e->b, p, end, lang, &com, k, (int)sizeof k);
        int s = p + e->left, n = end - s < cols ? end - s : cols;
        for (int i = 0; i < n; ) {                       /* runs of one colour */
            int j = i, kind = s - p + i < (int)sizeof k ? k[s - p + i] : K_TXT;
            while (j < n && (s - p + j < (int)sizeof k ? k[s - p + j] : K_TXT) == kind) j++;
            dl_text(tx + i * KW, ty + 1, e->b + s + i, j - i, S(K), kcol[kind], RX0, TY0, X1 - 8, TY1);
            if (kind == K_KEY) dl_text(tx + i * KW + 1, ty + 1, e->b + s + i, j - i, S(K), kcol[kind], RX0, TY0, X1 - 8, TY1);
            i = j;
        }
        if (focused && e->cur >= p && e->cur <= end) {   /* the cursor: a block, its character dark on it */
            int c = e->cur - p - e->left;
            if (c >= 0 && c < cols) {
                dl_rect(tx + c * KW, ty, tx + (c + 1) * KW, ty + KH, 0, C_ACC);
                if (e->cur < end) dl_text(tx + c * KW, ty + 1, e->b + e->cur, 1, S(K), C_BG, RX0, TY0, X1, TY1);
            }
        }
        if (end >= e->len) break;
        p = end + 1;
    }
    int total = line_of(e, e->len) + 1;                  /* where we are: a bar on the right */
    if (total > rows) {
        int h = TY1 - TY0, bh = h * rows / total < 20 ? 20 : h * rows / total, by = TY0 + (h - bh) * e->top / (total - rows > 0 ? total - rows : 1);
        if (by + bh > TY1) by = TY1 - bh;
        dl_rect(X1 - 6, by, X1 - 2, by + bh, 2, C_LINE);
    }

    /* the status bar: inverse */
    dl_rect(X0, sy, X1, Y1, 0, msg_err ? C_ERR : C_ACC);
    char t[160], *o = t;
    o += strlcpy(o, " ", 2);
    o += strlcpy(o, e->name, 48);
    o += strlcpy(o, "   Ln ", 8); o += num(o, cl + 1);
    o += strlcpy(o, ", Col ", 8); o += num(o, e->cur - bol(e, e->cur) + 1);
    static const char *ln[] = { "Text", "C", "Shell", "Markdown" };
    o += strlcpy(o, "   ", 4); o += strlcpy(o, lang == L_HASH ? "Shell / Python" : ln[lang], 20);
    o += strlcpy(o, "   ", 4); o += strlcpy(o, e->ro ? "read only" : e->dirty ? "changed" : "saved", 12);
    if (msg[0]) { o += strlcpy(o, "   ", 4); strlcpy(o, msg, sizeof t - (size_t)(o - t)); }
    dl_text(X0 + 8, sy + 5, t, strlen(t), S(2), C_BG, X0, sy, X1 - 10, Y1);
    const char *keys = "Ctrl+S save  Ctrl+W close  F5/F6 tabs  Ctrl+B tree ";
    int kl = strlen(keys), kx = X1 - 10 - tw(kl, 2);
    if (kx > X0 + 8 + tw(strlen(t), 2) + 30) dl_text(kx, sy + 5, keys, kl, S(2), C_BG, X0, sy, X1, Y1);
}
