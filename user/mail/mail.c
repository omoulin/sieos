/*
 * mail - Mail: IMAP or POP3 to read, SMTP to send, always over TLS
 * (docs/desktop.md, Mail). The command line, and the desktop's Mail
 * window, which runs it as you (mail -S) and talks to it on its console:
 *
 *   mail [list [FOLDER]]       the newest 100 messages: id, * if unread, date, from, subject
 *   mail folders               the folders (POP3: INBOX only)
 *   mail read ID [FOLDER]      a message as text (and marked read)
 *   mail delete ID [FOLDER]    deleted (IMAP: \Deleted, then EXPUNGE)
 *   mail send FILE             a draft: To:, Cc:, Subject: lines, a blank line, the text
 *   mail -S                    commands from the console, one a line, words split by
 *                              tabs; each answer ends with a line of one \x1e byte
 *
 *   Atlas's Mail ──"list\tINBOX"──► mail -S (as you) ──IMAP/POP3/SMTP over TLS──► servers
 *             ◄── lines, then \x1e ──┘
 *
 * The account is ~/.mail (key=value lines, only yours to read): name,
 * address, in (imap or pop3), in_host, in_port, out_host, out_port, user,
 * pass, keep (POP3: leave the messages on the server, 1 or 0). Ports 993,
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
#define MAXLIST 100

static struct { char name[64], addr[128], in[8], ihost[128], ohost[128], user[128], pass[128]; int iport, oport, keep; } A;
static char home[64], why[200];          /* why: the server's words when it says no */
static char *cfg_text;                   /* ~/.mail as last read */
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

/* ---- The account. */
static void conf(const char *t, const char *key, char *v, size_t cap)
{
    size_t kl = strlen(key);
    v[0] = 0;
    for (const char *p = t; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : 0)
        if (!memcmp(p, key, kl) && p[kl] == '=') {
            size_t i = 0;
            for (p += kl + 1; *p && *p != '\n' && *p != '\r' && i + 1 < cap; ) v[i++] = *p++;
            v[i] = 0;
            return;
        }
}
static void im_close(void);
static long load(void)
{
    char path[96], v[16];
    strlcpy(path + strlcpy(path, home, sizeof path), "/.mail", 8);
    char *t = file_get(path, 0);
    if (!t) return no("no account yet: set it up in Mail on the desktop, or in ~/.mail");
    if (!cfg_text || strcmp(t, cfg_text)) im_close();    /* (changed: connect again) */
    free(cfg_text);
    cfg_text = t;
    conf(t, "name", A.name, sizeof A.name); conf(t, "address", A.addr, sizeof A.addr);
    conf(t, "in", A.in, sizeof A.in);
    conf(t, "in_host", A.ihost, sizeof A.ihost); conf(t, "out_host", A.ohost, sizeof A.ohost);
    conf(t, "user", A.user, sizeof A.user); conf(t, "pass", A.pass, sizeof A.pass);
    int pop = !strcmp(A.in, "pop3");
    conf(t, "in_port", v, sizeof v); A.iport = v[0] ? strnum(v) : pop ? 995 : 993;
    conf(t, "out_port", v, sizeof v); A.oport = v[0] ? strnum(v) : 465;
    conf(t, "keep", v, sizeof v); A.keep = v[0] != '0';
    return 0;
}

/* ---- A connection: plain at first or TLS at once; lines and bytes in. */
typedef struct { int on; long s; tls_t *t; size_t pos, len; char in[4096]; } conn_t;
static long c_tls(conn_t *c, const char *host)
{
    long e;
    c->t = tls_connect(c->s, host, 0, &e);
    if (!c->t) c->on = 0;
    return c->t ? 0 : e;
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

/* ---- IMAP (RFC 3501). A command, then its untagged answers one by one
 * to cb (a literal {n} in one: its bytes as lit, the line around it joined). */
static conn_t im;
static int tagn;
static buf_t iline, ilit, imsg;
typedef void (*im_cb)(const char *line, const char *lit, size_t ln);
static void im_close(void) { c_close(&im); }
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
        if (!memcmp(iline.s, tag, tl)) {
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
static void folder_cb(const char *l, const char *lit, size_t ln)   /* * LIST (\HasNoChildren) "/" "INBOX" */
{
    if (!starts(l, "* LIST (")) return;
    const char *f = l + 8, *p = strchr(f, ')');
    if (!p || find_ci(f, p, "\\noselect")) return;
    for (p++; *p == ' '; p++) ;
    if (*p == '"') { p = strchr(p + 1, '"'); if (!p) return; p++; } else while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    char n[160];
    size_t o = 0;
    if (!*p && ln) { strlcpy(n, lit, ln + 1 < sizeof n ? ln + 1 : sizeof n); outf("%s\n", n); return; }
    int q = *p == '"';
    for (p += q; *p && (!q || *p != '"') && o + 1 < sizeof n; p++) {
        if (q && *p == '\\' && p[1]) p++;
        n[o++] = *p;
    }
    n[o] = 0;
    outf("%s\n", n);
}
static long exists;
static void select_cb(const char *l, const char *lit, size_t ln)   /* * 23 EXISTS */
{
    (void)lit; (void)ln;
    const char *s = strchr(l + 2, ' ');
    if (s && starts(s, " EXISTS")) exists = lead(l + 2) > 0 ? lead(l + 2) : 0;
}
static long im_select(const char *folder)
{
    buf_t c = { 0 };
    b_str(&c, "SELECT "); quote(&c, folder);
    exists = 0;
    long e = im_cmd(c.s, select_cb);
    b_free(&c);
    return e;
}
static void fetch_cb(const char *l, const char *lit, size_t ln)   /* * 5 FETCH (UID 9 FLAGS (\Seen) BODY[...] {n}) */
{
    const char *u = find_ci(l, l + strlen(l), "UID "), *f = find_ci(l, l + strlen(l), "FLAGS (");
    if (!u || !find_ci(l, l + strlen(l), "FETCH")) return;
    char id[24];
    size_t i = 0;
    for (u += 4; *u >= '0' && *u <= '9' && i + 1 < sizeof id; ) id[i++] = *u++;
    id[i] = 0;
    const char *fe = f ? strchr(f, ')') : 0;
    add_ent(id, lit, ln, fe && find_ci(f, fe, "\\seen"));
}
static void body_cb(const char *l, const char *lit, size_t ln)   /* (the literal kept: swapped out) */
{
    (void)lit; (void)ln;
    if (find_ci(l, l + strlen(l), "FETCH")) { buf_t t = imsg; imsg = ilit; ilit = t; }
}
static int is_uid(const char *id) { if (!*id) return 0; for (; *id; id++) if (*id < '0' || *id > '9') return 0; return 1; }

static long im_run(const char *cmd, const char *id, const char *folder)
{
    long e = im_open();
    if (e < 0) return e;
    if (!strcmp(cmd, "folders")) return im_cmd("LIST \"\" \"*\"", folder_cb);
    if ((e = im_select(folder)) < 0) return e;
    char c[160], *p;
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
    if (!strcmp(cmd, "read")) {
        p = c + strlcpy(c, "UID FETCH ", sizeof c); p += strlcpy(p, id, 24); strlcpy(p, " BODY[]", 8);
        imsg.len = 0;
        if ((e = im_cmd(c, body_cb)) < 0) return e;
        if (!imsg.len) return no("no such message");
        buf_t o = { 0 };
        mime_read(imsg.s, imsg.len, &o);
        b_add(&out, o.s, o.len);
        b_free(&o);
        return 0;
    }
    p = c + strlcpy(c, "UID STORE ", sizeof c); p += strlcpy(p, id, 24); strlcpy(p, " +FLAGS.SILENT (\\Deleted)", 30);
    if ((e = im_cmd(c, 0)) < 0 || (e = im_cmd("EXPUNGE", 0)) < 0) return e;
    outf("deleted\n");
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
static void local(char *path, const char *name)          /* ~/Mail/INBOX[/name] */
{
    strlcpy(path + strlcpy(path, home, 96), "/Mail/INBOX", 16);
    if (name) { strlcpy(path + strlen(path), "/", 2); strlcpy(path + strlen(path), name, 80); }
}
static void file_name(char *n, const char *uidl)        /* a UIDL as a file name: no '/', no leading '.' */
{
    size_t i = 0;
    for (; uidl[i] && uidl[i] > ' ' && i < 70; i++) n[i] = uidl[i] == '/' || (!i && uidl[i] == '.') ? '_' : uidl[i];
    n[i] = 0;
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
static long pop_list(void)
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
    print_ents();
    return 0;
}
static long pop_run(const char *cmd, const char *id)
{
    conn_t c;
    char path[180];
    long e = 0;
    if (!strcmp(cmd, "folders")) { outf("INBOX\n"); return 0; }
    if (!strcmp(cmd, "list")) {
        local(path, 0);
        strlcpy(path + strlen(home), "/Mail", 6); fs_mkdir(path, 0700);
        local(path, 0); fs_mkdir(path, 0700);
        if ((e = pop_open(&c)) >= 0) { e = pop_each(&c, 0, pop_get); if (e >= 0) e = pop_cmd(&c, "QUIT"); c_close(&c); }
        pop_list();
        return e;
    }
    if (!is_name(id)) return no("no such message");
    local(path, id);
    size_t len = 0;
    char *m = file_get(path, &len);
    if (!m) return no("no such message");
    if (!strcmp(cmd, "read")) {
        mime_read(m, len, &out);
        fs_setxattr(path, "seen", "1", 1);
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
static void header(buf_t *m, const char *name, const char *v)   /* (not ASCII: one encoded word) */
{
    if (ascii(v)) { b_fmt(m, "%s: %s\r\n", name, v); return; }
    char e[700];
    b64_encode(v, strlen(v) < 500 ? strlen(v) : 500, e);
    b_fmt(m, "%s: =?UTF-8?B?%s?=\r\n", name, e);
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
static long sm_send(const char *file)
{
    size_t dl = 0;
    char *d = file_get(file, &dl);
    if (!d) return no("cannot read the draft");
    char to[1024], cc[1024], subj[512], irt[256], refs[512];
    mime_header(d, dl, "to", to, sizeof to); mime_header(d, dl, "cc", cc, sizeof cc);
    mime_header(d, dl, "subject", subj, sizeof subj);
    mime_header(d, dl, "in-reply-to", irt, sizeof irt); mime_header(d, dl, "references", refs, sizeof refs);
    const char *body = d;                                /* after the blank line */
    for (const char *p = d; *p; p++) if (*p == '\n' && (p[1] == '\n' || (p[1] == '\r' && p[2] == '\n'))) { body = p + (p[1] == '\n' ? 2 : 3); break; }
    if (body == d) body = d + dl;
    if (!to[0]) { free(d); return no("no To: address"); }
    const char *dom = strchr(A.addr, '@') ? strchr(A.addr, '@') + 1 : "sieos";

    buf_t m = { 0 };                                     /* the message: CRLF lines, dots doubled */
    char date[40], id[24];
    uint8_t r[8];
    rfc_date(date);
    if (sys_random(r, sizeof r) < 0) memset(r, 7, sizeof r);
    for (int i = 0; i < 8; i++) { id[2 * i] = "0123456789abcdef"[r[i] >> 4]; id[2 * i + 1] = "0123456789abcdef"[r[i] & 15]; }
    id[16] = 0;
    if (A.name[0] && ascii(A.name)) b_fmt(&m, "From: \"%s\" <%s>\r\n", A.name, A.addr);
    else if (A.name[0]) { char e[200]; b64_encode(A.name, strlen(A.name) < 120 ? strlen(A.name) : 120, e); b_fmt(&m, "From: =?UTF-8?B?%s?= <%s>\r\n", e, A.addr); }
    else b_fmt(&m, "From: %s\r\n", A.addr);
    b_fmt(&m, "To: %s\r\n", to);
    if (cc[0]) b_fmt(&m, "Cc: %s\r\n", cc);
    header(&m, "Subject", subj);
    b_fmt(&m, "Date: %s\r\nMessage-ID: <%s@%s>\r\n", date, id, dom);
    if (irt[0]) b_fmt(&m, "In-Reply-To: %s\r\nReferences: %s%s%s\r\n", irt, refs, refs[0] ? " " : "", irt);
    b_str(&m, "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: 8bit\r\n\r\n");
    for (const char *p = body; *p; ) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l && p[l - 1] == '\r') l--;
        if (*p == '.') b_str(&m, ".");
        b_add(&m, p, l); b_str(&m, "\r\n");
        p = e ? e + 1 : p + strlen(p);
    }
    b_str(&m, ".\r\n");

    conn_t c;
    buf_t ehlo = { 0 };
    char cmd[1200];
    int nrcpt = 0;
    strlcpy(cmd + strlcpy(cmd, "EHLO ", sizeof cmd), dom, sizeof cmd - 5);
    long e = c_open(&c, A.ohost, A.oport, implicit(A.oport));
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
    for (int k = 0; k < 2 && e >= 0; k++)                 /* every address of To and Cc */
        for (const char *p = k ? cc : to; e >= 0 && *p; ) {
            const char *q = strchr(p, ','), *end = q ? q : p + strlen(p), *lt = 0, *gt = 0;
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
            p = q ? q + 1 : end;
        }
    if (e >= 0 && !nrcpt) e = no("no address to send to");
    if (e >= 0 && (e = sm_cmd(&c, "DATA", 0)) >= 0 && (e = c_send(&c, m.s, m.len)) >= 0 && (e = sm_reply(&c, 0)) >= 0) {
        sm_cmd(&c, "QUIT", 0);
        outf("sent to %d address%s\n", nrcpt, nrcpt == 1 ? "" : "es");
    }
    c_close(&c);
    b_free(&m); b_free(&ehlo);
    free(d);
    return e < 0 ? e : 0;
}

/* ---- The commands. */
static void command(int ac, char **av)
{
    const char *cmd = ac ? av[0] : "list";
    why[0] = 0;
    long e = load();
    if (e >= 0 && !strcmp(cmd, "send")) e = ac > 1 ? sm_send(av[1]) : no("usage: mail send FILE");
    else if (e >= 0 && (!strcmp(cmd, "list") || !strcmp(cmd, "folders") || !strcmp(cmd, "read") || !strcmp(cmd, "delete"))) {
        const char *id = ac > 1 ? av[1] : "", *folder = !strcmp(cmd, "list") ? (ac > 1 ? av[1] : "INBOX") : ac > 2 ? av[2] : "INBOX";
        if (!strcmp(A.in, "pop3")) e = pop_run(cmd, id);
        else {
            int was = im.on;
            e = im_run(cmd, id, folder);
            if (e < 0 && e != -EPROTO && was) { im_close(); e = im_run(cmd, id, folder); }   /* (a stale connection: once more) */
        }
    } else if (e >= 0) e = no("usage: mail [list [FOLDER] | folders | read ID [FOLDER] | delete ID [FOLDER] | send FILE]");
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
