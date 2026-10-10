/*
 * atlas - The SIEOS desktop, "Stage": one project at a time, one thing in
 * focus, the rest on a shelf. (The program keeps its old name, /bin/atlas.)
 *
 * One process does it all, to keep the system small: it drives the screen
 * (screen.c), draws (draw.c), shows the login screen and the first start
 * (setup.c), is the terminal of the shells in its windows, edits files
 * (edit.c) and talks to the assistant (panel.c). init supervises it.
 *
 *     con ──input events──► atlas ◄──CON_WRITE / CON_READ ("atlas#3")── sh
 *      (keyboard, mouse)      │  ├──AUTH_LOGIN──► auth ──starts──► sh (as you)
 *                             │  └──SIA_ASK / SIA_NEXT (its own thread)──► sia
 *                             └──draws──► the screen
 *
 * The screen, and the only four ideas to learn:
 *
 *   ┌──────────── Lens: one line to find a file, start an app, ask sia ──────┐
 *   │ PROJECTS │ ┌─────────── main ───────────┐ ┌──── beside ────┐           │
 *   │ SIEOS    │ │ the window in focus        │ │ (optional)     │           │
 *   │ Pi port  │ └────────────────────────────┘ └────────────────┘           │
 *   │ Inbox    │ IN THIS PROJECT: [card] [card] [card] [+]   the shelf       │
 *   └──────────┴─────────────────────────────────────────────────────────────┘
 *
 *   Projects   A file belongs to one project: its attribute "project=NAME"
 *              (the file server's index finds a project's files at once).
 *              Files without one are in the Inbox. Empty projects are kept
 *              in ~/.projects. Older "island.*" attributes are turned into
 *              a project the first time the desktop sees them.
 *   Stage      At most two windows: main (wide) and beside (narrow). A
 *              window is a terminal (a shell as you), the file editor, or
 *              the assistant. Every window belongs to a project.
 *   Shelf      The project's other windows, its files and its apps, as
 *              small cards drawn from their text (no pictures kept).
 *              Click: on the stage; drag up: main (left) or beside (right);
 *              drag onto a project: it moves there.
 *   Lens       Type to find files (by name), apps, or ask sia.
 *
 * Keys: Ctrl+Space the Lens, Ctrl+1..9 a project, Ctrl+Tab the next
 * window, Ctrl+\ show/hide beside, Ctrl+W window to the shelf, Ctrl+T a
 * terminal, Ctrl+S save (editor), Ctrl+V paste, Esc back.
 *
 * Nothing runs when nothing happens: no animation loop. The picture
 * changes only on an event (input, a shell's output, a piece of an answer,
 * the clock's minute), and only the changed region is sent to the screen.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"
#include "mk/crypto.h"               /* wipe: forget the password */

#define ATLAS_INPUT 100             /* from our input thread: con_event_t[] in sbuf */
#define ATLAS_TICK  102             /* from the clock thread: a new minute */

#define TCMAX 132                   /* a terminal: up to 132 x 60 characters */
#define TRMAX 60
#define HIST 500                    /* lines kept per terminal, scrollback included */
#define MAXITEM 256
#define MAXPROJ 24
#define MAXTERM 8
#define MAXAPP 16
#define W_EDIT MAXTERM              /* window ids: 0..MAXTERM-1 terminals, then these two */
#define W_SIA (MAXTERM + 1)
#define NWIN (MAXTERM + 2)
#define INBOX "Inbox"               /* the project of what has none (one name, kept here) */
#define TBAR 48                     /* a window's title strip */
#define CARDW 252
#define CARDH 140

typedef struct { char name[48], path[160], proj[32]; uint64_t size; int app; unsigned seen; char *pv; } item_t;
typedef struct { char name[32]; int n, main, beside; } proj_t;   /* main/beside: window ids, or -1 */
typedef struct {
    int used, chan, pid, cols, rows;
    int cx, cy, esc, back;          /* cursor (on the screen rows); scrolled back by `back` lines */
    uint64_t top;                   /* number of the line at the top of the screen */
    unsigned seen;                  /* lines typed when we last looked at the files */
    char *ring;                     /* the last HIST lines, TCMAX characters each */
    tty_t tty;
    char proj[32];                  /* its project (for this session) */
    char pend[160];                 /* an app's command line, typed when its shell first reads */
} term_t;

static item_t items[MAXITEM];
static proj_t projs[MAXPROJ];
static term_t terms[MAXTERM];
static struct { char name[48], path[96]; } apps[MAXAPP];   /* the system's launchers (/etc/apps) */
static int nitems, nproj, napps, next_chan = 1;
static char cur[32] = INBOX;        /* the project on the screen */
static int focus = -1;              /* the window with the keys, or -1 */
static int lens_on;                 /* the Lens has the keys */
static char edit_proj[32], sia_proj[32];
static int sia_on;                  /* the assistant's window exists */
static int shelf_off;               /* first card shown on the shelf */
static char toast[96];              /* a short message on the top bar (until the next action) */
int atlas_debug;                     /* /tmp/atlas.debug exists: log places (tests) */
char clip[4096];
int clip_n;

/* ---- Session state. */
static enum { GREETER, SESSION } mode;
static char user[32], home[64], gname[32], gpw[128], gmsg[80], query[48];
static int uid = -1, ugid = -1, gfield, first_start;
static mk_groups_t ugroups;
static long port, auth_port;
static int ptr_x = 960, ptr_y = 540, buttons, press_x, press_y;
static uint64_t frame_us;

/* ---- The layout, computed from the screen's size (any size works). */
static int M, RX1, SX0, SX1, SY0, SY1, SHY;   /* margin; rail's right; stage; the shelf's label */
static void geometry(void)
{
    M = SW >= 1600 ? 30 : 20;
    RX1 = M + (SW >= 1600 ? 300 : 230);
    SX0 = RX1 + M; SX1 = SW - M; SY0 = 90;
    SHY = SH - M - CARDH - 44;
    SY1 = SHY - 16;
}
static void slot(int w, int *x0, int *x1)       /* where window w is drawn on the stage, or 0 */
{
    proj_t *p = 0;
    for (int i = 0; i < nproj; i++) if (!strcmp(projs[i].name, cur)) p = &projs[i];
    int both = p && p->main >= 0 && p->beside >= 0, mid = SX0 + (SX1 - SX0 - 20) * 62 / 100;
    *x0 = *x1 = 0;
    if (!p) return;
    if (p->main == w) { *x0 = SX0; *x1 = both ? mid : SX1; }
    if (p->beside == w) { *x0 = both ? mid + 20 : SX0; *x1 = SX1; }
}

/* ---- Menus: one at a time, a title and entries. */
enum { A_OPEN = 1, A_BESIDE, A_SUB_MOVE, A_MOVE, A_INBOX, A_CLOSEWIN, A_NEWFILE, A_NEWTERM, A_SUB_APPS,
       A_RUNAPP, A_ASK, A_RENAME, A_DELETE, A_DELETE_YES, A_NEWPROJ, A_CANCEL };
#define MAXMENT 14
#define MENW 440
#define MENH 44
static struct {
    int on, x, y, n, win;                   /* win: the window it is about, or -1 */
    char title[48], path[160], proj[32];    /* the card's file; the project it is about */
    struct { char label[48], arg[160]; int act; } e[MAXMENT];
} mn;

/* A name typed in a small box ("New file", "New project", "Rename"). */
enum { P_NEWFILE = 1, P_NEWPROJ, P_RENAME };
static struct { int on, act; char title[48], buf[32], proj[32]; } pr;

/* What a held left button does once the pointer moves. */
static enum { D_NONE, D_CARD, D_SEL } drag;
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

/* ---- Files: the home folder, each file in one project (or the Inbox).
 * Launchers ("*.app") become app cards, named by their "name=". */
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

/* Atlas runs as root (it starts the shells, it owns the screen), so the
 * file server would let it do anything: before reading a file's text or
 * changing a file, atlas checks the rights of the user logged in. */
int atlas_may(const char *path, int write)
{
    siefs_stat_t st;
    if (uid < 0 || fs_stat(path, &st, 0)) return 0;
    if (uid == 0) return 1;
    int bit = write ? 2 : 4, in = (int)st.gid == ugid;
    if ((int)st.uid == uid) return st.mode >> 6 & bit;
    for (int i = 0; i < ugroups.n && !in; i++) in = ugroups.g[i] == (int)st.gid;
    return in ? st.mode >> 3 & bit : st.mode & bit;
}

/* The older desktop put files on "islands" (attributes "island.NAME", and
 * places in "atlas.*"). The first island that is not Driftwood becomes the
 * project; the rest goes. (Only if the user may change the file; else it is
 * read that way and left alone.) */
static void migrate(const char *path, char *proj)
{
    static char xa[2048];
    long n = fs_listxattr(path, xa, sizeof xa);
    int old = 0;
    char first[32] = "";
    for (const char *p = xa; n > 0 && p < xa + n; p += strlen(p) + 1) {
        if (!memcmp(p, "island.", 7) && p[7] && strcmp(p + 7, "Driftwood") && !first[0]) strlcpy(first, p + 7, sizeof first);
        old |= !memcmp(p, "island.", 7) || !memcmp(p, "atlas.", 6);
    }
    if (!old) return;
    if (!proj[0] && first[0]) strlcpy(proj, first, 32);
    if (!atlas_may(path, 1)) return;
    if (proj[0]) fs_setxattr(path, "project", proj, strlen(proj));
    for (const char *p = xa; n > 0 && p < xa + n; p += strlen(p) + 1)
        if (!memcmp(p, "island.", 7) || !memcmp(p, "atlas.", 6)) fs_rmxattr(path, p);
    say("atlas: migrated %s: project %s\n", path, proj[0] ? proj : INBOX);
}

static void scan_file(const char *name, const char *path)
{
    if (nitems == MAXITEM) return;
    item_t *it = &items[nitems];
    memset(it, 0, sizeof *it);
    long pl = fs_getxattr(path, "project", it->proj, sizeof it->proj - 1);
    it->proj[pl > 0 ? pl : 0] = 0;
    migrate(path, it->proj);
    if (!it->proj[0] || !strcmp(it->proj, INBOX)) strlcpy(it->proj, INBOX, sizeof it->proj);
    siefs_stat_t st;
    it->size = fs_stat(path, &st, 1) ? 0 : st.size;
    it->app = is_app(name);
    strlcpy(it->path, path, sizeof it->path);
    if (it->app && it->size < 4096) app_name(path, it->name, sizeof it->name);
    else strlcpy(it->name, name, sizeof it->name);
    nitems++;
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
            if (type == 8) scan_file(name, path);
        }
    free(buf);
    fs_close(h);
}

/* Projects that have nothing in them yet ("New project"): one name per
 * line in ~/.projects. (The older ~/.islands is read too, once.) */
static void home_file(char *p, const char *name) { size_t k = strlcpy(p, home, 64); strlcpy(p + k, name, 32); }
static void empty_set(const char *add, const char *drop)   /* add a name, or drop one */
{
    char p[96], out[1024];
    size_t n = 0, len;
    home_file(p, "/.projects");
    char *f = file_get(p, &len);
    for (char *l = f; l && *l; ) {
        char *e = strchr(l, '\n');
        size_t ll = e ? (size_t)(e - l) : strlen(l);
        if (ll && ll < 32 && n + ll + 2 < sizeof out && !(drop && strlen(drop) == ll && same_n(l, drop, ll)) &&
            !(add && strlen(add) == ll && same_n(l, add, ll))) {
            memcpy(out + n, l, ll); n += ll; out[n++] = '\n';
        }
        l = e ? e + 1 : l + ll;
    }
    free(f);
    if (add && strlen(add) < 32 && n + strlen(add) + 2 < sizeof out) { n += strlcpy(out + n, add, 32); out[n++] = '\n'; }
    fs_unlink(p);
    long h = fs_open(p, FS_WRONLY | FS_CREAT, 0644);
    if (h >= 0) { fs_write(h, 0, out, n); fs_close(h); fs_chown(p, uid, ugid); }
}

static int proj_find(const char *name)
{
    for (int i = 0; i < nproj; i++) if (!strcmp(projs[i].name, name)) return i;
    return -1;
}
static proj_t *here(void) { int i = proj_find(cur); return i >= 0 ? &projs[i] : &projs[nproj - 1]; }

static void proj_add(proj_t *out, int *n, const proj_t *old, int nold, const char *name)
{
    if (!name[0] || *n >= MAXPROJ - 1) return;
    for (int i = 0; i < *n; i++) if (!strcmp(out[i].name, name)) return;
    proj_t *p = &out[(*n)++];
    strlcpy(p->name, name, sizeof p->name);
    p->n = 0; p->main = p->beside = -1;
    for (int i = 0; i < nold; i++) if (!strcmp(old[i].name, name)) { p->main = old[i].main; p->beside = old[i].beside; }
}

/* Look at the home folder again: files, projects (sorted, the Inbox last). */
static void rescan(void)
{
    for (int i = 0; i < nitems; i++) { free(items[i].pv); items[i].pv = 0; }
    nitems = 0;
    scan_dir(home, 0);
    char p[96];
    size_t len;
    home_file(p, "/.islands");                           /* (once: the older empty islands) */
    char *f = file_get(p, &len);
    if (f) {
        for (char *l = f, *e; *l; l = e ? e + 1 : l + strlen(l)) {
            e = strchr(l, '\n');
            if (e) *e = 0;
            if (*l && strcmp(l, "Driftwood")) empty_set(l, 0);
            if (!e) break;
        }
        free(f);
        fs_unlink(p);
    }
    static proj_t old[MAXPROJ];
    int nold = nproj;
    memcpy(old, projs, sizeof *projs * nproj);
    nproj = 0;
    for (int i = 0; i < nitems; i++) if (strcmp(items[i].proj, INBOX)) proj_add(projs, &nproj, old, nold, items[i].proj);
    home_file(p, "/.projects");
    if ((f = file_get(p, &len))) {
        for (char *l = f, *e; *l; l = e ? e + 1 : l + strlen(l)) {
            e = strchr(l, '\n');
            if (e) *e = 0;
            if (*l && strcmp(l, INBOX)) proj_add(projs, &nproj, old, nold, l);
            if (!e) break;
        }
        free(f);
    }
    if (strcmp(cur, INBOX)) proj_add(projs, &nproj, old, nold, cur);   /* (a window's project stays) */
    for (int i = 1; i < nproj; i++)                      /* sorted by name */
        for (int j = i; j > 0 && strcmp(projs[j - 1].name, projs[j].name) > 0; j--) {
            proj_t t = projs[j]; projs[j] = projs[j - 1]; projs[j - 1] = t;
        }
    proj_t inbox = { INBOX, 0, -1, -1 };
    for (int i = 0; i < nold; i++) if (!strcmp(old[i].name, INBOX)) { inbox.main = old[i].main; inbox.beside = old[i].beside; }
    projs[nproj++] = inbox;
    for (int i = 0; i < nitems; i++) { int k = proj_find(items[i].proj); if (k >= 0) projs[k].n++; }
    char l[240];
    size_t n = 0;
    for (int i = 0; i < nproj && n < sizeof l - 50; i++) {
        n += strlcpy(l + n, projs[i].name, 33);
        l[n++] = '=';
        n += num(l + n, projs[i].n);
        l[n++] = ' ';
    }
    l[n] = 0;
    say("atlas: projects: %s\n", l);
}

/* A file's first lines (for its card), read only while the card is drawn. */
static void preview(item_t *it)
{
    if (it->pv || it->app) return;
    char buf[240];
    long h = atlas_may(it->path, 0) ? fs_open(it->path, FS_RDONLY, 0) : -EACCES;
    long n = h < 0 ? 0 : fs_read(h, 0, buf, sizeof buf);
    if (h >= 0) fs_close(h);
    if (n < 0) n = 0;
    if (!(it->pv = malloc(n + 1))) return;
    memcpy(it->pv, buf, n);
    it->pv[n] = 0;
}

/* ---- Windows: what each one is, and where. */
static int win_used(int w)
{
    return w >= 0 && w < MAXTERM ? terms[w].used : w == W_EDIT ? edit_active() : w == W_SIA ? sia_on : 0;
}
static const char *win_proj(int w) { return w < MAXTERM ? terms[w].proj : w == W_EDIT ? edit_proj : sia_proj; }
static void win_title(int w, char *t, size_t cap)
{
    if (w < MAXTERM) { size_t n = strlcpy(t, "Terminal ", cap); t[n + num(t + n, terms[w].chan)] = 0; }
    else if (w == W_EDIT) { strlcpy(t, base_name(edit_path()), cap); if (edit_dirty()) strlcpy(t + strlen(t), " *", cap - strlen(t)); }
    else strlcpy(t, "sia", cap);
}

/* Keep each project's stage pointing at its own live windows only. */
static void tidy(void)
{
    for (int i = 0; i < nproj; i++) {
        proj_t *p = &projs[i];
        if (p->main >= 0 && (!win_used(p->main) || strcmp(win_proj(p->main), p->name))) p->main = -1;
        if (p->beside >= 0 && (!win_used(p->beside) || strcmp(win_proj(p->beside), p->name))) p->beside = -1;
        if (p->main < 0 && p->beside >= 0) { p->main = p->beside; p->beside = -1; }
    }
    if (focus >= 0 && (!win_used(focus) || (here()->main != focus && here()->beside != focus))) focus = here()->main;
}

static void say_stage(void)
{
    proj_t *p = here();
    char a[48] = "-", b[48] = "-";
    if (p->main >= 0) win_title(p->main, a, sizeof a);
    if (p->beside >= 0) win_title(p->beside, b, sizeof b);
    say("atlas: stage %s: main=%s beside=%s\n", p->name, a, b);
}

static void put_main(int w)                              /* on the stage, wide, with the keys */
{
    proj_t *p = here();
    if (p->beside == w) p->beside = -1;
    if (p->main == W_SIA && w != W_SIA && p->beside < 0) p->beside = W_SIA;   /* (the assistant stays in view) */
    p->main = w;
    focus = w; lens_on = 0;
    tidy();
    say_stage();
}
static void put_beside(int w)
{
    proj_t *p = here();
    if (p->main == w) { p->main = p->beside; }
    p->beside = w;
    if (p->main < 0) { p->main = w; p->beside = -1; }
    focus = w; lens_on = 0;
    tidy();
    say_stage();
}
static void to_shelf(int w)
{
    proj_t *p = here();
    if (p->main == w) { p->main = p->beside; p->beside = -1; }
    else if (p->beside == w) p->beside = -1;
    tidy();
    say_stage();
}

/* ---- Click boxes. */
#define MAXHIT 500
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
    dl_text(x + 14, y + 10, s, strlen(s), S(2), on ? C_BG : C_TXT, 0, 0, SW, SH);
    if (kind) hitbox(x, y, x + w, y + 42, kind, arg);
    return x + w;
}
void label(int x, int y, const char *s, int k, uint32_t c) { dl_text(x, y, s, strlen(s), S(k), c, 0, 0, SW, SH); }

/* ---- The Lens: files (by name, every project), apps, and "Ask sia". */
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
    lens_on = 1;
    say("atlas: lens %s: %d files\n", query, total);
    return total;
}

/* ---- The picture. */
static unsigned frame;                  /* pictures built: a preview not shown in this one is dropped */
static const uint32_t dots[6] = { 0x4be3c1, 0x8b7bff, 0xffb86b, 0x6bb8ff, 0xff7aa8, 0xc6e36b };

static void draw_greeter(void)
{
    if (first_start) { setup_draw(); return; }          /* no account yet: the welcome screen */
    int x0 = SW / 2 - 360, y0 = SH / 2 - 250, x1 = SW / 2 + 360, y1 = SH / 2 + 250;
    dl_rect(x0, y0, x1, y1, 24, C_GLASS);
    dl_frame(x0, y0, x1, y1, 24, 1, C_LINE);
    label(x0 + 60, y0 + 50, "SIEOS", 7, C_ACC);
    label(x0 + 60, y0 + 140, "one project, one focus", 2, C_DIM);
    const char *names[2] = { "Name", "Password" };
    for (int f = 0; f < 2; f++) {
        int fy = y0 + 210 + f * 90;
        label(x0 + 60, fy + 12, names[f], 2, C_DIM);
        dl_rect(x0 + 230, fy, x1 - 60, fy + 50, 10, C_BG);
        dl_frame(x0 + 230, fy, x1 - 60, fy + 50, 10, gfield == f ? 2 : 1, gfield == f ? C_ACC : C_LINE);
        char st[64];
        int n = strlen(gpw) < sizeof st - 1 ? strlen(gpw) : sizeof st - 1;
        memset(st, '*', n);
        dl_text(x0 + 250, fy + 14, f ? st : gname, f ? n : (int)strlen(gname), S(2), C_TXT, 0, 0, x1 - 70, SH);
        int cw = tw(f ? n : (int)strlen(gname), 2);
        if (gfield == f) dl_rect(x0 + 252 + cw, fy + 12, x0 + 264 + cw, fy + 38, 0, C_ACC);
        hitbox(x0 + 230, fy, x1 - 60, fy + 50, H_FIELD, f);
    }
    if (gmsg[0]) label(x0 + 60, y0 + 400, gmsg, 2, C_ERR);
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

/* A window of the stage: its frame and title strip with buttons, then its body. */
enum { WB_SWAP, WB_SHELF, WB_CLOSE };
static void draw_window(int w, int x0, int x1)
{
    int y0 = SY0, y1 = SY1, f = focus == w && !lens_on;
    dl_rect(x0, y0, x1, y1, 16, C_GLASS);
    dl_frame(x0, y0, x1, y1, 16, f ? 2 : 1, f ? C_ACC : C_LINE);
    hitbox(x0, y0, x1, y1, H_WIN, w);
    char t[64];
    win_title(w, t, sizeof t);
    if (w < MAXTERM && terms[w].back) { size_t n = strlen(t); n += strlcpy(t + n, "  [history -", 16); t[n + num(t + n, terms[w].back)] = 0; strlcpy(t + strlen(t), "]", 2); }
    dl_text(x0 + 20, y0 + 14, t, strlen(t), S(2), f ? C_ACC : C_TXT, x0, y0, x1 - 260, y0 + TBAR);
    proj_t *p = here();
    int bx = x1 - 12;
    static const char *bl[3] = { "Swap", "Shelf", "Close" };
    for (int b = 2; b >= 0; b--) {
        if (b == WB_SWAP && !(p->main >= 0 && p->beside >= 0)) continue;
        bx -= tw(strlen(bl[b]), 2) + 28;
        pill(bx, y0 + 3, bl[b], 0, H_WBTN, w * 4 + b);
        if (atlas_debug) say("atlas: dbg wbtn %s %s at %d %d\n", t, bl[b], bx + 30, y0 + 24);
        bx -= 8;
    }
    dl_rect(x0 + 1, y0 + TBAR - 1, x1 - 1, y0 + TBAR, 0, C_LINE);
    if (w < MAXTERM) draw_term_body(w, x0, y0, x1, y1, f);
    else if (w == W_EDIT) edit_draw(x0, y0, x1, y1, f);
    else panel_draw(x0, y0, x1, y1, f);
}

/* The shelf: what is in this project but not on the stage. */
enum { C_WINDOW = 1, C_ITEM, C_ADD };
#define MAXCARD (MAXITEM + NWIN + 1)
static struct { short kind, i; } cards[MAXCARD];
static int ncards;
static void shelf_list(void)
{
    proj_t *p = here();
    ncards = 0;
    for (int w = 0; w < NWIN; w++)
        if (win_used(w) && !strcmp(win_proj(w), p->name) && w != p->main && w != p->beside) cards[ncards++] = (typeof(cards[0])){ C_WINDOW, w };
    for (int a = 0; a < 2; a++)                          /* files, then apps */
        for (int i = 0; i < nitems; i++)
            if (items[i].app == a && !strcmp(items[i].proj, p->name) &&
                !((p->main == W_EDIT || p->beside == W_EDIT) && !strcmp(items[i].path, edit_path()))) cards[ncards++] = (typeof(cards[0])){ C_ITEM, i };
    cards[ncards++] = (typeof(cards[0])){ C_ADD, 0 };
}
static int card_x(int k) { return SX0 + (k - shelf_off) * (CARDW + 18); }
static int shelf_fit(void) { return (SX1 - SX0 + 18) / (CARDW + 18); }

static void card_text(int k, char *title, size_t cap, char *kind, size_t kcap, const char *line[3])
{
    line[0] = line[1] = line[2] = 0;
    if (cards[k].kind == C_ADD) { strlcpy(title, "+ Add", cap); strlcpy(kind, "file, terminal, app", kcap); return; }
    if (cards[k].kind == C_WINDOW) {
        int w = cards[k].i;
        win_title(w, title, cap);
        if (w < MAXTERM) {                               /* its last non-empty lines */
            strlcpy(kind, "terminal", kcap);
            term_t *t = &terms[w];
            int got = 0;
            for (int r = t->rows - 1; r >= 0 && got < 2; r--) {
                const char *l = tline(t, t->top + r);
                int e = t->cols;
                while (e && l[e - 1] == ' ') e--;
                if (e) line[1 - got++] = l;
            }
        } else if (w == W_EDIT) strlcpy(kind, "editing", kcap);
        else { strlcpy(kind, "assistant", kcap); line[0] = panel_last(1); line[1] = panel_last(0); }
        return;
    }
    item_t *it = &items[cards[k].i];
    strlcpy(title, it->name, cap);
    if (it->app) { strlcpy(kind, "app", kcap); return; }
    char *p = kind + strlcpy(kind, "file  ", kcap);
    uint64_t s = it->size;
    p += num(p, s >= 1024 ? (long)(s / 1024) : (long)s);
    strlcpy(p, s >= 1024 ? " KB" : " bytes", 8);
    preview(it);
    it->seen = frame;
    line[0] = it->pv;
    line[1] = it->pv && strchr(it->pv, '\n') ? strchr(it->pv, '\n') + 1 : 0;
}

static void draw_card(int k, int x, int y, int lifted)
{
    char title[48], kind[32];
    const char *line[3];
    card_text(k, title, sizeof title, kind, sizeof kind, line);
    int add = cards[k].kind == C_ADD, chars = (CARDW - 28) / KW;
    dl_rect(x, y, x + CARDW, y + CARDH, 14, add ? C_BG : C_GLASS);
    dl_frame(x, y, x + CARDW, y + CARDH, 14, lifted ? 2 : 1, lifted ? C_ACC : C_LINE);
    if (cards[k].kind == C_ITEM && items[cards[k].i].app) {   /* an app: a square with its initial */
        dl_rect(x + 14, y + 14, x + 44, y + 44, 8, C_ACC);
        dl_text(x + 23, y + 18, title, 1, S(2), C_BG, x, y, x + CARDW, y + CARDH);
        dl_text(x + 56, y + 18, title, strlen(title) < (size_t)chars - 3 ? strlen(title) : (size_t)chars - 3, S(2), C_TXT, x, y, x + CARDW - 8, y + CARDH);
    } else dl_text(x + 14, y + 14, title, strlen(title) < (size_t)chars ? strlen(title) : (size_t)chars, S(2), add ? C_DIM : C_TXT, x, y, x + CARDW - 8, y + CARDH);
    label(x + 14, y + 52, kind, 2, C_DIM);
    for (int b = 0; b < 2; b++) {                        /* its first (or last) lines, small */
        if (!line[b]) continue;
        int n = 0;
        while (line[b][n] && line[b][n] != '\n' && n < (CARDW - 28) / CW) n++;
        dl_text(x + 14, y + 88 + b * 16, line[b], n, S(1), C_DIM, x, y, x + CARDW - 10, y + CARDH - 6);
    }
    hitbox(x, y, x + CARDW, y + CARDH, H_CARD, k);
}

static void draw_menu(void)
{
    if (!mn.on) return;
    int h = 52 + mn.n * MENH + 8;
    dl_rect(mn.x, mn.y, mn.x + MENW, mn.y + h, 14, C_GLASS);
    dl_frame(mn.x, mn.y, mn.x + MENW, mn.y + h, 14, 2, C_ACC);
    dl_text(mn.x + 18, mn.y + 16, mn.title, strlen(mn.title), S(2), C_DIM, mn.x, mn.y, mn.x + MENW - 10, mn.y + 52);
    for (int i = 0; i < mn.n; i++) {
        int iy = mn.y + 52 + i * MENH;
        if (i) dl_rect(mn.x + 14, iy, mn.x + MENW - 14, iy + 1, 0, C_BAR);
        int sub = mn.e[i].act == A_SUB_MOVE || mn.e[i].act == A_SUB_APPS;
        uint32_t c = mn.e[i].act == A_CANCEL ? C_DIM : mn.e[i].act == A_DELETE_YES ? C_ERR : C_TXT;
        dl_text(mn.x + 26, iy + 12, mn.e[i].label, strlen(mn.e[i].label), S(2), c, mn.x, iy, mn.x + MENW - 40, iy + MENH);
        if (sub) label(mn.x + MENW - 36, iy + 12, ">", 2, C_DIM);
        hitbox(mn.x, iy, mn.x + MENW, iy + MENH, H_MENU, i);
    }
}

static void draw_prompt(void)
{
    if (!pr.on) return;
    int x0 = SW / 2 - 330, y0 = SH / 2 - 120, x1 = SW / 2 + 330, y1 = SH / 2 + 110;
    dl_rect(x0, y0, x1, y1, 18, C_GLASS);
    dl_frame(x0, y0, x1, y1, 18, 2, C_ACC);
    label(x0 + 30, y0 + 26, pr.title, 2, C_TXT);
    dl_rect(x0 + 30, y0 + 74, x1 - 30, y0 + 124, 10, C_BG);
    dl_frame(x0 + 30, y0 + 74, x1 - 30, y0 + 124, 10, 2, C_ACC);
    label(x0 + 48, y0 + 88, pr.buf, 2, C_TXT);
    int cw = tw(strlen(pr.buf), 2);
    dl_rect(x0 + 50 + cw, y0 + 86, x0 + 62 + cw, y0 + 112, 0, C_ACC);
    int okx = x1 - 30 - (tw(2, 2) + 28);
    pill(okx, y1 - 66, "OK", 1, H_PR, 1);
    pill(okx - 20 - (tw(6, 2) + 28), y1 - 66, "Cancel", 0, H_PR, 0);
}

/* The Lens: the field, and its results under it. */
static int lens_x0(void) { int w = SW - 2 * 420 > 900 ? 900 : SW - 2 * 420 < 420 ? 420 : SW - 2 * 420; return SW / 2 - w / 2; }
static int lens_x1(void) { return SW - lens_x0(); }
static void draw_lens(void)
{
    int x0 = lens_x0(), x1 = lens_x1();
    dl_rect(x0, 16, x1, 64, 24, C_GLASS);
    dl_frame(x0, 16, x1, 64, 24, lens_on ? 2 : 1, lens_on ? C_ACC : C_LINE);
    hitbox(x0, 16, x1, 64, H_LENS, 0);
    if (query[0]) dl_text(x0 + 26, 29, query, strlen(query), S(2), C_TXT, x0, 16, x1 - 20, 64);
    else label(x0 + 26, 29, lens_on ? "a file, an app, or a question" : "Ask sia, open a file, run an app...  Ctrl+Space", 2, C_DIM);
    if (lens_on) { int cw = tw(strlen(query), 2); dl_rect(x0 + 28 + cw, 28, x0 + 38 + cw, 52, 0, C_ACC); }
    if (!lens_on || (!nres && !nfound)) return;
    int y = 72, last = 0;
    int h = 16;
    for (int i = 0; i < nres; i++) h += 46 + (res[i].kind != last ? 32 : 0), last = res[i].kind == R_SYSAPP ? R_APP : res[i].kind;
    if (nfound > nres) h += 40;
    dl_rect(x0, y, x1, y + h, 16, C_GLASS);
    dl_frame(x0, y, x1, y + h, 16, 1, C_LINE);
    y += 8; last = 0;
    static const char *grp[5] = { "", "FILES", "APPS", "APPS", "ASK SIA" };
    for (int i = 0; i < nres; i++) {
        int g = res[i].kind == R_SYSAPP ? R_APP : res[i].kind;
        if (g != last) { label(x0 + 26, y + 8, grp[g], 2, C_DIM); y += 32; last = g; }
        dl_rect(x0 + 10, y, x1 - 10, y + 42, 10, i == 0 ? C_BAR : C_GLASS);
        char t[96] = "", w[48] = "";
        if (res[i].kind == R_FILE) { strlcpy(t, items[res[i].i].name, sizeof t); strlcpy(w, items[res[i].i].proj, sizeof w); }
        else if (res[i].kind == R_APP) { strlcpy(t, items[res[i].i].name, sizeof t); strlcpy(w, items[res[i].i].proj, sizeof w); }
        else if (res[i].kind == R_SYSAPP) { strlcpy(t, apps[res[i].i].name, sizeof t); strlcpy(w, "start in this project", sizeof w); }
        else { strlcpy(t, query[0] == '?' ? query + 1 : query, sizeof t); strlcpy(w, "Tab", sizeof w); }
        dl_text(x0 + 26, y + 10, t, strlen(t), S(2), C_TXT, x0, y, x1 - 30 - tw(strlen(w), 2), y + 42);
        label(x1 - 26 - tw(strlen(w), 2), y + 10, w, 2, C_DIM);
        hitbox(x0 + 10, y, x1 - 10, y + 42, H_RES, i);
        if (atlas_debug) say("atlas: dbg res %d %s at %d %d\n", i, t, (x0 + x1) / 2, y + 21);
        y += 46;
    }
    if (nfound > nres) { char t[48]; t[num(t, nfound - nres)] = 0; strlcpy(t + strlen(t), " more", 8); label(x0 + 26, y + 8, t, 2, C_DIM); }
}

static void build(void)
{
    dl_reset();
    nhit = 0;
    frame++;
    dl_rect(0, 0, SW, SH, 0, C_BG);
    if (mode == GREETER) { dl_grid(0, 0, 40, C_BG, 0x111829); draw_greeter(); dl_pointer(ptr_x, ptr_y); return; }

    /* ---- the top bar: name, the Lens (drawn last: its results go over the stage), status */
    label(M, 26, "SIEOS", 2, C_TXT);
    char st[48], ck[8];
    long d = now_day();
    int k = 0;
    ck[k++] = '0' + d / 36000; ck[k++] = '0' + d / 3600 % 10; ck[k++] = ':';
    ck[k++] = '0' + d / 600 % 6; ck[k++] = '0' + d / 60 % 10; ck[k] = 0;
    int rx = SW - M - tw(5, 2);
    label(rx, 26, ck, 2, C_TXT);
    panel_status(st, sizeof st);
    rx -= tw(strlen(st), 2) + 30;
    label(rx, 26, st, 2, C_DIM);
    const char *net = port_find("net") > 0 ? "network" : "network: off";
    rx -= tw(strlen(net), 2) + 30;
    if (rx > lens_x1() + 10) label(rx, 26, net, 2, C_DIM);
    if (toast[0]) dl_text(M, 60, toast, strlen(toast), S(2), C_ACC, 0, 0, lens_x0() - 10, SH);

    /* ---- the rail: projects, then the Inbox, then you */
    dl_rect(M, SY0, RX1, SH - M, 16, C_GLASS);
    dl_frame(M, SY0, RX1, SH - M, 16, 1, C_LINE);
    hitbox(M, SY0, RX1, SH - M, H_PROJ, -1);
    label(M + 20, SY0 + 18, "PROJECTS", 2, C_DIM);
    for (int i = 0, y = SY0 + 56; i < nproj; i++, y += 50) {
        int ymax = SH - M - 170;                         /* (more projects than room: the last ones are not shown) */
        if (i < nproj - 1 && y > ymax - 50) continue;
        if (i == nproj - 1) {                            /* "+ New project" under the others, the Inbox at the bottom */
            if (y > ymax) y = ymax;
            label(M + 24, y + 12, "+ New project", 2, C_DIM);
            hitbox(M + 10, y, RX1 - 10, y + 46, H_BTN, 1);
            if (atlas_debug) say("atlas: dbg newproject at %d %d\n", (M + RX1) / 2, y + 23);
            y = SH - M - 120;
        }
        proj_t *p = &projs[i];
        int on = !strcmp(p->name, cur);
        if (on) dl_rect(M + 10, y, RX1 - 10, y + 46, 12, C_BAR);
        dl_rect(M + 24, y + 18, M + 34, y + 28, 5, i == nproj - 1 ? C_DIM : dots[i % 6]);
        dl_text(M + 48, y + 12, p->name, strlen(p->name), S(2), on ? C_TXT : C_DIM, M, y, RX1 - 60, y + 46);
        char c[8];
        c[num(c, p->n)] = 0;
        label(RX1 - 24 - tw(strlen(c), 2), y + 12, c, 2, C_DIM);
        hitbox(M + 10, y, RX1 - 10, y + 46, H_PROJ, i);
        if (atlas_debug) say("atlas: dbg project %s at %d %d\n", p->name, (M + RX1) / 2, y + 23);
    }
    dl_rect(M + 14, SH - M - 64, RX1 - 14, SH - M - 63, 0, C_LINE);
    dl_text(M + 24, SH - M - 46, user, strlen(user), S(2), C_TXT, M, 0, RX1 - 130, SH);
    pill(RX1 - 14 - tw(7, 2) - 28, SH - M - 54, "Log out", 0, H_BTN, 2);

    /* ---- the stage: at most two windows */
    tidy();
    proj_t *p = here();
    if (p->main < 0) {
        dl_rect(SX0, SY0, SX1, SY1, 16, C_BG);
        dl_frame(SX0, SY0, SX1, SY1, 16, 1, C_LINE);
        static const char *e[3] = { "Nothing on the stage.", "Click a card on the shelf below,",
                                    "or press Ctrl+T for a terminal, Ctrl+Space to find or ask." };
        for (int i = 0; i < 3; i++) label(SX0 + (SX1 - SX0) / 2 - tw(strlen(e[i]), 2) / 2, (SY0 + SY1) / 2 - 50 + i * 36, e[i], 2, i ? C_DIM : C_TXT);
    }
    for (int w = 0; w < NWIN; w++) {
        int x0, x1;
        if (w != p->main && w != p->beside) continue;
        slot(w, &x0, &x1);
        if (x1 > x0) draw_window(w, x0, x1);
    }

    /* ---- the shelf */
    shelf_list();
    int fit = shelf_fit();
    if (shelf_off > ncards - fit) shelf_off = ncards - fit;
    if (shelf_off < 0) shelf_off = 0;
    char sl[80];
    strlcpy(sl, "IN THIS PROJECT", sizeof sl);
    if (ncards > fit) strlcpy(sl + strlen(sl), "  (scroll for more)", sizeof sl - strlen(sl));
    label(SX0, SHY + 8, sl, 2, C_DIM);
    for (int c = shelf_off; c < ncards && c < shelf_off + fit; c++) {
        if (drag == D_CARD && drag_on && c == drag_arg) continue;
        draw_card(c, card_x(c), SH - M - CARDH, 0);
        if (atlas_debug) { char t[48], kd[32]; const char *l[3]; card_text(c, t, sizeof t, kd, sizeof kd, l); say("atlas: dbg card %s at %d %d\n", t, card_x(c) + CARDW / 2, SH - M - CARDH + 30); }
    }
    if (drag == D_CARD && drag_on) draw_card(drag_arg, ptr_x - CARDW / 2, ptr_y - 30, 1);   /* (being dragged) */
    for (int i = 0; i < nitems; i++)                     /* memory follows the view */
        if (items[i].pv && items[i].seen != frame) { free(items[i].pv); items[i].pv = 0; }

    draw_lens();
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
        proj_t *p = here();
        for (int w = 0; w < MAXTERM; w++)
            if (w == p->main || w == p->beside) {
                int x0, x1;
                slot(w, &x0, &x1);
                say("atlas: terminal %d at %d %d cell %d %d\n", terms[w].chan, x0 + 14, SY0 + TBAR + 8, KW, KH);
            }
        say("atlas: dbg stage %d %d %d %d, frame %lu us\n", SX0, SY0, SX1, SY1, (unsigned long)frame_us);
    }
    atlas_debug = 0;
}

/* ---- Projects: switching, and what their menus do. */
static void open_project(const char *name)
{
    strlcpy(cur, name, sizeof cur);
    shelf_off = 0;
    tidy();
    focus = here()->main;
    lens_on = 0;
    say("atlas: project %s\n", cur);
    say_stage();
    settle();
}
int atlas_open_project(const char *name)
{
    if (proj_find(name) < 0) return -ENOENT;
    open_project(name);
    return 0;
}

static long set_project(const char *path, const char *proj)   /* move a file (to the Inbox: no attribute) */
{
    if (!atlas_may(path, 1)) return -EACCES;
    if (!strcmp(proj, INBOX)) { fs_rmxattr(path, "project"); return 0; }
    return fs_setxattr(path, "project", proj, strlen(proj));
}

/* A file moved by hand: its project stays on the rail even if that was its last file. */
static long move_item(int i, const char *to)
{
    char from[32];
    strlcpy(from, items[i].proj, sizeof from);
    long e = set_project(items[i].path, to);
    int k = proj_find(from);
    if (!e && k >= 0 && projs[k].n == 1 && strcmp(from, INBOX)) empty_set(from, 0);
    say("atlas: %s moved to %s (%ld)\n", base_name(items[i].path), to, e);
    if (e < 0) strlcpy(toast, "Not allowed: not your file.", sizeof toast);
    return e;
}

static void rename_or_delete(const char *from, const char *to)   /* to = 0: delete (files to the Inbox) */
{
    int done = 0, denied = 0;
    for (int i = 0; i < nitems; i++) {
        if (strcmp(items[i].proj, from)) continue;
        if (set_project(items[i].path, to ? to : INBOX) < 0) denied++; else done++;
    }
    for (int t = 0; t < MAXTERM; t++) if (terms[t].used && !strcmp(terms[t].proj, from)) strlcpy(terms[t].proj, to ? to : INBOX, 32);
    if (!strcmp(edit_proj, from)) strlcpy(edit_proj, to ? to : INBOX, sizeof edit_proj);
    if (!strcmp(sia_proj, from)) strlcpy(sia_proj, to ? to : INBOX, sizeof sia_proj);
    empty_set(to, from);
    if (to) say("atlas: project %s renamed to %s (%d files, %d not allowed)\n", from, to, done, denied);
    else say("atlas: project %s deleted (%d files to the Inbox, %d not allowed)\n", from, done, denied);
    if (!strcmp(cur, from)) strlcpy(cur, to ? to : INBOX, sizeof cur);
    rescan();
    settle();
}

static int good_name(const char *s)                      /* letters, digits, ". _ -" and spaces */
{
    if (!*s || *s == '.' || *s == ' ') return 0;
    for (; *s; s++) if (!((*s | 32) >= 'a' && (*s | 32) <= 'z') && !(*s >= '0' && *s <= '9') && !strchr("._- ", *s)) return 0;
    return 1;
}

/* ---- Terminals: open (a shell for the user, on "atlas#N"), close. */
long atlas_auth(msg_t *m) { long e = call_named(&auth_port, "auth", m, 0); return e < 0 ? e : (long)m->w[0]; }

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
    next_chan++;
    term_t *w = &terms[t];
    memset(w, 0, sizeof *w);
    w->ring = ring;
    memset(ring, ' ', HIST * TCMAX);
    w->used = 1; w->chan = chan; w->pid = pid; w->cols = 80; w->rows = 25;
    strlcpy(w->proj, cur, sizeof w->proj);
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
    tidy();
}

static void new_term(int beside)
{
    long t = open_term(0);
    if (t < 0) { settle(); return; }
    if (beside) put_beside(t); else put_main(t);
    settle();
}

static void close_win(int w)
{
    if (w < MAXTERM) { if (terms[w].used) sys_kill(terms[w].pid); return; }   /* (closed when its shell ends) */
    if (w == W_EDIT) { if (edit_dirty()) { edit_key(27); if (edit_active()) { strlcpy(toast, "Not saved: Close again to discard.", sizeof toast); settle(); return; } } else edit_close(); }
    if (w == W_SIA) { sia_on = 0; say("atlas: assistant closed\n"); }
    tidy();
    say_stage();
    settle();
}

static void logout(void)
{
    for (int t = 0; t < MAXTERM; t++) if (terms[t].used) { sys_kill(terms[t].pid); close_term(&terms[t]); }
    for (int i = 0; i < nitems; i++) { free(items[i].pv); items[i].pv = 0; }
    edit_close();
    sia_on = 0;
    nitems = nproj = 0;
    strlcpy(cur, INBOX, sizeof cur);
    mode = GREETER;
    uid = ugid = -1; ugroups.n = 0; gname[0] = gpw[0] = gmsg[0] = query[0] = toast[0] = 0; gfield = 0;
    nres = nfound = 0; lens_on = 0; mn.on = pr.on = 0; focus = -1;
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

/* The system's launchers, for the Lens and "Open app". */
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
    uid = u.uid; ugid = u.gid;                          /* (for rescan's rights; undone if refused) */
    if (acct_groups(user, &ugroups) < 0) ugroups.n = 0;
    strlcpy(home, u.home, sizeof home);
    rescan();
    strlcpy(cur, projs[0].name, sizeof cur);            /* the first project (the Inbox if none) */
    long t = open_term(gpw);                            /* (auth checks the password: it starts the shell) */
    wipe(gpw, sizeof gpw);
    if (t < 0) {
        uid = ugid = -1; ugroups.n = 0; nitems = nproj = 0;
        strlcpy(gmsg, "Wrong name or password.", sizeof gmsg);
        say("atlas: login %s: refused\n", gname);
        return;
    }
    say("atlas: login %s: ok\n", user);
    gmsg[0] = 0;
    mode = SESSION;
    scan_apps();
    put_main(t);
    say("atlas: project %s\n", cur);
    settle();
}

/* ---- Files and apps: opening them. */
static int item_find(const char *path)
{
    for (int i = 0; i < nitems; i++) if (!strcmp(items[i].path, path)) return i;
    return -1;
}

static int open_file_beside(const char *path, int beside)   /* in the editor, in the file's project */
{
    if (edit_active() && strcmp(edit_path(), path) && edit_dirty()) {
        strlcpy(toast, "Save or close the file being edited first.", sizeof toast);
        say("atlas: %s: another file is being edited\n", path);
        settle();
        return -EBUSY;
    }
    if (edit_open(path) < 0) { settle(); return -EACCES; }
    int i = item_find(path);
    const char *proj = i >= 0 ? items[i].proj : INBOX;
    if (strcmp(cur, proj)) { strlcpy(cur, proj, sizeof cur); shelf_off = 0; say("atlas: project %s\n", cur); }
    strlcpy(edit_proj, cur, sizeof edit_proj);
    if (beside) put_beside(W_EDIT); else put_main(W_EDIT);
    settle();
    return 0;
}
static int open_file(const char *path) { return open_file_beside(path, 0); }

int atlas_open_file(const char *path)
{
    int best = -1;
    for (int i = 0; i < nitems; i++) {
        if (!strcmp(items[i].path, path)) { best = i; break; }
        size_t a = strlen(items[i].path), b = strlen(path);     /* or by its end: "notes/x.txt", "x.txt" */
        if (b < a && !strcmp(items[i].path + a - b, path) && items[i].path[a - b - 1] == '/') best = i;
    }
    if (best < 0) return -ENOENT;
    return open_file(items[best].path) < 0 ? -EACCES : 0;
}

static void prompt_open(int act, const char *title, const char *proj, const char *init)
{
    mn.on = 0;
    pr.on = 1; pr.act = act;
    strlcpy(pr.title, title, sizeof pr.title);
    strlcpy(pr.proj, proj ? proj : "", sizeof pr.proj);
    strlcpy(pr.buf, init ? init : "", sizeof pr.buf);
    say("atlas: prompt %s\n", title);
    redraw();
}

/* Start an app: a terminal (a shell as the logged-in user, never root) in
 * the current project, which runs the app's command when it first reads. */
static void app_start(const char *path, int beside)
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
    if (!strcmp(kind, "editor")) { prompt_open(P_NEWFILE, "New file: its name?", cur, 0); return; }
    if (rn[0] != '/') { say("atlas: app %s: no program\n", nm); settle(); return; }
    long t = open_term(0);
    if (t < 0) { settle(); return; }
    if (strcmp(rn, "/bin/sh")) {
        char *p = terms[t].pend;
        p += strlcpy(p, rn, 64);
        if (args[0]) { *p++ = ' '; strlcpy(p, args, 96); }
    }
    say("atlas: app %s started in %s (terminal %d)\n", nm, cur, terms[t].chan);
    if (beside) put_beside(t); else put_main(t);
    settle();
}

/* "Open app" in a project: its launcher is copied to ~/apps (once) and put
 * in the project, so it is on the project's shelf; then it starts. */
static void pin_app(const char *sys)
{
    char dir[96], path[160];
    strlcpy(dir + strlcpy(dir, home, sizeof dir), "/apps", 8);
    strlcpy(path + strlcpy(path, dir, sizeof path), "/", 2);
    strlcpy(path + strlen(path), base_name(sys), sizeof path - strlen(path));
    siefs_stat_t st;
    if (atlas_may(home, 1)) {
        if (fs_stat(dir, &st, 1) && !fs_mkdir(dir, 0755)) fs_chown(dir, uid, ugid);
        if (fs_stat(path, &st, 1)) {
            size_t n = 0;
            char *f = file_get(sys, &n);
            long h = f ? fs_open(path, FS_WRONLY | FS_CREAT | FS_EXCL, 0644) : -ENOENT;
            if (h >= 0) { fs_write(h, 0, f, n); fs_close(h); fs_chown(path, uid, ugid); }
            free(f);
        }
        if (!fs_stat(path, &st, 1)) { set_project(path, cur); rescan(); app_start(path, 0); return; }
    }
    app_start(sys, 0);                                   /* (not kept on the shelf) */
}

static void new_file(const char *proj, const char *name)
{
    char path[160];
    size_t k = strlcpy(path, home, sizeof path);
    path[k++] = '/';
    strlcpy(path + k, name, sizeof path - k);
    siefs_stat_t st;
    long e = !atlas_may(home, 1) ? -EACCES : !fs_stat(path, &st, 1) ? -EEXIST : 0;
    if (!e) {
        long h = fs_open(path, FS_WRONLY | FS_CREAT | FS_EXCL, 0644);
        e = h < 0 ? h : 0;
        if (h >= 0) fs_close(h);
    }
    if (!e) { fs_chown(path, uid, ugid); set_project(path, proj); }   /* (created by root: it is the user's) */
    say("atlas: new file %s in %s (%ld)\n", name, proj, e);
    rescan();
    if (e) { strlcpy(toast, e == -EEXIST ? "A file with that name exists." : "Cannot create it here.", sizeof toast); settle(); return; }
    open_file(path);
}

static void ask_sia(const char *q)                       /* the assistant's window, then the question */
{
    if (!sia_on || strcmp(sia_proj, cur)) { sia_on = 1; strlcpy(sia_proj, cur, sizeof sia_proj); }
    proj_t *p = here();
    if (p->main != W_SIA && p->beside != W_SIA) { if (p->main >= 0) put_beside(W_SIA); else put_main(W_SIA); }
    focus = W_SIA; lens_on = 0;
    say("atlas: assistant open\n");
    if (q && *q) panel_ask(q);
    settle();
}

static void prompt_done(void)
{
    pr.on = 0;
    char *b = pr.buf;
    while (*b && b[strlen(b) - 1] == ' ') b[strlen(b) - 1] = 0;
    if (!good_name(b) || !strcmp(b, INBOX)) { say("atlas: prompt: bad name\n"); strlcpy(toast, "Use letters, digits, . _ - and spaces.", sizeof toast); settle(); return; }
    if (pr.act == P_NEWFILE) { new_file(pr.proj, b); return; }
    if (proj_find(b) >= 0) { say("atlas: project %s already exists\n", b); strlcpy(toast, "That project exists.", sizeof toast); settle(); return; }
    if (pr.act == P_RENAME) { rename_or_delete(pr.proj, b); return; }
    empty_set(b, 0);                                     /* P_NEWPROJ */
    say("atlas: new project %s\n", b);
    rescan();
    open_project(b);
}

/* ---- Menus. */
static void menu_new(const char *title, const char *path, const char *proj, int win)
{
    mn.n = 0;
    strlcpy(mn.title, title, sizeof mn.title);
    strlcpy(mn.path, path ? path : "", sizeof mn.path);
    strlcpy(mn.proj, proj ? proj : "", sizeof mn.proj);
    mn.win = win;
}
static void menu_add(const char *l, int act, const char *arg)
{
    if (mn.n == MAXMENT) return;
    strlcpy(mn.e[mn.n].label, l, sizeof mn.e[0].label);
    strlcpy(mn.e[mn.n].arg, arg ? arg : "", sizeof mn.e[0].arg);
    mn.e[mn.n++].act = act;
}
static void menu_show(int x, int y)
{
    mn.on = 1;
    mn.x = x + MENW > SW - 10 ? SW - 10 - MENW : x;
    mn.y = y + 62 + mn.n * MENH > SH - 10 ? SH - 10 - 62 - mn.n * MENH : y;
    say("atlas: menu %s: %d entries\n", mn.title, mn.n);
    for (int i = 0; i < mn.n; i++) say("atlas: menu entry %d %s at %d %d\n", i, mn.e[i].label, mn.x + MENW / 2, mn.y + 52 + i * MENH + MENH / 2);
    redraw();
}
static void menu_projects(int act, const char *except)
{
    for (int i = 0; i < nproj; i++) if (strcmp(projs[i].name, except)) menu_add(projs[i].name, act, projs[i].name);
    menu_add("Cancel", A_CANCEL, 0);
}

static void card_menu(int k, int x, int y)
{
    if (cards[k].kind == C_ADD) {
        menu_new("Add to this project", 0, cur, -1);
        menu_add("New file", A_NEWFILE, 0);
        menu_add("New terminal", A_NEWTERM, 0);
        menu_add("Open app", A_SUB_APPS, 0);
        menu_add("Ask sia", A_ASK, 0);
        menu_add("Cancel", A_CANCEL, 0);
    } else if (cards[k].kind == C_WINDOW) {
        char t[48];
        win_title(cards[k].i, t, sizeof t);
        menu_new(t, 0, cur, cards[k].i);
        menu_add("Open", A_OPEN, 0);
        menu_add("Open beside", A_BESIDE, 0);
        menu_add("Move to project", A_SUB_MOVE, 0);
        menu_add("Close", A_CLOSEWIN, 0);
        menu_add("Cancel", A_CANCEL, 0);
    } else {
        item_t *it = &items[cards[k].i];
        menu_new(it->name, it->path, it->proj, -1);
        menu_add(it->app ? "Start" : "Open", A_OPEN, 0);
        menu_add(it->app ? "Start beside" : "Open beside", A_BESIDE, 0);
        menu_add("Move to project", A_SUB_MOVE, 0);
        if (strcmp(it->proj, INBOX)) menu_add("Move to the Inbox", A_INBOX, 0);
        menu_add("Cancel", A_CANCEL, 0);
    }
    menu_show(x, y);
}

static void proj_menu(int i, int x, int y)
{
    if (i < 0 || i == nproj - 1) {                       /* the rail's empty part, or the Inbox */
        menu_new("Projects", 0, 0, -1);
        menu_add("New project", A_NEWPROJ, 0);
        menu_add("Cancel", A_CANCEL, 0);
    } else {
        char t[48];
        strlcpy(t + strlcpy(t, "Project ", sizeof t), projs[i].name, sizeof t - 8);
        menu_new(t, 0, projs[i].name, -1);
        menu_add("Rename", A_RENAME, 0);
        menu_add("Delete the project", A_DELETE, 0);
        menu_add("New project", A_NEWPROJ, 0);
        menu_add("Cancel", A_CANCEL, 0);
    }
    menu_show(x, y);
}

static void apps_menu(void)
{
    char proj[32];
    strlcpy(proj, mn.proj, sizeof proj);
    menu_new("Open app", 0, proj, -1);
    for (int i = 0; i < napps; i++) menu_add(apps[i].name, A_RUNAPP, apps[i].path);
    menu_add("Cancel", A_CANCEL, 0);
    menu_show(mn.x, mn.y);
}

static void menu_do(int k)
{
    int act = mn.e[k].act, w = mn.win;
    char arg[160], path[160], proj[32];
    strlcpy(arg, mn.e[k].arg, sizeof arg);
    strlcpy(path, mn.path, sizeof path);
    strlcpy(proj, mn.proj, sizeof proj);
    mn.on = 0;
    switch (act) {
    case A_CANCEL: redraw(); return;
    case A_OPEN: case A_BESIDE: {
        int b = act == A_BESIDE;
        if (w >= 0) { if (b) put_beside(w); else put_main(w); settle(); return; }
        int i = item_find(path);
        if (i >= 0 && items[i].app) app_start(path, b); else open_file_beside(path, b);
        return;
    }
    case A_SUB_MOVE:
        menu_new("Move to:", path, proj, w);
        menu_projects(A_MOVE, cur);
        menu_show(mn.x, mn.y);
        return;
    case A_MOVE: case A_INBOX: {
        const char *to = act == A_INBOX ? INBOX : arg;
        if (w >= 0) {                                    /* a window: to that project (this session) */
            if (w < MAXTERM) strlcpy(terms[w].proj, to, 32);
            else if (w == W_EDIT) strlcpy(edit_proj, to, sizeof edit_proj);
            else strlcpy(sia_proj, to, sizeof sia_proj);
            say("atlas: window moved to %s\n", to);
            tidy(); settle();
            return;
        }
        int i = item_find(path);
        if (i >= 0) move_item(i, to);
        rescan(); settle();
        return;
    }
    case A_CLOSEWIN: close_win(w); return;
    case A_NEWFILE: prompt_open(P_NEWFILE, "New file: its name?", proj[0] ? proj : cur, 0); return;
    case A_NEWTERM: new_term(0); return;
    case A_SUB_APPS: strlcpy(mn.proj, proj, sizeof mn.proj); apps_menu(); return;
    case A_RUNAPP: pin_app(arg); return;
    case A_ASK: ask_sia(0); return;
    case A_RENAME: prompt_open(P_RENAME, "Rename the project to:", proj, proj); return;
    case A_DELETE: {
        char t[48];
        strlcpy(t + strlcpy(t, "Delete ", sizeof t), proj, sizeof t - 7);
        strlcpy(t + strlen(t), "? (files stay)", sizeof t - strlen(t));
        menu_new(t, 0, proj, -1);
        menu_add("Delete the project", A_DELETE_YES, 0);
        menu_add("Cancel", A_CANCEL, 0);
        menu_show(mn.x, mn.y);
        return;
    }
    case A_DELETE_YES: rename_or_delete(proj, 0); return;
    case A_NEWPROJ: prompt_open(P_NEWPROJ, "Name of the new project:", 0, 0); return;
    }
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
    if (lens_on) { for (int i = 0; i < clip_n && strlen(query) + 1 < sizeof query; i++) { size_t n = strlen(query); if (clip[i] >= 32) { query[n] = clip[i]; query[n + 1] = 0; } } search(); redraw(); return; }
    if (focus == W_SIA) { for (int i = 0; i < clip_n; i++) panel_key((uint8_t)clip[i]); redraw(); return; }
    if (focus == W_EDIT) { for (int i = 0; i < clip_n; i++) edit_key((uint8_t)clip[i]); redraw(); return; }
    if (focus < 0 || focus >= MAXTERM) return;
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
    query[0] = 0; nres = nfound = 0; lens_on = 0;
    if (k == R_FILE) { open_file(items[a].path); return; }
    if (k == R_APP) { strlcpy(cur, items[a].proj, sizeof cur); app_start(items[a].path, 0); return; }
    if (k == R_SYSAPP) { app_start(apps[a].path, 0); return; }
    char *s = q;
    while (*s == ' ') s++;
    say("atlas: ask from the lens\n");
    ask_sia(s);
}

static void focus_win(int w) { focus = w; lens_on = 0; }

static void click(int x, int y)
{
    int i = hit_at(x, y);
    if (pr.on) {                                         /* the name box: only its buttons */
        if (i >= 0 && hit[i].kind == H_PR) { if (hit[i].arg) prompt_done(); else { pr.on = 0; redraw(); } }
        return;
    }
    if (mn.on && (i < 0 || hit[i].kind != H_MENU)) { mn.on = 0; redraw(); return; }
    if (i < 0) { if (lens_on) { lens_on = 0; redraw(); } return; }
    int a = hit[i].arg, k = hit[i].kind;
    toast[0] = 0;
    if (k >= H_PANEL && k <= H_PMODE) focus_win(W_SIA);   /* the assistant's window (its buttons */
    if (panel_click(k, a)) { redraw(); return; }           /* may then put something else in focus) */
    switch (k) {
    case H_FIELD: if (first_start) setup_click(k, a); else gfield = a;
                  redraw(); return;
    case H_SETUP: setup_click(k, a); redraw(); return;
    case H_LOGIN: login(); redraw(); return;
    case H_LENS:  lens_on = 1; search(); redraw(); return;
    case H_RES:   open_result(a); return;
    case H_PROJ:  if (a >= 0 && a < nproj) open_project(projs[a].name); return;
    case H_CARD:
        if (cards[a].kind == C_ADD) { card_menu(a, x, y - 300); return; }
        if (cards[a].kind == C_WINDOW) { put_main(cards[a].i); settle(); return; }
        if (items[cards[a].i].app) { app_start(items[cards[a].i].path, 0); return; }
        open_file(items[cards[a].i].path);
        return;
    case H_WBTN: {
        int w = a / 4, b = a % 4;
        proj_t *p = here();
        if (b == WB_SWAP) { int t = p->main; p->main = p->beside; p->beside = t; say_stage(); }
        else if (b == WB_SHELF) to_shelf(w);
        else { close_win(w); return; }
        settle();
        return;
    }
    case H_WIN:   focus_win(a); sel.on = 0; redraw(); return;
    case H_ED:    focus_win(W_EDIT); edit_click(x, y); redraw(); return;
    case H_MENU:  menu_do(a); return;
    case H_BTN:
        if (a == 1) prompt_open(P_NEWPROJ, "Name of the new project:", 0, 0);
        else if (a == 2) { logout(); redraw(); }
        return;
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
    proj_t *p = here();
    /* shortcuts, wherever the keys go */
    if (c == 0 || c == 6) { lens_on = 1; mn.on = 0; search(); say("atlas: lens\n"); redraw(); return; }   /* Ctrl+Space, Ctrl+F */
    if (c == 20) { new_term(0); return; }                                                                 /* Ctrl+T */
    if (c == 22) { paste(); return; }                                                                     /* Ctrl+V */
    if (c >= KEY_C1 && c < KEY_C1 + 9) { int k = c - KEY_C1; if (k < nproj) open_project(projs[k].name); return; }   /* Ctrl+1..9 */
    if (c == 23) { if (focus >= 0) { to_shelf(focus); settle(); } return; }                               /* Ctrl+W */
    if (c == 28) {                                                                                         /* Ctrl+\ */
        if (p->beside >= 0) to_shelf(p->beside);
        else { shelf_list(); for (int k = 0; k < ncards; k++) if (cards[k].kind == C_WINDOW) { put_beside(cards[k].i); break; } }
        settle();
        return;
    }
    if (c == KEY_CTAB) {                                 /* the next window of this project, in the middle */
        int start = p->main < 0 ? -1 : p->main;
        for (int k = 1; k <= NWIN; k++) {
            int w = (start + k + NWIN) % NWIN;
            if (win_used(w) && !strcmp(win_proj(w), p->name) && w != p->beside) { put_main(w); break; }
        }
        settle();
        return;
    }
    if (mn.on && c == 27) { mn.on = 0; redraw(); return; }
    if (lens_on || focus < 0) {                          /* the Lens */
        size_t n = strlen(query);
        if (c == 27) { query[0] = 0; nres = nfound = 0; lens_on = 0; redraw(); return; }
        lens_on = 1;
        if (c == '\n') { if (nres) open_result(0); return; }
        if (c == '\t') { for (int i = 0; i < nres; i++) if (res[i].kind == R_ASK) { open_result(i); return; } return; }
        if (c == '\b') { if (n) query[n - 1] = 0; }
        else if (c >= 32 && c < 127 && n + 1 < sizeof query) { query[n] = c; query[n + 1] = 0; }
        else return;
        search();
        redraw();
        return;
    }
    if (focus == W_EDIT) { edit_key(c); if (!edit_active()) { tidy(); say_stage(); } settle(); return; }
    if (focus == W_SIA) {
        if (!panel_key(c) && c == 27) focus = -1;
        redraw();
        return;
    }
    term_t *t = &terms[focus];                           /* the focused terminal */
    int x0, x1;
    slot(focus, &x0, &x1);
    if (c == KEY_PGUP || c == KEY_PGDN) {                /* its history */
        int b = t->back + (c == KEY_PGUP ? t->rows / 2 : -t->rows / 2);
        t->back = b < 0 ? 0 : b > hist_max(t) ? hist_max(t) : b;
        say("atlas: terminal %d back %d\n", t->chan, t->back);
        redraw_box(x0, SY0, x1, SY1);
        return;
    }
    if (c < 0x100) {
        t->back = 0;
        tty_input(&t->tty, c);
        redraw_box(x0, SY0, x1, SY1);
    }
}

/* Which character cell of terminal t is under screen point (x, y). */
static void cell_at(int ti, int x, int y, uint64_t *l, int *c)
{
    term_t *t = &terms[ti];
    int x0, x1;
    slot(ti, &x0, &x1);
    int r = (y - SY0 - TBAR - 8) / KH, k = (x - x0 - 14) / KW;
    r = r < 0 ? 0 : r >= t->rows ? t->rows - 1 : r;
    k = k < 0 ? 0 : k >= t->cols ? t->cols - 1 : k;
    *l = t->top - t->back + r;
    *c = k;
}

/* A card let go: on the stage (left half: main, right half: beside), on a
 * project of the rail (it moves there), or anywhere else (nothing). */
static void drop(void)
{
    int k = drag_arg, x = ptr_x, y = ptr_y;
    if (x >= SX0 && x < SX1 && y >= SY0 && y < SY1 && cards[k].kind != C_ADD) {
        int beside = x >= (SX0 + SX1) / 2;
        say("atlas: card dropped on the stage (%s)\n", beside ? "beside" : "main");
        if (cards[k].kind == C_WINDOW) { if (beside) put_beside(cards[k].i); else put_main(cards[k].i); settle(); return; }
        item_t *it = &items[cards[k].i];
        if (it->app) app_start(it->path, beside); else open_file_beside(it->path, beside);
        return;
    }
    int i = nhit;                                        /* (the dragged card itself is on top: look under it) */
    while (--i >= 0 && !(hit[i].kind == H_PROJ && hit[i].arg >= 0 && x >= hit[i].x0 && x < hit[i].x1 && y >= hit[i].y0 && y < hit[i].y1));
    if (i >= 0 && cards[k].kind != C_ADD) {
        const char *to = projs[hit[i].arg].name;
        if (cards[k].kind == C_WINDOW) {
            int w = cards[k].i;
            if (w < MAXTERM) strlcpy(terms[w].proj, to, 32);
            else if (w == W_EDIT) strlcpy(edit_proj, to, sizeof edit_proj);
            else strlcpy(sia_proj, to, sizeof sia_proj);
            say("atlas: window moved to %s\n", to);
        } else {
            move_item(cards[k].i, to);
            rescan();
        }
        settle();
        return;
    }
    say("atlas: card dropped nowhere\n");
    redraw();
}

static void press(int x, int y)
{
    press_x = x; press_y = y;
    drag = D_NONE; drag_on = 0;
    int i = hit_at(x, y);
    if (mode != SESSION || i < 0 || mn.on || pr.on) return;
    if (hit[i].kind == H_CARD && cards[hit[i].arg].kind != C_ADD) { drag = D_CARD; drag_arg = hit[i].arg; }
    else if (hit[i].kind == H_WIN && hit[i].arg < MAXTERM && y > SY0 + TBAR) { drag = D_SEL; drag_arg = hit[i].arg; }
}

static void mouse(const con_event_t *e)
{
    int x = (int)((int64_t)e->x * SW >> 16), y = (int)((int64_t)e->y * SH >> 16);
    int b = e->buttons, was = buttons, ox = ptr_x, oy = ptr_y;
    ptr_x = x; ptr_y = y;
    buttons = b;
    if (e->code && mode == SESSION) {                    /* the wheel */
        int16_t w = (int16_t)e->code;
        int i = hit_at(x, y);
        if (y >= SHY && x >= SX0) { shelf_off -= w; redraw(); return; }   /* the shelf */
        if (i >= 0 && (hit[i].kind == H_WIN || hit[i].kind == H_ED || hit[i].kind == H_PANEL)) {
            int a = hit[i].kind == H_ED ? W_EDIT : hit[i].kind == H_PANEL ? W_SIA : hit[i].arg;
            if (a == W_EDIT) edit_wheel(w);
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
        if (!drag_on && drag == D_SEL) {                 /* the selection starts where the button went down */
            cell_at(drag_arg, press_x, press_y, &sel.al, &sel.ac);
            sel.t = drag_arg; sel.on = 1;
        }
        drag_on = 1;
        if (drag == D_SEL) cell_at(drag_arg, x, y, &sel.bl, &sel.bc);
        redraw();
        return;
    }
    if (!(b & MB_LEFT) && (was & MB_LEFT)) {
        int d = drag, on = drag_on;
        drag = D_NONE; drag_on = 0;
        if (!on) click(x, y);
        else if (d == D_CARD) { drag = D_CARD; drop(); drag = D_NONE; }
        else if (d == D_SEL) { if (sel.al != sel.bl || sel.ac != sel.bc) copy_sel(); else sel.on = 0; redraw(); }
        return;
    }
    if ((b & MB_MIDDLE) && !(was & MB_MIDDLE) && mode == SESSION) { paste(); return; }
    if (!(b & MB_RIGHT) && (was & MB_RIGHT) && mode == SESSION && !pr.on) {   /* right click: a menu */
        int i = hit_at(x, y), k = i < 0 ? 0 : hit[i].kind;
        if (k == H_MENU) return;
        mn.on = 0;
        if (k == H_CARD) { card_menu(hit[i].arg, x, y - 200); return; }
        if (k == H_PROJ) { proj_menu(hit[i].arg, x, y); return; }
        redraw();
        return;
    }
    dl_pointer(x, y);                                   /* only the pointer moved: */
    present(ox, oy, ox + 12, oy + 19);                  /* where it was, where it is */
    present(x, y, x + 12, y + 19);
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
    screen_t scr;
    if (screen_init(&scr)) {                             /* no screen: the terminal does the first start */
        say("atlas: no screen found: no desktop\n");
        no_desktop();
        return 0;
    }
    draw_init(&scr);
    geometry();
    ptr_x = SW / 2; ptr_y = SH / 2;
    say("atlas: screen: %s, %d x %d\n", scr.name, SW, SH);
    long con = 0;
    msg_t hi = { .w = { CON_HASINPUT } };
    if (call_named(&con, "console", &hi, 1) >= 0 && (long)hi.w[0] == 0) {
        /* A screen but no keyboard or pointer: say so on the screen, and
         * leave the first start and the logins to the serial console. */
        static const char *const l[] = { "SIEOS", "This screen works, but there is no keyboard or mouse yet.",
                                         "Use the serial console: log in there." };
        dl_reset();
        dl_rect(0, 0, SW, SH, 0, C_BG);
        for (int i = 0; i < 3; i++)
            dl_text(200, 380 + i * 70 + (i ? 60 : 0), l[i], strlen(l[i]), S(i ? 4 : 10), i ? C_TXT : C_ACC, 0, 0, SW, SH);
        present(0, 0, SW, SH);
        say("atlas: no keyboard or pointer: no desktop (the serial console is the way in)\n");
        no_desktop();
        return 0;
    }
    rtc_read();
    port = port_create("atlas");
    if (port < 0) return 1;
    sys_child_port(port);                                /* our shells' ends */
    greeter_check();
    redraw();
    say("atlas: greeter (%dx%d, full frame %lu us)\n", SW, SH, (unsigned long)frame_us);
    thread_start(input_thread, 0, 16384);
    thread_start(clock_thread, 0, 4096);
    panel_init(port);

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
            while ((pid = sys_wait(-1, &st, WAIT_NOHANG)) > 0)
                for (int t = 0; t < MAXTERM; t++)
                    if (terms[t].used && terms[t].pid == pid) { close_term(&terms[t]); closed = 1; }
            if (closed && mode == SESSION) { rescan(); say_stage(); settle(); }
            continue;
        }
        if (m.pid == self && m.w[0] == ATLAS_INPUT) {    /* (our own threads only: nobody */
            int n = m.rlen / sizeof(con_event_t);
            reply_val(from, 0);
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
        if (m.pid == self && m.w[0] == ATLAS_TICK) {     /* a new minute: the clock's corner */
            reply_val(from, 0);
            if (mode == SESSION) redraw_box(SW - 600, 10, SW, 70);
            continue;
        }
        /* a terminal's shell: CON_WRITE / CON_READ / CON_ECHO, its window in w[3] */
        term_t *t = 0;
        for (int i = 0; i < MAXTERM; i++) if (terms[i].used && terms[i].chan == (int)m.w[3]) t = &terms[i];
        if (!t || (m.uid != uid && m.uid != 0)) { reply_val(from, -EIO); continue; }
        switch (m.w[0]) {
        case CON_WRITE: {
            term_write(t, buf, m.rlen);
            reply_val(from, m.rlen);
            int x0, x1;
            slot(t - terms, &x0, &x1);
            if (mode == SESSION && x1 > x0 && !strcmp(t->proj, cur)) redraw_box(x0, SY0, x1, SY1);
            break;
        }
        case CON_READ:                                   /* the shell wants the next line: */
            if (t->tty.lines != t->seen && mode == SESSION) {   /* it ran a command: files may */
                t->seen = t->tty.lines;                  /* have changed (new projects...): look again */
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
        case CON_ECHO: t->tty.echo = m.w[1]; reply_val(from, 0); break;
        default: reply_val(from, -ENOSYS);
        }
    }
}

/* ---- For the assistant: the user's files and their projects. */
int atlas_files(char *out, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < nitems && n + strlen(items[i].path) + 48 < cap; i++) {
        n += strlcpy(out + n, items[i].path, cap - n);
        n += strlcpy(out + n, " (project ", cap - n);
        n += strlcpy(out + n, items[i].proj, cap - n);
        n += strlcpy(out + n, ")\n", cap - n);
    }
    out[n] = 0;
    return n;
}
