/*
 * atlas - The SIEOS desktop, laid out as BeOS was, in SIEOS's colours.
 *
 * One process does it all, to keep the system small: it drives the screen
 * (screen.c), draws (draw.c), shows the login screen and the first start
 * (setup.c), is the terminal of the shells in its windows, edits files
 * (edit.c), lists folders (files.c), keeps the settings (settings.c) and
 * talks to the assistant (panel.c). init supervises it.
 *
 *     con ──input events──► atlas ◄──CON_WRITE / CON_READ ("atlas#3")── sh
 *      (keyboard, mouse)      │  ├──AUTH_LOGIN──► auth ──starts──► sh (as you)
 *                             │  └──SIA_ASK / SIA_NEXT (its own thread)──► sia
 *                             └──draws──► the screen
 *
 * The screen:
 *
 *   ┌─ sia strip: find a file, start an app, ask sia ─┐ ┌── Deskbar ──────┐
 *   │                                                 │ │ (cube) SIEOS    │ the menu
 *   └─────────────────────────────────────────────────┘ │ sia: idle  12:30│
 *   ┌────────────┐                                      │ > Terminal      │ apps: running
 *   │ - Terminal □│                                     │   sia           │ ones have >
 *   ├────────────┴───────────────┐                      │   Files         │
 *   │  windows: a tab each,      │                      │   ...           │
 *   │  moved, resized, zoomed    │                      │ [1][2][3][4]    │ workspaces
 *   └────────────────────────────┘                      └─────────────────┘
 *
 * Windows (wm.c): terminals (a shell as you), the editor, sia, Settings,
 * Files. Right-click: a window's tab, its menu; a Deskbar app, its windows;
 * the desk, a menu to start things. Minimized windows come back from the
 * Deskbar or its menu.
 *
 * Keys: Ctrl+Space the sia strip, Ctrl+T a terminal, Ctrl+W close the
 * window, Ctrl+Tab the next window, Ctrl+1..4 a workspace, Ctrl+V paste,
 * Ctrl+S save (editor), Esc back.
 *
 * Nothing runs when nothing happens: no animation loop. The picture
 * changes only on an event (input, a shell's output, a piece of an answer,
 * the clock's minute), and only the changed region is sent to the screen.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"
#include "mk/crypto.h"               /* wipe: forget the password */

#define ATLAS_INPUT 100             /* from our input thread: con_event_t[] in sbuf */
#define ATLAS_TICK  102             /* from the clock thread: a new minute */

#define TCMAX 132                   /* a terminal: up to 132 x 60 characters */
#define TRMAX 60
#define HIST 500                    /* lines kept per terminal, scrollback included */
#define MAXITEM 256
#define MAXTERM 8
#define MAXAPP 16
#define W_EDIT MAXTERM              /* window ids: 0..MAXTERM-1 terminals, then these */
#define W_SIA (MAXTERM + 1)
#define W_SET (MAXTERM + 2)
#define W_FILES (MAXTERM + 3)
#define W_PINS (MAXTERM + 4)
#define W_REPO (MAXTERM + 5)
#define W_MAIL (MAXTERM + 6)
#define NWIN (MAXTERM + 7)
_Static_assert(NWIN == WM_MAX && W_SIA == WM_SIA && W_SET == WM_SET && W_EDIT == WM_EDIT && W_PINS == WM_PINS && W_FILES == WM_FILES && W_REPO == WM_REPO && W_MAIL == WM_MAIL, "wm.c knows the window ids");
#define TBAR 48                     /* the insides of a window start this far below their top */

typedef struct { char name[48], path[160]; int app; } item_t;
typedef struct {
    int used, chan, pid, cols, rows;
    int cx, cy, esc, back;          /* cursor (on the screen rows); scrolled back by `back` lines */
    uint64_t top;                   /* number of the line at the top of the screen */
    unsigned seen;                  /* lines typed when we last looked at the files */
    char *ring;                     /* the last HIST lines, TCMAX characters each */
    tty_t tty;
    char pend[160];                 /* an app's command line, typed when its shell first reads */
} term_t;

static item_t items[MAXITEM];       /* the user's files (for the sia strip and sia) */
static term_t terms[MAXTERM];
static struct { char name[48], path[96]; int kind; } apps[MAXAPP];   /* the system's launchers (/etc/apps) */
static int nitems, napps, next_chan = 1;
static int focus = -1;              /* the window with the keys, or -1 */
static int strip_on;                /* the sia strip has the keys */
static int sia_on, set_on, files_on, pins_on, repo_on;   /* those windows exist */
static int last_term = -1;          /* the terminal used last (the Deskbar shows it) */
static char toast[96];              /* a short message under the strip (until the next action) */
int atlas_debug;                     /* /tmp/atlas.debug exists: log places (tests) */
char clip[4096];
int clip_n;

/* ---- Session state. */
static enum { GREETER, SESSION } mode;
static char user[32], home[64], gname[32], gpw[128], gmsg[80], query[48];
static int uid = -1, ugid = -1, gfield, first_start;
static int no_input;                    /* no keyboard or pointer yet: the greeter says so */
static int m_speed = 100, m_swap, m_rev;   /* the user's mouse (~/.mouse): speed %, left-handed, wheel reversed */
static screen_t scr;
static mk_groups_t ugroups;
static long port, auth_port;
static int ptr_x = 960, ptr_y = 540, buttons, press_x, press_y;
static uint64_t frame_us;

/* ---- The layout, computed from the screen's size (any size works). */
static int M, DBW, DBX, STX1, STY1;     /* margin; the Deskbar's width and left; the strip's right and bottom */
#define DB_MENU 52                      /* the Deskbar: its menu row, tray, app rows, workspaces */
#define DB_TRAY 40
#define DB_ROW 40
#define DB_WS 48
static void geometry(void)
{
    M = 16;
    DBW = SW >= 1600 ? 300 : 260;
    DBX = SW - M - DBW;
    STX1 = DBX - M; STY1 = M + 48;
    wm_area(M, STY1 + 14 + TAB_H, STX1, SH - M);     /* (windows: under the strip, beside the Deskbar) */
}

/* ---- Menus: one at a time; entries with an icon, separators. */
enum { A_SEP = 1, A_TERM, A_SIA, A_FILES, A_EDITOR, A_SETTINGS, A_PINS, A_RUNAPP, A_WINDOW, A_LOGOUT,
       A_MIN, A_ZOOM, A_SENDWS, A_CLOSE };
#define MAXMENT 28
#define MENH 36
#define MENSEP 12
static struct {
    int on, x, y, w, n, win, hover;          /* win: the window it is about, or -1 */
    char title[48];
    struct { char label[48], arg[160]; int act, icon; } e[MAXMENT];
} mn;

/* A name typed in a small box (the editor's untitled file, saved). */
static struct { int on; char title[48], buf[32]; } pr;

/* What a held left button does once the pointer moves. */
static enum { D_NONE, D_SEL, D_WIN } drag;
static int drag_on, drag_arg;
static struct { int t, on; uint64_t al, bl; int ac, bc; } sel = { -1, 0, 0, 0, 0, 0 };

/* The clock: the time of day from the machine's clock at start, then the
 * kernel's monotonic time on top of it (no clock driver needed). */
static long rtc_day, rtc_t0;

/* A line in the kernel log (the tests follow these). */
static struct { char b[200]; size_t n; } logl;
static void logput(void *c, char ch) { (void)c; if (logl.n < sizeof logl.b) logl.b[logl.n++] = ch; }
void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    logl.n = 0;
    vformat(logput, 0, fmt, ap);
    va_end(ap);
    sys_debug(logl.b, logl.n);
}

/* v in decimal at p (no 0 added) -> its length. */
int num(char *p, long v)
{
    char t[20];
    int n = 0, k = 0;
    if (v < 0) { p[k++] = '-'; v = -v; }
    do t[n++] = '0' + v % 10; while (v /= 10);
    while (n) p[k++] = t[--n];
    return k;
}

static void rtc_read(void)
{
    long t = rtc_seconds();
    rtc_day = t < 0 ? 0 : t;
    rtc_t0 = sys_clock();
}
static long now_day(void) { return (rtc_day + (sys_clock() - rtc_t0) / 1000000000) % 86400; }

static void clock_thread(void *arg)                      /* one message a minute, nothing else */
{
    (void)arg;
    for (;;) {
        sys_sleep((uint64_t)(60 - now_day() % 60) * 1000000000 + 50000000);
        msg_t m = { .w = { ATLAS_TICK } };
        ipc_call(port, &m);
    }
}

/* ---- Terminals: a ring of lines (the screen is its last `rows` lines)
 * and a typed line (tty.c). */
static char *tline(const term_t *t, uint64_t abs) { return t->ring + (abs % HIST) * TCMAX; }
static int hist_max(const term_t *t) { return (int)(t->top < (uint64_t)(HIST - t->rows) ? t->top : (uint64_t)(HIST - t->rows)); }

static void term_scroll(term_t *t)
{
    t->top++;
    memset(tline(t, t->top + t->rows - 1), ' ', TCMAX);
}

static void term_put(term_t *t, char c)
{
    if (t->esc == 1) { t->esc = c == '[' ? 2 : 0; return; }
    if (t->esc == 2) {                                   /* ESC [ ... H, J, K */
        if (c < '@' || c > '~') return;
        t->esc = 0;
        if (c == 'H') t->cx = t->cy = 0;
        if (c == 'J') for (int r = 0; r < t->rows; r++) memset(tline(t, t->top + r), ' ', TCMAX);
        if (c == 'K') memset(tline(t, t->top + t->cy) + t->cx, ' ', TCMAX - t->cx);
        return;
    }
    switch (c) {
    case 033:  t->esc = 1; return;
    case '\n': t->cx = 0; t->cy++; break;
    case '\r': t->cx = 0; break;
    case '\b': if (t->cx) t->cx--; break;
    case '\t': t->cx = (t->cx + 8) & ~7; break;
    default:   if (c >= 32 && c < 127) tline(t, t->top + t->cy)[t->cx++] = c;
    }
    if (t->cx >= t->cols) { t->cx = 0; t->cy++; }
    if (t->cy >= t->rows) { term_scroll(t); t->cy = t->rows - 1; }
}
static void term_write(term_t *t, const char *s, size_t n)
{
    t->back = 0;                                         /* new output: back to the bottom */
    for (size_t i = 0; i < n; i++) term_put(t, s[i]);
}
static void echo_out(tty_t *tt, const char *s, size_t n) { term_write(tt->ctx, s, n); }

/* A new size in characters (the room its window gets): the cursor's line stays on the screen. */
static void term_resize(term_t *t, int cols, int rows)
{
    cols = cols < 20 ? 20 : cols > TCMAX ? TCMAX : cols;
    rows = rows < 4 ? 4 : rows > TRMAX ? TRMAX : rows;
    if (cols == t->cols && rows == t->rows) return;
    if (rows < t->rows && t->cy >= rows) { t->top += t->cy - rows + 1; t->cy = rows - 1; }
    for (int r = t->rows; r < rows; r++) memset(tline(t, t->top + r), ' ', TCMAX);   /* (old lines in the ring) */
    t->cols = cols; t->rows = rows;
    if (t->cx >= cols) t->cx = cols - 1;
    t->back = 0;
}

/* ---- Files: the home folder (for the sia strip and sia). Launchers
 * ("*.app") are named by their "name=". */
static int same_n(const char *a, const char *b, size_t n)   /* the first n characters (stops at a 0) */
{
    for (size_t i = 0; i < n; i++) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; }
    return 1;
}

/* A launcher's field ("key=value" lines) into out; 0 if missing. */
static int app_field(const char *text, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : 0) {
        if (!same_n(p, key, kl) || p[kl] != '=') continue;
        const char *v = p + kl + 1;
        size_t n = 0;
        while (v[n] && v[n] != '\n' && n + 1 < cap) { out[n] = (uint8_t)v[n] < 32 ? ' ' : v[n]; n++; }
        out[n] = 0;
        return 1;
    }
    out[0] = 0;
    return 0;
}
static int is_app(const char *name) { size_t n = strlen(name); return n > 4 && !strcmp(name + n - 4, ".app"); }
static void app_name(const char *path, char *out, size_t cap)   /* a launcher's name=, or its file name */
{
    size_t len = 0;
    char *f = file_get(path, &len);
    if (!f || !app_field(f, "name", out, cap) || !out[0]) strlcpy(out, base_name(path), cap);
    free(f);
}

/* Atlas runs as root (it owns the screen, it starts the shells), so the
 * file server would let it do anything: before reading a file's text or
 * changing a file, atlas checks the rights of the user logged in. */
int atlas_may(const char *path, int write)
{
    siefs_stat_t st;
    if (uid < 0 || fs_stat(path, &st, 0)) return 0;
    for (const char *p = path; write && *p; p++)        /* a snapshot: read only, for everyone */
        if (!memcmp(p, "/.snapshots", 11) && (p[11] == '/' || !p[11])) return 0;
    if (uid == 0) return 1;
    int bit = write ? 2 : 4, in = (int)st.gid == ugid;
    if ((int)st.uid == uid) return st.mode >> 6 & bit;
    for (int i = 0; i < ugroups.n && !in; i++) in = ugroups.g[i] == (int)st.gid;
    return in ? st.mode >> 3 & bit : st.mode & bit;
}

static void scan_dir(const char *dir, int depth)
{
    long h = fs_open(dir, FS_RDONLY | FS_DIRECTORY, 0);
    if (h < 0) return;
    char *buf = malloc(4096);                            /* (recursive: not static) */
    if (!buf) { fs_close(h); return; }
    uint64_t cs = 0, ino;
    long n;
    int type;
    char name[256];
    while ((n = fs_readdir(h, &cs, buf, 4096)) > 0)
        for (const char *p = buf; fs_dirent(&p, buf + n, &ino, &type, name); ) {
            if (name[0] == '.') continue;                /* hidden, ".", ".." */
            char path[160];
            size_t k = strlcpy(path, dir, sizeof path);
            if (k + strlen(name) + 2 > sizeof path) continue;
            if (path[k - 1] != '/') path[k++] = '/';
            strlcpy(path + k, name, sizeof path - k);
            if (type == 4 && depth < 4) { scan_dir(path, depth + 1); continue; }
            if (type != 8 || nitems == MAXITEM) continue;
            item_t *it = &items[nitems++];
            strlcpy(it->path, path, sizeof it->path);
            it->app = is_app(name);
            if (it->app) app_name(path, it->name, sizeof it->name); else strlcpy(it->name, name, sizeof it->name);
        }
    free(buf);
    fs_close(h);
}
static void rescan(void)
{
    nitems = 0;
    if (home[0]) scan_dir(home, 0);
    say("atlas: files: %d\n", nitems);
}

/* ---- Windows: what each one is. */
static int win_used(int w)
{
    return w >= 0 && w < MAXTERM ? terms[w].used : w == W_EDIT ? edit_active() : w == W_SIA ? sia_on :
           w == W_SET ? set_on : w == W_FILES ? files_on : w == W_PINS ? pins_on : w == W_REPO ? repo_on : w == W_MAIL ? mail_on() : 0;
}
static void win_title(int w, char *t, size_t cap)
{
    if (w < MAXTERM) { size_t n = strlcpy(t, "Terminal ", cap); t[n + num(t + n, terms[w].chan)] = 0; }
    else if (w == W_EDIT) { strlcpy(t, edit_name(), cap); if (edit_dirty()) strlcpy(t + strlen(t), " *", cap - strlen(t)); }
    else if (w == W_SIA) strlcpy(t, "sia", cap);
    else if (w == W_SET) strlcpy(t, "Settings", cap);
    else if (w == W_PINS) strlcpy(t, "Pins", cap);
    else if (w == W_REPO) strlcpy(t, "Git", cap);
    else if (w == W_MAIL) strlcpy(t, "Mail", cap);
    else strlcpy(t, "Files", cap);
    if (w < MAXTERM && terms[w].back) { size_t n = strlen(t); n += strlcpy(t + n, " [history -", 16); t[n + num(t + n, terms[w].back)] = 0; strlcpy(t + strlen(t), "]", 2); }
}
static int win_icon(int w) { return w < MAXTERM ? I_TERM : w == W_EDIT ? I_FILE : w == W_SIA ? I_SIA : w == W_SET ? I_SETTINGS : w == W_PINS ? I_PINS : w == W_REPO ? I_GIT : w == W_MAIL ? I_MAIL : I_FOLDER; }
int wm_icon(int w) { return win_icon(w); }

/* wm.c asks: is it there, its title, draw its inside. */
static void draw_term_body(int ti, int x0, int y0, int x1, int y1, int f);
int wm_state(int w) { return win_used(w); }
void wm_title(int w, char *t, size_t cap) { win_title(w, t, cap); }
void wm_info(int w, char *t, size_t cap)                 /* the strip's right side, its tab up */
{
    t[0] = 0;
    if (w < MAXTERM) {                                   /* "80 x 25", "history -12" */
        char *p = t + strlcpy(t, terms[w].back ? "history -" : "", cap);
        if (terms[w].back) { p[num(p, terms[w].back)] = 0; return; }
        p += num(p, terms[w].cols); p += strlcpy(p, " x ", 4); p[num(p, terms[w].rows)] = 0;
    } else if (w == W_EDIT) strlcpy(t, !edit_path()[0] ? "new file" : edit_dirty() ? "not saved" : "saved", cap);
    else if (w == W_FILES) strlcpy(t, files_dir(), cap);
    else if (w == W_PINS) strlcpy(t, "GPIO", cap);
    else if (w == W_REPO) strlcpy(t, repo_dir(), cap);
    else if (w == W_MAIL) mail_info(t, cap);
}
void wm_body(int w, int x0, int y0, int x1, int y1, int keys)
{
    if (w < MAXTERM) draw_term_body(w, x0, y0, x1, y1, keys);
    else if (w == W_EDIT) edit_draw(x0, y0, x1, y1, keys);
    else if (w == W_SIA) panel_draw(x0, y0, x1, y1, keys);
    else if (w == W_SET) settings_draw(x0, y0, x1, y1, keys);
    else if (w == W_PINS) pins_draw(x0, y0, x1, y1, keys);
    else if (w == W_REPO) repo_draw(x0, y0, x1, y1, keys);
    else if (w == W_MAIL) mail_draw(x0, y0, x1, y1, keys);
    else files_draw(x0, y0, x1, y1, keys);
}

/* Where window w is on the screen -> 1, or 0: not shown. (y0: its top;
 * its insides start at y0 + WM_BODY_DY + TBAR + 8: wm_body is given y0 + WM_BODY_DY.) */
static int win_box(int w, int *x0, int *y0, int *x1, int *y1) { return win_used(w) && wm_box(w, x0, y0, x1, y1); }

/* Keep the keys on a window that is on the screen. */
static void tidy(void)
{
    if (focus >= 0 && (!win_used(focus) || !wm_box(focus, &(int){0}, &(int){0}, &(int){0}, &(int){0}))) focus = wm_top();
}
static void show_win(int w)                              /* on the screen, in front, with the keys */
{
    wm_show(w);
    focus = w; strip_on = 0;
    if (w < MAXTERM) last_term = w;
    say("atlas: window %d shown\n", w);
}
static void min_win(int w)
{
    wm_min(w);
    if (focus == w) focus = wm_top();
}

/* ---- Click boxes. */
#define MAXHIT 600
static struct { int x0, y0, x1, y1, kind, arg; } hit[MAXHIT];
static int nhit;
void hitbox(int x0, int y0, int x1, int y1, int kind, int arg)
{
    if (nhit < MAXHIT && x1 > 0 && y1 > 0 && x0 < SW && y0 < SH)
        hit[nhit++] = (typeof(hit[0])){ x0, y0, x1, y1, kind, arg };
}
static int hit_at(int x, int y)                          /* the topmost box at (x, y), or -1 */
{
    for (int i = nhit - 1; i >= 0; i--)
        if (x >= hit[i].x0 && x < hit[i].x1 && y >= hit[i].y0 && y < hit[i].y1) return i;
    return -1;
}

/* A rounded "pill" with text, scale 2. Returns its right edge. */
int pill(int x, int y, const char *s, int on, int kind, int arg)
{
    int w = tw(strlen(s), 2) + 28;
    dl_rect(x, y, x + w, y + 42, 21, on ? C_ACC : C_GLASS);
    dl_frame(x, y, x + w, y + 42, 21, 1, on ? C_ACC : C_LINE);
    dl_text(x + 14, y + 10, s, strlen(s), S(2), on ? C_INK : C_TXT, 0, 0, SW, SH);
    if (kind) hitbox(x, y, x + w, y + 42, kind, arg);
    return x + w;
}
void label(int x, int y, const char *s, int k, uint32_t c) { dl_text(x, y, s, strlen(s), S(k), c, 0, 0, SW, SH); }
static void bold(int x, int y, const char *s, uint32_t c, int cx1)   /* (twice, a pixel apart) */
{
    dl_text(x, y, s, strlen(s), S(2), c, 0, 0, cx1, SH);
    dl_text(x + 1, y, s, strlen(s), S(2), c, 0, 0, cx1, SH);
}

/* ---- The sia strip: files (by name), apps, and "Ask sia". */
enum { R_FILE = 1, R_APP, R_SYSAPP, R_ASK };
#define MAXRES 8
static struct { int kind, i; } res[MAXRES];
static int nres, nfound;                                 /* nfound: the files of an attribute search */
static int has(const char *s, const char *q)
{
    for (; *s; s++) {
        int i = 0;
        while (q[i] && ((s[i] | 32) == (q[i] | 32))) i++;
        if (!q[i]) return 1;
    }
    return 0;
}
static void search(void)
{
    nres = 0; nfound = 0;
    if (!query[0]) return;
    if (query[0] != '?') {
        for (int i = 0; i < nitems && nres < 5; i++) if (!items[i].app && has(items[i].name, query)) res[nres++] = (typeof(res[0])){ R_FILE, i };
        for (int i = 0; i < nitems && nres < 7; i++) if (items[i].app && has(items[i].name, query)) res[nres++] = (typeof(res[0])){ R_APP, i };
        for (int i = 0; i < napps && nres < 7; i++) if (has(apps[i].name, query)) res[nres++] = (typeof(res[0])){ R_SYSAPP, i };
    }
    res[nres++] = (typeof(res[0])){ R_ASK, 0 };
}

int atlas_find(const char *key, const char *value)
{
    static char buf[4096];
    uint64_t cs = 0;
    long n;
    int total = 0;
    nres = 0;
    do {
        if ((n = fs_find(key, value, &cs, buf, sizeof buf)) <= 0) break;
        for (const char *p = buf; p < buf + n; p += strlen(p) + 1)
            for (int i = 0; i < nitems; i++)
                if (!strcmp(items[i].path, p) && atlas_may(p, 0)) {
                    total++;
                    if (nres < MAXRES) res[nres++] = (typeof(res[0])){ R_FILE, i };
                    break;
                }
    } while (cs);
    size_t k = strlcpy(query, key, sizeof query);
    if (k + 1 < sizeof query) { query[k] = '='; strlcpy(query + k + 1, value, sizeof query - k - 1); }
    nfound = total;
    strip_on = 1;
    say("atlas: strip %s: %d files\n", query, total);
    return total;
}

/* ---- The picture. */
static void draw_greeter(void)
{
    if (first_start) { setup_draw(); return; }          /* no account yet: the welcome screen */
    int x0 = SW / 2 - 360, y0 = SH / 2 - 230, x1 = SW / 2 + 360, y1 = SH / 2 + 250;
    static const char wt[] = "Welcome to SIEOS";     /* the card is a window: its tab first */
    int t = 44 + tw(sizeof wt - 1, 2) + 24;
    dl_rect(x0, y0 - TAB_H, x0 + t, y0 + 12, 10, C_ACC);
    bold(x0 + 22, y0 - TAB_H + 6, wt, C_INK, SW);
    dl_rect(x0, y0, x1, y1, 16, C_GLASS);
    dl_frame(x0, y0, x1, y1, 16, 2, C_ACC);
    dl_rect(x0 + 2, y0, x0 + t - 2, y0 + 2, 0, C_ACC);
    logo(x0 + 50, y0 + 40, 5);
    label(x0 + 150, y0 + 46, "SIEOS", 6, C_ACCT);
    label(x0 + 152, y0 + 120, "your windows, your files, sia", 2, C_DIM);
    const char *names[2] = { "Name", "Password" };
    for (int f = 0; f < 2; f++) {
        int fy = y0 + 190 + f * 90;
        label(x0 + 60, fy + 12, names[f], 2, C_DIM);
        dl_rect(x0 + 230, fy, x1 - 60, fy + 50, 10, C_FIELD);
        dl_frame(x0 + 230, fy, x1 - 60, fy + 50, 10, gfield == f ? 2 : 1, gfield == f ? C_ACC : C_LINE);
        char st[64];
        int n = strlen(gpw) < sizeof st - 1 ? strlen(gpw) : sizeof st - 1;
        memset(st, '*', n);
        dl_text(x0 + 250, fy + 14, f ? st : gname, f ? n : (int)strlen(gname), S(2), C_TXT, 0, 0, x1 - 70, SH);
        int cw = tw(f ? n : (int)strlen(gname), 2);
        if (gfield == f) dl_rect(x0 + 252 + cw, fy + 12, x0 + 264 + cw, fy + 38, 0, C_ACC);
        hitbox(x0 + 230, fy, x1 - 60, fy + 50, H_FIELD, f);
    }
    if (gmsg[0]) label(x0 + 60, y0 + 380, gmsg, 2, C_ERR);
    else if (no_input) label(x0 + 60, y0 + 380, "No keyboard or mouse yet: plug one in", 2, C_DIM);
    pill(x1 - 60 - tw(6, 2) - 28, y1 - 80, "Log in", 1, H_LOGIN, 0);
}

/* Is character (abs line, column) of terminal t selected? */
static int selected(int t, uint64_t l, int c)
{
    if (!sel.on || sel.t != t) return 0;
    uint64_t al = sel.al, bl = sel.bl;
    int ac = sel.ac, bc = sel.bc;
    if (al > bl || (al == bl && ac > bc)) { uint64_t tl = al; al = bl; bl = tl; int tc = ac; ac = bc; bc = tc; }
    return (l > al || (l == al && c >= ac)) && (l < bl || (l == bl && c <= bc));
}

/* A terminal's text in the rectangle under its title strip. */
static void draw_term_body(int ti, int x0, int y0, int x1, int y1, int f)
{
    term_t *t = &terms[ti];
    term_resize(t, (x1 - x0 - 28) / KW, (y1 - y0 - TBAR - 16) / KH);
    dl_rect(x0 + 2, y0 + TBAR, x1 - 2, y1 - 2, 0, C_TERM);
    uint64_t first = t->top - t->back;                   /* the line shown at the top */
    for (int r = 0; r < t->rows; r++) {
        uint64_t l = first + r;
        int ty = y0 + TBAR + 8 + r * KH;
        if (sel.on && sel.t == ti)                       /* the selection, behind the text */
            for (int c = 0; c < t->cols; c++) {
                if (!selected(ti, l, c)) continue;
                int e = c;
                while (e + 1 < t->cols && selected(ti, l, e + 1)) e++;
                dl_rect(x0 + 14 + c * KW, ty, x0 + 14 + (e + 1) * KW, ty + KH, 0, C_SEL);
                c = e;
            }
        const char *tl = tline(t, l);
        int n = t->cols;
        while (n && tl[n - 1] == ' ') n--;               /* (blank ends cost nothing to draw) */
        if (n) dl_text(x0 + 14, ty, tl, n, S(K), C_TERMFG, x0, y0, x1 - 4, y1 - 4);
    }
    if (f && !t->back)                                   /* the cursor */
        dl_rect(x0 + 14 + t->cx * KW, y0 + TBAR + 8 + t->cy * KH, x0 + 14 + t->cx * KW + KW - 2, y0 + TBAR + 6 + (t->cy + 1) * KH, 0, C_TERMFG);
    hitbox(x0, y0 + TBAR, x1, y1, H_WIN, ti);
}

/* ---- The Deskbar: the menu (the cube), a tray (sia, the clock), the
 * apps (running ones in bold, with a >), the workspaces. */
static const struct { const char *name; int icon, act; } dbapps[] = {
    { "Terminal", I_TERM, A_TERM }, { "sia", I_SIA, A_SIA }, { "Files", I_FOLDER, A_FILES },
    { "Text editor", I_FILE, A_EDITOR }, { "Pins", I_PINS, A_PINS }, { "Settings", I_SETTINGS, A_SETTINGS },
};
#define NDB (int)(sizeof dbapps / sizeof dbapps[0])
static int db_running(int k)
{
    if (dbapps[k].act == A_TERM) { for (int t = 0; t < MAXTERM; t++) if (terms[t].used) return 1; return 0; }
    return win_used(dbapps[k].act == A_SIA ? W_SIA : dbapps[k].act == A_FILES ? W_FILES : dbapps[k].act == A_EDITOR ? W_EDIT : dbapps[k].act == A_PINS ? W_PINS : W_SET);
}
static int db_h(void) { return DB_MENU + DB_TRAY + NDB * DB_ROW + DB_WS + 10; }
static void clock_text(char *ck)
{
    long d = now_day();
    ck[0] = '0' + d / 36000; ck[1] = '0' + d / 3600 % 10; ck[2] = ':';
    ck[3] = '0' + d / 600 % 6; ck[4] = '0' + d / 60 % 10; ck[5] = 0;
}
static void draw_deskbar(void)
{
    int x0 = DBX, y0 = M, x1 = DBX + DBW, y1 = M + db_h();
    dl_rect(x0, y0, x1, y1, 14, C_GLASS);
    dl_frame(x0, y0, x1, y1, 14, 1, C_LINE);
    hitbox(x0, y0, x1, y1, H_DESK, -1);
    int menu_open = mn.on && mn.win == -2;               /* the menu row: the cube, SIEOS */
    if (menu_open || (ptr_x >= x0 && ptr_x < x1 && ptr_y >= y0 && ptr_y < y0 + DB_MENU)) dl_rect(x0 + 6, y0 + 6, x1 - 6, y0 + DB_MENU - 4, 10, C_BAR);
    logo(x0 + 16, y0 + 10, 2);
    bold(x0 + 60, y0 + 15, "SIEOS", C_TXT, x1);
    label(x1 - 34, y0 + 15, "v", 2, C_DIM);
    hitbox(x0 + 6, y0 + 6, x1 - 6, y0 + DB_MENU - 4, H_DBMENU, 0);
    if (atlas_debug) say("atlas: dbg dbmenu at %d %d\n", x0 + 100, y0 + DB_MENU / 2);
    int ty = y0 + DB_MENU;                               /* the tray */
    dl_rect(x0 + 10, ty, x1 - 10, ty + 1, 0, C_LINE);
    char st[48], ck[8];
    panel_status(st, sizeof st);
    clock_text(ck);
    {                                                    /* sia's state: a dot (lit when sia runs), then the words */
        int idle = !strcmp(st, "sia: idle");
        dl_rect(x0 + 18, ty + 15, x0 + 28, ty + 25, 5, idle ? C_LINE : C_ACC);
        dl_text(x0 + 38, ty + 9, st, strlen(st), S(2), idle ? C_DIM : C_TXT, 0, 0, x1 - 100, SH);
    }
    dl_text(x1 - 16 - text_w(5, ST), ty + 6, ck, 5, ST, C_TXT, 0, 0, SW, SH);   /* (title size) */
    int ry = ty + DB_TRAY;                               /* the apps */
    dl_rect(x0 + 10, ry, x1 - 10, ry + 1, 0, C_LINE);
    for (int k = 0; k < NDB; k++) {
        int y = ry + 4 + k * DB_ROW, run = db_running(k);
        if (ptr_x >= x0 && ptr_x < x1 && ptr_y >= y && ptr_y < y + DB_ROW) dl_rect(x0 + 6, y, x1 - 6, y + DB_ROW - 2, 8, C_BAR);
        icon(dbapps[k].icon, x0 + 14, y + 7, 2);
        if (run) {                                       /* running: bold, a lit dot (and how many terminals) */
            bold(x0 + 50, y + 8, dbapps[k].name, C_TXT, x1 - 50);
            dl_rect(x1 - 30, y + 14, x1 - 18, y + 26, 6, C_ACC);
            if (dbapps[k].act == A_TERM) {
                int nt = 0;
                for (int t = 0; t < MAXTERM; t++) nt += terms[t].used;
                char c[4];
                if (nt > 1) { c[num(c, nt)] = 0; dl_text(x1 - 38 - text_w(strlen(c), SS), y + 11, c, strlen(c), SS, C_DIM, 0, 0, SW, SH); }
            }
        } else label(x0 + 50, y + 8, dbapps[k].name, 2, C_DIM);
        hitbox(x0 + 6, y, x1 - 6, y + DB_ROW - 2, H_DBAPP, k);
        if (atlas_debug) say("atlas: dbg dbapp %s at %d %d\n", dbapps[k].name, x0 + 120, y + DB_ROW / 2);
    }
    int wy = ry + 8 + NDB * DB_ROW, cw = (DBW - 20 - 3 * 8) / WM_NWS;   /* the workspaces */
    dl_rect(x0 + 10, wy - 2, x1 - 10, wy - 1, 0, C_LINE);
    for (int k = 0; k < WM_NWS; k++) {
        int cx = x0 + 10 + k * (cw + 8), on = k == wm_workspace();
        char d[2] = { (char)('1' + k), 0 };
        dl_rect(cx, wy + 6, cx + cw, wy + 40, 6, C_PANEL);   /* a cell: its windows, small (the current one outlined) */
        wm_minimap(k, cx + 22, wy + 11, cx + cw - 5, wy + 35, on ? C_DIM : C_LINE, C_TXT);
        dl_frame(cx, wy + 6, cx + cw, wy + 40, 6, on ? 2 : 1, on ? C_ACC : C_LINE);
        dl_text(cx + 6, wy + 9, d, 1, SS, on ? C_ACCT : C_DIM, 0, 0, SW, SH);
        hitbox(cx, wy + 6, cx + cw, wy + 40, H_DBWS, k);
        if (atlas_debug) say("atlas: dbg ws %d at %d %d\n", k + 1, cx + cw / 2, wy + 23);
    }
}

/* The sia strip, and its results under it. */
static void draw_strip(void)
{
    int x0 = M, y0 = M, x1 = STX1, y1 = STY1;
    dl_rect(x0, y0, x1, y1, 24, C_GLASS);
    dl_frame(x0, y0, x1, y1, 24, strip_on ? 2 : 1, strip_on ? C_ACC : C_LINE);
    hitbox(x0, y0, x1, y1, H_STRIP, 0);
    if (atlas_debug) say("atlas: dbg strip at %d %d\n", (x0 + x1) / 2, (y0 + y1) / 2);
    bold(x0 + 24, y0 + 13, "sia", C_ACCT, x1);
    int qx = x0 + 24 + tw(3, 2) + 24;
    dl_rect(qx - 12, y0 + 10, qx - 11, y1 - 10, 0, C_LINE);
    const char *net = port_find("net") > 0 ? "network" : "network: off";
    int nx = x1 - 24 - tw(strlen(net), 2);
    if (nx > qx + 400) label(nx, y0 + 13, net, 2, C_DIM); else nx = x1 - 20;
    if (query[0]) dl_text(qx, y0 + 13, query, strlen(query), S(2), C_TXT, x0, y0, nx - 20, y1);
    else label(qx, y0 + 13, strip_on ? "a file, an app, or a question" : "Ask sia, open a file, run an app...  Ctrl+Space", 2, C_DIM);
    if (strip_on) { int cw = tw(strlen(query), 2); dl_rect(qx + 2 + cw, y0 + 11, qx + 12 + cw, y0 + 35, 0, C_ACC); }
    if (toast[0]) dl_text(x0 + 24, y1 + 6, toast, strlen(toast), S(2), C_ACCT, 0, 0, x1, SH);
    if (!strip_on || (!nres && !nfound)) return;
    int rx1 = x0 + 900 < x1 ? x0 + 900 : x1, y = y1 + 6, last = 0, h = 16;
    for (int i = 0; i < nres; i++) h += 46 + (res[i].kind != last ? 32 : 0), last = res[i].kind == R_SYSAPP ? R_APP : res[i].kind;
    if (nfound > nres) h += 40;
    dl_rect(x0, y, rx1, y + h, 16, C_GLASS);
    dl_frame(x0, y, rx1, y + h, 16, 1, C_ACC);
    y += 8; last = 0;
    static const char *grp[5] = { "", "FILES", "APPS", "APPS", "ASK SIA" };
    for (int i = 0; i < nres; i++) {
        int g = res[i].kind == R_SYSAPP ? R_APP : res[i].kind;
        if (g != last) { label(x0 + 26, y + 8, grp[g], 2, C_DIM); y += 32; last = g; }
        dl_rect(x0 + 10, y, rx1 - 10, y + 42, 10, i == 0 ? C_BAR : C_GLASS);
        char t[96] = "", w[48] = "";
        int ic = I_FILE;
        if (res[i].kind == R_FILE) strlcpy(t, items[res[i].i].name, sizeof t);
        else if (res[i].kind == R_APP) { strlcpy(t, items[res[i].i].name, sizeof t); ic = I_APP; }
        else if (res[i].kind == R_SYSAPP) { strlcpy(t, apps[res[i].i].name, sizeof t); strlcpy(w, "app", sizeof w); ic = I_APP; }
        else { strlcpy(t, query[0] == '?' ? query + 1 : query, sizeof t); strlcpy(w, "Tab", sizeof w); ic = I_SIA; }
        icon(ic, x0 + 22, y + 9, 2);
        dl_text(x0 + 58, y + 10, t, strlen(t), S(2), C_TXT, x0, y, rx1 - 30 - tw(strlen(w), 2), y + 42);
        label(rx1 - 26 - tw(strlen(w), 2), y + 10, w, 2, C_DIM);
        hitbox(x0 + 10, y, rx1 - 10, y + 42, H_RES, i);
        if (atlas_debug) say("atlas: dbg res %d %s at %d %d\n", i, t, (x0 + rx1) / 2, y + 21);
        y += 46;
    }
    if (nfound > nres) { char t[48]; t[num(t, nfound - nres)] = 0; strlcpy(t + strlen(t), " more", 8); label(x0 + 26, y + 8, t, 2, C_DIM); }
}

static int menu_h(void) { int h = 16; for (int i = 0; i < mn.n; i++) h += mn.e[i].act == A_SEP ? MENSEP : MENH; return h; }
static void draw_menu(void)
{
    if (!mn.on) return;
    int h = menu_h(), y = mn.y + 8;
    dl_rect(mn.x, mn.y, mn.x + mn.w, mn.y + h, 12, C_GLASS);
    dl_frame(mn.x, mn.y, mn.x + mn.w, mn.y + h, 12, 2, C_ACC);
    for (int i = 0; i < mn.n; i++) {
        if (mn.e[i].act == A_SEP) { dl_rect(mn.x + 14, y + MENSEP / 2, mn.x + mn.w - 14, y + MENSEP / 2 + 1, 0, C_LINE); y += MENSEP; continue; }
        if (i == mn.hover) dl_rect(mn.x + 6, y, mn.x + mn.w - 6, y + MENH, 8, C_BAR);
        if (mn.e[i].icon >= 0) icon(mn.e[i].icon, mn.x + 16, y + 6, 2);
        uint32_t c = mn.e[i].act == A_CLOSE || mn.e[i].act == A_LOGOUT ? C_ERR : C_TXT;
        dl_text(mn.x + 54, y + 7, mn.e[i].label, strlen(mn.e[i].label), S(2), c, mn.x, y, mn.x + mn.w - 14, y + MENH);
        hitbox(mn.x, y, mn.x + mn.w, y + MENH, H_MENU, i);
        y += MENH;
    }
}

static void draw_prompt(void)
{
    if (!pr.on) return;
    int x0 = SW / 2 - 330, y0 = SH / 2 - 120, x1 = SW / 2 + 330, y1 = SH / 2 + 110;
    dl_rect(x0, y0, x1, y1, 18, C_GLASS);
    dl_frame(x0, y0, x1, y1, 18, 2, C_ACC);
    label(x0 + 30, y0 + 26, pr.title, 2, C_TXT);
    dl_rect(x0 + 30, y0 + 74, x1 - 30, y0 + 124, 10, C_FIELD);
    dl_frame(x0 + 30, y0 + 74, x1 - 30, y0 + 124, 10, 2, C_ACC);
    label(x0 + 48, y0 + 88, pr.buf, 2, C_TXT);
    int cw = tw(strlen(pr.buf), 2);
    dl_rect(x0 + 50 + cw, y0 + 86, x0 + 62 + cw, y0 + 112, 0, C_ACC);
    int okx = x1 - 30 - (tw(2, 2) + 28);
    pill(okx, y1 - 66, "OK", 1, H_PR, 1);
    pill(okx - 20 - (tw(6, 2) + 28), y1 - 66, "Cancel", 0, H_PR, 0);
}

/* An empty workspace: the time, large and dim, at the desk's bottom right,
 * and what to press under it. */
#define RC_K 10                                          /* (the clock's text scale) */
static const char rest_hint[] = "Ctrl+T a terminal   Ctrl+Space find or ask   the Deskbar: everything";
static void rest_box(int *x0, int *y0, int *x1, int *y1)
{
    int hw = text_w(sizeof rest_hint - 1, SS), cw = tw(5, RC_K);
    *x1 = SW - 72; *y1 = SH - 56;
    *x0 = *x1 - (hw > cw ? hw : cw); *y0 = *y1 - CH * RC_K - 44;
}
static void draw_rest(void)
{
    int x0, y0, x1, y1;
    char ck[8];
    rest_box(&x0, &y0, &x1, &y1);
    clock_text(ck);
    label(x1 - tw(5, RC_K), y0, ck, RC_K, C_LINE);
    dl_text(x1 - text_w(sizeof rest_hint - 1, SS), y1 - 22, rest_hint, sizeof rest_hint - 1, SS, C_DIM, 0, 0, SW, SH);
}

static void build(void)
{
    dl_reset();
    nhit = 0;
    dl_grid(0, 0, 40, C_BG, C_GRID);
    if (mode == GREETER) { draw_greeter(); dl_pointer(ptr_x, ptr_y); return; }
    hitbox(0, 0, SW, SH, H_DESK, 0);                     /* (the desk itself: its menu) */
    tidy();
    if (wm_top() < 0) draw_rest();
    wm_draw(focus, strip_on ? -1 : focus);
    draw_deskbar();
    draw_strip();
    draw_menu();
    draw_prompt();
    dl_pointer(ptr_x, ptr_y);
}

void redraw(void)
{
    uint64_t t0 = sys_clock();
    build();
    present(0, 0, SW, SH);
    frame_us = (sys_clock() - t0) / 1000;
}

void redraw_box(int x0, int y0, int x1, int y1)
{
    build();
    present(x0, y0, x1, y1);
}

/* Redraw, and (tests: /tmp/atlas.debug) log where things are. */
static void settle(void)
{
    siefs_stat_t st;
    atlas_debug = mode == SESSION && !fs_stat("/tmp/atlas.debug", &st, 1);
    redraw();
    if (atlas_debug) {
        for (int w = 0; w < MAXTERM; w++) {
            int x0, y0, x1, y1;
            if (win_box(w, &x0, &y0, &x1, &y1)) say("atlas: terminal %d at %d %d cell %d %d\n", terms[w].chan, x0 + 14, y0 + WM_BODY_DY + TBAR + 8, KW, KH);
        }
        say("atlas: dbg frame %lu us\n", (unsigned long)frame_us);
    }
    atlas_debug = 0;
}

/* ---- For Settings: the screen's size, the mouse. */
int atlas_uid(void) { return uid; }
void atlas_user(int *u, int *g, mk_groups_t *gr) { *u = uid; *g = ugid; *gr = ugroups; }
const char *atlas_home(void) { return uid >= 0 ? home : ""; }
int atlas_screen(int *w, int *h) { *w = PW; *h = PH; return scr.resizable; }

static void mouse_apply(void)                           /* con: how far a count goes, on this screen */
{
    long con = 0;
    msg_t m = { .w = { CON_MOUSE, m_speed, PW, PH } };
    call_named(&con, "console", &m, 1);
}

int atlas_set_size(int w, int h)
{
    int ow = PW, oh = PH, osw = SW, osh = SH;
    screen_t ns;
    if (!scr.resizable || w < 1024 || h < 600) return -EINVAL;
    PW = w; PH = h;
    if (screen_init(&ns)) { PW = ow; PH = oh; if (!screen_init(&ns)) scr = ns; draw_init(&scr); return -EIO; }
    scr = ns;
    draw_init(&scr);
    geometry();
    ptr_x = ptr_x * SW / osw; ptr_y = ptr_y * SH / osh;
    mouse_apply();
    settle();
    return 0;
}

void atlas_mouse_get(int *speed, int *swap, int *reverse) { *speed = m_speed; *swap = m_swap; *reverse = m_rev; }
void atlas_mouse(int speed, int swap, int reverse)
{
    m_speed = speed; m_swap = swap; m_rev = reverse;
    mouse_apply();
    char path[96], line[64], *p = line;
    strlcpy(path + strlcpy(path, home, sizeof path), "/.mouse", 8);
    p += strlcpy(p, "speed=", 8); p += num(p, speed);
    p += strlcpy(p, "\nswap=", 8); p += num(p, swap);
    p += strlcpy(p, "\nreverse=", 10); p += num(p, reverse);
    *p++ = '\n'; *p = 0;
    if (atlas_may(home, 1)) {
        long f = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC, 0644);
        if (f >= 0) { fs_write(f, 0, line, strlen(line)); fs_close(f); fs_chown(path, uid, ugid); }
    }
    say("atlas: mouse speed %d swap %d reverse %d\n", speed, swap, reverse);
}

static int conf_num(const char *text, const char *key, int def)   /* "key=N" in a small file */
{
    size_t kl = strlen(key);
    for (const char *p = text; p && *p; ) {
        if (same_n(p, key, kl) && p[kl] == '=') {
            int v = 0, any = 0;
            for (p += kl + 1; *p >= '0' && *p <= '9'; p++) { v = v * 10 + *p - '0'; any = 1; }
            return any ? v : def;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return def;
}
/* The look (draw.c) and the zoom: the user's own (~/.look), Phosphor at
 * 100% before a login. 100% is 0.75 screen pixels per picture pixel. */
static int zoom_pct = 100;
int atlas_look_get(void) { return draw_look_get(); }
int atlas_zoom_get(void) { return zoom_pct; }
static void zoom_apply(int pct)
{
    int osw = SW, osh = SH;
    zoom_pct = pct;
    draw_zoom(49152 * pct / 100);
    geometry();
    ptr_x = ptr_x * SW / osw; ptr_y = ptr_y * SH / osh;
}
static void look_save(void)
{
    char path[96], line[32], *p = line;
    strlcpy(path + strlcpy(path, home, sizeof path), "/.look", 7);
    p += strlcpy(p, "look=", 6); p += num(p, draw_look_get());
    p += strlcpy(p, "\nzoom=", 7); p += num(p, zoom_pct); *p++ = '\n'; *p = 0;
    if (uid >= 0 && atlas_may(home, 1)) {
        long f = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC, 0644);
        if (f >= 0) { fs_write(f, 0, line, strlen(line)); fs_close(f); fs_chown(path, uid, ugid); }
    }
}
void atlas_look(int k)
{
    if (k < 0 || k >= NLOOK) return;
    draw_look(k);
    look_save();
    say("atlas: look %d\n", k);
    redraw();
}
void atlas_zoom(int pct)
{
    if (pct < 50 || pct > 400) return;
    zoom_apply(pct);
    look_save();
    say("atlas: zoom %d%% (%d x %d)\n", pct, SW, SH);
    settle();
}
static void look_load(void)
{
    int k = LOOK_PHOSPHOR, z = 100;
    if (uid >= 0) {
        char path[96];
        size_t n = 0;
        strlcpy(path + strlcpy(path, home, sizeof path), "/.look", 7);
        char *f = atlas_may(path, 0) ? file_get(path, &n) : 0;
        if (f) {
            int v = conf_num(f, "look", k); k = v >= 0 && v < NLOOK ? v : k;
            v = conf_num(f, "zoom", z); z = v >= 50 && v <= 400 ? v : z;
            free(f);
        }
    }
    draw_look(k);
    if (z != zoom_pct) zoom_apply(z);
}

static void mouse_load(void)                            /* at login: the user's mouse; at logout: the default */
{
    look_load();
    m_speed = 100; m_swap = m_rev = 0;
    if (uid >= 0) {
        char path[96];
        size_t n = 0;
        strlcpy(path + strlcpy(path, home, sizeof path), "/.mouse", 8);
        char *f = atlas_may(path, 0) ? file_get(path, &n) : 0;
        if (f) {
            int sp = conf_num(f, "speed", 100);
            m_speed = sp >= 25 && sp <= 400 ? sp : 100;
            m_swap = conf_num(f, "swap", 0) == 1;
            m_rev = conf_num(f, "reverse", 0) == 1;
            free(f);
        }
    }
    mouse_apply();
}

/* ---- Terminals: open (a shell for the user, on "atlas#N"), close. */
long atlas_auth(msg_t *m) { long e = call_named(&auth_port, "auth", m, 0); return e < 0 ? e : (long)m->w[0]; }
/* The Wi-Fi server, only if it is already running (never waited for or
 * started: a machine without one would hang the desktop otherwise). */
long atlas_wifi(msg_t *m)
{
    long p = port_find("wifi");
    if (p <= 0) return -ENOENT;
    long e = ipc_call(p, m);
    return e < 0 ? e : (long)m->w[0];
}

static long open_term(const char *pw)
{
    int t = 0;
    while (t < MAXTERM && terms[t].used) t++;
    if (t == MAXTERM) { strlcpy(toast, "Too many terminals: close one first.", sizeof toast); return -EMFILE; }
    char *ring = malloc(HIST * TCMAX);
    if (!ring) return -ENOMEM;
    char req[200], *p = req;
    p += strlcpy(p, user, 32) + 1;
    p += strlcpy(p, pw ? pw : "", 128) + 1;
    p += strlcpy(p, "atlas#", 8);
    int chan = next_chan;                                /* (used up only if it opens) */
    p += num(p, chan);
    *p++ = 0;
    msg_t m = { .w = { AUTH_LOGIN, pw ? 0 : AUTH_NOPW }, .sbuf = req, .slen = p - req };
    long pid = atlas_auth(&m);
    wipe(req, sizeof req);
    if (pid < 0) { free(ring); return pid; }
    if (++next_chan == MAIL_CHAN) next_chan++;           /* (that one is Mail's) */
    term_t *w = &terms[t];
    memset(w, 0, sizeof *w);
    w->ring = ring;
    memset(ring, ' ', HIST * TCMAX);
    w->used = 1; w->chan = chan; w->pid = pid; w->cols = 80; w->rows = 25;
    tty_init(&w->tty, echo_out, w);
    say("atlas: terminal %d opened (pid %ld)\n", chan, pid);
    return t;
}

static void close_term(term_t *t)
{
    if (!t->used) return;
    say("atlas: terminal %d closed\n", t->chan);
    if (t->tty.reader) reply_val(t->tty.reader, -EIO);
    t->used = 0;
    free(t->ring);
    t->ring = 0;
    if (sel.t == t - terms) sel.on = 0;
    wm_gone(t - terms);
    if (last_term == t - terms) last_term = -1;
    tidy();
}

static void new_term(void)
{
    long t = open_term(0);
    if (t >= 0) show_win(t);
    settle();
}

static void close_win(int w)
{
    if (w < 0) return;
    if (w < MAXTERM) { if (terms[w].used) sys_kill(terms[w].pid); return; }   /* (closed when its shell ends) */
    if (w == W_EDIT && !edit_close_window()) { strlcpy(toast, "Not saved: close it again to discard.", sizeof toast); settle(); return; }
    if (w == W_SIA) { sia_on = 0; say("atlas: assistant closed\n"); }
    if (w == W_SET) { set_on = 0; settings_close(); say("atlas: settings closed\n"); }
    if (w == W_FILES) { files_on = 0; say("atlas: files closed\n"); }
    if (w == W_PINS) { pins_on = 0; pins_close(); say("atlas: pins closed\n"); }
    if (w == W_REPO) { repo_on = 0; repo_close(); say("atlas: git closed\n"); }
    if (w == W_MAIL) { mail_close(); say("atlas: mail closed\n"); }
    wm_gone(w);
    if (focus == w) focus = wm_top();
    settle();
}

static void logout(void)
{
    for (int t = 0; t < MAXTERM; t++) if (terms[t].used) { sys_kill(terms[t].pid); close_term(&terms[t]); }
    edit_close();
    sia_on = set_on = files_on = pins_on = repo_on = 0;
    repo_close();
    pins_logout();
    settings_close();
    mail_close();
    nitems = 0;
    mode = GREETER;
    uid = ugid = -1; ugroups.n = 0; gname[0] = gpw[0] = gmsg[0] = query[0] = toast[0] = home[0] = 0; gfield = 0;
    nres = nfound = 0; strip_on = 0; mn.on = pr.on = 0; focus = -1; last_term = -1;
    wm_reset();
    mouse_load();                                       /* (uid -1: the default mouse) */
    say("atlas: greeter\n");
}

/* No account yet? Then this screen does the first start, and says so to
 * the accounts server (login then waits instead of asking on the terminal). */
void greeter_check(void)
{
    msg_t m = { .w = { AUTH_STATE } };
    first_start = atlas_auth(&m) == 0;
    if (!first_start) return;
    msg_t d = { .w = { AUTH_DISPLAY, 1 } };
    atlas_auth(&d);
    say("atlas: first start: accounts needed (setup on the screen)\n");
}

static void login(void);
void greeter_login(const char *name, const char *pw)
{
    first_start = 0;
    strlcpy(gname, name, sizeof gname);
    strlcpy(gpw, pw, sizeof gpw);
    login();                                            /* (wipes gpw) */
}

/* The system's launchers (/etc/apps), for the strip and the menu. */
enum { K_TERMINAL, K_EDITOR, K_SETTINGS, K_GIT, K_MAIL };
static void scan_apps(void)
{
    napps = 0;
    long h = fs_open("/etc/apps", FS_RDONLY | FS_DIRECTORY, 0);
    if (h < 0) return;
    static char buf[4096];
    uint64_t cs = 0, ino;
    long n;
    int type;
    char name[256];
    while ((n = fs_readdir(h, &cs, buf, sizeof buf)) > 0)
        for (const char *p = buf; fs_dirent(&p, buf + n, &ino, &type, name) && napps < MAXAPP; ) {
            if (type != 8 || !is_app(name)) continue;
            strlcpy(apps[napps].path, "/etc/apps/", sizeof apps[0].path);
            strlcpy(apps[napps].path + 10, name, sizeof apps[0].path - 10);
            size_t len = 0;
            char *f = file_get(apps[napps].path, &len), kind[16];
            app_field(f, "kind", kind, sizeof kind);
            free(f);
            apps[napps].kind = !strcmp(kind, "editor") ? K_EDITOR : !strcmp(kind, "settings") ? K_SETTINGS : !strcmp(kind, "git") ? K_GIT : !strcmp(kind, "mail") ? K_MAIL : K_TERMINAL;
            app_name(apps[napps].path, apps[napps].name, sizeof apps[0].name);
            napps++;
        }
    fs_close(h);
}

static void login(void)
{
    acct_user_t u;
    strlcpy(user, gname, sizeof user);
    strlcpy(gmsg, "checking...", sizeof gmsg);
    redraw();
    if (acct_user(user, 0, &u)) { wipe(gpw, sizeof gpw); strlcpy(gmsg, "Wrong name or password.", sizeof gmsg); say("atlas: login %s: refused\n", gname); return; }
    uid = u.uid; ugid = u.gid;                          /* (for the rights checks; undone if refused) */
    if (acct_groups(user, &ugroups) < 0) ugroups.n = 0;
    strlcpy(home, u.home, sizeof home);
    long t = open_term(gpw);                            /* (auth checks the password: it starts the shell) */
    wipe(gpw, sizeof gpw);
    if (t < 0) {
        uid = ugid = -1; ugroups.n = 0; home[0] = 0;
        strlcpy(gmsg, "Wrong name or password.", sizeof gmsg);
        say("atlas: login %s: refused\n", gname);
        return;
    }
    say("atlas: login %s: ok\n", user);
    gmsg[0] = 0;
    mode = SESSION;
    rescan();
    mouse_load();
    scan_apps();
    show_win(t);
    settle();
}

/* ---- Files and apps: opening them. */
static int open_file(const char *path)                   /* in the editor */
{
    if (!atlas_may(path, 0) || edit_open(path) < 0) { strlcpy(toast, "Cannot open that file.", sizeof toast); settle(); return -EACCES; }
    show_win(W_EDIT);
    settle();
    return 0;
}

int atlas_open_file(const char *path)
{
    if (path[0] == '/' && atlas_may(path, 0)) return open_file(path) < 0 ? -EACCES : 0;
    int best = -1;
    for (int i = 0; i < nitems; i++) {
        if (!strcmp(items[i].path, path)) { best = i; break; }
        size_t a = strlen(items[i].path), b = strlen(path);     /* or by its end: "notes/x.txt", "x.txt" */
        if (b < a && !strcmp(items[i].path + a - b, path) && items[i].path[a - b - 1] == '/') best = i;
    }
    if (best < 0) return -ENOENT;
    return open_file(items[best].path) < 0 ? -EACCES : 0;
}

static void prompt_open(const char *title, const char *init)
{
    mn.on = 0;
    pr.on = 1;
    strlcpy(pr.title, title, sizeof pr.title);
    strlcpy(pr.buf, init ? init : "", sizeof pr.buf);
    say("atlas: prompt %s\n", title);
    redraw();
}

static void open_settings(void)
{
    if (!set_on) { settings_open(); set_on = 1; }
    show_win(W_SET);
    settle();
}
static void open_editor(void)                            /* the editor on its own: an empty, untitled tab */
{
    if (edit_new() < 0) { settle(); return; }
    show_win(W_EDIT);
    settle();
}
static void open_files(void)
{
    if (!files_on) { files_open(home); files_on = 1; }
    show_win(W_FILES);
    settle();
}

/* Start an app: a terminal (a shell as the logged-in user, never root)
 * which runs the app's command when it first asks for a line. */
static void app_start(const char *path)
{
    size_t n = 0;
    char *f = atlas_may(path, 0) || !memcmp(path, "/etc/apps/", 10) ? file_get(path, &n) : 0;
    char nm[48], rn[64], args[96], kind[16];
    if (!f) { say("atlas: app %s: cannot read it\n", path); settle(); return; }
    app_field(f, "name", nm, sizeof nm);
    app_field(f, "run", rn, sizeof rn);
    app_field(f, "args", args, sizeof args);
    app_field(f, "kind", kind, sizeof kind);
    free(f);
    if (!strcmp(kind, "editor")) { open_editor(); return; }
    if (!strcmp(kind, "settings")) { open_settings(); return; }
    if (!strcmp(kind, "git")) { repo_open(); repo_on = 1; show_win(W_REPO); settle(); return; }
    if (!strcmp(kind, "mail")) { mail_open(); show_win(W_MAIL); settle(); return; }
    if (rn[0] != '/') { say("atlas: app %s: no program\n", nm); settle(); return; }
    long t = open_term(0);
    if (t < 0) { settle(); return; }
    if (strcmp(rn, "/bin/sh")) {
        char *p = terms[t].pend;
        p += strlcpy(p, rn, 64);
        if (args[0]) { *p++ = ' '; strlcpy(p, args, 96); }
    }
    say("atlas: app %s started (terminal %d)\n", nm, terms[t].chan);
    show_win(t);
    settle();
}
int atlas_run_app(const char *path) { app_start(path); return 0; }

/* The editor's untitled tab: saved under this name, in the folder its
 * tree shows (the home folder unless moved). */
static void save_as(const char *name)
{
    char path[160];
    const char *dir = edit_dir()[0] ? edit_dir() : home;
    size_t k = strlcpy(path, dir, sizeof path);
    if (k && path[k - 1] != '/') path[k++] = '/';
    strlcpy(path + k, name, sizeof path - k);
    siefs_stat_t st;
    long e = !atlas_may(dir, 1) ? -EACCES : !fs_stat(path, &st, 1) ? -EEXIST : 0;
    if (!e) {
        long h = fs_open(path, FS_WRONLY | FS_CREAT | FS_EXCL, 0644);
        e = h < 0 ? h : 0;
        if (h >= 0) fs_close(h);
    }
    if (!e) fs_chown(path, uid, ugid);                   /* (created by root: it is the user's) */
    say("atlas: save as %s (%ld)\n", name, e);
    if (e) { strlcpy(toast, e == -EEXIST ? "A file with that name exists." : "Cannot save it in this folder.", sizeof toast); settle(); return; }
    edit_save_as(path);
    rescan();
    settle();
}
void atlas_save_as(void)
{
    const char *d = edit_dir()[0] ? edit_dir() : home;
    char t[48];
    size_t n = strlcpy(t, "Save in ", sizeof t);
    if (strlen(d) + n + 6 < sizeof t) { n += strlcpy(t + n, d, sizeof t - n); strlcpy(t + n, " as:", sizeof t - n); }
    else strlcpy(t, "Save as: its name?", sizeof t);
    prompt_open(t, 0);
}

static void ask_sia(const char *q)                       /* the assistant's window, then the question */
{
    sia_on = 1;
    show_win(W_SIA);
    say("atlas: assistant open\n");
    if (q && *q) panel_ask(q);
    settle();
}

static int good_name(const char *s)                      /* letters, digits, ". _ -" and spaces */
{
    if (!*s || *s == '.' || *s == ' ') return 0;
    for (; *s; s++) if (!((*s | 32) >= 'a' && (*s | 32) <= 'z') && !(*s >= '0' && *s <= '9') && !strchr("._- ", *s)) return 0;
    return 1;
}
static void prompt_done(void)
{
    pr.on = 0;
    char *b = pr.buf;
    while (*b && b[strlen(b) - 1] == ' ') b[strlen(b) - 1] = 0;
    if (!good_name(b)) { say("atlas: prompt: bad name\n"); strlcpy(toast, "Use letters, digits, . _ - and spaces.", sizeof toast); settle(); return; }
    save_as(b);
}

/* ---- Menus. */
static void menu_new(const char *title, int win) { mn.n = 0; mn.hover = -1; mn.win = win; strlcpy(mn.title, title, sizeof mn.title); }
static void menu_add(const char *l, int act, const char *arg, int ic)
{
    if (mn.n == MAXMENT) return;
    strlcpy(mn.e[mn.n].label, l, sizeof mn.e[0].label);
    strlcpy(mn.e[mn.n].arg, arg ? arg : "", sizeof mn.e[0].arg);
    mn.e[mn.n].act = act; mn.e[mn.n++].icon = ic;
}
static void menu_show(int x, int y)
{
    int w = 300;
    for (int i = 0; i < mn.n; i++) { int lw = tw(strlen(mn.e[i].label), 2) + 80; if (lw > w) w = lw; }
    mn.on = 1; mn.w = w;
    mn.x = x + w > SW - 10 ? SW - 10 - w : x;
    mn.y = y + menu_h() > SH - 10 ? SH - 10 - menu_h() : y;
    say("atlas: menu %s: %d entries\n", mn.title, mn.n);
    for (int i = 0, ey = mn.y + 8; i < mn.n; i++) {
        if (mn.e[i].act == A_SEP) { ey += MENSEP; continue; }
        say("atlas: menu entry %d %s at %d %d\n", i, mn.e[i].label, mn.x + mn.w / 2, ey + MENH / 2);
        ey += MENH;
    }
    redraw();
}

static void menu_starters(void)                          /* what every start menu begins with */
{
    menu_add("Terminal", A_TERM, 0, I_TERM);
    menu_add("sia", A_SIA, 0, I_SIA);
    menu_add("Files", A_FILES, 0, I_FOLDER);
    menu_add("Text editor", A_EDITOR, 0, I_FILE);
    menu_add("Pins", A_PINS, 0, I_PINS);
    menu_add("Settings", A_SETTINGS, 0, I_SETTINGS);
}
static void main_menu(void)                              /* the Deskbar's: start, the apps, the windows, leave */
{
    menu_new("SIEOS", -2);
    menu_starters();
    int sep = 0;
    for (int i = 0; i < napps; i++) {
        if (apps[i].kind == K_EDITOR || apps[i].kind == K_SETTINGS || !strcmp(base_name(apps[i].path), "shell.app")) continue;
        if (!sep++) menu_add("", A_SEP, 0, -1);
        menu_add(apps[i].name, A_RUNAPP, apps[i].path, apps[i].kind == K_GIT ? I_GIT : apps[i].kind == K_MAIL ? I_MAIL : I_APP);
    }
    sep = 0;
    for (int w = 0; w < NWIN; w++) {
        if (!win_used(w)) continue;
        if (!sep++) menu_add("", A_SEP, 0, -1);
        char t[48], a[4];
        win_title(w, t, sizeof t);
        if (wm_minimized(w)) strlcpy(t + strlen(t), " (hidden)", sizeof t - strlen(t));
        a[num(a, w)] = 0;
        menu_add(t, A_WINDOW, a, win_icon(w));
    }
    menu_add("", A_SEP, 0, -1);
    menu_add("Log out", A_LOGOUT, 0, -1);
    menu_show(DBX + DBW - 340, M + DB_MENU + 2);
}
static void desk_menu(int x, int y)
{
    menu_new("Desk", -1);
    menu_starters();
    menu_add("", A_SEP, 0, -1);
    menu_add("Log out", A_LOGOUT, 0, -1);
    menu_show(x, y);
}
static void app_menu(int k, int x, int y)               /* a Deskbar app: new one, its windows */
{
    char t[48] = "New ";
    menu_new(dbapps[k].name, -1);
    strlcpy(t + 4, dbapps[k].name, sizeof t - 4);
    menu_add(dbapps[k].act == A_TERM ? t : "Open", dbapps[k].act, 0, dbapps[k].icon);
    for (int w = 0; w < NWIN; w++) {
        if (!win_used(w) || win_icon(w) != dbapps[k].icon) continue;
        char a[4];
        win_title(w, t, sizeof t);
        if (wm_minimized(w)) strlcpy(t + strlen(t), " (hidden)", sizeof t - strlen(t));
        a[num(a, w)] = 0;
        menu_add(t, A_WINDOW, a, dbapps[k].icon);
    }
    menu_show(x - 320, y);
}
static void window_menu(int w, int x, int y)
{
    char t[48];
    win_title(w, t, sizeof t);
    menu_new(t, w);
    menu_add("Minimize", A_MIN, 0, -1);
    menu_add("Zoom", A_ZOOM, 0, -1);
    menu_add("", A_SEP, 0, -1);
    for (int k = 0; k < WM_NWS; k++) {
        if (k == wm_ws_of(w)) continue;
        char l[32] = "Send to workspace ", a[2] = { (char)('0' + k), 0 };
        l[18] = '1' + k; l[19] = 0;
        menu_add(l, A_SENDWS, a, -1);
    }
    menu_add("", A_SEP, 0, -1);
    menu_add("Close", A_CLOSE, 0, -1);
    menu_show(x, y);
}

static void db_start(int act)                            /* start (or bring back) one of the Deskbar's apps */
{
    switch (act) {
    case A_TERM: new_term(); return;
    case A_SIA: ask_sia(0); return;
    case A_FILES: open_files(); return;
    case A_EDITOR: if (edit_active()) { show_win(W_EDIT); settle(); } else open_editor(); return;
    case A_SETTINGS: open_settings(); return;
    case A_PINS: if (!pins_on) { pins_open(); pins_on = 1; } show_win(W_PINS); settle(); return;
    }
}
static void db_click(int k)                              /* running: its window in front (minimized: back); else start it */
{
    int act = dbapps[k].act;
    if (!db_running(k)) { db_start(act); return; }
    int w = act == A_TERM ? (last_term >= 0 && terms[last_term].used ? last_term : -1) :
            act == A_SIA ? W_SIA : act == A_FILES ? W_FILES : act == A_EDITOR ? W_EDIT : act == A_PINS ? W_PINS : W_SET;
    for (int t = 0; w < 0 && t < MAXTERM; t++) if (terms[t].used) w = t;
    show_win(w);
    settle();
}

static void menu_do(int k)
{
    int act = mn.e[k].act, w = mn.win;
    char arg[160];
    strlcpy(arg, mn.e[k].arg, sizeof arg);
    mn.on = 0;
    switch (act) {
    case A_SEP: redraw(); return;
    case A_TERM: case A_SIA: case A_FILES: case A_EDITOR: case A_SETTINGS: case A_PINS: db_start(act); return;
    case A_RUNAPP: app_start(arg); return;
    case A_WINDOW: { int v = 0; for (const char *p = arg; *p; p++) v = v * 10 + *p - '0'; show_win(v); settle(); return; }
    case A_LOGOUT: logout(); redraw(); return;
    case A_MIN: min_win(w); settle(); return;
    case A_ZOOM: wm_toggle_max(w); settle(); return;
    case A_SENDWS: wm_send(w, arg[0] - '0'); if (focus == w) focus = wm_top(); settle(); return;
    case A_CLOSE: close_win(w); return;
    }
    redraw();
}

/* ---- The clipboard: a selection in a terminal is copied at once; Ctrl+V
 * or the middle button types it where the keys go. */
static void copy_sel(void)
{
    term_t *t = &terms[sel.t];
    uint64_t al = sel.al, bl = sel.bl;
    int ac = sel.ac, bc = sel.bc;
    if (al > bl || (al == bl && ac > bc)) { uint64_t tl = al; al = bl; bl = tl; int tc = ac; ac = bc; bc = tc; }
    clip_n = 0;
    for (uint64_t l = al; l <= bl && clip_n < (int)sizeof clip - 2; l++) {
        const char *s = tline(t, l);
        int c0 = l == al ? ac : 0, c1 = l == bl ? bc : t->cols - 1;
        while (c1 >= c0 && s[c1] == ' ') c1--;            /* no trailing spaces */
        for (int c = c0; c <= c1 && clip_n < (int)sizeof clip - 2; c++) clip[clip_n++] = s[c];
        if (l < bl) clip[clip_n++] = '\n';
    }
    say("atlas: copied %d bytes\n", clip_n);
}

static void paste(void)
{
    if (!clip_n) return;
    if (strip_on || focus < 0) { for (int i = 0; i < clip_n && strlen(query) + 1 < sizeof query; i++) { size_t n = strlen(query); if (clip[i] >= 32) { query[n] = clip[i]; query[n + 1] = 0; } } strip_on = 1; search(); redraw(); return; }
    if (focus == W_SIA) { for (int i = 0; i < clip_n; i++) panel_key((uint8_t)clip[i]); redraw(); return; }
    if (focus == W_EDIT) { for (int i = 0; i < clip_n; i++) edit_key((uint8_t)clip[i]); redraw(); return; }
    if (focus == W_SET) { for (int i = 0; i < clip_n; i++) if (clip[i] >= 32) settings_key((uint8_t)clip[i]); redraw(); return; }
    if (focus == W_REPO) { for (int i = 0; i < clip_n; i++) if (clip[i] >= 32) repo_key((uint8_t)clip[i]); redraw(); return; }
    if (focus == W_MAIL) { for (int i = 0; i < clip_n; i++) mail_key((uint8_t)clip[i]); redraw(); return; }
    if (focus >= MAXTERM) return;
    term_t *t = &terms[focus];
    for (int i = 0; i < clip_n; i++) tty_input(&t->tty, clip[i]);
    say("atlas: pasted %d bytes\n", clip_n);
    redraw();
}

/* ---- Input. */
static void open_result(int i)
{
    int k = res[i].kind, a = res[i].i;
    char q[48];
    strlcpy(q, query[0] == '?' ? query + 1 : query, sizeof q);
    query[0] = 0; nres = nfound = 0; strip_on = 0;
    if (k == R_FILE) { atlas_open_file(items[a].path); return; }
    if (k == R_APP) { app_start(items[a].path); return; }
    if (k == R_SYSAPP) { app_start(apps[a].path); return; }
    char *s = q;
    while (*s == ' ') s++;
    say("atlas: ask from the strip\n");
    ask_sia(s);
}

static void focus_win(int w)
{
    if (w < 0 || !win_used(w)) return;
    if (focus != w || wm_top() != w) { wm_show(w); say("atlas: window %d in front\n", w); }
    focus = w; strip_on = 0;
    if (w < MAXTERM) last_term = w;
}

static void click(int x, int y)
{
    int i = hit_at(x, y);
    if (pr.on) {                                         /* the name box: only its buttons */
        if (i >= 0 && hit[i].kind == H_PR) { if (hit[i].arg) prompt_done(); else { pr.on = 0; redraw(); } }
        return;
    }
    if (mn.on && (i < 0 || hit[i].kind != H_MENU)) { mn.on = 0; redraw(); return; }
    if (i < 0) return;
    int a = hit[i].arg, k = hit[i].kind;
    toast[0] = 0;
    if (k >= H_PANEL && k <= H_PMODE) focus_win(W_SIA);   /* the assistant's window (its buttons */
    if (panel_click(k, a)) { redraw(); return; }           /* may then put something else in focus) */
    switch (k) {
    case H_FIELD: if (first_start) setup_click(k, a); else gfield = a;
                  redraw(); return;
    case H_SETUP: setup_click(k, a); redraw(); return;
    case H_LOGIN: login(); redraw(); return;
    case H_STRIP: strip_on = 1; search(); redraw(); return;
    case H_RES:   open_result(a); return;
    case H_MENU:  menu_do(a); return;
    case H_DBMENU: main_menu(); return;
    case H_DBAPP: db_click(a); return;
    case H_DBWS:  wm_switch(a); focus = wm_top(); strip_on = 0; settle(); return;
    case H_WTAB: {                                       /* in front; twice: minimized (as BeOS did) */
        static uint64_t last_t;
        static int last_w = -1;
        uint64_t t = sys_clock();
        focus_win(a);
        if (a == last_w && t - last_t < 450000000) { last_w = -1; min_win(a); settle(); return; }
        last_w = a; last_t = t;
        redraw();
        return;
    }
    case H_WMBTN: {
        int w = a / 4, b = a % 4;
        if (b == 1) { focus_win(w); wm_toggle_max(w); settle(); }
        else close_win(w);
        return;
    }
    case H_WEDGE: focus_win(a / 16); redraw(); return;
    case H_WIN:   focus_win(a); sel.on = 0; redraw(); return;
    case H_ED:    focus_win(W_EDIT); edit_click(a, x, y); if (!edit_active()) close_win(W_EDIT); else settle(); return;
    case H_SET:   focus_win(W_SET); settings_click(k, a); redraw(); return;
    case H_FILE:  focus_win(W_FILES); files_click(k, a); settle(); return;
    case H_PINS:  focus_win(W_PINS); pins_click(k, a); redraw(); return;
    case H_REPO:  focus_win(W_REPO); repo_click(k, a); redraw(); return;
    case H_MAIL:  focus_win(W_MAIL); mail_click(k, a); settle(); return;
    case H_DESK:  if (strip_on) { strip_on = 0; redraw(); } return;
    }
}

static void key(int c)
{
    if (mode == GREETER) {
        if (first_start) { setup_key(c); redraw(); return; }
        char *f = gfield ? gpw : gname;
        size_t n = strlen(f), max = gfield ? sizeof gpw : sizeof gname;
        if (c == '\t') gfield = !gfield;
        else if (c == '\n') { if (!gfield) gfield = 1; else login(); }
        else if (c == '\b') { if (n) f[n - 1] = 0; }
        else if (c >= 32 && c < 127 && n + 1 < max) { f[n] = c; f[n + 1] = 0; }
        redraw();
        return;
    }
    if (pr.on) {                                         /* typing a name */
        size_t n = strlen(pr.buf);
        if (c == '\n') prompt_done();
        else if (c == 27) { pr.on = 0; say("atlas: prompt cancelled\n"); redraw(); }
        else if (c == '\b') { if (n) pr.buf[n - 1] = 0; redraw_box(SW / 2 - 330, SH / 2 - 120, SW / 2 + 330, SH / 2 + 110); }
        else if (c >= 32 && c < 127 && n + 1 < sizeof pr.buf) { pr.buf[n] = c; pr.buf[n + 1] = 0; redraw_box(SW / 2 - 330, SH / 2 - 120, SW / 2 + 330, SH / 2 + 110); }
        return;
    }
    /* shortcuts, wherever the keys go */
    if (c == 0 || c == 6) { strip_on = 1; mn.on = 0; search(); say("atlas: strip\n"); redraw(); return; }   /* Ctrl+Space, Ctrl+F */
    if (c == 20) { new_term(); return; }                                                                  /* Ctrl+T */
    if (c == 22) { paste(); return; }                                                                     /* Ctrl+V */
    if (c >= KEY_C1 && c < KEY_C1 + WM_NWS) { wm_switch(c - KEY_C1); focus = wm_top(); strip_on = 0; settle(); return; }   /* Ctrl+1..4 */
    if (c == 23 && focus != W_EDIT) { close_win(focus); return; }                                         /* Ctrl+W (the editor: its tab) */                                                            /* Ctrl+W */
    if (c == KEY_CTAB) {                                 /* the next window of this workspace, in front */
        for (int k = 1; k <= NWIN; k++) {
            int w = (focus + k + NWIN) % NWIN;
            if (win_used(w) && !wm_minimized(w) && wm_ws_of(w) == wm_workspace()) { focus_win(w); break; }
        }
        settle();
        return;
    }
    if (mn.on && c == 27) { mn.on = 0; redraw(); return; }
    if (strip_on || focus < 0) {                         /* the sia strip */
        size_t n = strlen(query);
        if (c == 27) { query[0] = 0; nres = nfound = 0; strip_on = 0; redraw(); return; }
        strip_on = 1;
        if (c == '\n') { if (nres) open_result(0); return; }
        if (c == '\t') { for (int i = 0; i < nres; i++) if (res[i].kind == R_ASK) { open_result(i); return; } return; }
        if (c == '\b') { if (n) query[n - 1] = 0; }
        else if (c >= 32 && c < 127 && n + 1 < sizeof query) { query[n] = c; query[n + 1] = 0; }
        else return;
        search();
        redraw();
        return;
    }
    if (focus == W_EDIT) { edit_key(c); if (!edit_active()) { wm_gone(W_EDIT); focus = wm_top(); } settle(); return; }
    if (focus == W_SIA) { if (!panel_key(c) && c == 27) focus = -1; redraw(); return; }
    if (focus == W_SET) { if (settings_key(c)) redraw(); else if (c == 27) { focus = -1; redraw(); } return; }
    if (focus == W_FILES) { if (files_key(c)) settle(); return; }
    if (focus == W_PINS) { if (pins_key(c)) redraw(); else if (c == 27) { focus = -1; redraw(); } return; }
    if (focus == W_REPO) { if (repo_key(c)) redraw(); else if (c == 27) { focus = -1; redraw(); } return; }
    if (focus == W_MAIL) { if (mail_key(c)) redraw(); else if (c == 27) { focus = -1; redraw(); } return; }
    term_t *t = &terms[focus];                           /* the focused terminal */
    int x0, x1, y0, y1;
    if (!win_box(focus, &x0, &y0, &x1, &y1)) x0 = x1 = y0 = y1 = 0;
    if (c == KEY_PGUP || c == KEY_PGDN) {                /* its history */
        int b = t->back + (c == KEY_PGUP ? t->rows / 2 : -t->rows / 2);
        t->back = b < 0 ? 0 : b > hist_max(t) ? hist_max(t) : b;
        say("atlas: terminal %d back %d\n", t->chan, t->back);
        redraw_box(x0, y0 - TAB_H, x1, y1);
        return;
    }
    if (c < 0x100 || t->tty.echo == CON_RAW) {
        t->back = 0;
        tty_key(&t->tty, c);
        redraw_box(x0, y0, x1, y1);
    }
}

/* Which character cell of terminal t is under screen point (x, y). */
static void cell_at(int ti, int x, int y, uint64_t *l, int *c)
{
    term_t *t = &terms[ti];
    int x0, x1, y0, y1;
    if (!win_box(ti, &x0, &y0, &x1, &y1)) x0 = y0 = 0;
    int r = (y - (y0 + WM_BODY_DY) - TBAR - 8) / KH, k = (x - x0 - 14) / KW;
    r = r < 0 ? 0 : r >= t->rows ? t->rows - 1 : r;
    k = k < 0 ? 0 : k >= t->cols ? t->cols - 1 : k;
    *l = t->top - t->back + r;
    *c = k;
}

static void press(int x, int y)
{
    press_x = x; press_y = y;
    drag = D_NONE; drag_on = 0;
    int i = hit_at(x, y);
    if (mode != SESSION || i < 0 || mn.on || pr.on) return;
    int x0, y0, x1, y1;
    if ((hit[i].kind == H_WTAB || hit[i].kind == H_WEDGE) && wm_grab(hit[i].kind, hit[i].arg, x, y)) drag = D_WIN;
    else if (hit[i].kind == H_WIN && hit[i].arg < MAXTERM && win_box(hit[i].arg, &x0, &y0, &x1, &y1) && y > y0 + WM_STRIP) { drag = D_SEL; drag_arg = hit[i].arg; }
}

/* The menu's entry under the pointer (lit), or -1. */
static int menu_hover(int x, int y)
{
    if (!mn.on || x < mn.x || x >= mn.x + mn.w) return -1;
    for (int i = 0, ey = mn.y + 8; i < mn.n; i++) {
        int h = mn.e[i].act == A_SEP ? MENSEP : MENH;
        if (y >= ey && y < ey + h) return mn.e[i].act == A_SEP ? -1 : i;
        ey += h;
    }
    return -1;
}

static void mouse(const con_event_t *e)
{
    int x = (int)((int64_t)e->x * SW >> 16), y = (int)((int64_t)e->y * SH >> 16);
    int b = e->buttons, was = buttons, ox = ptr_x, oy = ptr_y;
    if (m_swap) b = (b & ~3) | (b & 1) << 1 | (b & 2) >> 1;   /* left-handed: the buttons swap */
    ptr_x = x; ptr_y = y;
    buttons = b;
    if (e->code && mode == SESSION) {                    /* the wheel */
        int16_t w = m_rev ? -(int16_t)e->code : (int16_t)e->code;
        int i = hit_at(x, y);
        if (i >= 0 && (hit[i].kind == H_WIN || hit[i].kind == H_ED || hit[i].kind == H_PANEL || hit[i].kind == H_SET || hit[i].kind == H_FILE || hit[i].kind == H_PINS || hit[i].kind == H_REPO || hit[i].kind == H_MAIL)) {
            int a = hit[i].kind == H_ED ? W_EDIT : hit[i].kind == H_PANEL ? W_SIA : hit[i].kind == H_SET ? W_SET :
                    hit[i].kind == H_FILE ? W_FILES : hit[i].kind == H_PINS ? W_PINS : hit[i].kind == H_REPO ? W_REPO : hit[i].kind == H_MAIL ? W_MAIL : hit[i].arg;
            if (a == W_EDIT) edit_wheel(hit[i].arg, w);
            else if (a == W_SET) settings_wheel(w);
            else if (a == W_FILES) files_wheel(w);
            else if (a == W_PINS) pins_wheel(w);
            else if (a == W_REPO) repo_wheel(w);
            else if (a == W_MAIL) mail_wheel(hit[i].arg, w);
            else if (a == W_SIA) panel_wheel(w);
            else if (a < MAXTERM) {
                term_t *t = &terms[a];
                int bk = t->back - 3 * w;
                t->back = bk < 0 ? 0 : bk > hist_max(t) ? hist_max(t) : bk;
                say("atlas: terminal %d back %d\n", t->chan, t->back);
            }
            redraw();
        }
        if (b == was) return;
    }
    if ((b & MB_LEFT) && !(was & MB_LEFT)) press(x, y);
    if ((b & MB_LEFT) && mode == SESSION && drag != D_NONE &&
        (drag_on || (x - press_x) * (x - press_x) + (y - press_y) * (y - press_y) > 64)) {
        if (drag == D_WIN) {                             /* a window moving or resizing */
            if (!drag_on) { focus_win(wm_grabbed()); drag_on = 1; redraw(); }
            wm_motion(x, y);
            dl_pointer(x, y);
            present_pointer(ox, oy, x, y);
            return;
        }
        if (!drag_on) {                                  /* the selection starts where the button went down */
            cell_at(drag_arg, press_x, press_y, &sel.al, &sel.ac);
            sel.t = drag_arg; sel.on = 1;
        }
        drag_on = 1;
        cell_at(drag_arg, x, y, &sel.bl, &sel.bc);
        redraw();
        return;
    }
    if (!(b & MB_LEFT) && (was & MB_LEFT)) {
        int d = drag, on = drag_on;
        drag = D_NONE; drag_on = 0;
        if (d == D_WIN) { if (on) { wm_drop(); redraw_box(DBX, M, DBX + DBW, M + db_h()); return; } wm_ungrab(); }   /* (the Deskbar's map of it too) */
        if (!on) click(x, y);
        else if (d == D_SEL) { if (sel.al != sel.bl || sel.ac != sel.bc) copy_sel(); else sel.on = 0; redraw(); }
        return;
    }
    if ((b & MB_MIDDLE) && !(was & MB_MIDDLE) && mode == SESSION) { paste(); return; }
    if (!(b & MB_RIGHT) && (was & MB_RIGHT) && mode == SESSION && !pr.on) {   /* right click: a menu */
        int i = hit_at(x, y), k = i < 0 ? 0 : hit[i].kind;
        if (k == H_MENU) return;
        mn.on = 0;
        if (k == H_WTAB) { focus_win(hit[i].arg); window_menu(hit[i].arg, x, y); return; }
        if (k == H_DBAPP) { app_menu(hit[i].arg, x, y); return; }
        if (k == H_DBMENU) { main_menu(); return; }
        if (k == H_DESK && hit[i].arg == 0) { desk_menu(x, y); return; }
        redraw();
        return;
    }
    if (mode == SESSION) {                               /* the pointer over a menu or the Deskbar: what it lights */
        int h = menu_hover(x, y), in = x >= DBX && x < DBX + DBW && y >= M && y < M + db_h();
        int was_in = ox >= DBX && ox < DBX + DBW && oy >= M && oy < M + db_h();
        if (h != mn.hover) { mn.hover = h; redraw_box(mn.x, mn.y, mn.x + mn.w, mn.y + menu_h()); }
        if (in || was_in) {
            int r = (y - M - DB_MENU - DB_TRAY) / DB_ROW, orr = (oy - M - DB_MENU - DB_TRAY) / DB_ROW;
            if (in != was_in || r != orr || (y < M + DB_MENU) != (oy < M + DB_MENU)) redraw_box(DBX, M, DBX + DBW, M + db_h());
        }
    }
    dl_pointer(x, y);                                   /* only the pointer moved: */
    present_pointer(ox, oy, x, y);                      /* where it was, where it is */
}

/* The input thread: waits for events from con, hands them to the main
 * loop (so all the drawing happens in one thread). */
static void input_thread(void *arg)
{
    (void)arg;
    static con_event_t ev[64];
    long con = 0;
    for (;;) {
        msg_t m = { .w = { CON_INPUT }, .rbuf = ev, .rlen = sizeof ev };
        if (call_named(&con, "console", &m, 1) < 0 || (long)m.w[0] <= 0) { sys_sleep(100000000); continue; }
        msg_t f = { .w = { ATLAS_INPUT, m.w[0] }, .sbuf = ev, .slen = m.w[0] * sizeof *ev };
        ipc_call(port, &f);
    }
}

static void no_desktop(void)                             /* tell auth: the terminal does the first start */
{
    msg_t d = { .w = { AUTH_DISPLAY, 0 } };
    msg_t q = { .w = { AUTH_STATE } };
    if (atlas_auth(&q) == 0) atlas_auth(&d);            /* (only matters before the first start) */
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    size_t dn = 0;                                      /* the size chosen in Settings (if the screen takes any) */
    char *dc = file_get("/etc/display.conf", &dn);
    if (dc) {
        int w = 0, h = 0;
        const char *p = dc;
        while (p && !same_n(p, "size=", 5)) { p = strchr(p, '\n'); if (p) p++; }
        if (p) { for (p += 5; *p >= '0' && *p <= '9'; p++) w = w * 10 + *p - '0'; if (*p == 'x') for (p++; *p >= '0' && *p <= '9'; p++) h = h * 10 + *p - '0'; }
        if (w >= 1024 && w <= 3840 && h >= 600 && h <= 2400) { PW = w; PH = h; }
        free(dc);
    }
    if (screen_init(&scr) && (PW != 1920 || PH != 1080)) { PW = 1920; PH = 1080; }   /* (that size failed: the default) */
    if (!scr.fb && screen_init(&scr)) {                  /* no screen: the terminal does the first start */
        say("atlas: no screen found: no desktop\n");
        no_desktop();
        return 0;
    }
    draw_init(&scr);
    draw_look(LOOK_PHOSPHOR);                           /* (the user's own at login) */
    geometry();
    ptr_x = SW / 2; ptr_y = SH / 2;
    say("atlas: screen: %s, %d x %d\n", scr.name, PW, PH);
    long con = 0;
    msg_t hi = { .w = { CON_HASINPUT } };
    rtc_read();
    port = port_create("atlas");
    if (port < 0) return 1;
    sys_child_port(port);                                /* our shells' ends */
    if (call_named(&con, "console", &hi, 1) >= 0 && (long)hi.w[0] == 0) {
        /* A screen but no keyboard or pointer yet (a USB one may come at
         * any time: its events then reach the input thread): the desktop
         * starts anyway, and the first start, if any, goes to the serial
         * console until a keyboard or mouse appears. */
        no_input = 1;
        say("atlas: no keyboard or pointer yet: the desktop waits for one\n");
        no_desktop();
    } else greeter_check();
    redraw();
    say("atlas: greeter (%dx%d, full frame %lu us)\n", PW, PH, (unsigned long)frame_us);
    thread_start(input_thread, 0, 16384);
    thread_start(clock_thread, 0, 4096);
    panel_init(port);
    settings_init(port);
    pins_init(port);
    wm_init(port);
    mouse_apply();

    mk_info_t in;
    sys_info(&in);
    int self = in.self_pid;
    static char buf[4096];
    for (;;) {
        msg_t m = { .rbuf = buf, .rlen = sizeof buf };
        long from = ipc_recv(port, &m);
        if (from < 0) continue;
        if (from == 0) {                                 /* a shell ended */
            int st, pid, closed = 0;
            while ((pid = sys_wait(-1, &st, WAIT_NOHANG)) > 0) {
                for (int t = 0; t < MAXTERM; t++)
                    if (terms[t].used && terms[t].pid == pid) { close_term(&terms[t]); closed = 1; }
                if (repo_ended(pid, st) && mode == SESSION && repo_on) redraw();
                mail_ended(pid);                         /* (or Mail's helper) */
            }
            if (closed && mode == SESSION) { rescan(); settle(); }
            continue;
        }
        if (m.pid == self && m.w[0] == ATLAS_INPUT) {    /* (our own threads only: nobody */
            int n = m.rlen / sizeof(con_event_t);
            reply_val(from, 0);
            if (no_input) {                              /* the first keyboard or mouse: */
                no_input = 0;                            /* still no account? this screen */
                say("atlas: keyboard or pointer connected\n");
                if (mode == GREETER) { greeter_check(); redraw(); }   /* does the first start now */
            }
            for (int i = 0; i < n; i++) {
                con_event_t *e = &((con_event_t *)buf)[i];
                if (e->type == EV_KEY) key(e->code); else mouse(e);
            }
            continue;
        }
        if (m.pid == self && m.w[0] == ATLAS_SIA) {      /* else may fake input or answers) */
            reply_val(from, 0);
            panel_event(&m, buf);
            continue;
        }
        if (m.pid == self && m.w[0] == ATLAS_REVERT) {   /* a new screen size, not kept in time */
            reply_val(from, 0);
            settings_revert();
            continue;
        }
        if (m.pid == self && m.w[0] == ATLAS_PINS) {     /* the Pins window: the levels, the step */
            reply_val(from, 0);
            int a0, b0, c0, d0;
            if (mode == SESSION && pins_on && pins_tick() && win_box(W_PINS, &a0, &b0, &c0, &d0)) redraw_box(a0, b0 + TBAR - 40, c0, d0);
            continue;
        }
        if (m.pid == self && m.w[0] == ATLAS_ANIM) {     /* the next frame of the tabs that move */
            reply_val(from, 0);
            if (mode == SESSION && wm_tick()) settle();   /* (a window has opened: all of it, its places logged) */
            continue;
        }
        if (m.pid == self && m.w[0] == ATLAS_TICK) {     /* a new minute: the Deskbar's tray */
            reply_val(from, 0);
            if (mode == SESSION) redraw_box(DBX, M + DB_MENU, DBX + DBW, M + DB_MENU + DB_TRAY);
            if (mode == SESSION && wm_top() < 0) { int x0, y0, x1, y1; rest_box(&x0, &y0, &x1, &y1); redraw_box(x0, y0, x1, y1); }   /* (the resting clock) */
            continue;
        }
        if (m.w[3] == MAIL_CHAN) {                       /* Mail's helper, as the user */
            if (m.uid == uid && mode == SESSION) { if (mail_console(from, &m, buf)) settle(); } else reply_val(from, -EIO);
            continue;
        }
        /* a terminal's shell: CON_WRITE / CON_READ / CON_ECHO, its window in w[3] */
        if ((m.uid == uid || !m.uid) && repo_con(from, &m, buf)) continue;   /* (the Git window's /bin/git) */
        term_t *t = 0;
        for (int i = 0; i < MAXTERM; i++) if (terms[i].used && terms[i].chan == (int)m.w[3]) t = &terms[i];
        if (!t || (m.uid != uid && m.uid != 0)) { reply_val(from, -EIO); continue; }
        switch (m.w[0]) {
        case CON_WRITE: {
            term_write(t, buf, m.rlen);
            reply_val(from, m.rlen);
            int x0, y0, x1, y1;
            if (mode == SESSION && win_box(t - terms, &x0, &y0, &x1, &y1)) redraw_box(x0, y0, x1, y1);
            break;
        }
        case CON_READ:                                   /* the shell wants the next line: */
            if (t->tty.lines != t->seen && mode == SESSION) {   /* it ran a command: files may */
                t->seen = t->tty.lines;                  /* have changed: look again */
                rescan();
                settle();
            }
            if (t->pend[0]) {                             /* an app's command: typed for it, once */
                for (const char *c = t->pend; *c; c++) tty_input(&t->tty, *c);
                tty_input(&t->tty, '\n');
                t->pend[0] = 0;
            }
            if (tty_read(&t->tty, from, m.w[1]) < 0) reply_val(from, -EBUSY);
            break;
        case CON_ECHO: tty_mode(&t->tty, (int)m.w[1]); reply_val(from, 0); break;
        default: reply_val(from, -ENOSYS);
        }
    }
}

/* ---- For the assistant: the user's files. */
int atlas_files(char *out, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < nitems && n + strlen(items[i].path) + 2 < cap; i++) {
        n += strlcpy(out + n, items[i].path, cap - n);
        n += strlcpy(out + n, "\n", cap - n);
    }
    out[n] = 0;
    return n;
}
