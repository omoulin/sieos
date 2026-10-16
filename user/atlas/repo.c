/*
 * repo.c - The Git window (docs/desktop.md, docs/git.md): a repository's
 * branch, its changed files (staged and not), the diff of the one chosen,
 * its history; Stage, Unstage, Commit (with a message), Pull, Push.
 *
 *   main   /home/alice/proj                       [Refresh] [Pull] [Push]
 *   CHANGES                   | diff --git a/a.txt b/a.txt
 *    M  b.txt                 | @@ -1 +1,2 @@
 *     M a.txt   (chosen)      |  first line
 *    ?? c.txt                 | +more
 *   [Stage] [Unstage]         |
 *   [message...    ] [Commit] |
 *   HISTORY                   |
 *   3f2a9c1 First commit      |
 *
 * It does not link git: it runs /bin/git (as the logged-in user, its
 * console a channel of atlas, REPO_CHAN) and reads what it prints: status
 * -s -b, log --oneline, diff; one command at a time, the next when the
 * last ends (atlas tells repo_ended). The repository: the Files window's
 * folder if it is in one, else the last one opened (~/.gitrepo), else the
 * first folder of the home folder that holds one.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"

#define TOP 48
#define ROW 32
#define DROW 20                          /* a line of the diff or the history (small text) */
#define MAXC 64
enum { J_STATUS = 1, J_LOG, J_DIFF, J_DO };
enum { B_REFRESH = -2, B_PULL = -3, B_PUSH = -4, B_STAGE = -5, B_UNSTAGE = -6, B_COMMIT = -7, B_FIELD = -8 };

static char dir[160], branch[64], msg[100], note[160];
static struct { char xy[3], path[120]; } chg[MAXC];
static int nchg, sel = -1, field, dscroll, nerr;
static char *logt, *difft;               /* the history's and the right pane's text */
static struct { int job; char args[200]; } q[6];
static int nq, job;                      /* the commands waiting, the one running (its job) */
static long pid;
static char *ob;                         /* what the running command printed */
static size_t on, ocap;
static char what[24];                    /* (the action running: "add", "commit", "push"...) */

static int in_repo(const char *d, char *top)    /* d or a folder above it holds .git -> 1, top: that folder */
{
    char p[200];
    siefs_stat_t st;
    strlcpy(top, d, 160);
    for (;;) {
        strlcpy(p, top, sizeof p);
        strlcpy(p + strlen(p), strcmp(top, "/") ? "/.git" : ".git", 8);
        if (!fs_stat(p, &st, 0) && (st.mode & SIEFS_IFMT) == SIEFS_IFDIR) return 1;
        char *s = top + strlen(top);
        while (s > top && *s != '/') s--;
        if (s <= top) return 0;
        *s = 0;
    }
}

static void push_job(int j, const char *a)
{
    if (nq == 6) return;
    q[nq].job = j;
    strlcpy(q[nq].args, a, sizeof q[0].args);
    nq++;
}
static void next(void)                   /* start the next command, if none runs */
{
    if (pid > 0 || !nq) return;
    char args[256];
    int j = q[0].job;
    size_t n = strlcpy(args, "git -C ", sizeof args);
    n += strlcpy(args + n, dir, sizeof args - n);
    args[n++] = ' ';
    strlcpy(args + n, q[0].args, sizeof args - n);
    memmove(q, q + 1, sizeof q[0] * (size_t)--nq);
    mk_spawn_t o = { .flags = SPAWN_CONSOLE | SPAWN_GROUPS };
    int uid, gid;
    atlas_user(&uid, &gid, &o.groups);
    strlcpy(o.console, "atlas#", sizeof o.console);
    num(o.console + 6, REPO_CHAN);
    on = 0;
    pid = spawn_file("/bin/git", args, uid, gid, &o);
    if (pid < 0) { strlcpy(note, "Cannot start /bin/git.", sizeof note); nerr = 1; pid = 0; nq = 0; return; }
    job = j;
}
static void refresh(void)
{
    push_job(J_STATUS, "status -s -b");
    push_job(J_LOG, "log --oneline -n 40");
    next();
}
static void show_diff(void)              /* the chosen file's diff (staged, if only that) */
{
    if (sel < 0 || sel >= nchg) return;
    char a[160];
    size_t n = strlcpy(a, chg[sel].xy[1] == ' ' ? "diff --cached " : "diff ", sizeof a);
    strlcpy(a + n, chg[sel].path, sizeof a - n);
    dscroll = 0;
    if (chg[sel].xy[0] == '?') {
        free(difft);
        difft = malloc(80);
        if (difft) strlcpy(difft, "Not followed by git yet: Stage adds it.\n", 80);
        return;
    }
    push_job(J_DIFF, a);
    next();
}

void repo_open(void)
{
    char top[160], *last = 0, p[200];
    const char *home = atlas_home();
    int found = files_dir()[0] && in_repo(files_dir(), top);
    strlcpy(p, home, sizeof p);
    strlcpy(p + strlen(p), "/.gitrepo", sizeof p - strlen(p));
    if (!found && (last = file_get(p, 0))) { char *e = strchr(last, '\n'); if (e) *e = 0; found = in_repo(last, top); }
    free(last);
    long h = found ? -1 : fs_open(home, FS_RDONLY | FS_DIRECTORY, 0);
    if (h >= 0) {                        /* the first folder at home that is a repository */
        static char buf[4096];
        char name[256];
        uint64_t cs = 0, ino;
        long n;
        int type;
        while (!found && (n = fs_readdir(h, &cs, buf, sizeof buf)) > 0)
            for (const char *e = buf; !found && fs_dirent(&e, buf + n, &ino, &type, name); ) {
                if (type != 4 || name[0] == '.') continue;
                char d[160];
                strlcpy(d, home, sizeof d);
                strlcpy(d + strlen(d), "/", sizeof d - strlen(d));
                strlcpy(d + strlen(d), name, sizeof d - strlen(d));
                char t2[160];
                if (in_repo(d, t2) && !strcmp(t2, d)) { found = 1; strlcpy(top, d, sizeof top); }
            }
        fs_close(h);
    }
    nchg = 0; sel = -1; branch[0] = 0; field = 0;
    free(logt); free(difft); logt = difft = 0;
    if (!found) { dir[0] = 0; strlcpy(note, "No repository: open its folder in Files, then the Git app.", sizeof note); nerr = 1; return; }
    strlcpy(dir, top, sizeof dir);
    long f = fs_open(p, FS_WRONLY | FS_CREAT | FS_TRUNC, 0644);
    if (f >= 0) { fs_write(f, 0, dir, strlen(dir)); fs_close(f); }
    note[0] = 0; nerr = 0;
    say("atlas: git: open %s\n", dir);
    refresh();
}
void repo_close(void) { nq = 0; dir[0] = 0; }
const char *repo_dir(void) { return dir; }

/* atlas's console channel REPO_CHAN: what git prints (a question gets no answer). */
int repo_con(long from, msg_t *m, const char *data)
{
    if ((int)m->w[3] != REPO_CHAN) return 0;
    if (m->w[0] == CON_WRITE) {
        if (on + m->rlen + 1 > ocap && on < (256 << 10)) { ocap = (on + m->rlen + 1) * 2; ob = realloc(ob, ocap); }
        if (ob && on + m->rlen + 1 <= ocap) { memcpy(ob + on, data, m->rlen); on += m->rlen; }
        reply_val(from, m->rlen);
    } else reply_val(from, m->w[0] == CON_ECHO ? 0 : -EIO);
    return 1;
}

static char *take(void)                  /* the output, as a string of its own */
{
    char *t = malloc(on + 1);
    if (t) { if (on) memcpy(t, ob, on); t[on] = 0; }
    return t;
}
/* /bin/git ended (atlas reaped pid): what it printed is used -> 1 if it was ours. */
int repo_ended(long p, int status)
{
    if (p != pid || !pid) return 0;
    pid = 0;
    if (!dir[0]) return 1;               /* (the window was closed meanwhile) */
    if (ob) ob[on] = 0;
    if (job == J_STATUS) {
        nchg = 0;
        for (char *s = ob, *e; s && *s; s = e + 1) {
            if (!(e = strchr(s, '\n'))) break;
            *e = 0;
            if (!memcmp(s, "## ", 3)) strlcpy(branch, s + 3, sizeof branch);
            else if (strlen(s) > 3 && nchg < MAXC) {
                memcpy(chg[nchg].xy, s, 2); chg[nchg].xy[2] = 0;
                strlcpy(chg[nchg].path, s + 3, sizeof chg[0].path);
                nchg++;
            }
        }
        if (status) { nerr = 1; strlcpy(note, ob && on ? ob : "git status failed", sizeof note); }
        if (sel >= nchg) sel = nchg - 1;
        say("atlas: git: %s: branch %s, %d changes\n", dir, branch, nchg);
        if (sel >= 0) show_diff();
    } else if (job == J_LOG) {
        free(logt); logt = take();
        int n = 0;
        for (char *s = logt; s && *s; s++) n += *s == '\n';
        say("atlas: git: log %d commits\n", n);
    } else if (job == J_DIFF) {
        free(difft); difft = take();
        int n = 0;
        for (char *s = difft; s && *s; s++) n += *s == '\n';
        say("atlas: git: diff %s (%d lines)\n", sel >= 0 ? chg[sel].path : "", n);
    } else {                             /* an action: its output in the right pane, its last line below */
        free(difft); difft = take(); dscroll = 0;
        const char *l = 0;
        size_t ll = 0;
        for (const char *s = difft; s && *s; ) {     /* its last line that says something */
            const char *e = strchr(s, '\n');
            size_t len = e ? (size_t)(e - s) : strlen(s);
            if (len) { l = s; ll = len; }
            s += len + (e != 0);
        }
        if (l) { ll = ll < sizeof note - 1 ? ll : sizeof note - 1; memcpy(note, l, ll); note[ll] = 0; }
        else strlcpy(note, status ? "failed" : "done", sizeof note);
        nerr = status != 0;
        say("atlas: git: %s -> %d\n", what, status);
        refresh();
    }
    next();
    return 1;
}

static void act(int b)
{
    if (!dir[0]) { repo_open(); return; }
    char a[200];
    const char *p = sel >= 0 && sel < nchg ? chg[sel].path : 0;
    size_t n;
    switch (b) {
    case B_REFRESH: refresh(); return;
    case B_PULL: strlcpy(what, "pull", sizeof what); push_job(J_DO, "pull"); break;
    case B_PUSH: strlcpy(what, "push", sizeof what); push_job(J_DO, "push"); break;
    case B_STAGE: case B_UNSTAGE:
        if (!p) { strlcpy(note, "Choose a file first.", sizeof note); nerr = 1; return; }
        strlcpy(what, b == B_STAGE ? "add" : "reset", sizeof what);
        n = strlcpy(a, b == B_STAGE ? "add " : "reset ", sizeof a);
        strlcpy(a + n, p, sizeof a - n);
        push_job(J_DO, a);
        break;
    case B_COMMIT:
        if (!msg[0]) { strlcpy(note, "Write a message first.", sizeof note); nerr = 1; field = 1; return; }
        strlcpy(what, "commit", sizeof what);
        n = strlcpy(a, "commit -m ", sizeof a);
        strlcpy(a + n, msg, sizeof a - n);
        push_job(J_DO, a);
        msg[0] = 0; field = 0;
        break;
    default: return;
    }
    strlcpy(note, "working...", sizeof note); nerr = 0;
    next();
}

static void lines(const char *t, int x0, int y, int x1, int y1, int skip, int diff)   /* text, a line per DROW */
{
    for (const char *s = t; s && *s && y + DROW <= y1; ) {
        const char *e = strchr(s, '\n');
        int n = e ? (int)(e - s) : (int)strlen(s);
        if (skip > 0) skip--;
        else {
            uint32_t c = !diff ? C_TXT : *s == '+' && s[1] != '+' ? C_ACCT : *s == '-' && s[1] != '-' ? C_ERR :
                         *s == '@' ? C_ACC : *s == ' ' ? C_TERMFG : C_DIM;
            dl_text(x0, y, s, n, SS, c, x0, y, x1, y + DROW);
            y += DROW;
        }
        s = e ? e + 1 : s + n;
    }
}

void repo_draw(int x0, int y0, int x1, int y1, int focused)
{
    int ty = y0 + TOP;
    hitbox(x0, ty, x1, y1, H_REPO, -1);
    dl_text(x0 + 16, ty + 10, branch[0] ? branch : "-", strlen(branch[0] ? branch : "-"), S(2), C_ACCT, x0, ty, x1 - 400, ty + 42);
    int bx = x0 + 16 + tw(strlen(branch[0] ? branch : "-"), 2) + 20;
    dl_text(bx, ty + 13, dir, strlen(dir), SS, C_DIM, x0, ty, x1 - 400, ty + 42);
    int px = x1 - 16 - (tw(7, 2) + 28) - (tw(4, 2) + 28) - (tw(4, 2) + 28) - 24;
    static const char *const bn[3] = { "Refresh", "Pull", "Push" };
    for (int i = 0; i < 3; i++) {
        if (atlas_debug) say("atlas: dbg git %s at %d %d\n", bn[i], px + 40, ty + 21);
        px = pill(px, ty, bn[i], 0, H_REPO, B_REFRESH - i) + 12;
    }
    int by = ty + 58, lx1 = x0 + (x1 - x0) * 40 / 100, rx0 = lx1 + 12, sy = y1 - 36;
    /* the right pane: the diff (or an action's output) */
    dl_rect(rx0, by, x1 - 10, sy - 6, 8, C_TERM);
    if (difft) lines(difft, rx0 + 12, by + 8, x1 - 20, sy - 10, dscroll, 1);
    else label(rx0 + 16, by + 12, nchg ? "Choose a file to see its changes." : "Nothing changed.", 2, C_DIM);
    /* the left: changes, Stage / Unstage, the message and Commit, the history */
    dl_text(x0 + 16, by, "CHANGES", 7, SS, C_DIM, 0, 0, SW, SH);
    int cy = by + 26, ch = (sy - by) * 45 / 100, rows = (ch - 26) / ROW;
    for (int i = 0; i < nchg && i < rows; i++, cy += ROW) {
        if (i == sel) dl_rect(x0 + 8, cy, lx1, cy + ROW - 2, 6, C_SEL);
        uint32_t c = chg[i].xy[0] == '?' ? C_DIM : chg[i].xy[0] != ' ' ? C_ACCT : C_TXT;
        dl_text(x0 + 16, cy + 6, chg[i].xy, 2, SS, c, 0, 0, SW, SH);
        dl_text(x0 + 16 + text_w(3, SS), cy + 4, chg[i].path, strlen(chg[i].path), S(2), C_TXT, x0, cy, lx1 - 4, cy + ROW);
        hitbox(x0 + 8, cy, lx1, cy + ROW - 2, H_REPO, i);
        if (atlas_debug) say("atlas: dbg gitfile %s at %d %d\n", chg[i].path, x0 + 120, cy + ROW / 2);
    }
    if (!nchg && dir[0]) label(x0 + 16, cy + 4, "Working tree clean", 2, C_DIM);
    cy = by + ch;
    int sx = pill(x0 + 16, cy, "Stage", 0, H_REPO, B_STAGE) + 12;
    if (atlas_debug) say("atlas: dbg git Stage at %d %d\n", x0 + 50, cy + 21);
    pill(sx, cy, "Unstage", 0, H_REPO, B_UNSTAGE);
    if (atlas_debug) say("atlas: dbg git Unstage at %d %d\n", sx + 50, cy + 21);
    cy += 54;
    int cw = tw(6, 2) + 28, fx1 = lx1 - cw - 12;
    dl_rect(x0 + 16, cy, fx1, cy + 42, 10, C_FIELD);
    dl_frame(x0 + 16, cy, fx1, cy + 42, 10, field && focused ? 2 : 1, field && focused ? C_ACC : C_LINE);
    int ml = strlen(msg), chars = (fx1 - x0 - 48) / KW, from = ml > chars ? ml - chars : 0;
    if (ml) dl_text(x0 + 30, cy + 10, msg + from, ml - from, S(2), C_TXT, x0 + 16, cy, fx1 - 8, cy + 42);
    else dl_text(x0 + 30, cy + 10, "Commit message", 14, S(2), C_DIM, x0 + 16, cy, fx1 - 8, cy + 42);
    if (field && focused) { int w = tw(ml - from, 2); dl_rect(x0 + 32 + w, cy + 9, x0 + 42 + w, cy + 33, 0, C_ACC); }
    hitbox(x0 + 16, cy, fx1, cy + 42, H_REPO, B_FIELD);
    if (atlas_debug) say("atlas: dbg git field at %d %d\n", (x0 + fx1) / 2, cy + 21);
    pill(fx1 + 12, cy, "Commit", 1, H_REPO, B_COMMIT);
    if (atlas_debug) say("atlas: dbg git Commit at %d %d\n", fx1 + 12 + cw / 2, cy + 21);
    cy += 58;
    dl_text(x0 + 16, cy, "HISTORY", 7, SS, C_DIM, 0, 0, SW, SH);
    if (logt) lines(logt, x0 + 16, cy + 24, lx1, sy - 6, 0, 0);
    /* what the last action said */
    if (note[0]) dl_text(x0 + 16, sy + 6, note, strlen(note), SS, nerr ? C_ERR : C_DIM, x0, sy, x1 - 16, y1);
}

int repo_click(int kind, int arg)
{
    if (kind != H_REPO) return 0;
    if (arg >= 0 && arg < nchg) { sel = arg; field = 0; show_diff(); return 1; }
    if (arg == B_FIELD) { field = 1; return 1; }
    if (arg < -1) act(arg);
    return 1;
}

int repo_key(int c)
{
    if (field) {
        size_t n = strlen(msg);
        if (c == '\n') act(B_COMMIT);
        else if (c == 27) field = 0;
        else if (c == '\b') { if (n) msg[n - 1] = 0; }
        else if (c >= 32 && c < 127 && n + 1 < sizeof msg) { msg[n] = (char)c; msg[n + 1] = 0; }
        else return 0;
        return 1;
    }
    if (c == KEY_DOWN && sel + 1 < nchg) sel++;
    else if (c == KEY_UP && sel > 0) sel--;
    else if (c == KEY_PGDN) { dscroll += 20; return 1; }
    else if (c == KEY_PGUP) { dscroll = dscroll > 20 ? dscroll - 20 : 0; return 1; }
    else return 0;
    show_diff();
    return 1;
}

void repo_wheel(int w) { dscroll += w * 3; if (dscroll < 0) dscroll = 0; }
