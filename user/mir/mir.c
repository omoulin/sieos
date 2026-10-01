/*
 * facet-mir [REQUEST] - MiR (Make it Real): the user describes an
 * application, sia makes it.
 *
 * A tall window holds the conversation: what the user asks, what sia
 * answers, and what it does meanwhile (writing files, building with SIEOS's
 * gcc and g++, running and testing the application, the compiler's output).
 * sia itself runs as mir-agent (libsia with MiR's application tools), spoken
 * to in JSON lines, so the window stays responsive while it works.  Run
 * starts the current application, Files opens its folder (~/apps/NAME),
 * Install asks sia to install it (it asks before doing so).  Text in the
 * conversation is selected with the mouse (whole lines) and copied with
 * Ctrl+C; the wheel, Page Up and Page Down scroll it; Escape interrupts sia.
 * A Facet application (libfacet), with libsia's JSON; the package mir.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "common.h"                    /* (the desktop applications': user/facet-apps) */
#include "libsia.h"
#include "mir.h"

#define HEAD_H   48
#define PADX     10
#define LINE_H   (FONT_H + 3)
#define GAP_H    8
#define OUT_KEEP (16 * 1024)              /* bytes of a command's output kept */
#define OUT_SHOWN 12                       /* its last lines shown */
#define BTN_H    26
#define FIELD_H  28

enum { E_USER, E_SIA, E_TOOL, E_OUT, E_NOTE, E_ERROR };
enum { L_GAP, L_TEXT, L_MONO, L_MORE };

struct entry {
    int type;
    struct sbuf text;
    bool open;                             /* still being written (a streamed answer) */
};

struct line {
    int e, off, len, kind;                 /* entry, bytes of its text, L_* */
};

static struct {
    struct entry *e;
    int ne, cape;
    struct line *ln;
    int nl, capl, layout_w;
    bool relayout;
    int scroll, total, view_h;             /* pixels */
    bool stick;                            /* follow the end */
    int sel_a, sel_b;                      /* selected lines */
    bool has_sel, selecting;
    struct fct_field in;
    pid_t agent;
    int to, from;
    struct sbuf rbuf;
    bool ready, busy, gone, vision;
    char model[128], project[40], status[200], confirm[300];
    bool confirming;
    char *pending;                         /* the first request, sent once sia is ready */
    int spin;
    long busy_since;                       /* when the request went (ms), for the time shown */
} M = { .to = -1, .from = -1, .stick = true };

/* ---------------- the conversation ---------------- */

static struct entry *add_entry(int type, const char *text)
{
    if (M.ne == M.cape) {
        M.cape = M.cape ? M.cape * 2 : 64;
        M.e = realloc(M.e, M.cape * sizeof(*M.e));
    }
    struct entry *e = &M.e[M.ne++];
    e->type = type;
    e->open = false;
    sb_init(&e->text);
    sb_puts(&e->text, text ? text : "");
    M.relayout = true;
    return e;
}

static struct entry *last_entry(void) { return M.ne ? &M.e[M.ne - 1] : NULL; }

static void append(int type, const char *text, size_t n)
{
    struct entry *e = last_entry();
    if (!e || e->type != type || (type == E_SIA && !e->open)) {
        e = add_entry(type, "");
        e->open = type == E_SIA;
    }
    sb_putn(&e->text, text, n);
    if (type == E_OUT && e->text.len > OUT_KEEP) {     /* only the end of a long output */
        size_t cut = e->text.len - OUT_KEEP;
        memmove(e->text.s, e->text.s + cut, e->text.len - cut + 1);
        e->text.len -= cut;
    }
    M.relayout = true;
}

/* ---------------- layout: entries into lines ---------------- */

static void add_line(int e, int off, int len, int kind)
{
    if (M.nl == M.capl) {
        M.capl = M.capl ? M.capl * 2 : 256;
        M.ln = realloc(M.ln, M.capl * sizeof(*M.ln));
    }
    M.ln[M.nl++] = (struct line){ e, off, len, kind };
}

static int width_n(const char *s, int n)
{
    char tmp[1024];
    if (n > (int)sizeof(tmp) - 1)
        n = sizeof(tmp) - 1;
    memcpy(tmp, s, n);
    tmp[n] = 0;
    return text_width(tmp);
}

/* Lines of text no wider than w: broken at spaces (or anywhere in a long word), and at newlines */
static void wrap(int ei, const char *s, int len, int w)
{
    int start = 0;
    while (start <= len) {
        int nl = start;
        while (nl < len && s[nl] != '\n')
            nl++;
        int a = start;                             /* the paragraph [start, nl) */
        if (a == nl)
            add_line(ei, a, 0, L_TEXT);
        while (a < nl) {
            int end = a, good = -1;
            while (end < nl) {
                int next = end;
                while (next < nl && s[next] != ' ')
                    next++;
                if (width_n(s + a, next - a) > w) {
                    if (good < 0) {                /* one long word: as much as fits */
                        good = a + 1;
                        while (good < next && width_n(s + a, good + 1 - a) <= w)
                            good++;
                    }
                    break;
                }
                good = next;
                end = next < nl ? next + 1 : next;
            }
            if (good < 0 || end >= nl)
                good = good < 0 ? nl : good;
            if (end >= nl && width_n(s + a, nl - a) <= w)
                good = nl;
            add_line(ei, a, good - a, L_TEXT);
            a = good;
            while (a < nl && s[a] == ' ')
                a++;
        }
        start = nl + 1;
    }
}

static int line_h(const struct line *l)
{
    return l->kind == L_GAP ? GAP_H : l->kind == L_MONO ? gfx_cell_h() + 1 : LINE_H;
}

static void layout(int w)
{
    M.nl = 0;
    int tw = w - 2 * PADX - 8;
    for (int i = 0; i < M.ne; i++) {
        struct entry *e = &M.e[i];
        const char *s = e->text.s ? e->text.s : "";
        int len = (int)e->text.len;
        if (e->type != E_OUT && e->type != E_TOOL)
            add_line(i, 0, 0, L_GAP);
        if (e->type == E_TOOL) {
            add_line(i, 0, len, L_TEXT);
        } else if (e->type == E_OUT) {
            while (len && s[len - 1] == '\n')
                len--;
            int nlines = 1;
            for (int k = 0; k < len; k++)
                nlines += s[k] == '\n';
            int skip = nlines > OUT_SHOWN ? nlines - OUT_SHOWN : 0, at = 0;
            if (skip)
                add_line(i, skip, 0, L_MORE);
            for (int k = 0; k < skip; k++)
                at = (int)(strchr(s + at, '\n') - s) + 1;
            while (at <= len) {
                const char *nlp = memchr(s + at, '\n', len - at);
                int end = nlp ? (int)(nlp - s) : len;
                add_line(i, at, end - at, L_MONO);
                at = end + 1;
            }
        } else {
            wrap(i, s, len, e->type == E_USER ? tw - 8 : tw);
        }
    }
    M.total = 0;
    for (int i = 0; i < M.nl; i++)
        M.total += line_h(&M.ln[i]);
    M.total += GAP_H;
    M.layout_w = w;
    M.relayout = false;
    if (M.has_sel && (M.sel_a >= M.nl || M.sel_b >= M.nl))
        M.has_sel = false;
}

/* ---------------- geometry ---------------- */

static struct rect btn_row(struct rect c) { return rect_make(PADX, c.h - 8 - BTN_H, c.w - 2 * PADX, BTN_H); }
static struct rect field_r(struct rect c) { return rect_make(PADX, btn_row(c).y - 8 - FIELD_H, c.w - 2 * PADX - 80, FIELD_H); }
static struct rect send_r(struct rect c) { return rect_make(c.w - PADX - 72, field_r(c).y + 1, 72, FIELD_H - 2); }
static struct rect status_r(struct rect c) { return rect_make(PADX, field_r(c).y - 24, c.w - 2 * PADX, 20); }
static struct rect talk_r(struct rect c) { return rect_make(0, HEAD_H, c.w, status_r(c).y - 4 - HEAD_H); }
static struct rect btn(struct rect c, int i) { struct rect r = btn_row(c); return rect_make(r.x + i * 86, r.y, 80, r.h); }

/* ---------------- talking to sia ---------------- */

static void say(const char *op, const char *key, const char *text, long num)
{
    if (M.to < 0)
        return;
    struct sbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"op\":\"%s\"", op);
    if (key && text) {
        sb_printf(&b, ",\"%s\":", key);
        sb_json_str(&b, text);
    } else if (key) {
        sb_printf(&b, ",\"%s\":%ld", key, num);
    }
    sb_puts(&b, "}\n");
    write(M.to, b.s, b.len);
    sb_free(&b);
}

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

static void ask(const char *text)
{
    add_entry(E_USER, text);
    M.stick = true;
    if (!M.ready) {
        free(M.pending);
        M.pending = strdup(text);
        return;
    }
    M.busy = true;
    M.busy_since = now_ms();
    snprintf(M.status, sizeof(M.status), "sia is thinking");
    say("ask", "text", text, 0);
}

static void start_agent(const char *project)
{
    int in[2], out[2];
    if (pipe(in) < 0 || pipe(out) < 0)
        return;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        int nul = open("/dev/null", O_WRONLY);
        dup2(nul, 2);
        bool chan = getenv("SIEOS_DESKTOP") != NULL;     /* fds 3 and 4: the desktop channel, sia's now */
        for (int fd = chan ? 5 : 3; fd < 64; fd++)
            close(fd);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);                  /* (sia waits for what it builds and runs) */
        char *av[] = { "mir-agent", (char *)project, NULL };
        execv(MIR_AGENT, av);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (pid < 0) {
        close(in[1]);
        close(out[0]);
        return;
    }
    M.agent = pid;
    M.to = in[1];
    M.from = out[0];
    fcntl(M.to, F_SETFD, FD_CLOEXEC);
    fcntl(M.from, F_SETFD, FD_CLOEXEC);
}

static void event(const char *line, size_t len)
{
    struct json *j = json_parse(line, len);
    const char *ev = json_get_str(j, "ev"), *text = json_get_str(j, "text");
    if (!ev) {
        json_free(j);
        return;
    }
    if (!strcmp(ev, "ready")) {
        M.ready = true;
        const struct json *v = json_get(j, "vision");
        M.vision = v && v->type == JSON_TRUE;
        snprintf(M.model, sizeof(M.model), "%s", json_get_str(j, "model") ? json_get_str(j, "model") : "");
        if (!M.vision)
            add_entry(E_NOTE, "The model in use does not see images: MiR checks that your application starts and "
                              "responds, but you will have to look at its window yourself. A model that sees images "
                              "can check it too: choose one in Settings, Assistant.");
        M.status[0] = 0;
        if (M.pending) {
            M.busy = true;
            M.busy_since = now_ms();
            snprintf(M.status, sizeof(M.status), "sia is thinking");
            say("ask", "text", M.pending, 0);
            free(M.pending);
            M.pending = NULL;
        }
    } else if (!strcmp(ev, "unavailable")) {
        char msg[400];
        snprintf(msg, sizeof(msg), "sia cannot work: %s. Register a model in Settings, Assistant, then open MiR again.",
                 text ? text : "no model");
        add_entry(E_ERROR, msg);
        M.status[0] = 0;
    } else if (!strcmp(ev, "thinking")) {
        const struct json *on = json_get(j, "on");
        if (on && on->type == JSON_TRUE)
            snprintf(M.status, sizeof(M.status), "sia is thinking");
    } else if (!strcmp(ev, "delta") && text) {
        append(E_SIA, text, strlen(text));
        M.status[0] = 0;
    } else if (!strcmp(ev, "delta_end")) {
        struct entry *e = last_entry();
        if (e && e->type == E_SIA)
            e->open = false;
    } else if (!strcmp(ev, "text") && text) {
        add_entry(E_SIA, text);
    } else if (!strcmp(ev, "tool") && text) {
        char t[300];
        snprintf(t, sizeof(t), "> %s", text);
        add_entry(E_TOOL, t);
        snprintf(M.status, sizeof(M.status), "%s", text);
    } else if (!strcmp(ev, "output") && text) {
        struct entry *e = last_entry();
        if (e && e->type == E_TOOL && !strncmp(e->text.s, "> wrote ", 8))
            ;                                      /* (app_write's own line says it) */
        else if (!(e && e->type == E_TOOL && !strncmp(text, "wrote ", 6)))
            append(E_OUT, text, strlen(text));
    } else if (!strcmp(ev, "confirm")) {
        snprintf(M.confirm, sizeof(M.confirm), "%s", text ? text : "go on");
        M.confirming = true;
    } else if (!strcmp(ev, "error") && text) {
        add_entry(E_ERROR, text);
    } else if (!strcmp(ev, "done")) {
        M.busy = false;
        M.status[0] = 0;
    } else if (!strcmp(ev, "project") && text) {
        snprintf(M.project, sizeof(M.project), "%s", text);
    } else if (!strcmp(ev, "running") && text) {
        char t[200];
        snprintf(t, sizeof(t), "%s is running: try it in its window.", text);
        add_entry(E_NOTE, t);
    } else if (!strcmp(ev, "installed") && text) {
        char t[200];
        snprintf(t, sizeof(t), "%s is installed: the SIEOS menu's My apps starts it.", text);
        add_entry(E_NOTE, t);
    } else if (!strcmp(ev, "vision") && text) {
        add_entry(E_NOTE, text);
    }
    json_free(j);
    M.relayout = true;
}

static int mir_pollfd(struct fct_view *v) { (void)v; return M.from; }

static void mir_readable(struct fct_view *v)
{
    char buf[4096];
    long n = read(M.from, buf, sizeof(buf));
    if (n <= 0) {
        close(M.from);
        close(M.to);
        M.from = M.to = -1;
        M.ready = M.busy = false;
        M.gone = true;
        add_entry(E_ERROR, "sia stopped. Close MiR and open it again.");
        fct_view_invalidate(v);
        return;
    }
    sb_putn(&M.rbuf, buf, n);
    for (;;) {
        char *nl = memchr(M.rbuf.s, '\n', M.rbuf.len);
        if (!nl)
            break;
        size_t len = nl - M.rbuf.s;
        event(M.rbuf.s, len);
        memmove(M.rbuf.s, nl + 1, M.rbuf.len - len - 1);
        M.rbuf.len -= len + 1;
        M.rbuf.s[M.rbuf.len] = 0;
    }
    fct_view_invalidate(v);
}

/* ---------------- the buttons ---------------- */

static void project_dir(char *buf, size_t n)
{
    const char *home = getenv("HOME");
    snprintf(buf, n, "%s/apps/%s", home ? home : "", M.project);
}

static void launch(char *const argv[], const char *dir)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (dir && chdir(dir) < 0)
            _exit(126);
        for (int fd = 3; fd < 64; fd++)
            close(fd);
        unsetenv("SIEOS_DESKTOP");
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        int nul = open("/dev/null", O_RDWR);
        dup2(nul, 0);
        dup2(nul, 1);
        dup2(nul, 2);
        execv(argv[0], argv);
        _exit(127);
    }
    if (pid > 0)                                   /* (reaped by the SIGCHLD handler) */
        return;
}

static void run_app(void)
{
    if (!M.project[0]) {
        add_entry(E_NOTE, "There is no application yet: describe the one you want.");
        return;
    }
    char dir[300], prog[360], mj[360];
    project_dir(dir, sizeof(dir));
    snprintf(prog, sizeof(prog), "%s/%s", dir, M.project);
    if (access(prog, X_OK) < 0) {
        add_entry(E_NOTE, "It is not built yet.");
        return;
    }
    snprintf(mj, sizeof(mj), "%s/mir.json", dir);
    char buf[4096] = "";
    int fd = open(mj, O_RDONLY);
    if (fd >= 0) {
        long k = read(fd, buf, sizeof(buf) - 1);
        buf[k > 0 ? k : 0] = 0;
        close(fd);
    }
    struct json *j = json_parse(buf, strlen(buf));
    const char *kind = json_get_str(j, "kind");
    if (kind && !strcmp(kind, "terminal")) {       /* in a shell terminal */
        char line[400];
        snprintf(line, sizeof(line), "cd %s && ./%s", dir, M.project);
        char *av[] = { "/bin/facet-terminal", "-s", "-e", line, NULL };
        launch(av, dir);
    } else {
        char *av[] = { prog, NULL };
        launch(av, dir);
    }
    json_free(j);
}

static void open_folder(void)
{
    if (!M.project[0])
        return;
    char dir[300];
    project_dir(dir, sizeof(dir));
    char *av[] = { "/bin/facet-files", dir, NULL };
    launch(av, NULL);
}

static void send_input(void)
{
    char *t = M.in.text;
    while (*t == ' ')
        t++;
    if (!*t || M.busy || M.gone)
        return;
    ask(t);
    fct_field_set(&M.in, "");
}

static void answer(int a)
{
    M.confirming = false;
    add_entry(E_NOTE, a ? "(yes)" : "(no)");
    say("confirm", "answer", NULL, a);
}

/* ---------------- drawing ---------------- */

static void draw_line(struct surface *s, const struct line *l, int x, int y, int w, bool sel)
{
    const struct entry *e = &M.e[l->e];
    const char *t = e->text.s ? e->text.s + l->off : "";
    int h = line_h(l);
    if (l->kind == L_GAP)
        return;
    if (e->type == E_USER) {
        gfx_fill(s, x, y, w, h, C_CONTENT_ALT);
        gfx_fill(s, x, y, 3, h, C_ACCENT);
    } else if (e->type == E_OUT) {
        gfx_fill(s, x + 12, y, w - 12, h, C_CONTENT_ALT);
    }
    if (sel)
        gfx_fill(s, x, y, w, h, C_SELECT);
    char buf[1024];
    int n = l->len < (int)sizeof(buf) - 1 ? l->len : (int)sizeof(buf) - 1;
    memcpy(buf, t, n);
    buf[n] = 0;
    switch (l->kind) {
    case L_MORE:
        snprintf(buf, sizeof(buf), "(%d earlier lines)", l->off);
        gfx_text(s, x + 18, y + 1, buf, C_DIM);
        break;
    case L_MONO: {
        int maxc = (w - 24) / gfx_cell_w();
        if (n > maxc && maxc > 0)
            buf[maxc] = 0;
        gfx_text_mono(s, x + 18, y, buf, C_FACE_SHADOW);
        break;
    }
    default: {
        color_t c = e->type == E_TOOL || e->type == E_NOTE ? C_DIM : e->type == E_ERROR ? C_BAD : C_TEXT;
        int tx = x + (e->type == E_USER ? 12 : 4);
        if (e->type == E_TOOL)
            while (n > 4 && text_width(buf) > w - 8)
                buf[--n] = 0;
        gfx_text(s, tx, y + 1, buf, c);
    }
    }
}

static void mir_draw(struct fct_view *v, struct surface *s, struct rect c)
{
    (void)v;
    gfx_fill(s, c.x, c.y, c.w, c.h, C_CONTENT);
    /* the head */
    gfx_fill(s, c.x, c.y, c.w, HEAD_H, C_FACE);
    icon_draw(s, ICON_MIR, c.x + PADX, c.y + 8, 32);
    gfx_text_bold(s, c.x + PADX + 40, c.y + 7, "MiR", C_TEXT);
    gfx_text(s, c.x + PADX + 40 + text_width_bold("MiR") + 8, c.y + 7, "Make it Real", C_DIM);
    char sub[200];
    if (M.project[0])
        snprintf(sub, sizeof(sub), "%s  -  ~/apps/%s", M.project, M.project);
    else
        snprintf(sub, sizeof(sub), "a new application");
    gfx_text(s, c.x + PADX + 40, c.y + 25, sub, C_FACE_SHADOW);
    gfx_hline(s, c.x, c.y + HEAD_H - 1, c.w, C_LINE);

    /* the conversation */
    struct rect t = talk_r(c);
    t.x += c.x;
    t.y += c.y;
    int tw = t.w - SB_W;
    if (M.relayout || M.layout_w != tw)
        layout(tw);
    M.view_h = t.h;
    int maxs = M.total > t.h ? M.total - t.h : 0;
    if (M.stick || M.scroll > maxs)
        M.scroll = maxs;
    struct rect clip = s->clip;
    s->clip = rect_intersect(clip, rect_make(t.x, t.y, tw, t.h));
    int y = t.y - M.scroll;
    int lo = M.sel_a < M.sel_b ? M.sel_a : M.sel_b, hi = M.sel_a < M.sel_b ? M.sel_b : M.sel_a;
    for (int i = 0; i < M.nl; i++) {
        int h = line_h(&M.ln[i]);
        if (y + h >= t.y && y < t.y + t.h)
            draw_line(s, &M.ln[i], t.x + PADX, y, tw - 2 * PADX, M.has_sel && i >= lo && i <= hi);
        y += h;
    }
    s->clip = clip;
    draw_scrollbar(s, rect_make(t.x + tw, t.y, SB_W, t.h), M.scroll, t.h, M.total > t.h ? M.total : t.h);
    gfx_hline(s, c.x, t.y + t.h, c.w, C_LINE);

    /* the status */
    struct rect st = status_r(c);
    if (M.confirming) {
        char q[360];
        snprintf(q, sizeof(q), "sia asks: %s?", M.confirm);
        gfx_text_bold(s, c.x + st.x, c.y + st.y + 2, q, C_TEXT);
    } else if (M.busy || !M.ready) {
        static const char *const dots[] = { ".  ", ".. ", "...", " ..", "  .", "   " };
        char line[300], took[32] = "";
        long secs = M.busy && M.busy_since ? (now_ms() - M.busy_since) / 1000 : 0;
        if (secs >= 5)                                 /* (a model that reasons can be silent for minutes) */
            snprintf(took, sizeof(took), "  %ld:%02ld", secs / 60, secs % 60);
        snprintf(line, sizeof(line), "%s %s%s", M.ready ? (M.status[0] ? M.status : "sia is working")
                                                        : M.gone ? "sia has stopped" : "connecting to the model",
                 M.gone ? "" : dots[M.spin % 6], took);
        gfx_text(s, c.x + st.x, c.y + st.y + 2, line, C_DIM);
    } else if (M.model[0]) {
        char line[200];
        snprintf(line, sizeof(line), "model: %s%s", M.model, M.vision ? " (sees images)" : "");
        gfx_text(s, c.x + st.x, c.y + st.y + 2, line, C_FACE_SHADOW);
    }

    /* the request being typed, and the buttons */
    struct rect f = field_r(c);
    f.x += c.x;
    f.y += c.y;
    fct_field_draw(s, f, &M.in, !M.confirming, NULL);
    if (!M.in.text[0])
        gfx_text(s, f.x + 10, f.y + (f.h - FONT_H) / 2, M.busy ? "sia is working (Escape interrupts it)"
                                                               : "Describe the application, or a change", C_DIM);
    struct rect sr = send_r(c);
    sr.x += c.x;
    sr.y += c.y;
    ui_button(s, sr, M.busy ? "Stop" : "Send", false);
    static const char *const labels[] = { "Run", "Files", "Install" }, *const yesno[] = { "Yes", "No" };
    int nb = M.confirming ? 2 : 3;
    for (int i = 0; i < nb; i++) {
        struct rect b = btn(c, i);
        b.x += c.x;
        b.y += c.y;
        ui_button(s, b, M.confirming ? yesno[i] : labels[i], false);
    }
}

/* ---------------- input ---------------- */

static int line_at(int y)                          /* y in the conversation's rectangle */
{
    int at = -M.scroll;
    for (int i = 0; i < M.nl; i++) {
        int h = line_h(&M.ln[i]);
        if (y < at + h)
            return i;
        at += h;
    }
    return M.nl - 1;
}

static void copy_selection(void)
{
    int lo = M.sel_a < M.sel_b ? M.sel_a : M.sel_b, hi = M.sel_a < M.sel_b ? M.sel_b : M.sel_a;
    struct sbuf b;
    sb_init(&b);
    for (int i = lo; i <= hi && i < M.nl; i++) {
        const struct line *l = &M.ln[i];
        if (l->kind == L_GAP)
            continue;
        if (b.len) {
            bool same = i > lo && M.ln[i - 1].e == l->e && l->kind == L_TEXT;
            sb_putc(&b, same ? ' ' : '\n');
        }
        if (l->kind != L_MORE)
            sb_putn(&b, M.e[l->e].text.s + l->off, l->len);
    }
    if (b.len)
        fct_clipboard_set(b.s, b.len);
    sb_free(&b);
}

static void scroll_by(struct fct_view *v, int dy)
{
    int maxs = M.total > M.view_h ? M.total - M.view_h : 0;
    M.scroll += dy;
    M.scroll = M.scroll < 0 ? 0 : M.scroll > maxs ? maxs : M.scroll;
    M.stick = M.scroll >= maxs;
    fct_view_invalidate(v);
}

static void mir_key(struct fct_view *v, const struct fct_key *k)
{
    if (!k->value)
        return;
    bool ctrl = k->mods & FCT_MOD_CTRL;
    if (k->ascii == 27) {
        if (M.confirming)
            answer(0);
        else if (M.busy && M.agent > 0)
            kill(M.agent, SIGINT);
    } else if (M.confirming && (k->ascii == 'y' || k->ascii == 'Y' || k->ascii == '\n')) {
        answer(1);
    } else if (M.confirming && (k->ascii == 'n' || k->ascii == 'N')) {
        answer(0);
    } else if (k->ascii == '\n' || k->ascii == '\r') {
        send_input();
    } else if (k->code == FCT_KEY_PGUP || k->code == FCT_KEY_PGDN) {
        scroll_by(v, (k->code == FCT_KEY_PGUP ? -1 : 1) * (M.view_h - LINE_H));
        return;
    } else if (ctrl && k->code == 0x2E && M.in.cur == M.in.anchor && M.has_sel) {   /* Ctrl+C: the conversation's lines */
        copy_selection();
        return;
    } else {
        fct_field_key(&M.in, k);
    }
    fct_view_invalidate(v);
}

static void mir_mouse(struct fct_view *v, int x, int y, int kind, int buttons)
{
    struct rect c = fct_view_content(v), t = talk_r(c);
    if (kind == FCT_MOUSE_WHEEL) {
        scroll_by(v, buttons * 3 * LINE_H);
        return;
    }
    if (fct_field_mouse(&M.in, field_r(c), x, y, kind)) {
        fct_view_invalidate(v);
        return;
    }
    if (M.selecting && kind == FCT_MOUSE_MOVE) {
        if (y < t.y)
            scroll_by(v, -LINE_H);
        else if (y >= t.y + t.h)
            scroll_by(v, LINE_H);
        M.sel_b = line_at(y - t.y);
        M.has_sel = true;
        fct_view_invalidate(v);
        return;
    }
    if (kind == FCT_MOUSE_UP) {
        M.selecting = false;
        return;
    }
    if ((kind != FCT_MOUSE_DOWN && kind != FCT_MOUSE_DOUBLE) || !(buttons & 1))
        return;
    if (rect_contains(rect_make(t.x, t.y, t.w - SB_W, t.h), x, y)) {
        int i = line_at(y - t.y);
        if (kind == FCT_MOUSE_DOUBLE && i >= 0) {  /* the whole entry */
            int a = i, b = i;
            while (a > 0 && M.ln[a - 1].e == M.ln[i].e)
                a--;
            while (b + 1 < M.nl && M.ln[b + 1].e == M.ln[i].e)
                b++;
            M.sel_a = a, M.sel_b = b, M.has_sel = true;
        } else {
            M.sel_a = M.sel_b = i;
            M.has_sel = false;
            M.selecting = true;
        }
        fct_view_invalidate(v);
        return;
    }
    if (x >= t.w - SB_W && rect_contains(t, x, y)) {
        int maxs = M.total > t.h ? M.total - t.h : 0;
        M.scroll = scrollbar_click(rect_make(t.w - SB_W, t.y, SB_W, t.h), y, M.scroll, t.h, M.total > t.h ? M.total : t.h);
        M.scroll = M.scroll < 0 ? 0 : M.scroll > maxs ? maxs : M.scroll;
        M.stick = M.scroll >= maxs;
        fct_view_invalidate(v);
        return;
    }
    if (rect_contains(send_r(c), x, y)) {
        if (M.busy && M.agent > 0)
            kill(M.agent, SIGINT);
        else
            send_input();
    } else if (M.confirming) {
        if (rect_contains(btn(c, 0), x, y))
            answer(1);
        else if (rect_contains(btn(c, 1), x, y))
            answer(0);
    } else if (rect_contains(btn(c, 0), x, y)) {
        run_app();
    } else if (rect_contains(btn(c, 1), x, y)) {
        open_folder();
    } else if (rect_contains(btn(c, 2), x, y)) {
        if (!M.project[0])
            add_entry(E_NOTE, "There is no application to install yet.");
        else if (!M.busy && M.ready)
            ask("Install the application.");
    }
    fct_view_invalidate(v);
}

static void mir_tick(struct fct_view *v)
{
    if (M.busy || !M.ready) {
        M.spin++;
        fct_view_invalidate(v);
    }
}

static void mir_destroy(struct fct_view *v)
{
    (void)v;
    if (M.agent > 0) {
        kill(M.agent, SIGINT);
        kill(M.agent, SIGTERM);
    }
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);                      /* (the programs it starts are not waited for) */
    const char *request = argc > 1 && argv[1][0] ? argv[1] : NULL;
    if (fct_app_init() < 0)
        return 1;
    start_agent(NULL);
    if (getenv("SIEOS_DESKTOP")) {                 /* the desktop channel is sia's */
        close(3);
        close(4);
    }
    sb_init(&M.rbuf);
    add_entry(E_NOTE, "Describe the application you want: what it is for, what it shows, what it does. sia asks "
                      "what it needs to know, then writes it, builds it, runs and tests it, and fixes it until it "
                      "works. Ask for changes the same way. Your applications are kept in ~/apps (say \"continue NAME\" "
                      "to go on with one).");
    if (request)
        ask(request);
    struct fct_window_attr a = { "MiR - Make it Real", FCT_POS_AUTO, FCT_POS_AUTO, 460, 600, 360, 400,
                                 FCT_WIN_POINTER };
    struct fct_view *v = fct_view_create(&a);
    if (!v)
        return 1;
    v->draw = mir_draw;
    v->key = mir_key;
    v->mouse = mir_mouse;
    v->tick = mir_tick;
    v->pollfd = mir_pollfd;
    v->readable = mir_readable;
    v->destroy = mir_destroy;
    return fct_main();
}
