/*
 * inst.c - The Install window (docs/install.md): SIEOS onto this PC's NVMe
 * drive, from the system started from the USB key. The disk work is
 * /bin/install's (user/install: the GPT, the EFI partition, SieFS, the
 * checks); this window runs it as root on the console "atlas#INST_CHAN"
 * and shows what it prints, in five steps:
 *
 *   drives     each drive: model, size, partitions; its free extents
 *              ("Install here", 4 GiB at least) and "Erase whole drive"
 *   model      (erase only) the drive's model, typed exactly
 *   summary    install -n: the plan, exactly what changes; Install
 *   progress   install: its steps and a bar ("progress N" lines)
 *   done       what it said; Restart, Close
 *
 * Root's password is asked first unless root is logged in (checked by the
 * accounts server, like Settings' system changes). The program asks again
 * on its own: "yes", or the model as typed here; it refuses anything else.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"
#include "mk/crypto.h"                   /* wipe */

#define TOP 48
#define LH 30                            /* a line of text */
enum { I_AUTH, I_DRIVES, I_MODEL, I_PLAN, I_RUN, I_DONE };
enum { B_OK = 1, B_BACK, B_INSTALL, B_RESTART, B_CLOSE, B_FIELD, B_FREE = 100, B_WHOLE = 200 };
#define MAXL 80

static int step, sel_d, sel_n, field_on, err;   /* sel_n: the free extent (0: the whole drive) */
static char pw[128], model[64], msg[160], answer[80];
static struct { char kind, text[110]; int d, n; } ln[MAXL];   /* what the program printed, a line each */
static int nl, pct;
static long pid, reader;                 /* the program; its CON_READ waiting for the answer */
static char part[256];                   /* a line not ended yet */
static size_t npart;
static char models[8][41];               /* each drive's model (from "drive D: MODEL (serial ...") */

static void add_line(const char *s)
{
    if (!memcmp(s, "progress ", 9)) { pct = (int)strnum(s + 9); return; }
    if (nl == MAXL) return;
    int d = 0, n = 0;
    char k = 'l';
    const char *t = s;
    if (!memcmp(s, "drive ", 6)) {
        k = 'd'; d = (int)(s[6] - '0'); t = s + 9;
        if (d >= 0 && d < 8) {
            const char *e = strchr(t, '(');
            size_t len = e && e > t ? (size_t)(e - t - 1) : strlen(t);
            if (len > 40) len = 40;
            memcpy(models[d], t, len); models[d][len] = 0;
        }
    } else if (!memcmp(s, "  free ", 7)) {
        const char *q = s + 7;
        while (*q >= '0' && *q <= '9') n = n * 10 + *q++ - '0';
        k = !memcmp(q, " ok:", 4) ? 'f' : 'l'; t = strchr(s, ':') ? strchr(s, ':') + 2 : s;
    } else if (!memcmp(s, "  whole ok", 10)) k = 'w';
    else if (!memcmp(s, "  refused: ", 11) || !memcmp(s, "error: ", 7)) { k = 'e'; t = strchr(s, ':') + 2; }
    else if (!memcmp(s, "plan: ", 6)) t = s + 6;
    else if (!memcmp(s, "step: ", 6) || !memcmp(s, "check: ", 7)) k = 's';
    else if (!memcmp(s, "done: ", 6) || !memcmp(s, "next: ", 6)) { k = 'k'; t = s + 6; }
    else if (!memcmp(s, "Type ", 5)) return;                     /* (its question: answered by us) */
    for (int i = nl - 1; i >= 0 && k != 'd'; i--) if (ln[i].kind == 'd') { d = (int)(ln[i].text[0] - '0'); break; }
    ln[nl].kind = k; ln[nl].d = d; ln[nl].n = n;
    if (k == 'd') { ln[nl].text[0] = '0' + d; strlcpy(ln[nl].text + 1, t, sizeof ln[0].text - 1); }
    else strlcpy(ln[nl].text, t, sizeof ln[0].text);
    if (k == 'e') err = 1;
    say("atlas: install: %s\n", s);
    nl++;
}

/* Run /bin/install with these arguments, as root, its console ours. */
static void irun(const char *args)
{
    nl = npart = 0; pct = 0; err = 0; reader = 0;
    mk_spawn_t o = { .flags = SPAWN_CONSOLE };
    strlcpy(o.console, "atlas#", sizeof o.console);
    num(o.console + 6, INST_CHAN);
    char a[64];
    strlcpy(a, "install ", sizeof a);
    strlcpy(a + 8, args, sizeof a - 8);
    pid = spawn_file("/bin/install", a, 0, 0, &o);
    if (pid < 0) { pid = 0; strlcpy(msg, "Cannot start /bin/install.", sizeof msg); err = 1; }
    say("atlas: install: running %s\n", a);
}
static void args_of(char *a, int dry)    /* "[-n ]free D N" / "[-n ]whole D" */
{
    char *p = a + strlcpy(a, dry ? "-n " : "", 4);
    p += strlcpy(p, sel_n ? "free " : "whole ", 7);
    p += num(p, sel_d);
    if (sel_n) { *p++ = ' '; p += num(p, sel_n); }
    *p = 0;
}

void inst_open(void)
{
    msg[0] = 0; field_on = 1;
    if (atlas_uid() == 0) { step = I_DRIVES; irun("list"); }
    else step = I_AUTH;
    say("atlas: install: open (step %d)\n", step);
}
void inst_close(void)
{
    wipe(pw, sizeof pw);
    if (pid && step != I_RUN) sys_kill(pid);     /* (an install under way is never cut short) */
    step = I_AUTH; nl = 0;
}

/* The console requests of /bin/install (atlas.c: channel INST_CHAN, from root). */
int inst_con(long from, msg_t *m, const char *data)
{
    if ((int)m->w[3] != INST_CHAN) return 0;
    if (m->w[0] == CON_WRITE) {
        for (uint64_t i = 0; i < m->rlen; i++) {
            if (data[i] == '\n' || npart == sizeof part - 1) { part[npart] = 0; add_line(part); npart = 0; }
            else part[npart++] = data[i];
        }
        reply_val(from, m->rlen);
        redraw();
    } else if (m->w[0] == CON_READ) {    /* its question: what the user chose here */
        char line[84];
        size_t n = strlcpy(line, answer, sizeof line - 1);
        npart = 0;                       /* (its question, not ended by a newline: dropped) */
        line[n++] = '\n';
        if (n > m->w[1]) n = m->w[1];
        msg_t r = { .w = { n }, .sbuf = line, .slen = n };
        ipc_reply(from, &r);
        say("atlas: install: answered its question\n");
    } else reply_val(from, m->w[0] == CON_ECHO ? 0 : -EIO);
    return 1;
}

/* It ended (atlas reaped it) -> 1 if it was ours. */
int inst_ended(long p, int status)
{
    if (!pid || p != pid) return 0;
    pid = 0;
    if (npart) { part[npart] = 0; add_line(part); npart = 0; }
    if (step == I_RUN) step = I_DONE;
    if (step == I_PLAN && status) err = 1;
    say("atlas: install: program ended (%d), step %d\n", status, step);
    return 1;
}

/* Is installing offered here (started from the USB key, an NVMe drive free)? */
int inst_offer(void)
{
    siefs_stat_t st;
    disk_raw_t d;
    msg_t m = { .w = { DISK_RAW_INFO, 0 }, .rbuf = &d, .rlen = sizeof d };
    long p = port_find("disk");
    return !fs_stat("/.snapshots/factory/boot", &st, 0) && p > 0 && ipc_call(p, &m) >= 0 && !m.w[0] && !d.busy;
}

static void go(int b)
{
    msg[0] = 0;
    if (step == I_AUTH && b == B_OK) {
        char req[160], *p = req;
        p += strlcpy(p, "root", 8) + 1;
        p += strlcpy(p, pw, 128) + 1;
        msg_t m = { .w = { AUTH_CHECK }, .sbuf = req, .slen = p - req };
        long r = atlas_auth(&m);
        wipe(req, sizeof req); wipe(pw, sizeof pw);
        if (r < 0) { strlcpy(msg, "Wrong password for root.", sizeof msg); say("atlas: install: root's password refused\n"); return; }
        step = I_DRIVES; irun("list");
    } else if (step == I_DRIVES && (b >= B_FREE && b < B_WHOLE + 8)) {
        sel_d = b >= B_WHOLE ? b - B_WHOLE : (b - B_FREE) / 16;
        sel_n = b >= B_WHOLE ? 0 : (b - B_FREE) % 16;
        if (sel_n) { char a[32]; args_of(a, 1); step = I_PLAN; irun(a); }
        else { step = I_MODEL; model[0] = 0; field_on = 1; }
    } else if (step == I_MODEL && b == B_OK) {
        if (strcmp(model, models[sel_d])) { strlcpy(msg, "That is not the drive's model: type it exactly as shown.", sizeof msg); say("atlas: install: model mistyped\n"); return; }
        char a[32]; args_of(a, 1); step = I_PLAN; irun(a);
    } else if (step == I_PLAN && b == B_INSTALL && !pid && !err) {
        strlcpy(answer, sel_n ? "yes" : model, sizeof answer);
        char a[32]; args_of(a, 0); step = I_RUN; irun(a);
    } else if (b == B_BACK && step != I_RUN) {
        if (pid) sys_kill(pid);
        step = step == I_DONE ? I_DRIVES : step == I_PLAN && !sel_n ? I_MODEL : I_DRIVES;
        if (step == I_DRIVES) irun("list");
    } else if (b == B_RESTART && step == I_DONE) { say("atlas: install: restart\n"); power(POWER_RESTART); }
    else if (b == B_CLOSE) atlas_close(WM_INST);
    say("atlas: install: step %d\n", step);
}

static int button(int x, int y, const char *s, int on, int b)
{
    int r = pill(x, y, s, on, H_INST, b);
    if (atlas_debug) say("atlas: dbg inst %d at %d %d\n", b, (x + r) / 2, y + 21);
    return r;
}
static void field(int x, int y, int x1, const char *lbl, const char *v, int hidden)
{
    label(x, y + 12, lbl, 2, C_DIM);
    int fx = x + tw(16, 2);
    dl_rect(fx, y, x1, y + 50, 10, C_FIELD);
    dl_frame(fx, y, x1, y + 50, 10, field_on ? 2 : 1, field_on ? C_ACC : C_LINE);
    char dots[64];
    int n = strlen(v) < 63 ? strlen(v) : 63;
    memset(dots, '*', n);
    dl_text(fx + 20, y + 14, hidden ? dots : v, n, S(2), C_TXT, 0, 0, x1 - 10, SH);
    if (field_on) { int cw = tw(n, 2); dl_rect(fx + 22 + cw, y + 12, fx + 34 + cw, y + 38, 0, C_ACC); }
    hitbox(fx, y, x1, y + 50, H_INST, B_FIELD);
}

void inst_draw(int x0, int y0, int x1, int y1, int focused)
{
    (void)focused;
    int x = x0 + 24, y = y0 + TOP + 16, by = y1 - 66;
    dl_rect(x0 + 2, y0 + TOP, x1 - 2, y1 - 2, 0, C_PANEL);
    hitbox(x0, y0 + TOP, x1, y1, H_INST, 0);
    static const char *title[] = { "Root's password", "Choose where", "Confirm the drive", "What will change",
                                   "Installing", "Finished" };
    label(x, y, title[step], 3, C_ACCT);
    y += 56;
    if (step == I_AUTH) {
        label(x, y, "Installing changes this computer's drives: root's password is needed.", 2, C_DIM);
        field(x, y + 50, x1 - 24, "Password", pw, 1);
        button(x1 - 24 - tw(8, 2) - 28, by, "Continue", 1, B_OK);
    } else if (step == I_MODEL) {
        label(x, y, "ERASE the whole drive: everything on it is lost. Type its model:", 2, C_ERR);
        label(x, y + 40, models[sel_d], 2, C_TXT);
        field(x, y + 90, x1 - 24, "Model", model, 0);
        button(x, by, "Back", 0, B_BACK);
        button(x1 - 24 - tw(4, 2) - 28, by, "Next", 1, B_OK);
    } else {
        if (step == I_DRIVES && !nl && pid) label(x, y, "Looking at the drives...", 2, C_DIM);
        for (int i = 0; i < nl && y < by - LH; i++) {           /* the program's lines */
            int k = ln[i].kind;
            if (step == I_RUN && k == 'l') continue;
            if (k == 'd') { y += 8; label(x, y, ln[i].text + 1, 2, C_TXT); y += LH + 4; continue; }
            if (k == 'f') { button(x + 20, y, "Install here", 1, B_FREE + ln[i].d * 16 + ln[i].n); label(x + 40 + tw(12, 2) + 28, y + 10, ln[i].text, 2, C_TXT); y += 50; continue; }
            if (k == 'w') { button(x + 20, y, "Erase whole drive", 0, B_WHOLE + ln[i].d); y += 50; continue; }
            uint32_t c = k == 'e' ? C_ERR : k == 'k' ? C_ACCT : step == I_DRIVES ? C_DIM : C_TXT;
            dl_text(x + (step == I_DRIVES ? 20 : 0), y, ln[i].text, strlen(ln[i].text), SS, c, 0, 0, x1 - 20, SH);
            y += LH - 4;
        }
        if (step == I_RUN || step == I_DONE) {                  /* the bar */
            int bx1 = x1 - 24, yy = by - 40;
            dl_rect(x, yy, bx1, yy + 20, 10, C_FIELD);
            dl_rect(x, yy, x + (bx1 - x) * (pct > 100 ? 100 : pct) / 100, yy + 20, 10, err ? C_ERR : C_ACC);
        }
        if (step == I_PLAN) {
            button(x, by, "Back", 0, B_BACK);
            const char *s = sel_n ? "Install" : "Erase and install";
            if (!pid && !err) button(x1 - 24 - tw(strlen(s), 2) - 28, by, s, 1, B_INSTALL);
        }
        if (step == I_DONE) {
            int r = button(x, by, "Back", 0, B_BACK);
            if (!err) r = button(r + 16, by, "Restart", 1, B_RESTART);
            button(r + 16, by, "Close", 0, B_CLOSE);
        }
    }
    if (msg[0]) label(x, by - 50, msg, 2, C_ERR);
}

int inst_click(int kind, int arg)
{
    if (kind != H_INST) return 0;
    if (arg == B_FIELD) field_on = 1;
    else if (arg) go(arg);
    return 1;
}

int inst_key(int c)
{
    char *f = step == I_AUTH ? pw : step == I_MODEL ? model : 0;
    size_t cap = step == I_AUTH ? sizeof pw : sizeof model;
    if (c == '\n') { go(step == I_PLAN ? B_INSTALL : B_OK); return 1; }
    if (c == 27) { if (step == I_DRIVES || step == I_AUTH) return 0; go(B_BACK); return 1; }
    if (!f) return 0;
    size_t n = strlen(f);
    if (c == '\b') { if (n) f[n - 1] = 0; return 1; }
    if (c >= 32 && c < 127 && n + 1 < cap) { f[n] = c; f[n + 1] = 0; return 1; }
    return 0;
}
