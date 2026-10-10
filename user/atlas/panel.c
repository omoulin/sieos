/*
 * panel.c - The assistant in the desktop: a conversation with sia (port
 * "sia", SIA_* in mk/proto.h), shown as a window of the stage.
 *
 * The desktop must never wait for the assistant, which may be slow, absent
 * or restarting. So one thread of ours does all the talking with sia, and
 * hands what comes back to the main loop as messages (ATLAS_SIA): "ready",
 * a piece of the answer, "done", an error. The main loop gives it work
 * through a shared request and a wake-up message, sent only when the thread
 * is known to be waiting for one (so the main loop never blocks either):
 *
 *   main loop                         panel thread                sia
 *   pending = question ──wake──►       SIA_ASK ─────────────────►
 *              ◄──ATLAS_SIA piece──    SIA_NEXT ◄── a piece ─────  (repeat)
 *              ◄──ATLAS_SIA done───    SIA_NEXT ◄── 0 (the end) ──
 *
 * Answers may carry actions, each on a line of its own, which become
 * buttons: [fly:PATH] opens a file; [project:NAME] opens a project;
 * [island:KEY=VALUE] lists the files with that attribute in the Lens (the
 * older [island:NAME] opens the project NAME).
 * The conversation's own instructions (SIA_CONTEXT, sent when it is opened)
 * tell the model so, and list the user's files. The panel continues the
 * last conversation (sia keeps it on the disk); "New" starts another. Facts
 * sia offers to remember ("[remember: ...]") become "Remember" buttons.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"

#define PMSG  12                    /* messages kept */
#define PTEXT 4096                  /* longest message kept */
#define NACT  4                     /* actions per answer */
#define LH    26                    /* a line of text in the panel, in pixels */
enum { WHO_YOU, WHO_SIA, WHO_NOTE, WHO_ERR };
enum { P_INFO = 1, P_PIECE, P_DONE, P_STOPPED, P_ERR, P_OFFER };   /* thread -> main loop */
enum { R_ASK = 1, R_NEW, R_REMEMBER };                       /* main loop -> thread */

typedef struct { int who, len, nact, id; char *text; struct { int fly; char arg[96]; } act[NACT]; } pmsg_t;   /* fly: 1 file, 0 island, 2 remember */
static pmsg_t msgs[PMSG];
static int nmsg, busy, ready, scroll, pieces, conv_new = 1, answers, logged;
static int rx0, ry0, rx1, ry1;            /* where the window was last drawn */
static char input[200];
static sia_info_t info;
static long atlas_port, qport;

/* shared with the thread */
static int pending, idle, stop_flag;
static char preq[SIA_MAX];
static int preq_n, preq_kind;
static char pctx[SIA_MAX];                  /* instructions for a new conversation (SIA_CONTEXT) */
static int pctx_n;

/* ---- The thread. */
static void post(int kind, const void *d, size_t n)
{
    msg_t m = { .w = { ATLAS_SIA, kind, n }, .sbuf = d, .slen = n };
    ipc_call(atlas_port, &m);
}

/* Wait until the main loop has work for us (see the top of the file). */
static void wait_work(void)
{
    for (;;) {
        if (__atomic_load_n(&pending, __ATOMIC_SEQ_CST)) return;
        __atomic_store_n(&idle, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&pending, __ATOMIC_SEQ_CST) && __atomic_exchange_n(&idle, 0, __ATOMIC_SEQ_CST)) return;
        msg_t r = { 0 };
        long f = ipc_recv(qport, &r);                    /* the main loop's wake-up */
        if (f > 0) reply_val(f, 0);
    }
}

static long sia_call(long *sia, msg_t *m, int repeat)
{
    long e = call_named(sia, "sia", m, repeat);
    return e < 0 ? e : (long)m->w[0];
}

static void get_info(long *sia)
{
    sia_info_t in;
    msg_t m = { .w = { SIA_INFO }, .rbuf = &in, .rlen = sizeof in };
    if (sia_call(sia, &m, 1) >= 0) post(P_INFO, &in, sizeof in);
}

static void thread(void *arg)
{
    (void)arg;
    static char q[SIA_MAX], buf[SIA_MAX], ctx[SIA_MAX];
    long sia = 0, sess = 0;
    int fresh = 0, nctx = 0;            /* fresh: "New" was pressed: do not resume the last one */
    /* (No question at start: sia is started on demand, so asking now would
     * load its model at every boot. The first question brings it up.) */
    for (;;) {
        wait_work();
        int kind = preq_kind, n = preq_n;
        memcpy(q, preq, n);
        if (pctx_n) { memcpy(ctx, pctx, pctx_n); nctx = pctx_n; pctx_n = 0; }
        __atomic_store_n(&pending, 0, __ATOMIC_SEQ_CST);
        if (kind == R_NEW) {
            if (sess > 0) { msg_t m = { .w = { SIA_CLOSE, sess } }; sia_call(&sia, &m, 1); }
            sess = 0;
            fresh = 1;
            continue;
        }
        if (kind == R_REMEMBER) {                        /* the user kept a fact sia offered */
            msg_t m = { .w = { SIA_FACTS, SIA_F_ADD }, .sbuf = q, .slen = n };
            sia_call(&sia, &m, 1);
            continue;
        }
        long e = 0;
        for (int tries = 0; tries < 2; tries++) {        /* sia stops when idle and forgets its */
            e = 0;                                       /* conversations: then open a new one, once */
            if (sess <= 0) {                             /* the last conversation goes on (saved by sia) */
                msg_t m = { .w = { SIA_OPEN, fresh ? 0 : SIA_RESUME, 0 } };
                sess = e = sia_call(&sia, &m, 1);
                if (e > 0) fresh = 0;
                if (e > 0 && nctx) { msg_t c = { .w = { SIA_CONTEXT, sess }, .sbuf = ctx, .slen = nctx }; sia_call(&sia, &c, 1); }
            }
            if (e >= 0) {
                msg_t m = { .w = { SIA_ASK, sess }, .sbuf = q, .slen = n };
                e = sia_call(&sia, &m, 0);
            }
            if (e >= 0 || tries) break;
            sess = 0;
        }
        while (e >= 0) {
            if (__atomic_exchange_n(&stop_flag, 0, __ATOMIC_SEQ_CST)) {
                msg_t m = { .w = { SIA_STOP, sess } };
                sia_call(&sia, &m, 1);
                post(P_STOPPED, 0, 0);
                break;
            }
            msg_t m = { .w = { SIA_NEXT, sess }, .rbuf = buf, .rlen = sizeof buf };
            if ((e = sia_call(&sia, &m, 0)) < 0) break;
            if (e == 0) {
                post(P_DONE, 0, 0);
                static char of[1024];                    /* facts sia offered to remember */
                msg_t o = { .w = { SIA_FACTS, SIA_F_OFFERED }, .rbuf = of, .rlen = sizeof of - 1 };
                if (sia_call(&sia, &o, 1) >= 0)
                    for (char *p = of, *x; p < of + o.rlen && (x = strchr(p, '\n')); p = x + 1) post(P_OFFER, p, x - p);
                break;
            }
            post(P_PIECE, buf, m.rlen < (uint64_t)e ? m.rlen : (uint64_t)e);
        }
        if (e < 0) {                                     /* (a restarted sia forgot the session) */
            sess = 0;
            char t[24];
            post(P_ERR, t, num(t, e));
        }
        __atomic_store_n(&stop_flag, 0, __ATOMIC_SEQ_CST);
        get_info(&sia);
    }
}

/* Give the thread its next request (only while it is not busy). */
static void request(int kind, const char *s, int n)
{
    memcpy(preq, s, n);
    preq_n = n; preq_kind = kind;
    __atomic_store_n(&pending, 1, __ATOMIC_SEQ_CST);
    if (__atomic_exchange_n(&idle, 0, __ATOMIC_SEQ_CST)) {
        msg_t m = { .w = { 1 } };
        ipc_call(qport, &m);
    }
}

void panel_init(long port)
{
    atlas_port = port;
    qport = port_create(0);
    thread_start(thread, 0, 16384);
}

/* ---- The conversation. */
static pmsg_t *add(int who, const char *s, int n)
{
    if (nmsg == PMSG) { free(msgs[0].text); memmove(msgs, msgs + 1, sizeof *msgs * (PMSG - 1)); nmsg--; }
    pmsg_t *m = &msgs[nmsg];
    memset(m, 0, sizeof *m);
    if (!(m->text = malloc(PTEXT))) return 0;
    nmsg++;
    m->who = who;
    m->len = n < PTEXT - 1 ? n : PTEXT - 1;
    memcpy(m->text, s, m->len);
    m->text[m->len] = 0;
    scroll = 0;
    return m;
}
static void note(int who, const char *s) { add(who, s, strlen(s)); }

/* Take the actions out of an answer: "[fly:...]", "[project:...]", "[island:k=v]". */
static void actions(pmsg_t *m)
{
    char *t = m->text;
    for (char *p = t; (p = strchr(p, '[')); ) {
        int fly = !memcmp(p, "[fly:", 5), isl = !memcmp(p, "[island:", 8), prj = !memcmp(p, "[project:", 9);
        char *e = strchr(p, ']');
        if (!memcmp(p, "[remember:", 10) && e) { memmove(p, e + 1, strlen(e + 1) + 1); continue; }   /* (a button, from sia) */
        if ((!fly && !isl && !prj) || !e || m->nact == NACT) { p++; continue; }
        char *a = p + (fly ? 5 : prj ? 9 : 8);
        size_t n = e - a < 95 ? e - a : 95;
        m->act[m->nact].fly = fly;
        memcpy(m->act[m->nact].arg, a, n);
        m->act[m->nact++].arg[n] = 0;
        memmove(p, e + 1, strlen(e + 1) + 1);           /* (not shown as text) */
    }
    m->len = strlen(t);
    while (m->len && (t[m->len - 1] == '\n' || t[m->len - 1] == ' ')) t[--m->len] = 0;
}

static void ask(const char *q)
{
    if (!*q || busy) return;
    note(WHO_YOU, q);
    static char full[SIA_MAX];
    int n = 0;
    if (conv_new) {                                      /* the first question: what the model should know */
        int c = strlcpy(pctx, "The user's desktop is Stage: projects on the left "   /* (sent once, as the */
            "(each file belongs to one project, or to the Inbox), at most two windows in the middle, "   /* conversation's */
            "and the project's other things on a shelf below. To offer a button that opens a file, "    /* own instructions) */
            "add a line [fly:PATH]. To offer a button that opens a project, add a line [project:NAME]. "
            "To list the files with an attribute, add a line [island:KEY=VALUE], for example "
            "[island:topic=notes]. The user's files:\n", sizeof pctx);
        c += atlas_files(pctx + c, sizeof pctx - c - 1 > 0 ? sizeof pctx - c - 1 : 0);
        pctx_n = c < (int)sizeof pctx ? c : (int)sizeof pctx - 1;
        conv_new = 0;
    }
    n += strlcpy(full + n, q, sizeof full - n);
    if (n >= (int)sizeof full) n = sizeof full - 1;
    add(WHO_SIA, "", 0);
    busy = 1; pieces = 0;
    say("atlas: sia: ask %d bytes\n", n);
    request(R_ASK, full, n);
}

void panel_ask(const char *q) { ask(q); }

void panel_status(char *out, size_t cap)
{
    strlcpy(out, !ready ? "sia: idle" : info.backend == SIA_REMOTE ? "sia: online" : "sia: local", cap);
}

/* The k-th line of text from the end of the conversation (k = 0: the last). */
const char *panel_last(int k)
{
    for (int i = nmsg - 1; i >= 0; i--) if (msgs[i].len && !k--) return msgs[i].text;
    return 0;
}

void panel_event(msg_t *m, const char *data)
{
    pmsg_t *last = nmsg ? &msgs[nmsg - 1] : 0;
    int n = m->rlen;
    switch (m->w[1]) {
    case P_INFO: {
        char was[sizeof info.model];
        memcpy(was, info.model, sizeof was);
        memcpy(&info, data, n < (int)sizeof info ? n : (int)sizeof info);
        info.model[sizeof info.model - 1] = 0;
        if (!ready || strcmp(was, info.model))
            say("atlas: sia: ready (%s, %s)\n", info.backend == SIA_REMOTE ? "online" : "local", info.model);
        ready = 1;
        break;
    }
    case P_PIECE:
        if (!last || last->who != WHO_SIA) return;
        if (!pieces++) say("atlas: sia: first piece\n");
        if (n > PTEXT - 1 - last->len) n = PTEXT - 1 - last->len;
        memcpy(last->text + last->len, data, n);
        last->len += n;
        last->text[last->len] = 0;
        scroll = 0;
        break;
    case P_DONE:
    case P_STOPPED:
        busy = 0;
        if (last && last->who == WHO_SIA) { actions(last); last->id = ++answers; }
        if (m->w[1] == P_STOPPED) { note(WHO_NOTE, "(stopped)"); say("atlas: sia: stopped after %d pieces\n", pieces); }
        else say("atlas: sia: done %d bytes, %d pieces, %d actions\n", last ? last->len : 0, pieces, last ? last->nact : 0);
        break;
    case P_OFFER:                                        /* a "Remember" button on the answer */
        if (last && last->who == WHO_SIA && last->nact < NACT && n > 0) {
            int k = n < 95 ? n : 95;
            last->act[last->nact].fly = 2;
            memcpy(last->act[last->nact].arg, data, k);
            last->act[last->nact++].arg[k] = 0;
            say("atlas: sia: offers to remember %s\n", last->act[last->nact - 1].arg);
        }
        break;
    case P_ERR:
        busy = 0;
        conv_new = 1;                                    /* (the next question starts over) */
        if (last && last->who == WHO_SIA && !last->len) { free(last->text); nmsg--; }
        note(WHO_ERR, "sia stopped answering (it may be restarting): ask again.");
        char t[24];
        memcpy(t, data, n < 23 ? n : 23);
        t[n < 23 ? n : 23] = 0;
        say("atlas: sia: error %s\n", t);
        break;
    }
    if (rx1 > rx0) redraw_box(rx0, ry0, rx1, ry1); else redraw();
}

/* ---- Input. */
static const char *sugg[3] = { "What is in this project?", "Show my demo files", "Find my notes" };

static void run_action(int i, int a)
{
    char t[120];
    if (msgs[i].act[a].fly == 2) {                       /* keep what sia offered to remember */
        request(R_REMEMBER, msgs[i].act[a].arg, strlen(msgs[i].act[a].arg));
        strlcpy(t, "Remembered: ", sizeof t);
        strlcpy(t + strlen(t), msgs[i].act[a].arg, sizeof t - strlen(t));
        note(WHO_NOTE, t);
        say("atlas: sia: remembered %s\n", msgs[i].act[a].arg);
        msgs[i].act[a].fly = 3;                          /* (done: the button goes) */
        return;
    }
    if (msgs[i].act[a].fly == 3) return;
    if (msgs[i].act[a].fly) {
        long e = atlas_open_file(msgs[i].act[a].arg);
        say("atlas: sia: action fly %s -> %ld\n", msgs[i].act[a].arg, e);
        if (e < 0) { strlcpy(t, "No such file: ", sizeof t); strlcpy(t + strlen(t), msgs[i].act[a].arg, sizeof t - strlen(t)); note(WHO_NOTE, t); }
        return;
    }
    char k[24], *eq = strchr(msgs[i].act[a].arg, '=');
    if (!eq) {                                           /* [project:NAME]: open it */
        long e = atlas_open_project(msgs[i].act[a].arg);
        say("atlas: sia: action project %s -> %ld\n", msgs[i].act[a].arg, e);
        if (e < 0) { strlcpy(t, "No such project: ", sizeof t); strlcpy(t + strlen(t), msgs[i].act[a].arg, sizeof t - strlen(t)); note(WHO_NOTE, t); }
        return;
    }
    size_t kl = eq - msgs[i].act[a].arg < 23 ? (size_t)(eq - msgs[i].act[a].arg) : 23;
    memcpy(k, msgs[i].act[a].arg, kl);
    k[kl] = 0;
    int n = atlas_find(k, eq + 1);
    say("atlas: sia: action find %s -> %d\n", msgs[i].act[a].arg, n);
    char *p = t + strlcpy(t, "Files with ", sizeof t);
    p += strlcpy(p, msgs[i].act[a].arg, 64);
    p += strlcpy(p, ": ", 3);
    p += num(p, n < 0 ? 0 : n);
    strlcpy(p, n == 1 ? " file" : " files", 8);
    note(WHO_NOTE, t);
}

int panel_click(int kind, int arg)
{
    if (kind < H_PANEL || kind > H_PMODE) return 0;     /* (only the panel's own kinds) */
    switch (kind) {
    case H_PSEND: if (!busy) { ask(input); input[0] = 0; } break;
    case H_PSTOP: if (busy) { __atomic_store_n(&stop_flag, 1, __ATOMIC_SEQ_CST); say("atlas: sia: stop\n"); } break;
    case H_PSUG:  ask(sugg[arg]); break;
    case H_PACT:  run_action(arg / NACT, arg % NACT); break;
    case H_PMODE: if (!busy) { request(R_NEW, "", 0); conv_new = 1; note(WHO_NOTE, "(new conversation)"); } break;
    }
    return 1;
}

int panel_key(int c)                                     /* (only while its window has the keys) */
{
    size_t n = strlen(input);
    if (c == '\n') { if (!busy && n) { ask(input); input[0] = 0; } }
    else if (c == 27 && busy) { __atomic_store_n(&stop_flag, 1, __ATOMIC_SEQ_CST); say("atlas: sia: stop\n"); }   /* Esc: stop */
    else if (c == '\b') { if (n) input[n - 1] = 0; }
    else if (c >= 32 && c < 127 && n + 1 < sizeof input) { input[n] = c; input[n + 1] = 0; }
    else return 0;
    return 1;
}

void panel_wheel(int w)
{
    scroll -= 2 * w;
    if (scroll < 0) scroll = 0;
}

/* ---- The picture: text wrapped at words, the newest at the bottom. */
#define MAXROW 400
static struct { short m, start, len, act; } row[MAXROW];

void panel_draw(int x0, int y0, int x1, int y1, int focused)
{
    rx0 = x0; ry0 = y0; rx1 = x1; ry1 = y1;
    hitbox(x0, y0 + 48, x1, y1, H_PANEL, 0);
    pill(x1 - 24 - tw(3, 2) - 28, y0 + 58, "New", 0, H_PMODE, 0);
    char st[96];
    if (!ready) strlcpy(st, busy ? "starting sia..." : "sia starts when you ask", sizeof st);
    else if (!info.ready || !info.model[0]) strlcpy(st, busy ? "no model loaded yet | answering..." : "no model loaded yet", sizeof st);
    else {
        char *p = st + strlcpy(st, info.model, 40);
        p += strlcpy(p, " | ", 4);
        p += num(p, info.tok_s_x100 / 100); *p++ = '.'; p += num(p, info.tok_s_x100 / 10 % 10);
        strlcpy(p, busy ? " tok/s | answering..." : " tok/s", 24);
    }
    dl_text(x0 + 24, y0 + 68, st, strlen(st), S(2), C_DIM, x0, y0, x1 - 120, y1);
    int ty0 = y0 + 112, ty1 = y1 - 96, chars = (x1 - x0 - 48) / tw(1, 2);
    if (!nmsg) {                                         /* nothing yet: what to ask */
        label(x0 + 24, ty0 + 6, "Ask about your files and projects.", 2, C_TXT);
        for (int i = 0; i < 3; i++) pill(x0 + 24, ty0 + 50 + i * 54, sugg[i], 0, H_PSUG, i);
    }
    int nr = 0;                                          /* the rows of every message */
    for (int i = 0; i < nmsg && nr < MAXROW - 4; i++) {
        pmsg_t *m = &msgs[i];
        const char *t = m->text;
        int p = 0, w = m->who == WHO_YOU ? chars - 2 : chars;
        if (!m->len && m->who == WHO_SIA) row[nr++] = (typeof(row[0])){ i, 0, 0, 0 };
        while (p < m->len && nr < MAXROW - 4) {
            int e = p, br = -1;
            while (e < m->len && t[e] != '\n' && e - p < w) { if (t[e] == ' ') br = e; e++; }
            if (e < m->len && t[e] != '\n' && e - p == w && br > p) e = br;   /* break at a space */
            row[nr++] = (typeof(row[0])){ i, p, e - p, 0 };
            p = e < m->len && (t[e] == '\n' || t[e] == ' ') ? e + 1 : e;
        }
        for (int a = 0; a < m->nact && nr < MAXROW - 2; a++) { row[nr++] = (typeof(row[0])){ i, a, 0, 1 }; row[nr++] = (typeof(row[0])){ i, a, 0, 2 }; }
        if (i + 1 < nmsg && nr < MAXROW) row[nr++] = (typeof(row[0])){ i, 0, 0, 3 };   /* a gap */
    }
    int vis = (ty1 - ty0) / LH, max = nr > vis ? nr - vis : 0;
    if (scroll > max) scroll = max;
    int first = nr > vis ? nr - vis - scroll : 0;
    for (int r = first; r < nr && r < first + vis; r++) {
        int y = ty0 + (r - first) * LH;
        pmsg_t *m = &msgs[row[r].m];
        if (row[r].act == 1) {                           /* an action: a button */
            char b[64];
            int a = row[r].start;
            const char *arg = m->act[a].arg;
            if (m->act[a].fly == 3) continue;            /* (already remembered) */
            strlcpy(b, m->act[a].fly == 2 ? "Remember: " : m->act[a].fly ? "Open " : strchr(arg, '=') ? "Files with " : "Project ", sizeof b);
            strlcpy(b + strlen(b), m->act[a].fly == 1 ? base_name(arg) : arg, sizeof b - strlen(b));
            int e = pill(x0 + 24, y + 2, b, 1, H_PACT, row[r].m * NACT + a);
            if (m->id > logged || atlas_debug) say("atlas: sia: button %d at %d %d\n", a, (x0 + 24 + e) / 2, y + 23);   /* (for the tests) */
            continue;
        }
        if (row[r].act) continue;
        uint32_t c = m->who == WHO_YOU ? C_ACC : m->who == WHO_SIA ? C_TXT : m->who == WHO_ERR ? C_ERR : C_DIM;
        int x = x0 + 24;
        if (m->who == WHO_YOU && row[r].start == 0) { label(x, y, ">", 2, C_ACC); }
        if (m->who == WHO_YOU) x += tw(2, 2);
        if (row[r].len) dl_text(x, y, m->text + row[r].start, row[r].len, S(2), c, x0, ty0, x1 - 20, ty1);
        if (busy && row[r].m == nmsg - 1 && r == nr - 1)   /* still coming: a cursor */
            dl_rect(x + tw(row[r].len, 2) + 4, y + 2, x + tw(row[r].len, 2) + 14, y + 22, 0, C_ACC);
    }

    for (int i = 0; i < nmsg; i++) if (msgs[i].nact && msgs[i].id > logged) logged = msgs[i].id;
    int iy0 = y1 - 72, iy1 = y1 - 22, ix1 = x1 - 24 - (busy ? tw(4, 2) : tw(3, 2)) - 40;
    dl_rect(x0 + 20, iy0, ix1, iy1, 12, C_BG);
    dl_frame(x0 + 20, iy0, ix1, iy1, 12, focused ? 2 : 1, focused ? C_ACC : C_ISLB);
    hitbox(x0 + 20, iy0, ix1, iy1, H_PIN, 0);
    int n = strlen(input), fit = (ix1 - x0 - 60) / tw(1, 2), s = n > fit ? n - fit : 0;
    if (n) dl_text(x0 + 36, iy0 + 14, input + s, n - s, S(2), C_TXT, x0, iy0, ix1 - 10, iy1);
    else label(x0 + 36, iy0 + 14, busy ? "answering..." : "Ask sia...", 2, C_DIM);
    if (focused) dl_rect(x0 + 38 + tw(n - s, 2), iy0 + 12, x0 + 48 + tw(n - s, 2), iy1 - 12, 0, C_ACC);
    if (busy) pill(ix1 + 12, iy0 + 4, "Stop", 1, H_PSTOP, 0);
    else pill(ix1 + 12, iy0 + 4, "Ask", n > 0, H_PSEND, 0);
}
