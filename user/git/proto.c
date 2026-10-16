/*
 * proto.c - Talking to another git: fetch (upload-pack) and push
 * (receive-pack), protocol v0, over git:// (TCP port 9418) or smart HTTP(S).
 *
 * Both start with the server listing its refs ("hash name" lines, the
 * first with its capabilities after a 0 byte). Lines are "pkt-lines":
 * 4 hex digits of length, then the data; "0000" ends a group.
 *   fetch: "want <hash>" for what we lack, "have <hash>" for commits we
 *          have (so it sends only what is new), "done"; the server answers
 *          ACK or NAK, then the pack (raw: no side-band asked).
 *   push:  "<old> <new> <ref>", then a pack of what the server lacks; it
 *          answers "unpack ok" and "ok <ref>" (report-status).
 * Over git:// it is one TCP conversation ("git-upload-pack /path\0host=..");
 * over HTTP a GET of info/refs?service=... then a POST of the rest.
 * Credentials for HTTP (Basic): from the URL (user:token@host), the
 * configuration (credential.username, credential.token), or asked.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "git.h"

typedef struct {
    int http;
    long sock;                                       /* git:// */
    char host[256], path[512], url[900], user[128], pass[256], auth[512];
    uint16_t port;
    uint8_t rb[16384];                               /* git://: read ahead */
    size_t rp, rn;
} conn_t;
static http_t ht;
static const oid_t zero;

static long rd(conn_t *c, void *b, size_t n)
{
    if (c->http) return http_read(&ht, b, n);
    if (c->rp == c->rn) {
        long r = net_recv(c->sock, c->rb, sizeof c->rb, 60000);
        if (r <= 0) return r;
        c->rp = 0; c->rn = (size_t)r;
    }
    size_t k = c->rn - c->rp < n ? c->rn - c->rp : n;
    memcpy(b, c->rb + c->rp, k);
    c->rp += k;
    return (long)k;
}
static int rd_all(conn_t *c, void *b, size_t n)
{
    for (size_t got = 0; got < n; ) {
        long r = rd(c, (char *)b + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}
/* One pkt-line into b (0-terminated) -> its length, -1 a flush, -2 an error. */
static long pkt(conn_t *c, char *b, size_t cap)
{
    char h[5];
    if (rd_all(c, h, 4)) return -2;
    long len = 0;
    for (int i = 0; i < 4; i++) {
        int d = h[i] >= '0' && h[i] <= '9' ? h[i] - '0' : h[i] >= 'a' && h[i] <= 'f' ? h[i] - 'a' + 10 : -1;
        if (d < 0) return -2;
        len = len * 16 + d;
    }
    if (!len) return -1;
    if (len < 4 || (size_t)(len - 4) >= cap || rd_all(c, b, (size_t)len - 4)) return -2;
    b[len - 4] = 0;
    if (!memcmp(b, "ERR ", 4)) { fail("the remote says: %s", b + 4); return -2; }
    return len - 4;
}
static void pkt_put(buf_t *b, const char *s, size_t n)
{
    char h[5];
    sfmt(h, sizeof h, "%04x", (unsigned)(n + 4));
    bput(b, h, 4);
    bput(b, s, n);
}

/* ---- connecting: git:// or http(s)://[user[:pass]@]host[:port]/path */
static void b64(char *o, const char *s)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(s), k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)(uint8_t)s[i] << 16 | (i + 1 < n ? (uint32_t)(uint8_t)s[i + 1] << 8 : 0) | (i + 2 < n ? (uint8_t)s[i + 2] : 0);
        o[k++] = t[v >> 18 & 63]; o[k++] = t[v >> 12 & 63];
        o[k++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        o[k++] = i + 2 < n ? t[v & 63] : '=';
    }
    o[k] = 0;
}
static int parse_url(conn_t *c, const char *url)
{
    memset(c, 0, sizeof *c);
    const char *p;
    if (!memcmp(url, "git://", 6)) { p = url + 6; c->port = 9418; }
    else if (!memcmp(url, "http://", 7) || !memcmp(url, "https://", 8)) { c->http = 1; p = url + (url[4] == 's' ? 8 : 7); }
    else return fail("unsupported URL %s (git://, http:// and https:// only)", url);
    const char *at = strchr(p, '@'), *sl = strchr(p, '/');
    if (at && (!sl || at < sl)) {                    /* user[:password]@ */
        const char *colon = strchr(p, ':');
        size_t ul = (size_t)((colon && colon < at ? colon : at) - p);
        if (ul >= sizeof c->user) return fail("bad URL");
        memcpy(c->user, p, ul);
        if (colon && colon < at && (size_t)(at - colon) <= sizeof c->pass) { memcpy(c->pass, colon + 1, (size_t)(at - colon - 1)); }
        p = at + 1;
    }
    size_t hl = sl ? (size_t)(sl - p) : strlen(p);
    if (!hl || hl >= sizeof c->host) return fail("bad URL %s", url);
    memcpy(c->host, p, hl);
    strlcpy(c->path, sl ? sl : "/", sizeof c->path);
    size_t pl = strlen(c->path);
    if (pl > 1 && c->path[pl - 1] == '/') c->path[pl - 1] = 0;
    char *colon = strchr(c->host, ':');
    if (colon && !c->http) { *colon = 0; c->port = (uint16_t)strnum(colon + 1); }
    sfmt(c->url, sizeof c->url, "%s://%s%s", c->http ? (url[4] == 's' ? "https" : "http") : "git", c->host, c->path);
    return 0;
}
static int ask(const char *q, char *a, size_t cap, int hide)   /* a line from the terminal */
{
    outf("%s", q);
    oflush();
    if (hide) con_echo(0);
    long n = con_read(a, cap - 1);
    if (hide) { con_echo(1); outf("\n"); }
    if (n <= 0) return -1;
    a[n] = 0;
    while (n && (a[n - 1] == '\n' || a[n - 1] == '\r')) a[--n] = 0;
    return 0;
}
static int credentials(conn_t *c)
{
    char *u = cfg_get("credential.username"), *t = cfg_get("credential.token"), q[1000], up[400];
    if (!c->user[0] && u) strlcpy(c->user, u, sizeof c->user);
    if (!c->pass[0] && t) strlcpy(c->pass, t, sizeof c->pass);
    free(u); free(t);
    sfmt(q, sizeof q, "Username for '%s': ", c->url);
    if (!c->user[0] && ask(q, c->user, sizeof c->user, 0)) return -1;
    sfmt(q, sizeof q, "Password for '%s': ", c->url);
    if (!c->pass[0] && ask(q, c->pass, sizeof c->pass, 1)) return -1;
    sfmt(up, sizeof up, "%s:%s", c->user, c->pass);
    b64(q, up);
    wipe(up, sizeof up);
    sfmt(c->auth, sizeof c->auth, "Authorization: Basic %s\r\n", q);
    return 0;
}
/* An HTTP request, with credentials if the server wants them -> 0 (ht: the answer). */
static int request(conn_t *c, const char *method, const char *url, const char *type, const buf_t *body)
{
    char extra[700];
    for (int tries = 0; ; tries++) {
        sfmt(extra, sizeof extra, "%s%s%s%s", type ? "Content-Type: " : "", type ? type : "", type ? "\r\n" : "", c->auth);
        long e = http_request(&ht, method, url, extra, body ? body->p : 0, body ? body->n : 0, 0, 60000);
        if (e < 0) return fail("%s: %s", c->url, net_strerror(e));
        if (ht.status != 401 || tries) break;
        http_close(&ht);
        c->pass[0] = c->auth[0] = 0;
        if (credentials(c)) return fail("%s needs a user and password (git config credential.token TOKEN)", c->url);
    }
    if (ht.status != 200) { http_close(&ht); return fail("%s: HTTP %d", url, ht.status); }
    return 0;
}

/* Connect and read the server's refs (adv: name -> hash) and capabilities. */
static int open_service(conn_t *c, const char *url, const char *service, ents_t *adv, char *caps, size_t cap)
{
    char line[1100], u[1100];
    if (parse_url(c, url)) return 1;
    if (c->http) {
        sfmt(u, sizeof u, "%s://%s%s/info/refs?service=%s", url[4] == 's' ? "https" : "http", c->host, c->path, service);
        if (c->user[0] && credentials(c)) return 1;
        if (request(c, "GET", u, 0, 0)) return 1;
        if (pkt(c, line, sizeof line) < 0 || memcmp(line, "# service=", 10)) { http_close(&ht); return fail("%s is not a smart HTTP git server", c->url); }
        if (pkt(c, line, sizeof line) != -1) { http_close(&ht); return fail("bad answer from %s", c->url); }
    } else {
        uint32_t ip;
        long e = net_resolve(c->host, &ip, 0);
        if (e >= 0) e = c->sock = net_connect(ip, c->port, 0);
        if (e < 0) return fail("%s: %s", c->url, net_strerror(e));
        buf_t b = { 0 };
        int n = sfmt(line, sizeof line, "%s %s", service, c->path);
        n += 1 + sfmt(line + n + 1, sizeof line - (size_t)n - 1, "host=%s", c->host);
        pkt_put(&b, line, (size_t)n + 2);
        e = net_send(c->sock, b.p, b.n);
        free(b.p);
        if (e < 0) { net_close(c->sock); return fail("%s: %s", c->url, net_strerror(e)); }
    }
    caps[0] = 0;
    for (long n; (n = pkt(c, line, sizeof line)) != -1; ) {
        if (n < 41 || line[40] != ' ') return n == -2 ? fail("%s: the connection ended", c->url) : fail("bad ref line from %s", c->url);
        if (line[n - 1] == '\n') line[--n] = 0;
        size_t nl = strlen(line);
        if ((long)nl < n) strlcpy(caps, line + nl + 1, cap);   /* (after the first line's 0 byte) */
        size_t l = strlen(line + 41);
        if (l > 3 && !strcmp(line + 41 + l - 3, "^{}")) continue;   /* (a tag's commit) */
        if (!strcmp(line + 41, "capabilities^{}")) continue;      /* (an empty repository) */
        unhex(ents_put(adv, line + 41)->id, line);
    }
    return 0;
}
static void close_conn(conn_t *c)
{
    if (c->http) http_close(&ht);
    else if (c->sock > 0) net_close(c->sock);
}
static int send_req(conn_t *c, const char *service, buf_t *b)   /* git://: on the socket; HTTP: a POST */
{
    if (!c->http) { long e = net_send(c->sock, b->p, b->n); return e < 0 ? fail("%s: %s", c->url, net_strerror(e)) : 0; }
    char u[1100], type[64];
    http_close(&ht);
    sfmt(u, sizeof u, "%s/%s", c->url, service);
    sfmt(type, sizeof type, "application/x-%s-request", service);
    return request(c, "POST", u, type, b);
}

/* ---- fetch */
typedef struct { buf_t *b; set_t *seen; int n; } have_t;
static void add_have(const char *name, const oid_t id, void *ctx)   /* our commits, newest first, up to 256 */
{
    have_t *h = ctx;
    (void)name;
    buf_t q = { 0 };
    char line[64], hx[41];
    bput(&q, id, 20);
    for (size_t i = 0; i < q.n && h->n < 256; i += 20) {
        commit_t c;
        if (!set_add(h->seen, (uint8_t *)q.p + i) || commit_read((uint8_t *)q.p + i, &c)) continue;
        hex(hx, (uint8_t *)q.p + i);
        pkt_put(h->b, line, (size_t)sfmt(line, sizeof line, "have %s\n", hx));
        h->n++;
        for (int k = 0; k < c.np; k++) bput(&q, c.parent[k], 20);
        free(c.raw);
    }
    free(q.p);
}
int fetch(const char *remote, const char *url, char *head, size_t cap)
{
    conn_t *c = malloc(sizeof *c);
    ents_t adv = { 0 };
    char caps[1000], line[200], hx[41];
    if (!c || open_service(c, url, "git-upload-pack", &adv, caps, sizeof caps)) return 1;
    head[0] = 0;                                     /* the remote's HEAD: its symref, or the branch it equals */
    const char *sr = caps;
    for (; *sr; sr++) if (!memcmp(sr, "symref=HEAD:", 12)) break;
    if (*sr) { size_t k = 0; for (sr += 12; *sr && *sr != ' ' && k < cap - 1; ) head[k++] = *sr++; head[k] = 0; }
    ent_t *hd = ents_find(&adv, "HEAD");
    for (int i = 0; !head[0] && hd && i < adv.n; i++)
        if (!memcmp(adv.e[i].path, "refs/heads/", 11) && !memcmp(adv.e[i].id, hd->id, 20)) strlcpy(head, adv.e[i].path, cap);
    buf_t req = { 0 };
    set_t want = { 0 };
    for (int i = 0; i < adv.n; i++) {
        if (memcmp(adv.e[i].path, "refs/heads/", 11) || obj_has(adv.e[i].id) || !set_add(&want, adv.e[i].id)) continue;
        hex(hx, adv.e[i].id);
        int n = sfmt(line, sizeof line, want.n == 1 ? "want %s ofs-delta thin-pack agent=sieos-git/1\n" : "want %s\n", hx);
        pkt_put(&req, line, (size_t)n);
    }
    int count = 0;
    if (want.n) {
        bput(&req, "0000", 4);
        set_t seen = { 0 };
        have_t h = { &req, &seen, 0 };
        ref_each("refs/", add_have, &h);
        pkt_put(&req, "done\n", 5);
        if (send_req(c, "git-upload-pack", &req)) return 1;
        buf_t pk = { 0 };                            /* ACK / NAK lines, then the pack */
        for (char h[4]; ; ) {
            if (rd_all(c, h, 4)) { close_conn(c); return fail("%s: no pack came", c->url); }
            if (!memcmp(h, "PACK", 4)) { bput(&pk, h, 4); break; }
            long len = 0;
            for (int i = 0; i < 4; i++) len = len * 16 + (h[i] <= '9' ? h[i] - '0' : (h[i] | 32) - 'a' + 10);
            if (len > 4 && (len > (long)sizeof line || rd_all(c, line, (size_t)len - 4))) { close_conn(c); return fail("%s: bad answer", c->url); }
        }
        char *chunk = malloc(65536);
        long r;
        while ((r = rd(c, chunk, 65536)) > 0) bput(&pk, chunk, (size_t)r);
        free(chunk);
        close_conn(c);
        if (r < 0) return fail("%s: the connection broke while receiving", c->url);
        if (pack_store(&pk, &count)) return 1;
        free(pk.p);
    } else {
        if (!c->http) net_send(c->sock, "0000", 4);  /* (nothing wanted: goodbye) */
        close_conn(c);
    }
    outf("From %s\n", c->url);
    if (count) outf("Received %d objects\n", count);
    for (int i = 0; i < adv.n; i++) {
        if (memcmp(adv.e[i].path, "refs/heads/", 11)) continue;
        char ref[400];
        oid_t old;
        sfmt(ref, sizeof ref, "refs/remotes/%s/%s", remote, adv.e[i].path + 11);
        int had = !ref_read(ref, old);
        if (had && !memcmp(old, adv.e[i].id, 20)) continue;
        if (ref_write(ref, adv.e[i].id)) return fail("cannot write %s", ref);
        char o[41], nw[41];
        hex(o, old); hex(nw, adv.e[i].id);
        o[7] = nw[7] = 0;
        if (had) outf("   %s..%s  %s -> %s/%s\n", o, nw, adv.e[i].path + 11, remote, adv.e[i].path + 11);
        else outf(" * [new branch]      %s -> %s/%s\n", adv.e[i].path + 11, remote, adv.e[i].path + 11);
    }
    free(c);
    return 0;
}

/* ---- push */
typedef struct { set_t *has; buf_t *list; } mark_t;   /* list: names of the objects to send, each with its type */
static int mark_tree(const oid_t t, mark_t *m)
{
    if (!set_add(m->has, t)) return 0;
    int type;
    size_t n;
    uint8_t *d = obj_read(t, &type, &n), ty = OBJ_TREE;
    if (!d || type != OBJ_TREE) { free(d); return -1; }
    if (m->list) { bput(m->list, t, 20); bput(m->list, &ty, 1); }
    for (const uint8_t *p = d; p < d + n; ) {
        int dir = *p == '4', link = p[0] == '1' && p[1] == '6';   /* 40000: a tree; 160000: a submodule */
        p += strlen((const char *)p) + 1;
        if (p + 20 > d + n) break;
        if (dir) { if (mark_tree(p, m)) { free(d); return -1; } }
        else if (!link && set_add(m->has, p) && m->list) { ty = OBJ_BLOB; bput(m->list, p, 20); bput(m->list, &ty, 1); }
        p += 20;
    }
    free(d);
    return 0;
}
int push(const char *url, const char *ref)
{
    conn_t *c = malloc(sizeof *c);
    ents_t adv = { 0 };
    char caps[1000], line[300], ho[41], hn[41];
    oid_t new, old, tree;
    if (ref_read(ref, new)) return fail("no branch %s here", ref);
    if (!c || open_service(c, url, "git-receive-pack", &adv, caps, sizeof caps)) return 1;
    ent_t *r = ents_find(&adv, ref);
    memcpy(old, r ? r->id : zero, 20);
    hex(ho, old); hex(hn, new);
    if (!memcmp(old, new, 20)) { close_conn(c); outf("Everything up-to-date\n"); return 0; }
    if (r && (!obj_has(old) || !is_ancestor(old, new))) {
        close_conn(c);
        return fail("rejected: %s is not ahead of the remote's (not a fast-forward): pull first", ref);
    }
    /* what the server has: its tips' history; their trees' objects aren't sent again */
    set_t hist = { 0 }, has = { 0 };
    oid_t dummy;
    mark_t m = { &has, 0 };
    for (int i = 0; i < adv.n; i++)
        if (obj_has(adv.e[i].id) && !commit_tree(adv.e[i].id, tree)) { walk(adv.e[i].id, 0, &hist, dummy); mark_tree(tree, &m); }
    buf_t list = { 0 }, q = { 0 };                   /* the commits to send, then their new trees and files */
    m.list = &list;
    if (set_add(&hist, new)) bput(&q, new, 20);     /* (the server may have it already) */
    for (size_t i = 0; i < q.n; i += 20) {
        commit_t cm;
        uint8_t ty = OBJ_COMMIT;
        if (commit_read((uint8_t *)q.p + i, &cm)) { close_conn(c); return fail("cannot read a commit to push"); }
        bput(&list, q.p + i, 20); bput(&list, &ty, 1);
        if (mark_tree(cm.tree, &m)) { close_conn(c); return fail("cannot read a tree to push"); }
        for (int k = 0; k < cm.np; k++) if (set_add(&hist, cm.parent[k])) bput(&q, cm.parent[k], 20);
        free(cm.raw);
    }
    int cnt = (int)(list.n / 21);
    buf_t req = { 0 };
    int n = sfmt(line, sizeof line, "%s %s %s", ho, hn, ref);
    n += 1 + sfmt(line + n + 1, sizeof line - (size_t)n - 1, "report-status agent=sieos-git/1\n");
    pkt_put(&req, line, (size_t)n);
    bput(&req, "0000", 4);
    size_t start = req.n;                            /* the pack: whole objects, no deltas */
    uint8_t h[12] = { 'P', 'A', 'C', 'K', 0, 0, 0, 2, (uint8_t)(cnt >> 24), (uint8_t)(cnt >> 16), (uint8_t)(cnt >> 8), (uint8_t)cnt };
    bput(&req, h, 12);
    for (int i = 0; i < cnt; i++) {
        int t;
        size_t l, hl = 0;
        uint8_t *d = obj_read((uint8_t *)list.p + 21 * i, &t, &l), *z;
        if (!d) { close_conn(c); return fail("an object to push is missing"); }
        h[hl++] = (uint8_t)(t << 4 | (l & 15) | (l > 15 ? 0x80 : 0));
        for (size_t v = l >> 4; v; v >>= 7) h[hl++] = (uint8_t)((v & 127) | (v > 127 ? 0x80 : 0));
        bput(&req, h, hl);
        z = malloc(ZBOUND(l));
        bput(&req, z, zdeflate(d, l, z));
        free(z); free(d);
    }
    oid_t sum;
    sha1(sum, req.p + start, req.n - start);
    bput(&req, sum, 20);
    if (send_req(c, "git-receive-pack", &req)) return 1;
    int ok = 0, unpacked = 0;
    for (long k; (k = pkt(c, line, sizeof line)) >= 0; ) {
        if (k && line[k - 1] == '\n') line[k - 1] = 0;
        if (!strcmp(line, "unpack ok")) unpacked = 1;
        else if (!memcmp(line, "ok ", 3)) ok = 1;
        else outf("remote: %s\n", line);
    }
    close_conn(c);
    outf("To %s\n", c->url);
    ho[7] = hn[7] = 0;
    if (!unpacked || !ok) return fail("the push was refused (%s)", unpacked ? "the ref was not updated" : "the pack was not accepted");
    if (r) outf("   %s..%s  %s -> %s\n", ho, hn, ref + 11, ref + 11);
    else outf(" * [new branch]      %s -> %s\n", ref + 11, ref + 11);
    free(c);
    return 0;
}
