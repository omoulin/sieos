/*
 * mail.c - The Mail window (docs/desktop.md): folders, the messages, the
 * one open, writing one, the account. The talking is done by /bin/mail
 * (user/mail), started as the logged-in user on the console "atlas#1000":
 * this window sends it a command a line (CON_READ's answer) and reads
 * its answer (CON_WRITE) up to a line of one \x1e byte. So the desktop
 * never waits on a server and links no TLS, and what comes from the
 * network is parsed in a process of the user's, not in the desktop's.
 *
 *   ┌─────────────────────────────────────────────────────────────────┐
 *   │ [Check] [New] [Reply] [Forward] [Delete]              [Account] │
 *   ├ FOLDERS ──────┬─ FROM ───── SUBJECT ─────────────────── DATE ───┤
 *   │ INBOX         │ ● Alice     Lunch on Friday        2026-10-16   │ unread: bold
 *   │ Sent          │   Bob       Quarterly report       2026-10-15   │
 *   │ Trash         ├─────────────────────────────────────────────────┤
 *   │               │ From: Alice <alice@example.org>   (the one open)│
 *   │               │ Subject: Lunch on Friday                        │
 *   │               │ The text, wrapped to the window...              │
 *   ├───────────────┴─────────────────────────────────────────────────┤
 *   │ INBOX   4 messages, 3 unread                                    │ status (inverse)
 *   └─────────────────────────────────────────────────────────────────┘
 *
 * The account (~/.mail, only the user's to read) is asked the first time.
 * Text is shown in ASCII (the desktop's font): accented letters lose
 * their accents, other characters show as '?'.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"
#include "mk/crypto.h"                   /* wipe: the password */

#define TOP 48                           /* the insides start this far below the y0 we are given */
#define BAR 54                           /* the buttons */
#define ROW 34                           /* a message in the list */
#define FROW 30                          /* a folder */
#define STATUS 32
#define MAXF 24
#define MAXM 100
#define QN 8                             /* commands waiting for the helper */
#define TMAX (256 << 10)                 /* the open message's text, at most */
#define BMAX (32 << 10)                  /* what is written, at most */
enum { V_LIST, V_COMPOSE, V_ACCOUNT };
enum { M_BODY, M_TEXT, M_BTN = 100, M_OPT = 200, M_FIELD = 300, M_FOLDER = 400, M_ROW = 500 };
enum { B_CHECK, B_NEW, B_REPLY, B_FWD, B_DELETE, B_ACCOUNT, B_SEND, B_CANCEL, B_SAVE, NB };
enum { O_IMAP, O_POP, O_KEEP, O_NOKEEP };
enum { F_NAME, F_ADDR, F_IHOST, F_IPORT, F_OHOST, F_OPORT, F_USER, F_PASS, F_TO, F_CC, F_SUBJ, NF, F_BODY = NF };
static const char *const bname[NB] = { "Check", "New", "Reply", "Forward", "Delete", "Account", "Send", "Cancel", "Save" };
static const char *const flabel[NF] = { "Your name", "Address", "Incoming", "Port", "Outgoing", "Port",
                                        "User name", "Password", "To", "Cc", "Subject" };
static const char *const keys_[] = { "name", "address", "in_host", "in_port", "out_host", "out_port", "user", "pass" };

static int view, on, pop, keep = 1, field = -1, has_acct;
static char fld[NF][160], folder[64] = "INBOX", status[120];
static int status_err;
static char folders[MAXF][64];
static int nfold;
static struct { char id[72], date[17], from[64], subj[128]; int seen; } msg[MAXM];
static int nmsg, sel = -1, lscroll, tscroll;
static char *text, irt[200];             /* the message open (the helper's text); its Message-ID */
static int tlen;
static char *body;                       /* what is written */
static int blen, bcur, bscroll, brows;
static int X0, Y0, X1, Y1;

/* ---- The helper, /bin/mail -S, and what it is asked. */
static int pid;
static long reader;                      /* its CON_READ, waiting for a command */
static size_t want;
static char q[QN][240];
static int nq, busy;
static char *resp;
static size_t rlen, rcap;
static char tail[3];                     /* the last bytes it wrote: the answer's end? */

static void set_status(const char *s, int err) { strlcpy(status, s, sizeof status); status_err = err; }
int mail_on(void) { return on; }

static void path_of(char *p, const char *name) { strlcpy(p + strlcpy(p, atlas_home(), 96), name, 32); }
static void own(const char *p)           /* (made by the desktop, root: the user's) */
{
    acct_user_t u;
    if (!acct_user(0, atlas_uid(), &u)) fs_chown(p, u.uid, u.gid);
}
static const char *find(const char *s, const char *w)   /* w in s, or 0 */
{
    for (size_t n = strlen(w); *s; s++) if (!memcmp(s, w, n)) return s;
    return 0;
}
static int has_line(const char *t, const char *l)
{
    for (size_t n = strlen(l); t && *t; t = strchr(t, '\n'), t = t ? t + 1 : 0) if (!memcmp(t, l, n) && (t[n] == '\n' || !t[n])) return 1;
    return 0;
}

static void helper(void)
{
    acct_user_t u;
    if (pid > 0 || acct_user(0, atlas_uid(), &u)) return;
    mk_spawn_t o = { .flags = SPAWN_CONSOLE };
    char *c = o.console + strlcpy(o.console, "atlas#", sizeof o.console);
    c[num(c, MAIL_CHAN)] = 0;
    long p = spawn_file("/bin/mail", "mail -S", u.uid, u.gid, &o);
    if (p < 0) { set_status("Cannot start /bin/mail.", 1); nq = 0; return; }
    pid = (int)p; reader = 0; busy = 0; rlen = 0;
    say("atlas: mail: helper started (pid %d)\n", pid);
}
static void pump(void)                   /* the next command, when the helper asks for one */
{
    if (!nq) return;
    helper();
    if (busy || !reader) return;
    char line[244];
    size_t n = strlcpy(line, q[0], sizeof line - 1);
    line[n++] = '\n';
    if (n > want) n = want;
    msg_t m = { .w = { n }, .sbuf = line, .slen = n };
    ipc_reply(reader, &m);
    reader = 0; busy = 1; rlen = 0;
    memset(tail, 0, sizeof tail);
}
static void ask(const char *a, const char *b, const char *c)   /* a command, its words (b, c: or 0) */
{
    if (nq == QN) return;
    char *p = q[nq++];
    size_t n = strlcpy(p, a, 240);
    if (b) { p[n++] = '\t'; n += strlcpy(p + n, b, 240 - n); }
    if (c && n < 239) { p[n++] = '\t'; strlcpy(p + n, c, 240 - n); }
    if (!busy) set_status(!strcmp(a, "send") ? "Sending..." : "Checking...", 0);
    pump();
}

/* UTF-8 into the desktop's ASCII, in place: accents dropped, the rest '?'. */
static size_t fold(char *s, size_t n)
{
    static const char l1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    size_t o = 0;
    for (size_t i = 0; i < n; ) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x80) { s[o++] = (char)c; i++; continue; }
        int k = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        unsigned u = c & (0x3f >> k);
        for (int j = 1; j <= k && i + j < n; j++) u = u << 6 | ((uint8_t)s[i + j] & 63);
        i += k + 1;
        s[o++] = u >= 0xc0 && u <= 0xff ? l1[u - 0xc0] : u == 0xa0 ? ' ' : u == 0x2018 || u == 0x2019 ? '\'' :
                 u == 0x201c || u == 0x201d ? '"' : u == 0x2013 || u == 0x2014 ? '-' : '?';
    }
    return o;
}

/* The next tab-separated word of a line (cut in place). */
static char *word(char **p)
{
    char *w = *p, *e = w;
    while (*e && *e != '\t' && *e != '\n') e++;
    *p = *e ? e + 1 : e;
    *e = 0;
    return w;
}
static const char *hdr(const char *name, char *out, size_t cap)   /* a header line of the open message */
{
    size_t nl = strlen(name);
    out[0] = 0;
    for (const char *p = text; p && *p && *p != '\n'; p = strchr(p, '\n'), p = p ? p + 1 : 0)
        if (!memcmp(p, name, nl) && p[nl] == ':') {
            size_t i = 0;
            for (p += nl + 2; *p && *p != '\n' && i + 1 < cap; ) out[i++] = *p++;
            out[i] = 0;
            break;
        }
    return out;
}

static int counts(void)                  /* the status: "INBOX   4 messages, 1 unread" -> unread */
{
    int u = 0;
    for (int i = 0; i < nmsg; i++) u += !msg[i].seen;
    char *s = status + strlcpy(status, folder, sizeof status);
    s += strlcpy(s, "   ", 4); s += num(s, nmsg);
    s += strlcpy(s, " messages, ", 12); s += num(s, u); strlcpy(s, " unread", 8);
    status_err = 0;
    return u;
}

/* An answer from the helper: what the command asked, then the next one. */
static void answer(void)
{
    if (!resp && !(resp = malloc(rcap = 16))) { rcap = 0; nq = busy = 0; return; }
    if (rlen >= 2 && resp[rlen - 1] == '\n' && resp[rlen - 2] == '\x1e') rlen -= 2;
    resp[rlen] = 0;
    rlen = fold(resp, rlen);
    resp[rlen] = 0;
    char *c = q[0], *a = word(&c), *b = word(&c), *err = 0;
    for (char *p = resp; *p; p = strchr(p, '\n'), p = p ? p + 1 : 0) if (!memcmp(p, "error: ", 7)) { err = p + 7; break; }
    if (err) { char *e = strchr(err, '\n'); if (e) *e = 0; err[0] = err[0] >= 'a' && err[0] <= 'z' ? err[0] - 32 : err[0]; }
    if (!strcmp(a, "folders") && !err) {
        nfold = 0;
        for (char *p = resp; *p && nfold < MAXF; ) strlcpy(folders[nfold++], word(&p), 64);
        say("atlas: mail: folders: %d\n", nfold);
    } else if (!strcmp(a, "list")) {
        char keep_id[72];
        strlcpy(keep_id, sel >= 0 && sel < nmsg ? msg[sel].id : "", sizeof keep_id);
        nmsg = 0; sel = -1;
        for (char *p = resp; *p && nmsg < MAXM && memcmp(p, "error: ", 7); ) {
            char *l = p, *e = strchr(p, '\n');
            p = e ? e + 1 : p + strlen(p);
            if (e) *e = 0;
            strlcpy(msg[nmsg].id, word(&l), 72);
            msg[nmsg].seen = *word(&l) != '*';
            strlcpy(msg[nmsg].date, word(&l), 17);
            strlcpy(msg[nmsg].from, word(&l), 64);
            strlcpy(msg[nmsg].subj, word(&l), 128);
            if (!strcmp(msg[nmsg].id, keep_id)) sel = nmsg;
            nmsg++;
        }
        int u = counts();
        say("atlas: mail: list %s: %d messages, %d unread\n", b, nmsg, u);
    } else if (!strcmp(a, "read") && !err) {
        free(text);
        tlen = rlen < TMAX ? (int)rlen : TMAX;
        text = malloc(tlen + 1);
        if (text) { memcpy(text, resp, tlen); text[tlen] = 0; } else tlen = 0;
        tscroll = 0;
        if (sel >= 0 && sel < nmsg) msg[sel].seen = 1;
        int na = 0;
        for (char *p = text; p && *p && *p != '\n'; p = strchr(p, '\n'), p = p ? p + 1 : 0) na += !memcmp(p, "Attachment:", 11);
        hdr("Message-ID", irt, sizeof irt);
        say("atlas: mail: read %s: %d bytes, %d attachments\n", b, tlen, na);
        counts();
    } else if (!strcmp(a, "delete") && !err) {
        say("atlas: mail: deleted %s\n", b);
        free(text); text = 0; tlen = 0; sel = -1;
        set_status("Deleted.", 0);
    } else if (!strcmp(a, "send") && !err) {
        char p[96];
        path_of(p, "/.mail-draft");
        fs_unlink(p);
        view = V_LIST; field = -1;
        say("atlas: mail: sent\n");
        set_status("Sent.", 0);
    }
    if (err) { set_status(err, 1); say("atlas: mail: %s: error: %s\n", a, err); }
    int relist = !strcmp(a, "delete") && !err;
    memmove(q[0], q[1], (QN - 1) * sizeof q[0]);
    nq--; busy = 0;
    if (relist) ask("list", folder, 0);
    pump();
}

/* CON_WRITE / CON_READ from the helper (atlas.c: its console's channel). */
int mail_console(long from, msg_t *m, const char *data)
{
    if (m->w[0] == CON_READ) { reader = from; want = m->w[1]; pump(); return 0; }
    if (m->w[0] != CON_WRITE) { reply_val(from, m->w[0] == CON_ECHO ? 0 : -ENOSYS); return 0; }
    size_t n = m->rlen;
    reply_val(from, n);
    if (rlen + n + 1 > rcap && rlen + n < 2 * TMAX) {
        size_t c = rcap ? rcap * 2 : 16384;
        while (c < rlen + n + 1) c *= 2;
        char *r = realloc(resp, c);
        if (r) { resp = r; rcap = c; }
    }
    if (rlen + n + 1 <= rcap) { memcpy(resp + rlen, data, n); rlen += n; resp[rlen] = 0; }   /* (too long: the rest dropped) */
    for (size_t i = n > 3 ? n - 3 : 0; i < n; i++) { tail[0] = tail[1]; tail[1] = tail[2]; tail[2] = data[i]; }
    if (!busy || tail[1] != '\x1e' || tail[2] != '\n' || (tail[0] && tail[0] != '\n')) return 0;
    answer();
    return 1;
}
void mail_ended(int p)
{
    if (p != pid) return;
    pid = 0; reader = 0; busy = 0; nq = 0;
    if (on) { set_status("The mail program stopped.", 1); redraw(); }
}

/* ---- The account: ~/.mail. */
static void acct_load(void)
{
    char p[96];
    path_of(p, "/.mail");
    char *t = file_get(p, 0);
    has_acct = t != 0;
    for (int f = 0; f <= F_PASS; f++) {
        fld[f][0] = 0;
        size_t kl = strlen(keys_[f]);
        for (const char *l = t; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : 0)
            if (!memcmp(l, keys_[f], kl) && l[kl] == '=') {
                size_t i = 0;
                for (l += kl + 1; *l && *l != '\n' && i + 1 < sizeof fld[0]; ) fld[f][i++] = *l++;
                fld[f][i] = 0;
                break;
            }
    }
    pop = has_line(t, "in=pop3");
    keep = !has_line(t, "keep=0");
    if (t) wipe(t, strlen(t));
    free(t);
    if (!fld[F_IPORT][0]) strlcpy(fld[F_IPORT], pop ? "995" : "993", sizeof fld[0]);
    if (!fld[F_OPORT][0]) strlcpy(fld[F_OPORT], "465", sizeof fld[0]);
}
static void acct_save(void)
{
    if (!strchr(fld[F_ADDR], '@') || !fld[F_IHOST][0] || !fld[F_OHOST][0]) { set_status("Give an address and both servers.", 1); return; }
    if (strnum(fld[F_IPORT]) <= 0 || strnum(fld[F_OPORT]) <= 0) { set_status("A port is a number.", 1); return; }
    if (!fld[F_USER][0]) strlcpy(fld[F_USER], fld[F_ADDR], sizeof fld[0]);
    static char t[1600];
    size_t n = 0;
    for (int f = 0; f <= F_PASS; f++) {
        n += strlcpy(t + n, keys_[f], sizeof t - n); t[n++] = '=';
        n += strlcpy(t + n, fld[f], sizeof t - n); t[n++] = '\n';
        if (f == F_ADDR) n += strlcpy(t + n, pop ? "in=pop3\n" : "in=imap\n", sizeof t - n);
    }
    n += strlcpy(t + n, keep ? "keep=1\n" : "keep=0\n", sizeof t - n);
    char p[96];
    path_of(p, "/.mail");
    long h = fs_open(p, FS_WRONLY | FS_CREAT | FS_TRUNC, 0600), r = h;
    if (h >= 0) { fs_chmod(p, 0600); own(p); r = fs_write(h, 0, t, n); fs_close(h); }
    wipe(t, sizeof t);
    if (r < 0) { set_status("Could not save the account.", 1); return; }
    say("atlas: mail: account saved (%s)\n", pop ? "pop3" : "imap");
    has_acct = 1; view = V_LIST; field = -1;
    nfold = nmsg = 0; sel = -1; free(text); text = 0; tlen = 0;
    strlcpy(folder, "INBOX", sizeof folder);
    ask("folders", 0, 0);
    ask("list", folder, 0);
}

/* ---- Writing: new, reply, forward; sent through a draft file. */
static void compose(int how)                             /* 0 new, 1 reply, 2 forward */
{
    char from[160], subj[160], date[64], to[160];
    if (!body && !(body = malloc(BMAX + 1))) return;
    blen = bcur = bscroll = 0;
    for (int f = F_TO; f <= F_SUBJ; f++) fld[f][0] = 0;
    if (how && text) {
        hdr("From", from, sizeof from); hdr("Subject", subj, sizeof subj); hdr("Date", date, sizeof date); hdr("To", to, sizeof to);
        int re = how == 1 ? !memcmp(subj, "Re:", 3) || !memcmp(subj, "RE:", 3) : !memcmp(subj, "Fwd:", 4);
        char *s = fld[F_SUBJ] + strlcpy(fld[F_SUBJ], re ? "" : how == 1 ? "Re: " : "Fwd: ", sizeof fld[0]);
        strlcpy(s, subj, sizeof fld[0] - (s - fld[F_SUBJ]));
        if (how == 1) strlcpy(fld[F_TO], from, sizeof fld[0]);
        char *b = body, *e = body + BMAX;
        b += strlcpy(b, "\n\n", e - b);
        if (how == 1) { b += strlcpy(b, "On ", e - b); b += strlcpy(b, date, e - b); b += strlcpy(b, ", ", e - b);
                        b += strlcpy(b, from, e - b); b += strlcpy(b, " wrote:\n", e - b); }
        else { b += strlcpy(b, "---------- Forwarded message ----------\nFrom: ", e - b); b += strlcpy(b, from, e - b);
               b += strlcpy(b, "\nDate: ", e - b); b += strlcpy(b, date, e - b); b += strlcpy(b, "\nSubject: ", e - b);
               b += strlcpy(b, subj, e - b); b += strlcpy(b, "\nTo: ", e - b); b += strlcpy(b, to, e - b); b += strlcpy(b, "\n\n", e - b); }
        const char *t = find(text, "\n\n");
        for (t = t ? t + 2 : text + tlen; *t && b + 4 < e; ) {   /* the text, quoted (reply) */
            if (how == 1) b += strlcpy(b, *t == '>' ? ">" : "> ", e - b);
            while (*t && *t != '\n' && b + 1 < e) *b++ = *t++;
            if (*t == '\n' && b + 1 < e) *b++ = *t++;
        }
        blen = b - body;
    }
    if (how != 1) irt[0] = 0;
    body[blen] = 0;
    view = V_COMPOSE;
    field = how == 1 ? F_BODY : F_TO;
    status[0] = 0;
    say("atlas: mail: compose (%s)\n", how == 1 ? "reply" : how == 2 ? "forward" : "new");
}
static void send(void)
{
    if (!fld[F_TO][0]) { set_status("Who is it to?", 1); field = F_TO; return; }
    char p[96];
    path_of(p, "/.mail-draft");
    long h = fs_open(p, FS_WRONLY | FS_CREAT | FS_TRUNC, 0600), r = h;
    if (h >= 0) {
        static char t[1024];
        size_t n = 0;
        static const char *const hn[] = { "To: ", "Cc: ", "Subject: " };
        for (int f = F_TO; f <= F_SUBJ; f++) { n += strlcpy(t + n, hn[f - F_TO], sizeof t - n); n += strlcpy(t + n, fld[f], sizeof t - n); t[n++] = '\n'; }
        if (irt[0]) { n += strlcpy(t + n, "In-Reply-To: ", sizeof t - n); n += strlcpy(t + n, irt, sizeof t - n); t[n++] = '\n'; }
        t[n++] = '\n';
        fs_chmod(p, 0600); own(p);
        r = fs_write(h, 0, t, n);
        if (r >= 0) r = fs_write(h, n, body, blen);
        fs_close(h);
    }
    if (r < 0) { set_status("Could not write the draft.", 1); return; }
    ask("send", p, 0);
}

/* ---- Opening and closing. */
void mail_open(void)
{
    if (on) return;
    on = 1;
    acct_load();
    view = has_acct ? V_LIST : V_ACCOUNT;
    field = has_acct ? -1 : F_NAME;
    nfold = nmsg = 0; sel = -1; lscroll = 0;
    strlcpy(folder, "INBOX", sizeof folder);
    set_status(has_acct ? "" : "Your mail account, once: it is kept in ~/.mail.", 0);
    if (has_acct) { ask("folders", 0, 0); ask("list", folder, 0); }
    else say("atlas: mail: account form\n");
}
void mail_close(void)
{
    if (pid > 0) sys_kill(pid);
    pid = 0; reader = 0; nq = busy = 0; on = 0;
    free(text); text = 0; tlen = 0;
    free(body); body = 0; blen = 0;
    free(resp); resp = 0; rlen = rcap = 0;
    wipe(fld, sizeof fld);
}
void mail_info(char *t, size_t cap)
{
    strlcpy(t, view == V_ACCOUNT ? "account" : view == V_COMPOSE ? "new message" : folder, cap);
}

/* ---- Drawing. */
static void button(int *x, int y, int b, int lit)
{
    int x1 = pill(*x, y, bname[b], lit, H_MAIL, M_BTN + b);
    if (atlas_debug) say("atlas: dbg mailbtn %s at %d %d\n", bname[b], (*x + x1) / 2, y + 21);
    *x = x1 + 8;
}
static void tfield(int x, int y, int xe, int f)
{
    int lit = field == f, n = strlen(fld[f]), chars = (xe - x - 32) / KW;
    char st[64];
    const char *t = fld[f];
    if (f == F_PASS) { n = n < 63 ? n : 63; memset(st, '*', n); t = st; }
    int from = n > chars ? n - chars : 0;
    dl_rect(x, y, xe, y + 44, 10, C_FIELD);
    dl_frame(x, y, xe, y + 44, 10, lit ? 2 : 1, lit ? C_ACC : C_LINE);
    dl_text(x + 14, y + 11, t + from, n - from, S(2), C_TXT, x, y, xe - 10, y + 44);
    if (lit) { int w = tw(n - from, 2); dl_rect(x + 16 + w, y + 10, x + 26 + w, y + 34, 0, C_ACC); }
    hitbox(x, y, xe, y + 44, H_MAIL, M_FIELD + f);
    if (atlas_debug) say("atlas: dbg mailfield %d at %d %d\n", f, (x + xe) / 2, y + 22);
}
static void option(int *x, int y, const char *s, int lit, int o)
{
    int x1 = pill(*x, y, s, lit, H_MAIL, M_OPT + o);
    if (atlas_debug) say("atlas: dbg mailopt %s at %d %d\n", s, (*x + x1) / 2, y + 21);
    *x = x1 + 8;
}

static void account_draw(int x0, int y0, int x1, int y1)
{
    dl_rect(x0, y0, x1, y1, 0, C_PANEL);
    int lx = x0 + 24, cx = x0 + 24 + tw(10, 2), y = y0 + 16, x;
    dl_text(lx, y, "ACCOUNT", 7, ST, C_ACCT, 0, 0, SW, SH);
    dl_text(lx + 1, y, "ACCOUNT", 7, ST, C_ACCT, 0, 0, SW, SH);
    y += 50;
    for (int f = F_NAME; f <= F_PASS && y + 44 < y1; f++) {
        if (f == F_IHOST) {                              /* incoming: IMAP or POP3 */
            label(lx, y + 10, "Receive", 2, C_DIM);
            x = cx; option(&x, y, "IMAP", !pop, O_IMAP); option(&x, y, "POP3", pop, O_POP);
            if (pop) { label(x + 16, y + 10, "Leave on server", 2, C_DIM); x += 16 + tw(16, 2);
                       option(&x, y, "Yes", keep, O_KEEP); option(&x, y, "No", !keep, O_NOKEEP); }
            y += 54;
        }
        int port = f == F_IPORT || f == F_OPORT;
        if (port) continue;
        label(lx, y + 10, flabel[f], 2, C_DIM);
        int xe = x1 - 24, has_port = f == F_IHOST || f == F_OHOST;
        if (has_port) { xe -= tw(6, 2) + 28; tfield(xe + 12, y, x1 - 24, f + 1); }
        tfield(cx, y, xe, f);
        y += 54;
    }
}

static void list_draw(int x0, int y0, int x1, int y1)
{
    dl_rect(x0, y0, x1, y1, 0, C_TERM);
    hitbox(x0, y0, x1, y1, H_MAIL, M_BODY);
    int dw = text_w(16, SS) + 24, fw = (x1 - x0) * 26 / 100, fx = x0 + 34, sx = fx + fw, dx = x1 - dw;
    dl_text(fx, y0 + 8, "FROM", 4, SS, C_DIM, 0, 0, SW, SH);
    dl_text(sx, y0 + 8, "SUBJECT", 7, SS, C_DIM, 0, 0, SW, SH);
    dl_text(dx, y0 + 8, "DATE", 4, SS, C_DIM, 0, 0, SW, SH);
    int ly = y0 + 30, n = (y1 - ly - 4) / ROW;
    dl_rect(x0 + 10, ly - 1, x1 - 10, ly, 0, C_LINE);
    if (lscroll > nmsg - n) lscroll = nmsg - n;
    if (lscroll < 0) lscroll = 0;
    if (!nmsg) {
        const char *m = busy || nq ? "Checking..." : "No messages";
        label((x0 + x1) / 2 - tw(strlen(m), 2) / 2, (ly + y1) / 2 - KH / 2, m, 2, C_DIM);
    }
    for (int r = 0; r < n && lscroll + r < nmsg; r++) {
        int i = lscroll + r, y = ly + 2 + r * ROW, b = !msg[i].seen;
        if (i == sel) dl_rect(x0 + 6, y, x1 - 6, y + ROW - 2, 6, C_SEL);
        else if (r & 1) dl_rect(x0 + 6, y, x1 - 6, y + ROW - 2, 6, C_GLASS);
        if (b) dl_rect(x0 + 14, y + 12, x0 + 22, y + 20, 4, C_ACC);   /* unread: a dot, and bold */
        for (int k = 0; k <= b; k++) {
            dl_text(fx + k, y + 6, msg[i].from, strlen(msg[i].from), S(2), C_TXT, x0, y, sx - 16, y + ROW);
            dl_text(sx + k, y + 6, msg[i].subj, strlen(msg[i].subj), S(2), C_TXT, x0, y, dx - 16, y + ROW);
        }
        dl_text(dx, y + 8, msg[i].date, strlen(msg[i].date), SS, C_DIM, 0, 0, x1 - 8, SH);
        hitbox(x0 + 6, y, x1 - 6, y + ROW - 2, H_MAIL, M_ROW + i);
        if (atlas_debug) say("atlas: dbg mailrow %s at %d %d %s from=%s subj=%s\n", msg[i].id, sx + 40, y + ROW / 2, b ? "new" : "seen", msg[i].from, msg[i].subj);
    }
}

/* A line of text from p, at most cols characters, broken at a space if it
 * can be -> where the next one starts; *n: its length. */
static const char *wrap(const char *p, int cols, int *n)
{
    int k = 0, sp = -1;
    while (p[k] && p[k] != '\n' && k < cols) { if (p[k] == ' ') sp = k; k++; }
    if (!p[k] || p[k] == '\n') { *n = k; return p[k] ? p + k + 1 : p + k; }
    if (sp > 0) { *n = sp; return p + sp + 1; }
    *n = k;
    return p + k;
}

static void read_draw(int x0, int y0, int x1, int y1)
{
    dl_rect(x0, y0, x1, y1, 0, C_TERM);
    hitbox(x0, y0, x1, y1, H_MAIL, M_TEXT);
    if (!text) {
        const char *m = nmsg ? "Click a message to read it" : "";
        label((x0 + x1) / 2 - tw(strlen(m), 2) / 2, (y0 + y1) / 2 - KH / 2, m, 2, C_DIM);
        return;
    }
    int y = y0 + 10, lx = x0 + 20, vx = lx + tw(9, 2);
    static const char *const hs[] = { "From", "To", "Cc", "Date", "Subject" };
    char v[200];
    for (int i = 0; i < 5; i++) {                        /* the headers (the subject in bold) */
        if (!hdr(hs[i], v, sizeof v)[0] && i != 4) continue;
        label(lx, y, hs[i], 2, C_DIM);
        for (int k = 0; k <= (i == 4); k++) dl_text(vx + k, y, v, strlen(v), S(2), C_TXT, x0, y0, x1 - 10, y1);
        y += 28;
    }
    for (const char *p = text; *p && *p != '\n'; ) {     /* the attachments: names only */
        const char *e = strchr(p, '\n');
        if (!memcmp(p, "Attachment: ", 12)) {
            icon(I_FILE, lx, y + 2, 2);
            dl_text(lx + 34, y, p + 12, (e ? e : p + strlen(p)) - p - 12, S(2), C_ACCT, x0, y0, x1 - 10, y1);
            y += 28;
        }
        p = e ? e + 1 : p + strlen(p);
    }
    y += 6;
    dl_rect(x0 + 10, y, x1 - 10, y + 1, 0, C_LINE);
    y += 10;
    const char *p = find(text, "\n\n");
    p = p ? p + 2 : text + tlen;
    int cols = (x1 - lx - 20) / KW, rows = (y1 - y - 4) / KH, n;
    if (cols < 10) cols = 10;
    for (int r = 0; r < tscroll && *p; r++) p = wrap(p, cols, &n);   /* (scrolled) */
    if (!*p && tscroll > 0) tscroll--;
    for (int r = 0; r < rows && *p; r++) {
        const char *s = p;
        p = wrap(p, cols, &n);
        if (n) dl_text(lx, y + r * KH, s, n, S(K), s[0] == '>' ? C_DIM : C_TERMFG, x0, y0, x1 - 8, y1);
    }
}

static void compose_draw(int x0, int y0, int x1, int y1, int focused)
{
    dl_rect(x0, y0, x1, y1, 0, C_PANEL);
    int lx = x0 + 20, cx = lx + tw(8, 2), y = y0 + 10;
    for (int f = F_TO; f <= F_SUBJ; f++) { label(lx, y + 10, flabel[f], 2, C_DIM); tfield(cx, y, x1 - 20, f); y += 52; }
    int ty = y + 4, cols = (x1 - x0 - 40) / KW;
    if (cols < 10) cols = 10;
    brows = (y1 - ty - 8) / KH;
    if (brows < 1) brows = 1;
    dl_rect(x0 + 10, ty - 4, x1 - 10, y1 - 4, 8, C_TERM);
    hitbox(x0 + 10, ty - 4, x1 - 10, y1 - 4, H_MAIL, M_TEXT);
    int crow = -1, ccol = 0;                             /* where the cursor is: rows cut at cols */
    for (int i = 0, r = 0; ; r++) {
        int s = i, c = 0;
        while (i < blen && body[i] != '\n' && c < cols) { i++; c++; }
        if (crow < 0 && bcur >= s && bcur <= i && !(bcur == i && c == cols && i < blen && body[i] != '\n')) { crow = r; ccol = bcur - s; }
        if (i < blen && body[i] == '\n') i++;
        else if (i >= blen) break;
    }
    if (crow < 0) crow = 0;
    if (crow < bscroll) bscroll = crow;
    if (crow >= bscroll + brows) bscroll = crow - brows + 1;
    for (int i = 0, r = 0; i < blen && r < bscroll + brows; ) {   /* the rows */
        int s = i, c = 0;
        while (i < blen && body[i] != '\n' && c < cols) { i++; c++; }
        if (r >= bscroll && c) dl_text(x0 + 20, ty + (r - bscroll) * KH, body + s, c, S(K), body[s] == '>' ? C_DIM : C_TERMFG, x0, ty, x1 - 12, y1 - 6);
        if (i < blen && body[i] == '\n') i++;
        r++;
    }
    if (focused && field == F_BODY && crow >= bscroll) {
        int cxp = x0 + 20 + ccol * KW, cyp = ty + (crow - bscroll) * KH;
        dl_rect(cxp, cyp, cxp + KW, cyp + KH, 0, C_ACC);
        if (bcur < blen && body[bcur] != '\n') dl_text(cxp, cyp, body + bcur, 1, S(K), C_INK, x0, ty, x1, y1);
    }
    if (atlas_debug) say("atlas: dbg mailbody at %d %d\n", (x0 + x1) / 2, (ty + y1) / 2);
}

void mail_draw(int x0, int y0, int x1, int y1, int focused)
{
    X0 = x0 + 2; Y0 = y0 + TOP; X1 = x1 - 2; Y1 = y1 - 2;
    hitbox(X0, Y0, X1, Y1, H_MAIL, M_BODY);
    int sy = Y1 - STATUS, x = X0 + 12, ty = Y0 + 6;
    dl_rect(X0, Y0, X1, Y0 + BAR, 0, C_PANEL);           /* the buttons */
    dl_rect(X0, Y0 + BAR - 1, X1, Y0 + BAR, 0, C_LINE);
    if (view == V_LIST) {
        for (int b = B_CHECK; b <= B_DELETE; b++) button(&x, ty, b, 0);
        x = X1 - 12 - tw(7, 2) - 28;
        if (x > X0 + 12) button(&x, ty, B_ACCOUNT, 0);
    } else if (view == V_COMPOSE) { button(&x, ty, B_SEND, 1); button(&x, ty, B_CANCEL, 0); }
    else { button(&x, ty, B_SAVE, 1); if (has_acct) button(&x, ty, B_CANCEL, 0); }
    int top = Y0 + BAR, fw = X1 - X0 < 700 ? 0 : (X1 - X0) * 20 / 100;   /* the folders */
    if (fw > 300) fw = 300;
    int rx = X0;
    if (fw) {
        dl_rect(X0, top, X0 + fw, sy, 0, C_PANEL);
        dl_rect(X0 + fw, top, X0 + fw + 1, sy, 0, C_LINE);
        dl_text(X0 + 14, top + 12, "FOLDERS", 7, SS, C_DIM, 0, 0, SW, SH);
        for (int i = 0, y = top + 40; i < nfold && y + FROW < sy; i++, y += FROW) {
            int cur = !strcmp(folders[i], folder);
            if (cur) dl_rect(X0 + 2, y, X0 + fw - 2, y + FROW, 0, C_SEL);
            for (int k = 0; k <= cur; k++) dl_text(X0 + 14 + k, y + 4, folders[i], strlen(folders[i]), S(2), cur ? C_TXT : C_DIM, X0, y, X0 + fw - 6, y + FROW);
            hitbox(X0, y, X0 + fw, y + FROW, H_MAIL, M_FOLDER + i);
            if (atlas_debug) say("atlas: dbg mailfolder %s at %d %d\n", folders[i], X0 + 60, y + FROW / 2);
        }
        rx = X0 + fw + 1;
    }
    if (view == V_ACCOUNT) account_draw(rx, top, X1, sy);
    else if (view == V_COMPOSE) compose_draw(rx, top, X1, sy, focused);
    else {
        int ly1 = top + (sy - top) * 40 / 100;
        list_draw(rx, top, X1, ly1);
        dl_rect(rx, ly1, X1, ly1 + 1, 0, C_LINE);
        read_draw(rx, ly1 + 1, X1, sy);
    }
    dl_rect(X0, sy, X1, Y1, 0, status_err ? C_ERR : C_ACC);   /* the status bar: inverse */
    char t[160];
    t[0] = ' ';
    strlcpy(t + 1, status[0] ? status : view == V_COMPOSE ? "New message" : folder, sizeof t - 1);
    dl_text(X0 + 8, sy + 5, t, strlen(t), S(2), C_INK, X0, sy, X1 - 10, Y1);
    const char *k = view == V_LIST ? "Up/Down a message  Del delete " : view == V_COMPOSE ? "Tab next field " : "Tab next field  Enter save ";
    int kl = strlen(k), kx = X1 - 10 - tw(kl, 2);
    if (kx > X0 + 8 + tw(strlen(t), 2) + 30) dl_text(kx, sy + 5, k, kl, S(2), C_INK, X0, sy, X1, Y1);
}

/* ---- Input. */
static void open_msg(int i)
{
    if (i < 0 || i >= nmsg) return;
    sel = i;
    ask("read", msg[i].id, folder);
}
static void button_do(int b)
{
    switch (b) {
    case B_CHECK: ask("folders", 0, 0); ask("list", folder, 0); break;
    case B_NEW: compose(0); break;
    case B_REPLY: case B_FWD: if (text) compose(b == B_REPLY ? 1 : 2); else set_status("Open a message first.", 1); break;
    case B_DELETE: if (sel >= 0 && sel < nmsg) ask("delete", msg[sel].id, folder); else set_status("Pick a message first.", 1); break;
    case B_ACCOUNT: acct_load(); view = V_ACCOUNT; field = F_NAME; status[0] = 0; say("atlas: mail: account form\n"); break;
    case B_SEND: send(); break;
    case B_CANCEL: view = V_LIST; field = -1; status[0] = 0; if (has_acct) acct_load(); break;
    case B_SAVE: acct_save(); break;
    }
}
int mail_click(int kind, int arg)
{
    if (kind != H_MAIL) return 0;
    if (arg >= M_ROW) open_msg(arg - M_ROW);
    else if (arg >= M_FOLDER) {
        strlcpy(folder, folders[arg - M_FOLDER], sizeof folder);
        nmsg = 0; sel = -1; lscroll = 0; free(text); text = 0; tlen = 0;
        ask("list", folder, 0);
    } else if (arg >= M_FIELD) field = arg - M_FIELD;
    else if (arg >= M_OPT) {
        int o = arg - M_OPT;
        if (o <= O_POP && pop != (o == O_POP)) {          /* the usual port goes with it */
            pop = o == O_POP;
            const char *ip = fld[F_IPORT];
            if (!ip[0] || !strcmp(ip, "993") || !strcmp(ip, "995")) strlcpy(fld[F_IPORT], pop ? "995" : "993", sizeof fld[0]);
            else if (!strcmp(ip, "143") || !strcmp(ip, "110")) strlcpy(fld[F_IPORT], pop ? "110" : "143", sizeof fld[0]);
        } else if (o >= O_KEEP) keep = o == O_KEEP;
    } else if (arg >= M_BTN) button_do(arg - M_BTN);
    else if (arg == M_TEXT && view == V_COMPOSE) field = F_BODY;
    else if (view != V_ACCOUNT) field = -1;
    return 1;
}
void mail_wheel(int arg, int w)
{
    if (view == V_COMPOSE) { bscroll -= 3 * w; if (bscroll < 0) bscroll = 0; return; }
    if (arg == M_TEXT) { tscroll -= 3 * w; if (tscroll < 0) tscroll = 0; }
    else { lscroll -= 3 * w; if (lscroll < 0) lscroll = 0; }
}
static void body_key(int c)
{
    int col = bcur, ls;
    while (col > 0 && body[col - 1] != '\n') col--;
    ls = col; col = bcur - ls;
    if (c == KEY_LEFT) { if (bcur) bcur--; }
    else if (c == KEY_RIGHT) { if (bcur < blen) bcur++; }
    else if (c == KEY_HOME) bcur = ls;
    else if (c == KEY_END) { while (bcur < blen && body[bcur] != '\n') bcur++; }
    else if (c == KEY_UP || c == KEY_DOWN) {             /* the line before / after, same column if it can */
        int s = ls;
        if (c == KEY_UP) { if (!ls) return; s = ls - 1; while (s > 0 && body[s - 1] != '\n') s--; }
        else { s = bcur; while (s < blen && body[s] != '\n') s++; if (s == blen) return; s++; }
        bcur = s;
        for (int k = 0; k < col && bcur < blen && body[bcur] != '\n'; k++) bcur++;
    } else if (c == '\b' || c == KEY_DEL) {
        int at = c == '\b' ? bcur - 1 : bcur;
        if (at < 0 || at >= blen) return;
        memmove(body + at, body + at + 1, blen - at - 1);
        blen--; bcur = at;
    } else if ((c == '\n' || c == '\t' || (c >= 32 && c < 127)) && blen < BMAX) {
        memmove(body + bcur + 1, body + bcur, blen - bcur);
        body[bcur++] = c == '\t' ? ' ' : (char)c;
        blen++;
    }
    body[blen] = 0;
}
int mail_key(int c)
{
    if (view == V_LIST) {
        if (c == KEY_DOWN) open_msg(sel + 1 < nmsg ? sel + 1 : sel);
        else if (c == KEY_UP) open_msg(sel > 0 ? sel - 1 : 0);
        else if (c == KEY_DEL) button_do(B_DELETE);
        else if (c == KEY_PGDN) tscroll += 10;
        else if (c == KEY_PGUP) tscroll = tscroll > 10 ? tscroll - 10 : 0;
        else return 0;
        if (sel >= 0 && sel < lscroll) lscroll = sel;
        return 1;
    }
    if (c == 27) { if (view == V_COMPOSE || has_acct) button_do(B_CANCEL); return 1; }
    int first = view == V_COMPOSE ? F_TO : F_NAME, last = view == V_COMPOSE ? F_BODY : F_PASS;
    if (c == '\t' || c == KEY_BTAB) {                    /* the next field (or the one before) */
        field = field < first || field > last ? first : field + (c == '\t' ? 1 : -1);
        if (field > last) field = first;
        if (field < first) field = last;
        return 1;
    }
    if (field == F_BODY) { body_key(c); return 1; }
    if (field < 0) return 0;
    if (c == '\n') { if (view == V_ACCOUNT) acct_save(); else field = field == F_SUBJ ? F_BODY : field + 1; return 1; }
    char *f = fld[field];
    size_t n = strlen(f);
    if (c == '\b') { if (n) f[n - 1] = 0; }
    else if (c >= 32 && c < 127 && n + 1 < sizeof fld[0]) { f[n] = (char)c; f[n + 1] = 0; }
    else return 0;
    return 1;
}
