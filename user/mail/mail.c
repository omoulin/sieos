/*
 * mail - Mail: IMAP or POP3 to read, SMTP to send, always over TLS
 * (docs/desktop.md, Mail). The command line, and the desktop's Mail
 * window, which runs it as you (mail -S) and talks to it on its console:
 *
 *   mail [list [FOLDER]]       the newest 300 messages: id, * if unread, date, from, subject
 *   mail folders               the folders and their unread messages (POP3: INBOX only)
 *   mail read ID [FOLDER]      a message as text (and marked read)
 *   mail save ID [FOLDER [N [DIR]]]  its attachments (N: the Nth only) into ~/Downloads (or DIR)
 *   mail delete ID [FOLDER]    to the Trash; from the Trash (or POP3), for good
 *   mail send FILE             a draft: To:, Cc:, Subject:, Attach: (a file) lines, a blank
 *                              line, the text; a copy goes into Sent (IMAP)
 *   mail mailto:URL            a message to that address: the text is typed, then sent
 *   mail mailto URL            (the window's) the draft a mailto: link makes
 *   mail -a N ...              the Nth account (from 0; the first by default)
 *   mail -S                    commands from the console, one a line, words split by
 *                              tabs; each answer ends with a line of one \x1e byte
 *
 *   Atlas's Mail ──"list\tINBOX"──► mail -S (as you) ──IMAP/POP3/SMTP over TLS──► servers
 *             ◄── lines, then \x1e ──┘
 *
 * The accounts are ~/.mail (key=value lines, only yours to read; a line
 * "[account]" starts the next one): name, address, in (imap or pop3),
 * in_host, in_port, out_host, out_port, user, pass, keep (POP3: leave the
 * messages on the server, 1 or 0), sent (0: no copy in Sent), selfsigned
 * (1: a certificate no authority signed may be accepted: its SHA-256 is
 * then pinned, in_pin and out_pin, and any other refused), fav (the
 * window's favorite folders). Ports 993,
 * 995 and 465 speak TLS at once; others (143, 110, 587) start in clear
 * and must switch to TLS (STARTTLS, STLS) before the password goes.
 * IMAP keeps its connection between commands; POP3 copies new messages
 * into ~/Mail/INBOX, one file each, named by the server's UIDL (so none
 * is fetched twice; read ones carry the attribute seen=1).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mail.h"
#include "mk/crypto.h"                  /* wipe */

#define TMO 30000                        /* ms: one answer from a server */
#define MAXLIST 300
#define MAXA 8                           /* accounts */

static struct acct { char name[64], addr[128], in[8], ihost[128], ohost[128], user[128], pass[128];
                     uint8_t pin[2][32]; int iport, oport, keep, sent, self; } acc[MAXA];
#define A acc[cur]
static int cur;                          /* the account of this command */
static char home[64], why[200];          /* why: the server's words when it says no */
static const char *xa[2];                /* save's N and DIR */
static buf_t out;                        /* what goes to the console next */

static void oput(void *ctx, char c) { b_add(ctx, &c, 1); }
static void outf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vformat(oput, &out, fmt, ap);
    va_end(ap);
}
static void flush(void)
{
    for (size_t i = 0; i < out.len; i += 4000) con_write(out.s + i, out.len - i < 4000 ? out.len - i : 4000);
    out.len = 0;
}
static long no(const char *s) { strlcpy(why, s, sizeof why); return -EPROTO; }
static long lead(const char *s)                          /* the number s starts with, or -1 */
{
    long v = -1;
    for (; *s >= '0' && *s <= '9'; s++) v = (v < 0 ? 0 : v * 10) + *s - '0';
    return v;
}
static int num_str(char *p, long v)                      /* decimal digits -> how many */
{
    char t[20];
    int n = 0, k = 0;
    do t[n++] = '0' + v % 10; while (v /= 10);
    while (n) p[k++] = t[--n];
    return k;
}
static int starts(const char *s, const char *w) { return same_ci(s, w, strlen(w)); }
static const char *nl(const char *p) { p = strchr(p, '\n'); return p ? p + 1 : 0; }   /* the next line, or 0 */
static const uint8_t zero[32];

/* ---- The account. */
static void conf(const char *t, const char *key, char *v, size_t cap)
{
    size_t kl = strlen(key);
    v[0] = 0;
    for (const char *p = t; p && *p && *p != '['; p = nl(p))   /* (up to the next account) */
        if (!memcmp(p, key, kl) && p[kl] == '=') {
            size_t i = 0;
            for (p += kl + 1; *p && *p != '\n' && *p != '\r' && i + 1 < cap; ) v[i++] = *p++;
            v[i] = 0;
            return;
        }
}
static void im_close(void);
static long load(void)                   /* account cur (changed: connect again) */
{
    char path[96], v[72];
    strlcpy(path + strlcpy(path, home, sizeof path), "/.mail", 8);
    char *t = file_get(path, 0);
    if (!t) return no("no account yet: set it up in Mail on the desktop, or in ~/.mail");
    const char *s = *t == '[' ? nl(t) : t;               /* (a first "[account]": nothing before it) */
    for (int k = 0; s && k < cur; k++) {                 /* after the cur-th "[account]" line */
        while (s && *s != '[') s = nl(s);
        if (s) s = nl(s);
    }
    if (!s) { free(t); return no("no such account"); }
    static struct acct n;
    memset(&n, 0, sizeof n);
    conf(s, "name", n.name, sizeof n.name); conf(s, "address", n.addr, sizeof n.addr);
    conf(s, "in", n.in, sizeof n.in);
    conf(s, "in_host", n.ihost, sizeof n.ihost); conf(s, "out_host", n.ohost, sizeof n.ohost);
    conf(s, "user", n.user, sizeof n.user); conf(s, "pass", n.pass, sizeof n.pass);
    int pop = !strcmp(n.in, "pop3");
    conf(s, "in_port", v, sizeof v); n.iport = v[0] ? strnum(v) : pop ? 995 : 993;
    conf(s, "out_port", v, sizeof v); n.oport = v[0] ? strnum(v) : 465;
    conf(s, "keep", v, sizeof v); n.keep = v[0] != '0';
    conf(s, "sent", v, sizeof v); n.sent = v[0] != '0';
    conf(s, "selfsigned", v, sizeof v); n.self = v[0] == '1';
    for (int w = 0; w < 2 && n.self; w++) {              /* the pins: 64 hex digits */
        conf(s, w ? "out_pin" : "in_pin", v, sizeof v);
        for (int i = 0; i < 64 && hexv(v[i]) >= 0; i++) n.pin[w][i / 2] |= hexv(v[i]) << (i & 1 ? 0 : 4);
    }
    wipe(t, strlen(t)); free(t);
    if (memcmp(&n, &A, sizeof n)) { im_close(); A = n; }
    wipe(&n, sizeof n);
    return 0;
}

/* ---- A connection: plain at first or TLS at once; lines and bytes in. */
typedef struct { int on; long s; tls_t *t; size_t pos, len; char in[4096]; } conn_t;
static long c_tls(conn_t *c, const char *host)          /* (host A.ohost: the outgoing server's pin) */
{
    long e;
    const uint8_t *pin = A.pin[host == A.ohost];
    uint8_t fp[32];
    memcpy(fp, pin, 32);
    c->t = tls_open(c->s, host, 0, fp, &e);
    if (c->t) return 0;
    c->on = 0;
    if (!memcmp(fp, pin, 32)) return e;
    if (!A.self) return no("the server's certificate is not trusted (a self-signed one can be accepted: Account)");
    char *w = why + strlcpy(why, memcmp(pin, zero, 32) ? "the server's certificate changed: refused; SHA-256 now " : "untrusted certificate, SHA-256 ", sizeof why);
    for (int i = 0; i < 32; i++) { *w++ = "0123456789ABCDEF"[fp[i] >> 4]; *w++ = "0123456789ABCDEF"[fp[i] & 15]; *w++ = i < 31 ? ':' : 0; }
    return -EPROTO;
}
static long c_open(conn_t *c, const char *host, int port, int tls)
{
    uint32_t ip;
    c->on = 0; c->t = 0; c->pos = c->len = 0;
    long e = net_resolve(host, &ip, TMO);
    if (e < 0) return e;
    if ((c->s = net_connect(ip, port, TMO)) < 0) return c->s;
    c->on = 1;
    return tls ? c_tls(c, host) : 0;
}
static void c_close(conn_t *c)
{
    if (!c->on) return;
    if (c->t) tls_close(c->t); else net_close(c->s);
    c->on = 0; c->t = 0;
}
static long c_send(conn_t *c, const void *s, size_t n) { return c->t ? tls_write(c->t, s, n) : net_send(c->s, s, n); }
static long c_fill(conn_t *c)
{
    if (c->pos < c->len) return 1;
    long n = c->t ? tls_read(c->t, c->in, sizeof c->in, TMO) : net_recv(c->s, c->in, sizeof c->in, TMO);
    if (n == 0) n = -ECONNRESET;
    if (n > 0) { c->pos = 0; c->len = n; }
    return n;
}
static long c_line(conn_t *c, char *b, size_t cap)      /* without its CRLF; a longer one is cut */
{
    size_t o = 0;
    for (long e; ; ) {
        if ((e = c_fill(c)) < 0) return e;
        char ch = c->in[c->pos++];
        if (ch == '\n') { if (o && b[o - 1] == '\r') o--; b[o] = 0; return o; }
        if (o + 1 < cap) b[o++] = ch;
    }
}
static long c_bytes(conn_t *c, buf_t *b, size_t n)
{
    for (long e; n; ) {
        if ((e = c_fill(c)) < 0) return e;
        size_t k = c->len - c->pos < n ? c->len - c->pos : n;
        if (b->len + k <= MAXMSG) b_add(b, c->in + c->pos, k);
        c->pos += k; n -= k;
    }
    return 0;
}
static int implicit(int port) { return port == 993 || port == 995 || port == 465; }

/* ---- The messages listed: sorted by date, newest first. */
static struct ent { char id[72], date[17], from[96], subj[160]; int seen; } ents[MAXLIST];
static int nent;
static void add_ent(const char *id, const char *hdr, size_t hl, int seen)
{
    if (nent == MAXLIST) return;
    struct ent *e = &ents[nent++];
    char raw[512], d[512];
    strlcpy(e->id, id, sizeof e->id);
    e->seen = seen;
    mime_header(hdr, hl, "date", raw, sizeof raw); mime_date(raw, e->date);
    mime_header(hdr, hl, "subject", raw, sizeof raw); mime_words(raw, e->subj, sizeof e->subj);
    mime_header(hdr, hl, "from", raw, sizeof raw); mime_words(raw, d, sizeof d);
    char *lt = strchr(d, '<'), *s = d;                   /* the name if there is one, else the address */
    if (lt && lt > d) { while (lt > d && (lt[-1] == ' ' || lt[-1] == '"')) lt--; *lt = 0; if (*s == '"') s++; }
    else if (lt) { s = lt + 1; if (strchr(s, '>')) *strchr(s, '>') = 0; }
    strlcpy(e->from, s, sizeof e->from);
    for (char *p = e->subj; *p; p++) if (*p == '\t') *p = ' ';
    for (char *p = e->from; *p; p++) if (*p == '\t') *p = ' ';
}
static void print_ents(void)
{
    for (int i = 0; i < nent; i++) {                     /* (newest first: the dates, then the order of arrival) */
        int best = i;
        for (int j = i + 1; j < nent; j++) if (strcmp(ents[j].date, ents[best].date) >= 0) best = j;
        struct ent t = ents[i]; ents[i] = ents[best]; ents[best] = t;
        outf("%s\t%c\t%s\t%s\t%s\n", ents[i].id, ents[i].seen ? ' ' : '*', ents[i].date, ents[i].from, ents[i].subj);
    }
}

/* ---- A message read: as text, or its attachments saved (files of the
 * user's: the name made safe, numbered if one is there already). */
static void file_name(char *n, const char *s)           /* a UIDL or a name as a file name: no '/', no leading '.' */
{
    size_t i = 0;
    for (; s[i] && s[i] != '\r' && s[i] != '\n' && i < 70; i++)
        n[i] = s[i] == '/' || (uint8_t)s[i] < ' ' || (uint8_t)s[i] > '~' || (!i && s[i] == '.') ? '_' : s[i];
    n[i] = 0;
}
static long deliver(const char *cmd, const char *m, size_t len)
{
    if (*cmd == 'r') { mime_read(m, len, &out); return 0; }
    char dir[160], path[240], raw[160], name[72];
    siefs_stat_t st;
    int only = xa[0] ? strnum(xa[0]) : 0, i = only > 0 ? only - 1 : 0, cnt;
    if (xa[1] && xa[1][0]) strlcpy(dir, xa[1], sizeof dir);
    else strlcpy(dir + strlcpy(dir, home, sizeof dir), "/Downloads", 12);
    fs_mkdir(dir, 0700);
    buf_t d = { 0 };
    do {
        d.len = 0;
        if ((cnt = mime_part(m, len, i, raw, sizeof raw, &d)) <= i) break;
        file_name(name, raw[0] ? raw : "attachment");
        for (int k = 0; k < 10; k++) {                   /* name, 1-name, 2-name... */
            char *p = path + strlcpy(path, dir, sizeof path);
            *p++ = '/';
            if (k) { *p++ = '0' + k; *p++ = '-'; }
            strlcpy(p, name, 80);
            if (fs_stat(path, &st, 1)) break;
        }
        long h = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC, 0600), r = h;
        if (h >= 0) { r = fs_write(h, 0, d.s ? d.s : "", d.len); fs_close(h); }
        if (r < 0) { b_free(&d); return no("cannot write the attachment there"); }
        outf("saved\t%s\n", path);
    } while (!only && ++i < cnt);
    b_free(&d);
    return cnt > i || (!only && cnt) ? 0 : no("no such attachment");
}

/* ---- IMAP (RFC 3501). A command, then its untagged answers one by one
 * to cb (a literal {n} in one: its bytes as lit, the line around it joined).
 * Each account keeps its connection; its Trash and Sent are found in LIST
 * (SPECIAL-USE \Trash, \Sent, RFC 6154; else by their usual names). */
static conn_t ims[MAXA];
#define im ims[cur]
static int tagn, strong;                 /* strong: a role found by its flag (not by a name) */
static buf_t iline, ilit, imsg, names;   /* names: LIST's, a line each */
static char role[MAXA][2][64];           /* Trash, Sent */
static const char *lit;                  /* APPEND's message: sent when the server says "+" */
static size_t litn;
typedef void (*im_cb)(const char *line, const char *lit, size_t ln);
static void im_close(void) { c_close(&im); role[cur][0][0] = role[cur][1][0] = 0; }
static long im_cmd(const char *cmd, im_cb cb)
{
    char tag[16] = "a", line[2048];
    int tl = 1 + num_str(tag + 1, ++tagn);
    tag[tl++] = ' '; tag[tl] = 0;
    buf_t c = { 0 }, junk = { 0 };
    b_fmt(&c, "%s%s\r\n", tag, cmd);
    long e = c_send(&im, c.s, c.len);
    wipe(c.s, c.len); b_free(&c);
    if (e < 0) return e;
    for (;;) {
        iline.len = ilit.len = 0;
        for (int nl = 0; ; nl++) {                       /* a whole answer, its literals read (the first kept) */
            if ((e = c_line(&im, line, sizeof line)) < 0) break;
            b_str(&iline, line);
            char *br = e > 2 && line[e - 1] == '}' ? strchr(iline.s, '{') : 0;
            while (br && strchr(br + 1, '{')) br = strchr(br + 1, '{');
            if (!br) break;
            long n = lead(br + 1);
            iline.len = br - iline.s; iline.s[iline.len] = 0;
            if (n < 0 || (e = c_bytes(&im, nl ? &junk : &ilit, n)) < 0) { e = n < 0 ? -EPROTO : e; break; }
            junk.len = 0;
        }
        b_free(&junk);
        if (e < 0) return e;
        if (iline.s[0] == '+' && lit) {                  /* go on: the literal */
            if ((e = c_send(&im, lit, litn)) < 0 || (e = c_send(&im, "\r\n", 2)) < 0) return e;
            lit = 0;
            continue;
        }
        if (!memcmp(iline.s, tag, tl)) {
            lit = 0;
            if (starts(iline.s + tl, "OK")) return 0;
            return no(iline.s + tl);
        }
        if (iline.s[0] == '*' && cb) cb(iline.s, ilit.s ? ilit.s : "", ilit.len);
    }
}
static void quote(buf_t *b, const char *s)
{
    b_str(b, "\"");
    for (; *s; s++) { if (*s == '"' || *s == '\\') b_str(b, "\\"); b_add(b, s, 1); }
    b_str(b, "\"");
}
static long im_do(const char *verb, const char *box, const char *tail, im_cb cb)   /* verb "box" tail */
{
    buf_t c = { 0 };
    b_str(&c, verb); quote(&c, box); b_str(&c, tail);
    long e = im_cmd(c.s, cb);
    b_free(&c);
    return e;
}
static long im_open(void)
{
    if (im.on) return 0;
    char line[1024];
    long e = c_open(&im, A.ihost, A.iport, implicit(A.iport));
    if (e >= 0 && (e = c_line(&im, line, sizeof line)) >= 0 && !starts(line, "* OK")) e = no(line);
    if (e >= 0 && !im.t && (e = im_cmd("STARTTLS", 0)) >= 0) e = c_tls(&im, A.ihost);
    if (e >= 0) {
        buf_t c = { 0 };
        b_str(&c, "LOGIN "); quote(&c, A.user); b_str(&c, " "); quote(&c, A.pass);
        e = im_cmd(c.s, 0);
        wipe(c.s, c.len); b_free(&c);
    }
    if (e < 0) c_close(&im);
    return e;
}
static void folder_cb(const char *l, const char *lt, size_t ln)   /* * LIST (\HasNoChildren) "/" "INBOX" */
{
    static const char *const rn[2][4] = { { "trash", "deleted items", "deleted messages", "bin" },
                                          { "sent", "sent items", "sent mail", "sent messages" } };
    if (!starts(l, "* LIST (")) return;
    const char *f = l + 8, *fe = strchr(f, ')'), *p = fe;
    if (!p || find_ci(f, p, "\\noselect") || find_ci(f, p, "\\nonexistent")) return;
    for (p++; *p == ' '; p++) ;
    if (*p == '"') { p = strchr(p + 1, '"'); if (!p) return; p++; } else while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    char n[160], *b = n;
    size_t o = 0;
    if (!*p && ln) strlcpy(n, lt, ln + 1 < sizeof n ? ln + 1 : sizeof n);
    else {
        int q = *p == '"';
        for (p += q; *p && (!q || *p != '"') && o + 1 < sizeof n; p++) {
            if (q && *p == '\\' && p[1]) p++;
            n[o++] = *p;
        }
        n[o] = 0;
    }
    for (char *c = n; *c; c++) if (*c == '/' || *c == '.') b = c + 1;   /* (its last part) */
    for (int r = 0; r < 2; r++) {
        int fl = find_ci(f, fe, r ? "\\sent" : "\\trash") != 0, nm = 0;
        for (int j = 0; j < 4; j++) nm |= strlen(b) == strlen(rn[r][j]) && same_ci(b, rn[r][j], strlen(b));
        if ((fl && !(strong >> r & 1)) || (nm && !role[cur][r][0])) { strlcpy(role[cur][r], n, 64); strong |= fl << r; }
    }
    b_str(&names, n); b_str(&names, "\n");
}
static long im_list(void)                                /* the folders' names, and Trash and Sent */
{
    names.len = 0; strong = 0;
    role[cur][0][0] = role[cur][1][0] = 0;
    return im_cmd("LIST \"\" \"*\"", folder_cb);
}
static long exists;
static void select_cb(const char *l, const char *lt, size_t ln)   /* * 23 EXISTS; * STATUS "x" (UNSEEN 2) */
{
    (void)lt; (void)ln;
    const char *s = strchr(l + 2, ' '), *u = find_ci(l, l + strlen(l), "(UNSEEN ");
    if (s && starts(s, " EXISTS")) exists = lead(l + 2) > 0 ? lead(l + 2) : 0;
    if (u && starts(l, "* STATUS")) exists = lead(u + 8);
}
static void fetch_cb(const char *l, const char *lt, size_t ln)   /* * 5 FETCH (UID 9 FLAGS (\Seen) BODY[...] {n}) */
{
    const char *u = find_ci(l, l + strlen(l), "UID "), *f = find_ci(l, l + strlen(l), "FLAGS (");
    if (!u || !find_ci(l, l + strlen(l), "FETCH")) return;
    char id[24];
    size_t i = 0;
    for (u += 4; *u >= '0' && *u <= '9' && i + 1 < sizeof id; ) id[i++] = *u++;
    id[i] = 0;
    const char *fe = f ? strchr(f, ')') : 0;
    add_ent(id, lt, ln, fe && find_ci(f, fe, "\\seen"));
}
static void body_cb(const char *l, const char *lt, size_t ln)   /* (the literal kept: swapped out) */
{
    (void)lt; (void)ln;
    if (find_ci(l, l + strlen(l), "FETCH")) { buf_t t = imsg; imsg = ilit; ilit = t; }
}
static int is_uid(const char *id) { if (!*id) return 0; for (; *id; id++) if (*id < '0' || *id > '9') return 0; return 1; }

static long im_run(const char *cmd, const char *id, const char *folder)
{
    long e = im_open();
    if (e < 0) return e;
    if (!strcmp(cmd, "folders")) {                       /* each with its unread messages */
        if ((e = im_list()) < 0) return e;
        for (const char *p = names.s; p && *p; p = nl(p)) {
            char n[160];
            size_t k = strchr(p, '\n') - p;
            strlcpy(n, p, k + 1 < sizeof n ? k + 1 : sizeof n);
            exists = 0;
            im_do("STATUS ", n, " (UNSEEN)", select_cb);
            outf("%s\t%ld\n", n, exists);
        }
        return 0;
    }
    if ((e = im_do("SELECT ", folder, "", select_cb)) < 0) return e;
    if (!strcmp(cmd, "list")) {
        if (!exists) return 0;
        long from = exists > MAXLIST ? exists - MAXLIST + 1 : 1;
        buf_t b = { 0 };
        b_fmt(&b, "FETCH %ld:* (UID FLAGS BODY.PEEK[HEADER.FIELDS (FROM SUBJECT DATE)])", from);
        nent = 0;
        e = im_cmd(b.s, fetch_cb);
        b_free(&b);
        if (e >= 0) print_ents();
        return e;
    }
    if (!is_uid(id)) return no("no such message");
    char c[160], *p;
    if (strcmp(cmd, "delete")) {                         /* read, save */
        p = c + strlcpy(c, "UID FETCH ", sizeof c); p += strlcpy(p, id, 24); strlcpy(p, *cmd == 'r' ? " BODY[]" : " BODY.PEEK[]", 13);
        imsg.len = 0;
        if ((e = im_cmd(c, body_cb)) < 0) return e;
        if (!imsg.len) return no("no such message");
        return deliver(cmd, imsg.s, imsg.len);
    }
    const char *tr = role[cur][0];                       /* delete: a copy in the Trash first (not from it) */
    if (!tr[0] && (e = im_list()) < 0) return e;
    int move = tr[0] && strcmp(tr, folder);
    p = c + strlcpy(c, "UID COPY ", sizeof c); p += strlcpy(p, id, 24); strlcpy(p, " ", 2);
    if (move && (e = im_do(c, tr, "", 0)) < 0) return e;
    p = c + strlcpy(c, "UID STORE ", sizeof c); p += strlcpy(p, id, 24); strlcpy(p, " +FLAGS.SILENT (\\Deleted)", 30);
    if ((e = im_cmd(c, 0)) < 0 || (e = im_cmd("EXPUNGE", 0)) < 0) return e;
    if (move) outf("moved to %s\n", tr); else outf("deleted\n");
    return 0;
}

/* ---- POP3 (RFC 1939): new messages copied into ~/Mail/INBOX. */
static char pline[16384];                                /* (a message's lines can be long) */
static long pop_cmd(conn_t *c, const char *cmd)
{
    long e;
    if (cmd) { buf_t b = { 0 }; b_fmt(&b, "%s\r\n", cmd); e = c_send(c, b.s, b.len); wipe(b.s, b.len); b_free(&b); if (e < 0) return e; }
    if ((e = c_line(c, pline, sizeof pline)) < 0) return e;
    return starts(pline, "+OK") ? 0 : no(pline);
}
static long pop_multi(conn_t *c, buf_t *b)               /* lines up to ".", unstuffed */
{
    for (long e; ; ) {
        if ((e = c_line(c, pline, sizeof pline)) < 0) return e;
        if (!strcmp(pline, ".")) return 0;
        if (b->len + e + 2 <= MAXMSG) { b_str(b, pline + (pline[0] == '.')); b_str(b, "\r\n"); }
    }
}
static long pop_open(conn_t *c)
{
    char cmd[160];
    long e = c_open(c, A.ihost, A.iport, implicit(A.iport));
    if (e >= 0) e = pop_cmd(c, 0);
    if (e >= 0 && !c->t && (e = pop_cmd(c, "STLS")) >= 0) e = c_tls(c, A.ihost);
    if (e >= 0) { strlcpy(cmd + strlcpy(cmd, "USER ", sizeof cmd), A.user, sizeof cmd - 5); e = pop_cmd(c, cmd); }
    if (e >= 0) { strlcpy(cmd + strlcpy(cmd, "PASS ", sizeof cmd), A.pass, sizeof cmd - 5); e = pop_cmd(c, cmd); wipe(cmd, sizeof cmd); }
    if (e < 0) c_close(c);
    return e;
}
static void local(char *path, const char *name)          /* ~/Mail/INBOX[-account][/name] */
{
    char *p = path + strlcpy(path, home, 96);
    p += strlcpy(p, "/Mail/INBOX", 16);
    if (cur) { *p++ = '-'; p[num_str(p, cur)] = 0; }
    if (name) { strlcpy(path + strlen(path), "/", 2); strlcpy(path + strlen(path), name, 80); }
}
static int is_name(const char *id) { return *id && *id != '.' && !strchr(id, '/') && strlen(id) < 72; }

/* The server's messages and their UIDLs; for each, what to do. */
static long pop_each(conn_t *c, const char *only, long (*fn)(conn_t *, long, const char *))
{
    buf_t u = { 0 };
    long e = pop_cmd(c, "UIDL");
    if (e >= 0) e = pop_multi(c, &u);
    for (char *p = u.s; e >= 0 && p && *p; p = strchr(p, '\n'), p = p ? p + 1 : 0) {
        char n[72], *sp = strchr(p, ' ');
        if (!sp) continue;
        file_name(n, sp + 1);
        if (!only || !strcmp(n, only)) e = fn(c, lead(p), n);
    }
    b_free(&u);
    return e;
}
static long pop_dele(conn_t *c, long k, const char *n)
{
    (void)n;
    char cmd[32] = "DELE ";
    cmd[5 + num_str(cmd + 5, k)] = 0;
    return pop_cmd(c, cmd);
}
static long pop_get(conn_t *c, long k, const char *n)   /* a new one: RETR, kept; (not keep: DELE) */
{
    char path[180], tmp[184], cmd[32];
    siefs_stat_t st;
    local(path, n);
    if (fs_stat(path, &st, 1)) {
        strlcpy(cmd, "LIST ", sizeof cmd); cmd[5 + num_str(cmd + 5, k)] = 0;
        long e = pop_cmd(c, cmd);                        /* +OK k size: too big, left there */
        const char *sz = e >= 0 ? strchr(pline + 4, ' ') : 0;
        if (e < 0) return e;
        if (sz && lead(sz + 1) > MAXMSG) return 0;
        memcpy(cmd, "RETR", 4);
        buf_t m = { 0 };
        if ((e = pop_cmd(c, cmd)) < 0 || (e = pop_multi(c, &m)) < 0) { b_free(&m); return e; }
        strlcpy(tmp + strlcpy(tmp, path, sizeof tmp), "~", 2);   /* (whole, or not at all) */
        long h = fs_open(tmp, FS_WRONLY | FS_CREAT | FS_TRUNC, 0600);
        if (h >= 0) { fs_write(h, 0, m.s ? m.s : "", m.len); fs_close(h); fs_rename(tmp, path); }
        b_free(&m);
        if (h < 0) return no("cannot write in ~/Mail/INBOX");
    }
    return A.keep ? 0 : pop_dele(c, k, n);
}
static long pop_list(int print)
{
    char path[180], name[256];
    static char d[4096];
    local(path, 0);
    long h = fs_open(path, FS_RDONLY | FS_DIRECTORY, 0);
    if (h < 0) return 0;
    uint64_t cs = 0, ino;
    long n;
    int type;
    nent = 0;
    while ((n = fs_readdir(h, &cs, d, sizeof d)) > 0)
        for (const char *p = d; fs_dirent(&p, d + n, &ino, &type, name); ) {
            size_t l = strlen(name), len = 0;
            if (type != 8 || name[l - 1] == '~') continue;
            local(path, name);
            char *m = file_get(path, &len), v[4];
            if (m) add_ent(name, m, len, fs_getxattr(path, "seen", v, sizeof v) > 0);
            free(m);
        }
    fs_close(h);
    if (print) print_ents();
    return 0;
}
static long pop_run(const char *cmd, const char *id)
{
    conn_t c;
    char path[180];
    long e = 0;
    if (!strcmp(cmd, "folders")) {
        int u = 0;
        pop_list(0);
        for (int i = 0; i < nent; i++) u += !ents[i].seen;
        outf("INBOX\t%d\n", u);
        return 0;
    }
    if (!strcmp(cmd, "list")) {
        local(path, 0);
        strlcpy(path + strlen(home), "/Mail", 6); fs_mkdir(path, 0700);
        local(path, 0); fs_mkdir(path, 0700);
        if ((e = pop_open(&c)) >= 0) { e = pop_each(&c, 0, pop_get); if (e >= 0) e = pop_cmd(&c, "QUIT"); c_close(&c); }
        pop_list(1);
        return e;
    }
    if (!is_name(id)) return no("no such message");
    local(path, id);
    size_t len = 0;
    char *m = file_get(path, &len);
    if (!m) return no("no such message");
    if (strcmp(cmd, "delete")) {
        e = deliver(cmd, m, len);
        if (*cmd == 'r') fs_setxattr(path, "seen", "1", 1);
    } else {                                             /* delete: on the server too */
        if ((e = pop_open(&c)) >= 0) { e = pop_each(&c, id, pop_dele); if (e >= 0) e = pop_cmd(&c, "QUIT"); c_close(&c); }
        if (e >= 0) { fs_unlink(path); outf("deleted\n"); }
    }
    free(m);
    return e;
}

/* ---- SMTP (RFC 5321, submission RFC 6409; AUTH PLAIN or LOGIN). */
static long sm_reply(conn_t *c, buf_t *all)             /* -> its code (2xx, 3xx), or -error */
{
    for (long e; ; ) {
        if ((e = c_line(c, pline, sizeof pline)) < 0) return e;
        if (e < 3) return no(pline);
        if (all) { b_str(all, pline); b_str(all, "\n"); }
        if (pline[3] != '-') break;
    }
    long code = (pline[0] - '0') * 100 + (pline[1] - '0') * 10 + (pline[2] - '0');
    return code >= 200 && code < 400 ? code : no(pline);
}
static long sm_cmd(conn_t *c, const char *cmd, buf_t *all)
{
    buf_t b = { 0 };
    b_fmt(&b, "%s\r\n", cmd);
    long e = c_send(c, b.s, b.len);
    wipe(b.s, b.len); b_free(&b);
    return e < 0 ? e : sm_reply(c, all);
}
static void b64_line(char *o, size_t cap, const char *pre, const void *s, size_t n)   /* pre + base64(s) */
{
    size_t l = strlcpy(o, pre, cap);
    if (l + n * 4 / 3 + 5 < cap) b64_encode(s, n, o + l);
}
static int ascii(const char *s) { for (; *s; s++) if ((uint8_t)*s >= 0x80) return 0; return 1; }
static void word(buf_t *m, const char *v, int q)         /* v, "v" (q), or (not ASCII) one encoded word */
{
    if (ascii(v)) { b_fmt(m, q ? "\"%s\"" : "%s", v); return; }
    char e[700];
    b64_encode(v, strlen(v) < 500 ? strlen(v) : 500, e);
    b_fmt(m, q ? "\"=?UTF-8?B?%s?=\"" : "=?UTF-8?B?%s?=", e);
}
static void rfc_date(char *o)                            /* "Thu, 16 Oct 2026 10:20:30 +0000" */
{
    net_info_t ni;
    if (net_info(&ni) < 0) ni.time = 0;
    char d[20];
    date_str(ni.time * 1000000000LL, d);                 /* YYYY-MM-DD HH:MM */
    static const char days[] = "ThuFriSatSunMonTueWed", mons[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int m = (d[5] - '0') * 10 + d[6] - '0', s = (int)(ni.time % 60);
    char *p = o;
    memcpy(p, days + ni.time / 86400 % 7 * 3, 3); p += 3; *p++ = ','; *p++ = ' ';
    *p++ = d[8]; *p++ = d[9]; *p++ = ' '; memcpy(p, mons + (m - 1) * 3, 3); p += 3; *p++ = ' ';
    memcpy(p, d, 4); p += 4; *p++ = ' '; memcpy(p, d + 11, 5); p += 5;
    *p++ = ':'; *p++ = '0' + s / 10; *p++ = '0' + s % 10;
    strlcpy(p, " +0000", 8);
}
static const char *ctype(const char *n)                  /* a file's type, by its name's end */
{
    static const char *const t[] = { ".pdf", "application/pdf", ".png", "image/png", ".jpg", "image/jpeg", ".txt", "text/plain",
                                     ".html", "text/html", ".zip", "application/zip", 0 };
    size_t l = strlen(n);
    for (int i = 0; t[i]; i += 2) if (l > strlen(t[i]) && same_ci(n + l - strlen(t[i]), t[i], strlen(t[i]))) return t[i + 1];
    return "application/octet-stream";
}
static long im_append(buf_t *m)                          /* a copy of what was sent, in Sent (IMAP) */
{
    char t[40] = " (\\Seen) {";
    long e = im_open();
    if (e >= 0 && !role[cur][1][0]) e = im_list();
    if (e < 0 || !role[cur][1][0]) return e < 0 ? e : no("no Sent folder");
    t[10 + num_str(t + 10, m->len)] = 0;
    strlcpy(t + strlen(t), "}", 2);
    lit = m->s; litn = m->len;
    return im_do("APPEND ", role[cur][1], t, 0);
}
/* A draft (To:, Cc:, Subject:, In-Reply-To:, References:, Attach: lines,
 * a blank line, the text) sent from account cur: MIME, multipart/mixed
 * with the files attached (base64); then a copy in Sent. */
static long sm_send(const char *d, size_t dl)
{
    char to[1024], cc[1024], subj[512], irt[256], refs[512], date[40], id[24];
    mime_header(d, dl, "to", to, sizeof to); mime_header(d, dl, "cc", cc, sizeof cc);
    mime_header(d, dl, "subject", subj, sizeof subj);
    mime_header(d, dl, "in-reply-to", irt, sizeof irt); mime_header(d, dl, "references", refs, sizeof refs);
    const char *body = d + dl;                           /* after the blank line */
    for (const char *p = d; *p; p++) if (*p == '\n' && (p[1] == '\n' || (p[1] == '\r' && p[2] == '\n'))) { body = p + (p[1] == '\n' ? 2 : 3); break; }
    if (!to[0]) return no("no To: address");
    const char *dom = strchr(A.addr, '@') ? strchr(A.addr, '@') + 1 : "sieos";

    buf_t m = { 0 };                                     /* the message: CRLF lines */
    uint8_t r[8];
    rfc_date(date);
    if (sys_random(r, sizeof r) < 0) memset(r, 7, sizeof r);
    for (int i = 0; i < 8; i++) { id[2 * i] = "0123456789abcdef"[r[i] >> 4]; id[2 * i + 1] = "0123456789abcdef"[r[i] & 15]; }
    id[16] = 0;
    b_str(&m, "From: ");
    if (A.name[0]) { word(&m, A.name, ascii(A.name)); b_fmt(&m, " <%s>\r\n", A.addr); } else b_fmt(&m, "%s\r\n", A.addr);
    b_fmt(&m, "To: %s\r\n", to);
    if (cc[0]) b_fmt(&m, "Cc: %s\r\n", cc);
    b_str(&m, "Subject: "); word(&m, subj, 0);
    b_fmt(&m, "\r\nDate: %s\r\nMessage-ID: <%s@%s>\r\n", date, id, dom);
    if (irt[0]) b_fmt(&m, "In-Reply-To: %s\r\nReferences: %s%s%s\r\n", irt, refs, refs[0] ? " " : "", irt);
    int multi = find_ci(d, body, "\nattach:") || same_ci(d, "attach:", 7);
    b_str(&m, "MIME-Version: 1.0\r\n");
    if (multi) b_fmt(&m, "Content-Type: multipart/mixed; boundary=\"=_%s\"\r\n\r\n--=_%s\r\n", id, id);
    b_str(&m, "Content-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: 8bit\r\n\r\n");
    for (const char *p = body; *p; ) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        b_add(&m, p, l && p[l - 1] == '\r' ? l - 1 : l); b_str(&m, "\r\n");
        p += l + !!e;
    }
    long e = 0;
    for (const char *p = d; e >= 0 && p && p < body; p = nl(p)) {   /* the files */
        if (!same_ci(p, "attach:", 7)) continue;
        char path[240], *n = path;
        size_t i = 0, fl = 0;
        for (p += 7; *p == ' '; p++) ;
        while (*p && *p != '\r' && *p != '\n' && i + 1 < sizeof path) path[i++] = *p++;
        path[i] = 0;
        char *f = file_get(path, &fl);
        if (!f) { e = no("cannot read a file attached"); break; }
        for (char *c = path; *c; c++) if (*c == '/') n = c + 1;
        b_fmt(&m, "--=_%s\r\nContent-Type: %s; name=", id, ctype(n)); word(&m, n, 1);
        b_str(&m, "\r\nContent-Disposition: attachment; filename="); word(&m, n, 1);
        b_str(&m, "\r\nContent-Transfer-Encoding: base64\r\n\r\n");
        for (i = 0; i < fl; i += 57) {                   /* 76 characters a line */
            char l[80];
            b64_encode(f + i, fl - i < 57 ? fl - i : 57, l);
            b_str(&m, l); b_str(&m, "\r\n");
        }
        free(f);
    }
    if (multi) b_fmt(&m, "--=_%s--\r\n", id);

    conn_t c;
    buf_t ehlo = { 0 }, dt = { 0 };
    char cmd[1200];
    int nrcpt = 0;
    strlcpy(cmd + strlcpy(cmd, "EHLO ", sizeof cmd), dom, sizeof cmd - 5);
    if (e >= 0) e = c_open(&c, A.ohost, A.oport, implicit(A.oport));
    else c.on = 0;
    if (e >= 0) e = sm_reply(&c, 0);
    if (e >= 0) e = sm_cmd(&c, cmd, &ehlo);
    if (e >= 0 && !c.t) {                                /* 587: TLS first, then again */
        if ((e = sm_cmd(&c, "STARTTLS", 0)) >= 0 && (e = c_tls(&c, A.ohost)) >= 0) { ehlo.len = 0; e = sm_cmd(&c, cmd, &ehlo); }
    }
    if (e >= 0 && A.user[0]) {
        const char *au = ehlo.s ? find_ci(ehlo.s, ehlo.s + ehlo.len, "AUTH") : 0;
        if (au && !find_ci(au, strchr(au, '\n'), "PLAIN") && find_ci(au, strchr(au, '\n'), "LOGIN")) {
            b64_line(cmd, sizeof cmd, "", A.user, strlen(A.user));
            if ((e = sm_cmd(&c, "AUTH LOGIN", 0)) >= 0 && (e = sm_cmd(&c, cmd, 0)) >= 0) {
                b64_line(cmd, sizeof cmd, "", A.pass, strlen(A.pass));
                e = sm_cmd(&c, cmd, 0);
            }
        } else {
            char pl[300];
            size_t ul = strlen(A.user), pw = strlen(A.pass);
            if (ul + pw + 2 > sizeof pl) e = no("user name or password too long");
            else {
                pl[0] = 0; memcpy(pl + 1, A.user, ul); pl[ul + 1] = 0; memcpy(pl + ul + 2, A.pass, pw);
                b64_line(cmd, sizeof cmd, "AUTH PLAIN ", pl, ul + pw + 2);
                wipe(pl, sizeof pl);
                e = sm_cmd(&c, cmd, 0);
            }
        }
        wipe(cmd, sizeof cmd);
        if (e < 0 && e == -EPROTO) strlcpy(why + strlcpy(why, "login refused: ", sizeof why), pline, sizeof why - 15);
    }
    if (e >= 0) { strlcpy(cmd, "MAIL FROM:<", sizeof cmd); strlcpy(cmd + 11, A.addr, 200); strlcpy(cmd + strlen(cmd), ">", 2); e = sm_cmd(&c, cmd, 0); }
    for (int k = 0; k < 2 && e >= 0; k++)                 /* every address of To and Cc (commas in quotes are names') */
        for (const char *p = k ? cc : to; e >= 0 && *p; ) {
            const char *end = p, *lt = 0, *gt = 0;
            for (int q = 0; *end && (q || *end != ','); end++) q ^= *end == '"';
            for (const char *s = p; s < end; s++) { if (*s == '<') lt = s + 1; if (*s == '>') gt = s; }
            const char *a = lt && gt > lt ? lt : p, *ae = lt && gt > lt ? gt : end;
            while (a < ae && *a == ' ') a++;
            while (ae > a && ae[-1] == ' ') ae--;
            if (ae > a && ae - a < 200) {
                char *o = cmd + strlcpy(cmd, "RCPT TO:<", sizeof cmd);
                memcpy(o, a, ae - a); strlcpy(o + (ae - a), ">", 2);
                e = sm_cmd(&c, cmd, 0);
                nrcpt++;
            }
            p = *end ? end + 1 : end;
        }
    if (e >= 0 && !nrcpt) e = no("no address to send to");
    for (const char *p = m.s; e >= 0 && p && *p; p = nl(p)) {   /* DATA: dots doubled, a dot at the end */
        if (*p == '.') b_str(&dt, ".");
        b_add(&dt, p, strchr(p, '\n') ? (size_t)(strchr(p, '\n') - p + 1) : strlen(p));
    }
    b_str(&dt, ".\r\n");
    if (e >= 0 && (e = sm_cmd(&c, "DATA", 0)) >= 0 && (e = c_send(&c, dt.s, dt.len)) >= 0 && (e = sm_reply(&c, 0)) >= 0) {
        sm_cmd(&c, "QUIT", 0);
        outf("sent to %d address%s\n", nrcpt, nrcpt == 1 ? "" : "es");
    }
    c_close(&c);
    if (e >= 0 && A.sent && strcmp(A.in, "pop3")) {
        long r = im_append(&m);
        if (r < 0 && r != -EPROTO) { im_close(); r = im_append(&m); }   /* (a stale connection: once more) */
        if (r < 0) outf("no copy in Sent: %s\n", why[0] ? why : net_strerror(r)); else outf("a copy in %s\n", role[cur][1]);
        why[0] = 0;
    }
    b_free(&m); b_free(&ehlo); b_free(&dt);
    return e < 0 ? e : 0;
}

/* ---- mailto: links (RFC 6068): the draft they make. */
static void pct(buf_t *b, const char *s, const char *e, int text)   /* %XX decoded (a header: no control characters) */
{
    for (; s < e; s++) {
        char c = *s;
        if (c == '%' && s + 2 < e && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) { c = (char)(hexv(s[1]) << 4 | hexv(s[2])); s += 2; }
        if (c == '\r' || ((uint8_t)c < ' ' && !(text && c == '\n'))) continue;
        b_add(b, &c, 1);
    }
}
static void mailto(const char *u, buf_t *o)
{
    static const char *const k[] = { "to", "cc", "subject", "body" };
    buf_t v[4] = { { 0 } };
    if (same_ci(u, "mailto:", 7)) u += 7;
    const char *q = strchr(u, '?');
    pct(&v[0], u, q ? q : u + strlen(u), 0);
    for (const char *p = q; p && *p++; p = strchr(p, '&')) {   /* ?key=value&... */
        const char *eq = strchr(p, '='), *amp = strchr(p, '&');
        if (!amp) amp = p + strlen(p);
        for (int i = 0; i < 4 && eq && eq < amp; i++)
            if ((size_t)(eq - p) == strlen(k[i]) && same_ci(p, k[i], eq - p)) {
                if (v[i].len && i < 2) b_str(&v[i], ", ");
                pct(&v[i], eq + 1, amp, i == 3);
            }
    }
    b_fmt(o, "To: %s\nCc: %s\nSubject: %s\n\n%s", v[0].s ? v[0].s : "", v[1].s ? v[1].s : "", v[2].s ? v[2].s : "", v[3].s ? v[3].s : "");
    for (int i = 0; i < 4; i++) b_free(&v[i]);
}

/* ---- The commands. */
static void command(int ac, char **av)
{
    why[0] = 0;
    cur = 0;
    if (ac > 1 && !strcmp(av[0], "-a")) { cur = strnum(av[1]); ac -= 2; av += 2; }
    const char *cmd = ac ? av[0] : "list", *id = ac > 1 ? av[1] : "";
    xa[0] = ac > 3 ? av[3] : 0; xa[1] = ac > 4 ? av[4] : 0;
    long e = cur < 0 || cur >= MAXA ? (cur = 0, no("no such account")) : 0;
    if (e >= 0 && !strcmp(cmd, "mailto")) { mailto(id, &out); return; }
    if (e >= 0) e = load();
    buf_t d = { 0 };
    if (e < 0) ;
    else if (!strcmp(cmd, "send")) {
        size_t dl = 0;
        char *f = ac > 1 ? file_get(av[1], &dl) : 0;
        e = f ? sm_send(f, dl) : no("usage: mail send FILE (a draft)");
        free(f);
    } else if (same_ci(cmd, "mailto:", 7)) {             /* a terminal: the text typed, up to a line "." */
        char l[1024];
        mailto(cmd, &d);
        outf("%s(the text; a line with only . sends it)\n", d.s);
        flush();
        for (long n; (n = con_read(l, sizeof l - 1)) > 0 && !(l[0] == '.' && (n == 1 || l[1] == '\n')); ) b_add(&d, l, n);
        e = sm_send(d.s, d.len);
    } else if (!strcmp(cmd, "list") || !strcmp(cmd, "folders") || !strcmp(cmd, "read") || !strcmp(cmd, "delete") || !strcmp(cmd, "save")) {
        const char *folder = !strcmp(cmd, "list") ? (ac > 1 ? av[1] : "INBOX") : ac > 2 ? av[2] : "INBOX";
        if (!strcmp(A.in, "pop3")) e = pop_run(cmd, id);
        else {
            int was = im.on;
            e = im_run(cmd, id, folder);
            if (e < 0 && e != -EPROTO && was) { im_close(); e = im_run(cmd, id, folder); }   /* (a stale connection: once more) */
        }
    } else e = no("usage: mail [-a N] [list [FOLDER] | folders | read|save|delete ID [FOLDER] | send FILE | mailto:URL]");
    b_free(&d);
    if (e < 0) { outf("error: %s\n", why[0] ? why : net_strerror(e)); if (e != -EPROTO) im_close(); }
}

int main(int argc, char **argv)
{
    mk_ident_t me;
    acct_user_t u;
    if (sys_ident(0, &me) < 0 || acct_user(0, me.uid, &u)) { printf("mail: who are you?\n"); return 1; }
    strlcpy(home, u.home, sizeof home);
    if (argc < 2 || strcmp(argv[1], "-S")) { command(argc - 1, argv + 1); flush(); return 0; }
    static char line[1024], *av[8];
    for (;;) {                                           /* serve: a line, an answer, \x1e */
        long n = con_read(line, sizeof line - 1);
        if (n <= 0) return 0;
        line[n] = 0;
        if (n && line[n - 1] == '\n') line[--n] = 0;
        int ac = 0;
        for (char *p = line; *p && ac < 8; ) { av[ac++] = p; p = strchr(p, '\t'); if (!p) break; *p++ = 0; }
        command(ac, av);
        outf("\x1e\n");
        flush();
    }
}
