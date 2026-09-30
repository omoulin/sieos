/*
 * facet-sipm - SiPM, the SIEOS Package Manager: pkg in a window.
 *
 * The packages the repositories offer and the installed ones (pkg query),
 * filtered (all, installed, with an update) and searched; the selected one's
 * details, and Install, Upgrade or Remove; Refresh (pkg update) and Upgrade
 * all.  The work is pkg's (set-user-ID root): SiPM runs it, shows what it
 * prints, and reloads the lists when it is done.  When the session is not
 * root's, changes ask for root's password first, handed to pkg on its
 * standard input (-P), as the installer does with sieinstall.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "common.h"
#include <errno.h>
#include <strings.h>
#include <signal.h>
#include <sys/wait.h>

#define PAD    16
#define ROW    30
#define BTN_W  104
#define BTN_H  26
#define TOP    108                               /* the list's top */
#define BOTTOM 88                                /* the status area under the list: 3 lines, the result */
#define MAXPK  512
#define NLOG   3

enum { F_ALL, F_INSTALLED, F_UPDATES };
enum { S_IDLE, S_PASS, S_RUN };

struct pk {
    char name[64], avail[64], inst[64], deps[256], summary[160];
    unsigned long size;
};

static struct {
    struct pk pk[MAXPK];
    int npk;
    int view[MAXPK], nview;                      /* the filtered list: indexes into pk */
    int filter, first, sel;                      /* sel: an index into pk, -1 */
    char search[48];
    bool search_focus;
    char repo[160];
    int state;
    bool is_root;
    char pass[64];
    char op[16], opname[64];                     /* what runs, or waits for the password */
    pid_t pid;
    int out;
    char line[256];
    int linelen;
    char log[NLOG][160];
    int nlog;
    char status[200];
    bool status_bad;
} m = { .sel = -1, .out = -1 };

/* ---------------------------------------------------------------- data */

static bool contains_nocase(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n))
            return true;
    return !n;
}

static bool has_update(const struct pk *p)
{
    return strcmp(p->inst, "-") && strcmp(p->avail, "-") && strcmp(p->avail, p->inst);
}

static void filter(void)
{
    m.nview = 0;
    for (int i = 0; i < m.npk; i++) {
        const struct pk *p = &m.pk[i];
        if (m.filter == F_INSTALLED && !strcmp(p->inst, "-"))
            continue;
        if (m.filter == F_UPDATES && !has_update(p))
            continue;
        if (m.search[0] && !contains_nocase(p->name, m.search) && !contains_nocase(p->summary, m.search))
            continue;
        m.view[m.nview++] = i;
    }
    if (m.first > m.nview)
        m.first = 0;
}

static void load(void)
{
    char keep[64];
    snprintf(keep, sizeof(keep), "%s", m.sel >= 0 ? m.pk[m.sel].name : "");
    m.npk = 0;
    m.sel = -1;
    FILE *f = popen("/bin/pkg query 2>/dev/null", "r");
    if (f) {
        char l[1024];
        while (fgets(l, sizeof(l), f) && m.npk < MAXPK) {
            l[strcspn(l, "\n")] = 0;
            char *fld[6];
            int n = 0;
            for (char *s = l; n < 6; n++) {
                fld[n] = s;
                char *t = strchr(s, '\t');
                if (!t) {
                    n++;
                    break;
                }
                *t = 0;
                s = t + 1;
            }
            if (n < 6)
                continue;
            struct pk *p = &m.pk[m.npk++];
            snprintf(p->name, sizeof(p->name), "%s", fld[0]);
            snprintf(p->avail, sizeof(p->avail), "%s", fld[1]);
            snprintf(p->inst, sizeof(p->inst), "%s", fld[2]);
            p->size = strtoul(fld[3], NULL, 10);
            snprintf(p->deps, sizeof(p->deps), "%s", strcmp(fld[4], "-") ? fld[4] : "");
            snprintf(p->summary, sizeof(p->summary), "%s", fld[5]);
        }
        pclose(f);
    }
    /* sorted by name */
    for (int i = 0; i < m.npk; i++)
        for (int j = i + 1; j < m.npk; j++)
            if (strcmp(m.pk[i].name, m.pk[j].name) > 0) {
                struct pk t = m.pk[i];
                m.pk[i] = m.pk[j];
                m.pk[j] = t;
            }
    for (int i = 0; i < m.npk; i++)
        if (!strcmp(m.pk[i].name, keep))
            m.sel = i;
    filter();
}

static void load_repo(void)
{
    snprintf(m.repo, sizeof(m.repo), "no repository in /etc/pkg/repos");
    FILE *f = fopen("/etc/pkg/repos", "r");
    if (!f)
        return;
    char l[256];
    while (fgets(l, sizeof(l), f)) {
        l[strcspn(l, "\r\n")] = 0;
        char *s = l;
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s && *s != '#') {
            snprintf(m.repo, sizeof(m.repo), "%s", s);
            break;
        }
    }
    fclose(f);
}

/* ---------------------------------------------------------------- layout */

static struct rect list_rect(struct rect c)
{
    int w = (c.w - 2 * PAD) * 11 / 20;
    return rect_make(c.x + PAD, c.y + TOP, w, c.h - TOP - BOTTOM - PAD);
}
static struct rect detail_rect(struct rect c)
{
    struct rect l = list_rect(c);
    return rect_make(l.x + l.w + 12, l.y, c.x + c.w - PAD - (l.x + l.w + 12), l.h);
}
static int visible_rows(struct rect c) { return list_rect(c).h / ROW; }
static struct rect row_rect(struct rect c, int i)
{
    struct rect l = list_rect(c);
    return rect_make(l.x + 1, l.y + 1 + i * ROW, l.w - SB_W - 2, ROW);
}
static struct rect tab_rect(struct rect c, int i) { return rect_make(c.x + PAD + i * 92, c.y + 72, 88, BTN_H); }
static struct rect search_rect(struct rect c) { return rect_make(c.x + PAD + 3 * 92 + 8, c.y + 72, 200, BTN_H); }
static struct rect tool_rect(struct rect c, int i)     /* 0: Refresh (rightmost), 1: Upgrade all */
{
    return rect_make(c.x + c.w - PAD - (i + 1) * BTN_W - i * 8, c.y + 72, BTN_W, BTN_H);
}
static struct rect action_rect(struct rect c, int i)   /* the details' buttons, bottom right */
{
    struct rect d = detail_rect(c);
    return rect_make(d.x + d.w - 12 - (i + 1) * BTN_W - i * 8, d.y + d.h - 12 - BTN_H, BTN_W, BTN_H);
}
static struct rect pass_rect(struct rect c) { return rect_make(c.x + PAD + 130, c.y + c.h - BOTTOM + 4, 200, 24); }
static struct rect pass_btn(struct rect c, int i)      /* 0: Cancel (rightmost), 1: OK */
{
    return rect_make(c.x + c.w - PAD - (i + 1) * BTN_W - i * 8, c.y + c.h - BOTTOM + 3, BTN_W, BTN_H);
}

/* The selected package's actions. */
static int actions(const char **labels)
{
    if (m.sel < 0 || m.state != S_IDLE)
        return 0;
    const struct pk *p = &m.pk[m.sel];
    int n = 0;
    if (!strcmp(p->inst, "-")) {
        if (strcmp(p->avail, "-"))
            labels[n++] = "Install";
    } else {
        labels[n++] = "Remove";
        if (has_update(p))
            labels[n++] = "Upgrade";
    }
    return n;
}

/* ---------------------------------------------------------------- drawing */

static void fit_text(struct surface *s, int x, int y, int w, const char *t, color_t col, bool bold)
{
    char b[200];
    snprintf(b, sizeof(b), "%s", t);
    size_t n = strlen(b);
    while (n > 1 && text_width(b) > w) {         /* shorten, with an ellipsis */
        b[--n] = 0;
        if (n > 3)
            memcpy(b + n - 3, "...", 3);
    }
    if (bold)
        gfx_text_bold(s, x, y, b, col);
    else
        gfx_text(s, x, y, b, col);
}

static int text_wrapped(struct surface *s, int x, int y, int w, const char *t, color_t col)
{
    char line[200];
    int n = 0;
    const char *p = t;
    while (*p) {
        int take = 0, last_space = -1;
        while (p[take] && take < (int)sizeof(line) - 1) {
            memcpy(line, p, take + 1);
            line[take + 1] = 0;
            if (text_width(line) > w)
                break;
            if (p[take] == ' ')
                last_space = take;
            take++;
        }
        if (p[take] && last_space > 0)
            take = last_space + 1;
        memcpy(line, p, take);
        line[take] = 0;
        gfx_text(s, x, y + n * (FONT_H + 3), line, col);
        n++;
        p += take;
        if (!take)
            break;
    }
    return n * (FONT_H + 3);
}

static void field(struct surface *s, struct rect f, const char *text, bool focus, const char *hint)
{
    gfx_fill(s, f.x, f.y, f.w, f.h, C_CONTENT);
    gfx_bevel(s, f.x, f.y, f.w, f.h, 1, false, focus ? C_ACCENT : C_LINE, C_FACE_DARK);
    int tw = 0;
    if (text[0])
        tw = gfx_text(s, f.x + 6, f.y + (f.h - FONT_H) / 2, text, C_TEXT);
    else if (hint && !focus)
        gfx_text(s, f.x + 6, f.y + (f.h - FONT_H) / 2, hint, C_DIM);
    if (focus)
        gfx_vline(s, f.x + 7 + tw, f.y + (f.h - FONT_H) / 2, FONT_H, C_ACCENT);
}

static void draw(struct fct_view *w, struct surface *s, struct rect c)
{
    gfx_fill(s, c.x, c.y, c.w, c.h, C_FACE);
    char b[256];

    /* header */
    icon_draw(s, ICON_PROGRAM, c.x + PAD, c.y + 12, 36);
    gfx_text_bold(s, c.x + PAD + 48, c.y + 14, "SiPM", C_TEXT);
    snprintf(b, sizeof(b), "SIEOS Package Manager  -  %s", m.repo);
    fit_text(s, c.x + PAD + 48, c.y + 34, c.w - 2 * PAD - 48, b, C_DIM, false);
    gfx_hline(s, c.x + PAD, c.y + 60, c.w - 2 * PAD, C_LINE);

    /* toolbar */
    static const char *tabs[] = { "All", "Installed", "Updates" };
    int updates = 0;
    for (int i = 0; i < m.npk; i++)
        updates += has_update(&m.pk[i]);
    for (int i = 0; i < 3; i++) {
        if (i == F_UPDATES && updates)
            snprintf(b, sizeof(b), "Updates (%d)", updates);
        else
            snprintf(b, sizeof(b), "%s", tabs[i]);
        ui_button(s, tab_rect(c, i), b, m.filter == i);
    }
    field(s, search_rect(c), m.search, m.search_focus, "Search");
    ui_button(s, tool_rect(c, 0), "Refresh", false);
    ui_button(s, tool_rect(c, 1), "Upgrade all", false);
    if (m.state != S_IDLE || !updates) {
        struct rect r = tool_rect(c, 1);
        gfx_blend_fill(s, r.x, r.y, r.w, r.h, C_FACE, 150);
    }
    if (m.state != S_IDLE) {
        struct rect r = tool_rect(c, 0);
        gfx_blend_fill(s, r.x, r.y, r.w, r.h, C_FACE, 150);
    }

    /* the list */
    struct rect l = list_rect(c);
    gfx_fill(s, l.x, l.y, l.w, l.h, C_CONTENT);
    gfx_bevel(s, l.x, l.y, l.w, l.h, 1, false, C_LINE, C_FACE_DARK);
    int vis = visible_rows(c);
    if (!m.nview) {
        const char *t = !m.npk ? "No package list yet: Refresh fetches it."
                      : m.filter == F_UPDATES ? "Everything is up to date."
                      : m.filter == F_INSTALLED ? "No package is installed." : "No package matches.";
        text_wrapped(s, l.x + 12, l.y + 12, l.w - 24, t, C_DIM);
    }
    for (int i = 0; i < vis && m.first + i < m.nview; i++) {
        int k = m.view[m.first + i];
        const struct pk *p = &m.pk[k];
        struct rect r = row_rect(c, i);
        if (k == m.sel)
            gfx_fill(s, r.x, r.y, r.w, r.h, C_SELECT);
        else if ((m.first + i) & 1)
            gfx_fill(s, r.x, r.y, r.w, r.h, C_CONTENT_ALT);
        const char *tag = has_update(p) ? "update" : strcmp(p->inst, "-") ? "installed" : "";
        int tw = tag[0] ? text_width(tag) : 0;
        fit_text(s, r.x + 8, r.y + (ROW - FONT_H) / 2, 130, p->name, C_TEXT, true);
        fit_text(s, r.x + 146, r.y + (ROW - FONT_H) / 2, r.w - 160 - tw, p->summary, C_DIM, false);
        if (tag[0])
            gfx_text(s, r.x + r.w - 8 - tw, r.y + (ROW - FONT_H) / 2, tag, has_update(p) ? C_ACCENT : C_GOOD);
    }
    draw_scrollbar(s, rect_make(l.x + l.w - SB_W - 1, l.y + 1, SB_W, l.h - 2), m.first, vis, m.nview);

    /* the details */
    struct rect d = detail_rect(c);
    ui_panel(s, d, false);
    if (m.sel >= 0) {
        const struct pk *p = &m.pk[m.sel];
        int x = d.x + 14, y = d.y + 14, tw = d.w - 28;
        gfx_text_bold(s, x, y, p->name, C_TEXT);
        y += FONT_H + 10;
        y += text_wrapped(s, x, y, tw, p->summary[0] ? p->summary : "(no summary)", C_TEXT) + 10;
        struct { const char *k; char v[96]; } rows[4];
        snprintf(rows[0].v, sizeof(rows[0].v), "%s", strcmp(p->avail, "-") ? p->avail : "not in the repositories");
        snprintf(rows[1].v, sizeof(rows[1].v), "%s", strcmp(p->inst, "-") ? p->inst : "no");
        snprintf(rows[2].v, sizeof(rows[2].v), "%lu KiB installed", (p->size + 1023) / 1024);
        snprintf(rows[3].v, sizeof(rows[3].v), "%s", p->deps[0] ? p->deps : "nothing else");
        rows[0].k = "Available";
        rows[1].k = "Installed";
        rows[2].k = "Size";
        rows[3].k = "Needs";
        for (int i = 0; i < 4; i++) {
            gfx_text(s, x, y, rows[i].k, C_DIM);
            fit_text(s, x + 86, y, tw - 86, rows[i].v, C_TEXT, false);
            y += FONT_H + 6;
        }
        if (has_update(p))
            text_wrapped(s, x, y + 6, tw, "A newer version is available.", C_ACCENT);
        const char *labels[3];
        int na = actions(labels);
        for (int i = 0; i < na; i++)
            ui_button(s, action_rect(c, i), labels[i], false);
    } else {
        text_wrapped(s, d.x + 14, d.y + 14, d.w - 28, "Select a package to see it, and install, upgrade or remove it.",
                     C_DIM);
    }

    /* the status area: the password, or pkg's words */
    int by = c.y + c.h - BOTTOM;
    if (m.state == S_PASS) {
        struct rect f = pass_rect(c);
        gfx_text(s, c.x + PAD, f.y + 4, "Root password:", C_TEXT);
        char dots[64];
        size_t n = strlen(m.pass);
        memset(dots, '*', n);
        dots[n] = 0;
        field(s, f, dots, true, NULL);
        gfx_text(s, c.x + PAD, f.y + 30, "Changing the installed software needs the administrator (root).", C_DIM);
        ui_button(s, pass_btn(c, 1), "OK", false);
        ui_button(s, pass_btn(c, 0), "Cancel", false);
    } else {
        for (int i = 0; i < m.nlog; i++)
            fit_text(s, c.x + PAD, by + 4 + i * (FONT_H + 2), c.w - 2 * PAD, m.log[i], C_DIM, false);
        if (m.status[0])
            fit_text(s, c.x + PAD, by + 4 + NLOG * (FONT_H + 2), c.w - 2 * PAD, m.status,
                     m.status_bad ? C_BAD : m.state == S_RUN ? C_TEXT : C_GOOD, true);
    }
    (void)w;
}

/* ---------------------------------------------------------------- running pkg */

static void add_log(const char *t)
{
    if (!t[0])
        return;
    if (m.nlog == NLOG) {
        memmove(m.log[0], m.log[1], sizeof(m.log[0]) * (NLOG - 1));
        m.nlog = NLOG - 1;
    }
    snprintf(m.log[m.nlog++], sizeof(m.log[0]), "%s", t);
}

static void run(struct fct_view *w)
{
    int to[2], from[2];
    if (pipe(to) < 0 || pipe(from) < 0) {
        snprintf(m.status, sizeof(m.status), "could not run pkg: %s", strerror(errno));
        m.status_bad = true;
        m.state = S_IDLE;
        return;
    }
    m.pid = fork();
    if (m.pid == 0) {
        dup2(to[0], 0);
        dup2(from[1], 1);
        dup2(from[1], 2);
        close(to[1]);
        close(from[0]);
        const char *argv[6];
        int n = 0;
        argv[n++] = "pkg";
        argv[n++] = m.op;
        if (!m.is_root)
            argv[n++] = "-P";
        if (m.opname[0])
            argv[n++] = m.opname;
        argv[n] = NULL;
        execv("/bin/pkg", (char *const *)argv);
        printf("cannot run /bin/pkg: %s\n", strerror(errno));
        _exit(127);
    }
    close(to[0]);
    close(from[1]);
    if (m.pid < 0) {
        snprintf(m.status, sizeof(m.status), "could not run pkg: %s", strerror(errno));
        m.status_bad = true;
        m.state = S_IDLE;
        close(to[1]);
        close(from[0]);
        return;
    }
    if (!m.is_root)
        dprintf(to[1], "%s\n", m.pass);
    memset(m.pass, 0, sizeof(m.pass));
    close(to[1]);
    m.out = from[0];
    m.state = S_RUN;
    m.nlog = 0;
    m.linelen = 0;
    m.status_bad = false;
    snprintf(m.status, sizeof(m.status), "%s%s%s...",
             !strcmp(m.op, "update") ? "Refreshing the package list" :
             !strcmp(m.op, "install") ? "Installing" : !strcmp(m.op, "remove") ? "Removing" : "Upgrading",
             m.opname[0] ? " " : "", m.opname);
    fct_view_invalidate(w);
}

/* Asks for the password first unless the session is root's. */
static void request(struct fct_view *w, const char *op, const char *name)
{
    if (m.state != S_IDLE)
        return;
    snprintf(m.op, sizeof(m.op), "%s", op);
    snprintf(m.opname, sizeof(m.opname), "%s", name ? name : "");
    m.search_focus = false;
    if (m.is_root) {
        run(w);
    } else {
        m.pass[0] = 0;
        m.state = S_PASS;
    }
    fct_view_invalidate(w);
}

static void finished(struct fct_view *w)
{
    close(m.out);
    m.out = -1;
    int st = 0;
    waitpid(m.pid, &st, 0);
    if (m.linelen) {
        m.line[m.linelen] = 0;
        add_log(m.line);
        m.linelen = 0;
    }
    bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    m.state = S_IDLE;
    m.status_bad = !ok;
    if (ok) {
        snprintf(m.status, sizeof(m.status), "Done.");
    } else if (m.nlog) {                         /* the error: the result line, not also a log line */
        snprintf(m.status, sizeof(m.status), "%s", m.log[--m.nlog]);
    } else {
        snprintf(m.status, sizeof(m.status), "pkg failed");
    }
    load();
    fct_view_invalidate(w);
}

static int pollfd(struct fct_view *w)
{
    (void)w;
    return m.out;
}

static void readable(struct fct_view *w)
{
    char buf[512];
    ssize_t n = read(m.out, buf, sizeof(buf));
    if (n <= 0) {
        finished(w);
        return;
    }
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == '\n' || m.linelen == (int)sizeof(m.line) - 1) {
            m.line[m.linelen] = 0;
            m.linelen = 0;
            const char *t = !strncmp(m.line, "pkg: ", 5) ? m.line + 5 : m.line;
            add_log(t);
        } else {
            m.line[m.linelen++] = buf[i];
        }
    }
    fct_view_invalidate(w);
}

/* ---------------------------------------------------------------- input */

static void select_row(int k)
{
    m.sel = k;
    m.search_focus = false;
}

static void scroll_to_sel(struct fct_view *w)
{
    int vis = visible_rows(fct_view_content(w));
    int pos = -1;
    for (int i = 0; i < m.nview; i++)
        if (m.view[i] == m.sel)
            pos = i;
    if (pos < 0)
        return;
    if (pos < m.first)
        m.first = pos;
    else if (pos >= m.first + vis)
        m.first = pos - vis + 1;
}

static void mouse(struct fct_view *w, int x, int y, int kind, int buttons)
{
    (void)buttons;
    if (kind != FCT_MOUSE_DOWN && kind != FCT_MOUSE_DOUBLE)
        return;
    struct rect c = fct_view_content(w);
    if (m.state == S_PASS) {
        if (rect_contains(pass_btn(c, 1), x, y))
            run(w);
        else if (rect_contains(pass_btn(c, 0), x, y)) {
            memset(m.pass, 0, sizeof(m.pass));
            m.state = S_IDLE;
        }
        fct_view_invalidate(w);
        return;
    }
    for (int i = 0; i < 3; i++)
        if (rect_contains(tab_rect(c, i), x, y)) {
            m.filter = i;
            m.first = 0;
            filter();
        }
    m.search_focus = rect_contains(search_rect(c), x, y);
    if (m.state == S_IDLE && rect_contains(tool_rect(c, 0), x, y))
        request(w, "update", NULL);
    if (m.state == S_IDLE && rect_contains(tool_rect(c, 1), x, y)) {
        bool any = false;
        for (int i = 0; i < m.npk; i++)
            any |= has_update(&m.pk[i]);
        if (any)
            request(w, "upgrade", NULL);
    }
    struct rect l = list_rect(c);
    struct rect sb = rect_make(l.x + l.w - SB_W - 1, l.y + 1, SB_W, l.h - 2);
    int vis = visible_rows(c);
    if (rect_contains(sb, x, y))
        m.first = scrollbar_click(sb, y, m.first, vis, m.nview);
    else
        for (int i = 0; i < vis && m.first + i < m.nview; i++)
            if (rect_contains(row_rect(c, i), x, y)) {
                select_row(m.view[m.first + i]);
                if (kind == FCT_MOUSE_DOUBLE && m.sel >= 0) {   /* double-click: the first action */
                    const char *labels[3];
                    if (actions(labels))
                        request(w, !strcmp(labels[0], "Install") ? "install" : "remove", m.pk[m.sel].name);
                }
            }
    const char *labels[3];
    int na = actions(labels);
    for (int i = 0; i < na; i++)
        if (rect_contains(action_rect(c, i), x, y))
            request(w, !strcmp(labels[i], "Install") ? "install" : !strcmp(labels[i], "Remove") ? "remove" : "upgrade",
                    m.pk[m.sel].name);
    fct_view_invalidate(w);
}

static void key(struct fct_view *w, const struct fct_key *k)
{
    if (!k->value)
        return;
    bool bs = k->ascii == '\b' || k->code == 0x0E, enter = k->ascii == '\n' || k->ascii == '\r';
    if (m.state == S_PASS) {
        size_t n = strlen(m.pass);
        if (bs) {
            if (n)
                m.pass[n - 1] = 0;
        } else if (enter) {
            run(w);
        } else if (k->ascii == 27) {
            memset(m.pass, 0, sizeof(m.pass));
            m.state = S_IDLE;
        } else if (k->ascii >= 32 && k->ascii < 127 && n + 1 < sizeof(m.pass)) {
            m.pass[n] = k->ascii;
            m.pass[n + 1] = 0;
        }
        fct_view_invalidate(w);
        return;
    }
    if (m.search_focus && (bs || (k->ascii >= 32 && k->ascii < 127) || k->ascii == 27)) {
        size_t n = strlen(m.search);
        if (bs) {
            if (n)
                m.search[n - 1] = 0;
        } else if (k->ascii == 27) {
            m.search[0] = 0;
            m.search_focus = false;
        } else if (n + 1 < sizeof(m.search)) {
            m.search[n] = k->ascii;
            m.search[n + 1] = 0;
        }
        m.first = 0;
        filter();
        fct_view_invalidate(w);
        return;
    }
    if ((k->code == FCT_KEY_DOWN || k->code == FCT_KEY_UP) && m.nview) {
        int pos = -1;
        for (int i = 0; i < m.nview; i++)
            if (m.view[i] == m.sel)
                pos = i;
        pos = k->code == FCT_KEY_DOWN ? (pos < 0 ? 0 : MIN(pos + 1, m.nview - 1)) : (pos <= 0 ? 0 : pos - 1);
        select_row(m.view[pos]);
        scroll_to_sel(w);
        fct_view_invalidate(w);
    } else if (enter && m.sel >= 0) {
        const char *labels[3];
        if (actions(labels))
            request(w, !strcmp(labels[0], "Install") ? "install" : "remove", m.pk[m.sel].name);
    }
}

static void destroy(struct fct_view *w)
{
    (void)w;
    memset(m.pass, 0, sizeof(m.pass));           /* (a running pkg finishes on its own) */
}

int main(void)
{
    if (fct_app_init() < 0)
        return 1;
    signal(SIGPIPE, SIG_IGN);
    m.is_root = getuid() == 0;
    load_repo();
    load();
    struct fct_window_attr at = { "SiPM", FCT_POS_AUTO, FCT_POS_AUTO, 780, 500, 640, 400, 0 };
    struct fct_view *w = fct_view_create(&at);
    if (!w)
        return 1;
    w->draw = draw;
    w->mouse = mouse;
    w->key = key;
    w->pollfd = pollfd;
    w->readable = readable;
    w->destroy = destroy;
    return fct_main();
}
