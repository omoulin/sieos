/*
 * mail.c - The Mail window (docs/desktop.md): the accounts' folders, the
 * messages, the one open, writing one, the accounts. The talking is done
 * by /bin/mail (user/mail), started as the logged-in user on the console
 * "atlas#1000": this window sends it a command a line (CON_READ's answer;
 * "-a N" first: the account) and reads its answer (CON_WRITE) up to a
 * line of one \x1e byte. So the desktop never waits on a server and links
 * no TLS, and what comes from the network is parsed in a process of the
 * user's, not in the desktop's.
 *
 *   ┌─────────────────────────────────────────────────────────────────┐
 *   │ [Check] [New] [Reply] [Reply all] [Forward] [Delete]  [Account] │
 *   ├ FAVORITES ────┬─ FROM ───── SUBJECT ─────────────────── DATE ───┤
 *   │ INBOX      3 *│ ● Alice     Lunch on Friday        2026-10-16   │ unread: bold
 *   │ alice@home    │   Bob       Quarterly report       2026-10-15   │
 *   │ Sent         *├─────────────────────────────────────────────────┤ * a favorite:
 *   │ Trash        *│ From: Alice <alice@example.org>   (the one open)│   at the top
 *   │ bob@work      │ [] report.pdf      (a click: into ~/Downloads)  │
 *   │ INBOX      1 *│ The text, wrapped to the window...              │
 *   ├───────────────┴─────────────────────────────────────────────────┤
 *   │ INBOX   4 messages, 3 unread                                    │ status (inverse)
 *   └─────────────────────────────────────────────────────────────────┘
 *
 * The accounts (~/.mail, only the user's to read; "[account]" lines between
 * them) are asked the first time. Files are attached by picking them in the
 * Files window. A server whose certificate no authority signed: its SHA-256
 * is shown, and pinned once accepted (accounts with "Self-signed" only).
 * Text is shown in ASCII (the desktop's font): accented letters lose their
 * accents, other characters show as '?'.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"
#include "mk/crypto.h"                   /* wipe: the passwords */

#define TOP 48                           /* the insides start this far below the y0 we are given */
#define BAR 54                           /* the buttons */
#define ROW 34                           /* a message in the list */
#define FROW 30                          /* a folder */
#define STATUS 32
#define BAND 88                          /* a certificate to accept: the question, over the status */
#define MAXA 8                           /* accounts */
#define MAXF 64
#define MAXM 300
#define NATT 8                           /* files attached */
#define QN 16                            /* commands waiting for the helper */
#define QL 320
#define TMAX (256 << 10)                 /* the open message's text, at most */
#define BMAX (32 << 10)                  /* what is written, at most */
enum { V_LIST, V_COMPOSE, V_ACCOUNT };
enum { M_BODY, M_TEXT, M_BTN = 100, M_OPT = 200, M_FIELD = 300, M_ACC = 340, M_HEAD = 360, M_ATT = 380, M_CATT = 400,
       M_FOLDER = 500, M_STAR = 600, M_ROW = 700 };
enum { B_CHECK, B_NEW, B_REPLY, B_ALL, B_FWD, B_DELETE, B_ACCOUNT, B_SEND, B_ATTACH, B_CANCEL, B_SAVE, B_REMOVE,
       B_ACCEPT, B_REFUSE, NB };
enum { F_NAME, F_ADDR, F_IHOST, F_IPORT, F_OHOST, F_OPORT, F_USER, F_PASS, F_TO, F_CC, F_SUBJ, NF, F_BODY = NF };
enum { O_POP, O_KEEP, O_SELF, O_SENT, NO };   /* an account's yes/no options; M_OPT + 2 o (+1: no) */
static const char *const bname[NB] = { "Check", "New", "Reply", "Reply all", "Forward", "Delete", "Account", "Send",
                                       "Attach", "Cancel", "Save", "Remove", "Accept", "Refuse" };
static const char *const flabel[NF] = { "Your name", "Address", "Incoming", "Port", "Outgoing", "Port",
                                        "User name", "Password", "To", "Cc", "Subject" };
static const char *const keys_[] = { "name", "address", "in_host", "in_port", "out_host", "out_port", "user", "pass",
                                     "in", "keep", "selfsigned", "sent", "in_pin", "out_pin", "fav" };

typedef struct { char f[F_TO][160], pin[2][65], fav[512]; int o[NO]; } acct_t;   /* fav: its ~/.mail lines, "fav=name\n" */
static acct_t ac[MAXA];
static int nac, edit, cur, from;         /* the accounts; the one edited, the one shown, the one writing */
static int view, on, field = -1;
static char fld[NF][160], folder[64] = "INBOX", status[200];
static int status_err;
static struct { char name[64]; short a, unseen; } fo[MAXF];
static int nfo;
static struct { char id[72], date[17], from[64], subj[128]; int seen; } msg[MAXM];
static int nmsg, sel = -1, lscroll, tscroll;
static char *text, irt[200];             /* the message open (the helper's text); its Message-ID */
static int tlen;
static char *body;                       /* what is written */
static int blen, bcur, bscroll, brows;
static char att[NATT][160];              /* the files attached to it */
static int natt, picking;                /* picking: Files' next double-click attaches */
static int pk = -1, pw;                  /* a certificate to accept: the account, its server (1: outgoing) */
static char pfp[100];                    /* its SHA-256 */
static int X0, Y0, X1, Y1;

/* ---- The helper, /bin/mail -S, and what it is asked. */
static int pid;
static long reader;                      /* its CON_READ, waiting for a command */
static size_t want;
static char q[QN][QL];
static int nq, busy;
static char *resp;
static size_t rlen, rcap;
static char tail[3];                     /* the last bytes it wrote: the answer's end? */

static void set_status(const char *s, int err) { strlcpy(status, s, sizeof status); status_err = err; }
int mail_on(void) { return on; }
static char *fv(int f) { return f < F_TO ? ac[edit].f[f] : fld[f]; }   /* a field's text */

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
static int ci(const char *a, const char *b, size_t n)   /* the same n characters, any case */
{
    for (size_t i = 0; i < n; i++) if ((a[i] | 32) != (b[i] | 32) || !a[i]) return 0;
    return 1;
}
static char *fav_at(int i, char *line)  /* folder i's "fav=name\n" in its account's, or 0 */
{
    char *f = ac[fo[i].a].fav, *p = f;
    strlcpy(line + strlcpy(line, "fav=", 72), fo[i].name, 66);
    strlcpy(line + strlen(line), "\n", 2);
    while ((p = (char *)find(p, line)) && p > f && p[-1] != '\n') p++;
    return p;
}
static int is_fav(int i) { char l[72]; return fav_at(i, l) != 0; }

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
    char line[QL + 4];
    size_t n = strlcpy(line, q[0], sizeof line - 1);
    line[n++] = '\n';
    if (n > want) n = want;
    msg_t m = { .w = { n }, .sbuf = line, .slen = n };
    ipc_reply(reader, &m);
    reader = 0; busy = 1; rlen = 0;
    memset(tail, 0, sizeof tail);
}
static void ask(int a, const char *c, const char *w1, const char *w2, const char *w3, const char *w4)   /* -a N c w... */
{
    if (nq == QN) return;
    const char *w[5] = { c, w1, w2, w3, w4 };
    char *p = q[nq++];
    int n = 3 + num(p + 3, a);
    memcpy(p, "-a\t", 3);
    for (int i = 0; i < 5 && w[i]; i++) { p[n++] = '\t'; n += strlcpy(p + n, w[i], QL - n); if (n > QL - 2) n = QL - 2; }
    p[n] = 0;
    if (!busy) set_status(!strcmp(c, "send") ? "Sending..." : "Checking...", 0);
    pump();
}
static void check(void)                  /* every account's folders, and the folder shown */
{
    for (int k = 0; k < nac; k++) ask(k, "folders", 0, 0, 0, 0);
    if (nac) ask(cur, "list", folder, 0, 0, 0);
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
static const char *hdr(const char *t, const char *name, char *out, size_t cap)   /* a header line of t (the open message...) */
{
    size_t nl = strlen(name);
    out[0] = 0;
    for (const char *p = t; p && *p && *p != '\n'; p = strchr(p, '\n'), p = p ? p + 1 : 0)
        if (!memcmp(p, name, nl) && p[nl] == ':') {
            size_t i = 0;
            for (p += nl + 1 + (p[nl + 1] == ' '); *p && *p != '\n' && i + 1 < cap; ) out[i++] = *p++;
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
static void compose(int how);

/* An answer from the helper: what the command asked, then the next one. */
static void answer(void)
{
    if (!resp && !(resp = malloc(rcap = 16))) { rcap = 0; nq = busy = 0; return; }
    if (rlen >= 2 && resp[rlen - 1] == '\n' && resp[rlen - 2] == '\x1e') rlen -= 2;
    resp[rlen] = 0;
    rlen = fold(resp, rlen);
    resp[rlen] = 0;
    char *c = q[0], *w[8], *err = 0, fwd[96];
    for (int i = 0; i < 8; i++) w[i] = word(&c);
    int k = (int)strnum(w[1]);
    const char *a = w[2], *b = w[3];
    if (k < 0 || k >= MAXA) k = 0;
    for (char *p = resp; *p; p = strchr(p, '\n'), p = p ? p + 1 : 0) if (!memcmp(p, "error: ", 7)) { err = p + 7; break; }
    if (err) { char *e = strchr(err, '\n'); if (e) *e = 0; err[0] = err[0] >= 'a' && err[0] <= 'z' ? err[0] - 32 : err[0]; }
    if (!strcmp(a, "folders") && !err) {
        int j = 0, n = 0, u = 0;
        for (int i = 0; i < nfo; i++) if (fo[i].a != k) fo[j++] = fo[i];
        for (char *p = resp; *p && j < MAXF; n++, j++) {
            strlcpy(fo[j].name, word(&p), 64);
            long v = strnum(word(&p));
            fo[j].unseen = v > 0 ? (short)v : 0; fo[j].a = (short)k;
            u += fo[j].unseen;
        }
        nfo = j;
        say("atlas: mail: folders %d: %d, %d unread\n", k, n, u);
    } else if (!strcmp(a, "list") && k == cur && !strcmp(b, folder)) {
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
        say("atlas: mail: list %d %s: %d messages, %d unread\n", k, b, nmsg, u);
    } else if (!strcmp(a, "read") && !err) {
        free(text);
        tlen = rlen < TMAX ? (int)rlen : TMAX;
        text = malloc(tlen + 1);
        if (text) { memcpy(text, resp, tlen); text[tlen] = 0; } else tlen = 0;
        tscroll = 0;
        if (sel >= 0 && sel < nmsg && !msg[sel].seen) {  /* one unread less */
            msg[sel].seen = 1;
            for (int i = 0; i < nfo; i++) if (fo[i].a == cur && !strcmp(fo[i].name, folder) && fo[i].unseen) fo[i].unseen--;
        }
        int na = 0;
        for (char *p = text; p && *p && *p != '\n'; p = strchr(p, '\n'), p = p ? p + 1 : 0) na += !memcmp(p, "Attachment:", 11);
        hdr(text, "Message-ID", irt, sizeof irt);
        say("atlas: mail: read %s: %d bytes, %d attachments\n", b, tlen, na);
        counts();
    } else if (!strcmp(a, "mailto") && !err) {
        compose(0);
        static const char *const hn[] = { "To", "Cc", "Subject" };
        for (int f = F_TO; f <= F_SUBJ; f++) hdr(resp, hn[f - F_TO], fld[f], sizeof fld[0]);
        const char *t = find(resp, "\n\n");
        blen = t ? (int)strlcpy(body, t + 2, BMAX + 1) : 0;
        if (blen > BMAX) blen = BMAX;
        field = fld[F_TO][0] ? F_BODY : F_TO;
        say("atlas: mail: mailto: to=%s subject=%s\n", fld[F_TO], fld[F_SUBJ]);
    } else if (!err && (!strcmp(a, "delete") || !strcmp(a, "send") || !strcmp(a, "save"))) {
        path_of(fwd, "/.mail-fwd");
        int fw = !strcmp(a, "save") && !strcmp(w[6], fwd);
        for (char *p = resp, *e; fw && *p; p = e ? e + 1 : p + strlen(p)) {   /* forwarding: its files attached */
            if ((e = strchr(p, '\n'))) *e = 0;
            if (!memcmp(p, "saved\t", 6) && natt < NATT) strlcpy(att[natt++], p + 6, sizeof att[0]);
        }
        if (!fw) {                                       /* else the answer's lines: the status */
            char *s = status, *e = status + sizeof status - 3;
            for (const char *p = resp; *p && s < e; p++) { if (*p == '\n') { if (p[1]) { *s++ = ';'; *s++ = ' '; } } else *s++ = *p == '\t' ? ' ' : *p; }
            *s = 0;
            status[0] = status[0] >= 'a' && status[0] <= 'z' ? status[0] - 32 : status[0];
            status_err = 0;
        }
        if (*a == 'd') { free(text); text = 0; tlen = 0; sel = -1; }
        if (*a == 's' && a[1] == 'e') {                  /* sent: the draft and the forwarded files go */
            b = "";
            char p[96];
            path_of(p, "/.mail-draft");
            fs_unlink(p);
            for (int i = 0; i < natt; i++) if (!memcmp(att[i], fwd, strlen(fwd))) fs_unlink(att[i]);
            natt = 0;
            view = V_LIST; field = -1;
        }
        say("atlas: mail: %s%s%s: %s\n", *b ? a : "sent", *b ? " " : "", b, fw ? "for forwarding" : status);
    }
    if (err) {
        set_status(err, 1); say("atlas: mail: %s: error: %s\n", a, err);
        if (!memcmp(err, "Untrusted certificate, SHA-256 ", 31)) { pk = k; pw = *a == 's' && a[1] == 'e'; strlcpy(pfp, err + 31, sizeof pfp); }
    }
    int relist = *a == 'd' && !err;
    memmove(q[0], q[1], (QN - 1) * sizeof q[0]);
    nq--; busy = 0;
    if (relist) { ask(k, "list", folder, 0, 0, 0); ask(k, "folders", 0, 0, 0, 0); }
    pump();
}

/* CON_WRITE / CON_READ from the helper (atlas.c: its console's channel). */
int mail_console(long from_, msg_t *m, const char *data)
{
    if (m->w[0] == CON_READ) { reader = from_; want = m->w[1]; pump(); return 0; }
    if (m->w[0] != CON_WRITE) { reply_val(from_, m->w[0] == CON_ECHO ? 0 : -ENOSYS); return 0; }
    size_t n = m->rlen;
    reply_val(from_, n);
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

/* ---- The accounts: ~/.mail. */
static void acct_new(int k)
{
    wipe(&ac[k], sizeof ac[k]);
    ac[k].o[O_KEEP] = ac[k].o[O_SENT] = 1;
}
static void ports(acct_t *a)             /* (the usual ones when none is given) */
{
    if (!a->f[F_IPORT][0]) strlcpy(a->f[F_IPORT], a->o[O_POP] ? "995" : "993", 160);
    if (!a->f[F_OPORT][0]) strlcpy(a->f[F_OPORT], "465", 160);
}
static void acct_load(void)
{
    char p[96], v[512];
    path_of(p, "/.mail");
    char *t = file_get(p, 0);
    wipe(ac, sizeof ac);
    nac = t != 0;
    acct_new(0);
    for (const char *l = t && *t == '[' ? strchr(t, '\n') : t; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : 0) {
        if (*l == '\n') continue;
        if (*l == '[') { if (nac < MAXA) acct_new(nac++); continue; }
        const char *eq = strchr(l, '='), *e = strchr(l, '\n');
        if (!e) e = l + strlen(l);
        if (!eq || eq > e) continue;
        size_t n = (size_t)(e - eq - 1) < sizeof v - 1 ? (size_t)(e - eq - 1) : sizeof v - 1;
        memcpy(v, eq + 1, n); v[n] = 0;
        if (n && v[n - 1] == '\r') v[n - 1] = 0;
        acct_t *a = &ac[nac - 1];
        int key = 0;
        while (key < 15 && (strlen(keys_[key]) != (size_t)(eq - l) || memcmp(l, keys_[key], eq - l))) key++;
        if (key < F_TO) strlcpy(a->f[key], v, 160);
        else if (key == 8) a->o[O_POP] = !strcmp(v, "pop3");
        else if (key < 12) a->o[key - 8] = key == 10 ? v[0] == '1' : v[0] != '0';
        else if (key < 14) strlcpy(a->pin[key - 12], v, 65);
        else if (key == 14 && strlen(a->fav) + (e - l) + 2 < sizeof a->fav) { strlcpy(a->fav + strlen(a->fav), l, e - l + 2); }
    }
    if (t) wipe(t, strlen(t));
    free(t);
    wipe(v, sizeof v);
    for (int k = 0; k < nac; k++) ports(&ac[k]);
}
static void put(char *t, size_t *n, const char *k, const char *v)   /* key=value\n */
{
    if (*n + strlen(k) + strlen(v) + 3 > MAXA * 2400) return;
    *n += strlcpy(t + *n, k, 64); t[(*n)++] = '=';
    *n += strlcpy(t + *n, v, 512); t[(*n)++] = '\n';
}
static int acct_write(void)
{
    char *t = malloc(MAXA * 2400), p[96];
    size_t n = 0;
    if (!t) return -1;
    for (int k = 0; k < nac; k++) {
        acct_t *a = &ac[k];
        if (k) n += strlcpy(t + n, "[account]\n", 12);
        for (int f = 0; f < F_TO; f++) put(t, &n, keys_[f], a->f[f]);
        put(t, &n, "in", a->o[O_POP] ? "pop3" : "imap");
        for (int o = O_KEEP; o < NO; o++) put(t, &n, keys_[8 + o], a->o[o] ? "1" : "0");
        for (int w = 0; w < 2; w++) if (a->pin[w][0]) put(t, &n, keys_[12 + w], a->pin[w]);
        n += strlcpy(t + n, a->fav, sizeof a->fav);
    }
    path_of(p, "/.mail");
    long h = fs_open(p, FS_WRONLY | FS_CREAT | FS_TRUNC, 0600), r = h;
    if (h >= 0) { fs_chmod(p, 0600); own(p); r = fs_write(h, 0, t, n); fs_close(h); }
    wipe(t, n); free(t);
    return r < 0 ? -1 : 0;
}
static void acct_save(void)
{
    acct_t *a = &ac[edit];
    if (!strchr(a->f[F_ADDR], '@') || !a->f[F_IHOST][0] || !a->f[F_OHOST][0]) { set_status("Give an address and both servers.", 1); return; }
    if (strnum(a->f[F_IPORT]) <= 0 || strnum(a->f[F_OPORT]) <= 0) { set_status("A port is a number.", 1); return; }
    if (!a->f[F_USER][0]) strlcpy(a->f[F_USER], a->f[F_ADDR], 160);
    if (!a->o[O_SELF]) a->pin[0][0] = a->pin[1][0] = 0;   /* (self-signed off: what was accepted is forgotten) */
    if (edit == nac) nac++;
    if (acct_write() < 0) { set_status("Could not save the accounts.", 1); return; }
    say("atlas: mail: account %d saved (%s)\n", edit, a->o[O_POP] ? "pop3" : "imap");
    view = V_LIST; field = -1;
    nfo = nmsg = 0; sel = -1; free(text); text = 0; tlen = 0;
    cur = edit; pk = -1;
    strlcpy(folder, "INBOX", sizeof folder);
    check();
}
static void acct_form(int k)
{
    view = V_ACCOUNT; field = F_NAME; status[0] = 0;
    if (k == nac && k < MAXA) { acct_new(k); ports(&ac[k]); }
    if (k <= nac && k < MAXA) edit = k;
    say("atlas: mail: account form %d\n", edit);
}

/* ---- Writing: new, reply, reply all, forward; sent through a draft file. */
static int has(const char *l, const char *a, size_t n) { for (; *l; l++) if (ci(l, a, n)) return 1; return 0; }
static void others(char *dst, const char *src)           /* src's addresses not ours, not in To, not in dst: added to dst */
{
    for (const char *p = src; *p; ) {
        const char *e = p, *s = p, *a, *ae;
        for (int qd = 0; *e && (qd || *e != ','); e++) qd ^= *e == '"';
        while (s < e && *s == ' ') s++;
        const char *t = e;
        while (t > s && t[-1] == ' ') t--;
        a = s; ae = t;
        for (const char *c = s; c < t; c++) { if (*c == '<') a = c + 1; if (*c == '>') ae = c; }
        int skip = ae <= a || has(fld[F_TO], a, ae - a) || has(dst, a, ae - a);
        for (int k = 0; k < nac && !skip; k++) skip = strlen(ac[k].f[F_ADDR]) == (size_t)(ae - a) && ci(ac[k].f[F_ADDR], a, ae - a);
        size_t n = strlen(dst);
        if (!skip && n + (t - s) + 3 < sizeof fld[0]) { if (n) n += strlcpy(dst + n, ", ", 3); memcpy(dst + n, s, t - s); dst[n + (t - s)] = 0; }
        p = *e ? e + 1 : e;
    }
}
static void compose(int how)                             /* 0 new, 1 reply, 2 reply all, 3 forward */
{
    char fr[160], subj[160], date[64], to[160], cc[160], rt[160];
    if (!body && !(body = malloc(BMAX + 1))) return;
    blen = bcur = bscroll = 0; natt = 0;
    from = cur < nac ? cur : 0;
    for (int f = F_TO; f <= F_SUBJ; f++) fld[f][0] = 0;
    int re = how == 1 || how == 2;
    if (how && text) {
        hdr(text, "From", fr, sizeof fr); hdr(text, "Subject", subj, sizeof subj); hdr(text, "Date", date, sizeof date);
        hdr(text, "To", to, sizeof to); hdr(text, "Cc", cc, sizeof cc); hdr(text, "Reply-To", rt, sizeof rt);
        int has = re ? ci(subj, "Re:", 3) : ci(subj, "Fwd:", 4);
        char *s = fld[F_SUBJ] + strlcpy(fld[F_SUBJ], has ? "" : re ? "Re: " : "Fwd: ", sizeof fld[0]);
        strlcpy(s, subj, sizeof fld[0] - (s - fld[F_SUBJ]));
        if (re) strlcpy(fld[F_TO], rt[0] ? rt : fr, sizeof fld[0]);
        if (how == 2) { others(fld[F_CC], to); others(fld[F_CC], cc); }
        char *b = body, *e = body + BMAX;
        b += strlcpy(b, "\n\n", e - b);
        if (re) { b += strlcpy(b, "On ", e - b); b += strlcpy(b, date, e - b); b += strlcpy(b, ", ", e - b);
                  b += strlcpy(b, fr, e - b); b += strlcpy(b, " wrote:\n", e - b); }
        else { b += strlcpy(b, "---------- Forwarded message ----------\nFrom: ", e - b); b += strlcpy(b, fr, e - b);
               b += strlcpy(b, "\nDate: ", e - b); b += strlcpy(b, date, e - b); b += strlcpy(b, "\nSubject: ", e - b);
               b += strlcpy(b, subj, e - b); b += strlcpy(b, "\nTo: ", e - b); b += strlcpy(b, to, e - b); b += strlcpy(b, "\n\n", e - b); }
        const char *t = find(text, "\n\n");
        for (t = t ? t + 2 : text + tlen; *t && b + 4 < e; ) {   /* the text, quoted (reply) */
            if (re) b += strlcpy(b, *t == '>' ? ">" : "> ", e - b);
            while (*t && *t != '\n' && b + 1 < e) *b++ = *t++;
            if (*t == '\n' && b + 1 < e) *b++ = *t++;
        }
        blen = b - body;
        if (how == 3 && find(text, "\nAttachment: ") && sel >= 0) {   /* forward: its files too (saved, then attached) */
            char d[96];
            path_of(d, "/.mail-fwd");
            ask(cur, "save", msg[sel].id, folder, "0", d);
        }
    }
    if (!re) irt[0] = 0;
    body[blen] = 0;
    view = V_COMPOSE;
    field = re ? F_BODY : F_TO;
    status[0] = 0;
    static const char *const hw[] = { "new", "reply", "reply all", "forward" };
    say("atlas: mail: compose (%s)\n", hw[how]);
}
static void send(void)
{
    if (!fld[F_TO][0]) { set_status("Who is it to?", 1); field = F_TO; return; }
    char p[96];
    path_of(p, "/.mail-draft");
    long h = fs_open(p, FS_WRONLY | FS_CREAT | FS_TRUNC, 0600), r = h;
    if (h >= 0) {
        static char t[3072];
        size_t n = 0;
        static const char *const hn[] = { "To: ", "Cc: ", "Subject: " };
        for (int f = F_TO; f <= F_SUBJ; f++) { n += strlcpy(t + n, hn[f - F_TO], sizeof t - n); n += strlcpy(t + n, fld[f], sizeof t - n); t[n++] = '\n'; }
        if (irt[0]) { n += strlcpy(t + n, "In-Reply-To: ", sizeof t - n); n += strlcpy(t + n, irt, sizeof t - n); t[n++] = '\n'; }
        for (int i = 0; i < natt; i++) { n += strlcpy(t + n, "Attach: ", sizeof t - n); n += strlcpy(t + n, att[i], sizeof t - n); t[n++] = '\n'; }
        t[n++] = '\n';
        fs_chmod(p, 0600); own(p);
        r = fs_write(h, 0, t, n);
        if (r >= 0) r = fs_write(h, n, body, blen);
        fs_close(h);
    }
    if (r < 0) { set_status("Could not write the draft.", 1); return; }
    ask(from, "send", p, 0, 0, 0);
}
int mail_pick(const char *path)          /* Files' double-click, while picking: attached */
{
    if (!picking) return 0;
    picking = 0;
    if (natt < NATT) strlcpy(att[natt++], path, sizeof att[0]);
    say("atlas: mail: attached %s\n", path);
    status[0] = 0;
    atlas_show(WM_MAIL);
    return 1;
}

/* ---- Opening and closing. */
void mail_open(void)
{
    if (on) return;
    on = 1;
    acct_load();
    nfo = nmsg = 0; sel = -1; lscroll = 0; cur = 0; pk = -1;
    strlcpy(folder, "INBOX", sizeof folder);
    if (nac) { view = V_LIST; field = -1; status[0] = 0; check(); return; }
    acct_form(0);
    set_status("Your mail account, once: it is kept in ~/.mail.", 0);
}
void mail_mailto(const char *url)        /* a mailto: link: a message to write */
{
    mail_open();
    if (nac) ask(cur, "mailto", url, 0, 0, 0);
}
void mail_close(void)
{
    if (pid > 0) sys_kill(pid);
    pid = 0; reader = 0; nq = busy = 0; on = 0; picking = 0;
    free(text); text = 0; tlen = 0;
    free(body); body = 0; blen = 0;
    free(resp); resp = 0; rlen = rcap = 0;
    wipe(fld, sizeof fld); wipe(ac, sizeof ac);
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
    const char *t = fv(f);
    int lit = field == f, n = strlen(t), fr = 0;
    char st[64];
    if (f == F_PASS) { n = n < 63 ? n : 63; memset(st, '*', n); t = st; }
    while (fr < n && text_fit(t + fr, n - fr, SF(2), xe - x - 32) < n - fr) fr++;   /* (the end, when long) */
    dl_rect(x, y, xe, y + 44, 10, C_FIELD);
    dl_frame(x, y, xe, y + 44, 10, lit ? 2 : 1, lit ? C_ACC : C_LINE);
    dl_text(x + 14, y + 11, t + fr, n - fr, SF(2), C_TXT, x, y, xe - 10, y + 44);
    if (lit) { int w = text_sw(t + fr, n - fr, SF(2)); dl_rect(x + 16 + w, y + 10, x + 26 + w, y + 34, 0, C_ACC); }
    hitbox(x, y, xe, y + 44, H_MAIL, M_FIELD + f);
    if (atlas_debug) say("atlas: dbg mailfield %d at %d %d\n", f, (x + xe) / 2, y + 22);
}
static void option(int *x, int y, const char *s, int lit, int o)
{
    int x1 = pill(*x, y, s, lit, H_MAIL, M_OPT + o);
    if (atlas_debug) say("atlas: dbg mailopt %d at %d %d\n", o, (*x + x1) / 2, y + 21);
    *x = x1 + 8;
}
static void yesno(int *x, int y, const char *l, int o)   /* "Label [Yes] [No]" */
{
    label(*x, y + 10, l, 2, C_DIM);
    *x += tws(l, 2) + 16;
    option(x, y, "Yes", ac[edit].o[o], 2 * o); option(x, y, "No", !ac[edit].o[o], 2 * o + 1);
    *x += 16;
}
static void acct_pills(int x, int y, int k, int add)     /* the accounts (and New): k lit */
{
    for (int i = 0; i < nac + add && i < MAXA; i++) {
        const char *s = i < nac ? ac[i].f[F_ADDR][0] ? ac[i].f[F_ADDR] : "(account)" : "New account";
        int x1 = pill(x, y, s, i == k, H_MAIL, M_ACC + i);
        if (atlas_debug) say("atlas: dbg mailacc %d at %d %d\n", i, (x + x1) / 2, y + 21);
        x = x1 + 8;
    }
}

static void account_draw(int x0, int y0, int x1, int y1)
{
    dl_rect(x0, y0, x1, y1, 0, C_PANEL);
    int lx = x0 + 24, cx = x0 + 24 + tw(10, 2), y = y0 + 16, x;
    acct_t *a = &ac[edit];
    label(lx, y + 10, "Account", 2, C_DIM);
    acct_pills(cx, y, edit, 1);
    y += 60;
    for (int f = F_NAME; f <= F_PASS && y + 44 < y1; f++) {
        if (f == F_IHOST) {                              /* incoming: IMAP or POP3 */
            label(lx, y + 10, "Receive", 2, C_DIM);
            x = cx; option(&x, y, "IMAP", !a->o[O_POP], 1); option(&x, y, "POP3", a->o[O_POP], 0);
            x += 16;
            if (a->o[O_POP]) yesno(&x, y, "Leave on server", O_KEEP); else yesno(&x, y, "Copy to Sent", O_SENT);
            y += 54;
        }
        if (f == F_IPORT || f == F_OPORT) continue;
        label(lx, y + 10, flabel[f], 2, C_DIM);
        int xe = x1 - 24, has_port = f == F_IHOST || f == F_OHOST;
        if (has_port) { xe -= tw(6, 2) + 28; tfield(xe + 12, y, x1 - 24, f + 1); }   /* (6 digits: mono's width is room enough) */
        tfield(cx, y, xe, f);
        y += 54;
    }
    x = cx;
    if (y + 44 < y1) yesno(&x, y, "Accept a self-signed certificate", O_SELF);
}

static void list_draw(int x0, int y0, int x1, int y1)
{
    dl_rect(x0, y0, x1, y1, 0, C_TERM);
    hitbox(x0, y0, x1, y1, H_MAIL, M_BODY);
    int dw = text_w(16, SS) + 24, fw = (x1 - x0) * 26 / 100, fx = x0 + 34, sx = fx + fw, dx = x1 - dw;
    dl_text(fx, y0 + 8, "FROM", 4, SSS, C_DIM, 0, 0, SW, SH);
    dl_text(sx, y0 + 8, "SUBJECT", 7, SSS, C_DIM, 0, 0, SW, SH);
    dl_text(dx, y0 + 8, "DATE", 4, SSS, C_DIM, 0, 0, SW, SH);
    int ly = y0 + 30, n = (y1 - ly - 4) / ROW;
    dl_rect(x0 + 10, ly - 1, x1 - 10, ly, 0, C_LINE);
    if (lscroll > nmsg - n) lscroll = nmsg - n;
    if (lscroll < 0) lscroll = 0;
    if (!nmsg) {
        const char *m = busy || nq ? "Checking..." : "No messages";
        label((x0 + x1) / 2 - tws(m, 2) / 2, (ly + y1) / 2 - KH / 2, m, 2, C_DIM);
    }
    for (int r = 0; r < n && lscroll + r < nmsg; r++) {
        int i = lscroll + r, y = ly + 2 + r * ROW, b = !msg[i].seen;
        if (i == sel) dl_rect(x0 + 6, y, x1 - 6, y + ROW - 2, 6, C_SEL);
        else if (r & 1) dl_rect(x0 + 6, y, x1 - 6, y + ROW - 2, 6, C_PANEL);
        if (b) dl_rect(x0 + 14, y + 12, x0 + 22, y + 20, 4, C_ACC);   /* unread: a dot, and bold */
        for (int k = 0; k <= b; k++) {
            dl_text(fx + k, y + 6, msg[i].from, strlen(msg[i].from), SF(2), C_TXT, x0, y, sx - 16, y + ROW);
            dl_text(sx + k, y + 6, msg[i].subj, strlen(msg[i].subj), SF(2), C_TXT, x0, y, dx - 16, y + ROW);
        }
        dl_text(dx, y + 8, msg[i].date, strlen(msg[i].date), SSS, C_DIM, 0, 0, x1 - 8, SH);
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
        label((x0 + x1) / 2 - tws(m, 2) / 2, (y0 + y1) / 2 - KH / 2, m, 2, C_DIM);
        return;
    }
    int y = y0 + 10, lx = x0 + 20, vx = lx + tw(9, 2), na = 0;
    static const char *const hs[] = { "From", "To", "Cc", "Date", "Subject" };
    char v[200];
    for (int i = 0; i < 5; i++) {                        /* the headers (the subject in bold) */
        if (!hdr(text, hs[i], v, sizeof v)[0] && i != 4) continue;
        label(lx, y, hs[i], 2, C_DIM);
        for (int k = 0; k <= (i == 4); k++) dl_text(vx + k, y, v, strlen(v), SF(2), C_TXT, x0, y0, x1 - 10, y1);
        y += 28;
    }
    for (const char *p = text; *p && *p != '\n'; ) {     /* the attachments: a click saves one */
        const char *e = strchr(p, '\n');
        if (!memcmp(p, "Attachment: ", 12)) {
            int l = (e ? e : p + strlen(p)) - p - 12;
            icon(I_FILE, lx, y + 2, 2);
            dl_text(lx + 34, y, p + 12, l, SF(2), C_ACCT, x0, y0, x1 - 10, y1);
            hitbox(lx, y, lx + 34 + text_sw(p + 12, l, SF(2)), y + 26, H_MAIL, M_ATT + na);
            if (atlas_debug) say("atlas: dbg mailatt %d at %d %d\n", na + 1, lx + 40, y + 13);
            na++;
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
    if (nac > 1) { label(lx, y + 10, "From", 2, C_DIM); acct_pills(cx, y, from, 0); y += 52; }
    for (int f = F_TO; f <= F_SUBJ; f++) { label(lx, y + 10, flabel[f], 2, C_DIM); tfield(cx, y, x1 - 20, f); y += 52; }
    if (natt) {                                          /* the files attached: a click takes one off */
        int x = cx;
        icon(I_FILE, lx, y + 2, 2);
        for (int i = 0; i < natt; i++) {
            const char *n = base_name(att[i]);
            int w = tws(n, 2);
            dl_text(x, y, n, strlen(n), SF(2), C_ACCT, x0, y0, x1 - 10, y1);
            hitbox(x, y, x + w, y + 26, H_MAIL, M_CATT + i);
            if (atlas_debug) say("atlas: dbg mailcatt %s at %d %d\n", n, x + w / 2, y + 13);
            x += w + 24;
        }
        y += 34;
    }
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

static void folder_row(int i, int x0, int x1, int *y)    /* name, unread, the star */
{
    int c = fo[i].a == cur && !strcmp(fo[i].name, folder), f = is_fav(i), yy = *y;
    char u[8];
    if (c) dl_rect(x0 + 2, yy, x1 - 2, yy + FROW, 0, C_SEL);
    int un = fo[i].unseen ? num(u, fo[i].unseen) : 0, ux = x1 - 34 - text_sw(u, un, SF(2));
    for (int k = 0; k <= c; k++) dl_text(x0 + 14 + k, yy + 4, fo[i].name, strlen(fo[i].name), SF(2), c ? C_TXT : C_DIM, x0, yy, ux - 6, yy + FROW);
    for (int k = 0; k < 2 && un; k++) dl_text(ux + k, yy + 4, u, un, SF(2), C_TXT, x0, yy, x1, yy + FROW);
    dl_text(x1 - 26, yy + 4, "*", 1, SF(2), f ? C_ACC : C_LINE, x0, yy, x1, yy + FROW);
    hitbox(x0, yy, x1, yy + FROW, H_MAIL, M_FOLDER + i);
    hitbox(x1 - 32, yy, x1, yy + FROW, H_MAIL, M_STAR + i);
    if (atlas_debug) say("atlas: dbg mailfolder %d/%s at %d %d unseen=%d%s\natlas: dbg mailstar %d/%s at %d %d\n", fo[i].a, fo[i].name,
                         x0 + 60, yy + FROW / 2, fo[i].unseen, f ? " fav" : "", fo[i].a, fo[i].name, x1 - 20, yy + FROW / 2);
    *y += FROW;
}

void mail_draw(int x0, int y0, int x1, int y1, int focused)
{
    X0 = x0 + 2; Y0 = y0 + TOP; X1 = x1 - 2; Y1 = y1 - 2;
    hitbox(X0, Y0, X1, Y1, H_MAIL, M_BODY);
    int sy = Y1 - STATUS - (pk >= 0 ? BAND : 0), x = X0 + 12, ty = Y0 + 6;
    dl_rect(X0, Y0, X1, Y0 + BAR, 0, C_PANEL);           /* the buttons */
    dl_rect(X0, Y0 + BAR - 1, X1, Y0 + BAR, 0, C_LINE);
    if (view == V_LIST) {
        for (int b = B_CHECK; b <= B_DELETE; b++) button(&x, ty, b, 0);
        x = X1 - 12 - tws(bname[B_ACCOUNT], 2) - 28;
        if (x > X0 + 12) button(&x, ty, B_ACCOUNT, 0);
    } else if (view == V_COMPOSE) { button(&x, ty, B_SEND, 1); button(&x, ty, B_ATTACH, 0); button(&x, ty, B_CANCEL, 0); }
    else { button(&x, ty, B_SAVE, 1); if (edit < nac) button(&x, ty, B_REMOVE, 0); if (nac) button(&x, ty, B_CANCEL, 0); }
    int top = Y0 + BAR, fw = X1 - X0 < 700 ? 0 : (X1 - X0) * 20 / 100;   /* the folders */
    if (fw > 300) fw = 300;
    int rx = X0;
    if (fw) {
        int y = top + 8, xe = X0 + fw, any = 0;
        dl_rect(X0, top, xe, sy, 0, C_PANEL);
        dl_rect(xe, top, xe + 1, sy, 0, C_LINE);
        for (int i = 0; i < nfo; i++) any |= is_fav(i);
        for (int k = any ? -1 : 0; k < nac && y + 2 * FROW < sy; k++) {   /* the favorites, then each account's */
            const char *h = k < 0 ? "FAVORITES" : ac[k].f[F_ADDR];
            dl_text(X0 + 14, y + 8, h, strlen(h), SSS, k < 0 ? C_DIM : C_ACCT, X0, y, xe - 6, y + FROW);
            if (k >= 0) hitbox(X0, y, xe, y + FROW, H_MAIL, M_HEAD + k);
            y += FROW;
            for (int i = 0; i < nfo && y + FROW < sy; i++) if (k < 0 ? is_fav(i) : fo[i].a == k && !is_fav(i)) folder_row(i, X0, xe, &y);
            y += 6;
        }
        rx = xe + 1;
    }
    if (view == V_ACCOUNT) account_draw(rx, top, X1, sy);
    else if (view == V_COMPOSE) compose_draw(rx, top, X1, sy, focused);
    else {
        int ly1 = top + (sy - top) * 40 / 100;
        list_draw(rx, top, X1, ly1);
        dl_rect(rx, ly1, X1, ly1 + 1, 0, C_LINE);
        read_draw(rx, ly1 + 1, X1, sy);
    }
    if (pk >= 0) {                                       /* a certificate to accept (or not) */
        const char *h = pw ? ac[pk].f[F_OHOST] : ac[pk].f[F_IHOST];
        char t[200];
        strlcpy(t + strlcpy(t, "No authority vouches for the certificate of ", sizeof t), h, 120);
        dl_rect(X0, sy, X1, sy + BAND, 0, C_PANEL);
        dl_rect(X0, sy, X1, sy + 1, 0, C_LINE);
        label(X0 + 16, sy + 10, t, 2, C_TXT);
        label(X0 + 16, sy + 40, "Accept it if its SHA-256 is your server's:", 2, C_DIM);
        dl_text(X0 + 16, sy + 66, pfp, strlen(pfp), SS, C_ACCT, X0, sy, X1, sy + BAND);
        x = X1 - 24 - tws(bname[B_ACCEPT], 2) - tws(bname[B_REFUSE], 2) - 64;
        button(&x, sy + 24, B_ACCEPT, 1); button(&x, sy + 24, B_REFUSE, 0);
        sy += BAND;
    }
    dl_rect(X0, sy, X1, Y1, 0, status_err ? C_ERR : C_ACC);   /* the status bar: inverse */
    char t[220];
    t[0] = ' ';
    strlcpy(t + 1, status[0] ? status : view == V_COMPOSE ? "New message" : folder, sizeof t - 1);
    dl_text(X0 + 8, sy + 5, t, strlen(t), SF(2), C_INK, X0, sy, X1 - 10, Y1);
    const char *k = view == V_LIST ? "F5 check   Ctrl+N new   Ctrl+R reply   Del delete " : view == V_COMPOSE ? "Tab next field   Ctrl+Enter send " : "Tab next field   Enter save ";
    int kl = strlen(k), kx = X1 - 10 - tws(k, 2);
    if (kx > X0 + 8 + tws(t, 2) + 30) dl_text(kx, sy + 5, k, kl, SF(2), C_INK, X0, sy, X1, Y1);
}

/* ---- Input. */
static void open_msg(int i)
{
    if (i < 0 || i >= nmsg) return;
    sel = i;
    ask(cur, "read", msg[i].id, folder, 0, 0);
}
static void button_do(int b)
{
    switch (b) {
    case B_CHECK: check(); break;
    case B_NEW: if (nac) compose(0); break;
    case B_REPLY: case B_ALL: case B_FWD: if (text) compose(b == B_FWD ? 3 : b - B_REPLY + 1); else set_status("Open a message first.", 1); break;
    case B_DELETE: if (sel >= 0 && sel < nmsg) ask(cur, "delete", msg[sel].id, folder, 0, 0); else set_status("Pick a message first.", 1); break;
    case B_ACCOUNT: acct_load(); acct_form(cur); break;
    case B_SEND: send(); break;
    case B_ATTACH: picking = 1; set_status("Double-click a file in Files to attach it.", 0); atlas_show(WM_FILES); break;
    case B_CANCEL: view = V_LIST; field = -1; status[0] = 0; picking = 0; acct_load(); break;
    case B_SAVE: acct_save(); break;
    case B_REMOVE:                                       /* the account edited, and its folders, go */
        memmove(&ac[edit], &ac[edit + 1], (MAXA - edit - 1) * sizeof ac[0]);
        nac--;
        if (acct_write() < 0) { set_status("Could not save the accounts.", 1); break; }
        say("atlas: mail: account %d removed\n", edit);
        nfo = nmsg = 0; sel = -1; free(text); text = 0; cur = 0; pk = -1;
        strlcpy(folder, "INBOX", sizeof folder);
        if (nac) { view = V_LIST; field = -1; check(); } else acct_form(0);
        break;
    case B_ACCEPT:                                       /* pinned: then asked again */
        for (int i = 0, n = 0; pfp[i] && n < 64; i++) if (pfp[i] != ':') ac[pk].pin[pw][n++] = pfp[i], ac[pk].pin[pw][n] = 0;
        if (acct_write() < 0) { set_status("Could not save the accounts.", 1); break; }
        say("atlas: mail: certificate accepted (account %d, %s)\n", pk, pw ? "outgoing" : "incoming");
        status[0] = 0;
        if (pw && view == V_COMPOSE) { pk = -1; send(); break; }
        ask(pk, "folders", 0, 0, 0, 0);
        if (pk == cur) ask(cur, "list", folder, 0, 0, 0);
        pk = -1;
        break;
    case B_REFUSE: pk = -1; status[0] = 0; break;
    }
}
int mail_click(int kind, int arg)
{
    if (kind != H_MAIL) return 0;
    if (arg >= M_ROW) open_msg(arg - M_ROW);
    else if (arg >= M_STAR) {                            /* a favorite, or not any more */
        int i = arg - M_STAR;
        char *f = ac[fo[i].a].fav, l[72], *p = fav_at(i, l);
        size_t n = strlen(l);
        if (p) memmove(p, p + n, strlen(p + n) + 1);
        else if (strlen(f) + n < sizeof ac[0].fav) strlcpy(f + strlen(f), l, n + 1);
        say("atlas: mail: favorites %d: %s\n", fo[i].a, p ? "-" : l);
        if (acct_write() < 0) set_status("Could not save the accounts.", 1);
    } else if (arg >= M_FOLDER) {
        cur = fo[arg - M_FOLDER].a;
        strlcpy(folder, fo[arg - M_FOLDER].name, sizeof folder);
        nmsg = 0; sel = -1; lscroll = 0; free(text); text = 0; tlen = 0;
        ask(cur, "list", folder, 0, 0, 0);
    } else if (arg >= M_CATT) { int i = arg - M_CATT; memmove(att[i], att[i + 1], (NATT - i - 1) * sizeof att[0]); natt--; }
    else if (arg >= M_ATT) {                             /* an attachment: saved in ~/Downloads */
        char n[8];
        n[num(n, arg - M_ATT + 1)] = 0;
        if (sel >= 0) ask(cur, "save", msg[sel].id, folder, n, 0);
    } else if (arg >= M_HEAD) { acct_load(); acct_form(arg - M_HEAD); }
    else if (arg >= M_ACC) { if (view == V_COMPOSE) from = arg - M_ACC; else acct_form(arg - M_ACC); }
    else if (arg >= M_FIELD) field = arg - M_FIELD;
    else if (arg >= M_OPT) {
        int o = arg - M_OPT, *v = &ac[edit].o[o / 2], was = *v;
        *v = !(o & 1);
        char *ip = ac[edit].f[F_IPORT];
        if (o < 2 && was != *v) {                        /* IMAP / POP3: the usual port goes with it */
            if (!ip[0] || !strcmp(ip, "993") || !strcmp(ip, "995")) strlcpy(ip, *v ? "995" : "993", 160);
            else if (!strcmp(ip, "143") || !strcmp(ip, "110")) strlcpy(ip, *v ? "110" : "143", 160);
        }
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
    static const int sk[] = { KEY_F1 + 4, B_CHECK, 14, B_NEW, 18, B_REPLY, KEY_CSA + 17, B_ALL, 12, B_FWD, KEY_DEL, B_DELETE };
    if (view == V_LIST) {                                /* F5, Ctrl+N, Ctrl+R, Ctrl+Shift+R, Ctrl+L, Del */
        for (int i = 0; i < 12; i += 2) if (c == sk[i]) { button_do(sk[i + 1]); return 1; }
        if (c == KEY_DOWN) open_msg(sel + 1 < nmsg ? sel + 1 : sel);
        else if (c == KEY_UP) open_msg(sel > 0 ? sel - 1 : 0);
        else if (c == KEY_PGDN) tscroll += 10;
        else if (c == KEY_PGUP) tscroll = tscroll > 10 ? tscroll - 10 : 0;
        else return 0;
        if (sel >= 0 && sel < lscroll) lscroll = sel;
        return 1;
    }
    if (c == 27) { if (view == V_COMPOSE || nac) button_do(B_CANCEL); return 1; }
    if (c == KEY_CRET && view == V_COMPOSE) { send(); return 1; }
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
    char *f = fv(field);
    size_t n = strlen(f);
    if (c == '\b') { if (n) f[n - 1] = 0; }
    else if (c >= 32 && c < 127 && n + 1 < sizeof fld[0]) { f[n] = (char)c; f[n + 1] = 0; }
    else return 0;
    return 1;
}
