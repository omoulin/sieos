/*
 * memory.c - sia's memory on the disk.
 *
 *   /var/sia/UID/            one folder per user (root's, 0700: siad alone)
 *       facts                what sia remembers about the user, one per line
 *       next, last           the next conversation number; the most recent one
 *       N.log                conversation N: a list of records
 *
 * A record is "K LEN\n" + LEN bytes + "\n", K being:
 *   U  the user's message        A  sia's answer
 *   C  extra instructions (SIA_CONTEXT; the last one counts)
 *   S  a summary of everything before it (compaction: older turns dropped)
 *   T  the title (the first question, shortened)
 * Conversations only grow by records added at the end, so a crash loses at
 * most the record being written; a record cut short is ignored when read.
 * Compaction writes the log anew (N.new, then renamed over N.log).
 *
 * Why not ~/.sia: siad runs as root; in a folder the user controls, a
 * symbolic link could make it read or write anywhere. Here no path comes
 * from a user, and the uid is the one the kernel stamped on the request.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "memory.h"

static int max_convs = 32, max_bytes = 256 << 10;

void mem_setup(int convs, int kb)
{
    if (convs > 0) max_convs = convs;
    if (kb > 0) max_bytes = kb << 10;
}

static char *dec(char *p, long v)                /* v in decimal at p; returns the end */
{
    char t[24];
    int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    do t[n++] = '0' + v % 10; while (v /= 10);
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}

/* "/var/sia/UID" (made if missing), and "/var/sia/UID/NAME" */
static void path(char *out, int uid, const char *name, int id)
{
    char *p = out + strlcpy(out, "/var/sia/", FS_PATH);
    p = dec(p, uid);
    if (!name && id <= 0) return;
    *p++ = '/';
    if (id > 0) { p = dec(p, id); name = name ? name : ".log"; }
    strlcpy(p, name, FS_PATH - (p - out));
}

static int folder(int uid)
{
    char d[FS_PATH];
    fs_mkdir("/var", 0755);
    fs_mkdir("/var/sia", 0700);
    path(d, uid, 0, 0);
    long e = fs_mkdir(d, 0700);
    return e == -EEXIST ? 0 : (int)e;
}

/* A small file (a number, the facts) written whole: NAME.new, then renamed. */
static int put(const char *p, const char *text, size_t n)
{
    char t[FS_PATH + 8];
    strlcpy(t, p, FS_PATH);
    strlcpy(t + strlen(t), ".new", 8);
    long h = fs_open(t, FS_WRONLY | FS_CREAT | FS_TRUNC, 0600);
    if (h < 0) return (int)h;
    long r = fs_write(h, 0, text, n);
    fs_close(h);
    if (r < 0) { fs_unlink(t); return (int)r; }
    return (int)fs_rename(t, p);
}

static long get_num(int uid, const char *name)
{
    char p[FS_PATH];
    size_t n = 0;
    path(p, uid, name, 0);
    char *f = file_get(p, &n);
    if (!f) return 0;
    while (n && (f[n - 1] == '\n' || f[n - 1] == ' ')) f[--n] = 0;
    long v = strnum(f);
    free(f);
    return v > 0 ? v : 0;
}

static void put_num(int uid, const char *name, long v)
{
    char p[FS_PATH], t[24];
    path(p, uid, name, 0);
    char *e = dec(t, v);
    *e++ = '\n';
    put(p, t, e - t);
}

int mem_exists(int uid, int id)
{
    char p[FS_PATH];
    siefs_stat_t st;
    if (id <= 0) return 0;
    path(p, uid, 0, id);
    return fs_stat(p, &st, 1) == 0;
}

int mem_last(int uid)
{
    long id = get_num(uid, "last");
    return id > 0 && mem_exists(uid, id) ? (int)id : 0;
}

/* A new, empty conversation; the oldest ones beyond the limit are deleted. */
int mem_new(int uid)
{
    if (folder(uid) < 0) return -EIO;
    long id = get_num(uid, "next");
    if (id < 1) id = 1;
    char p[FS_PATH];
    path(p, uid, 0, id);
    long h = fs_open(p, FS_WRONLY | FS_CREAT | FS_TRUNC, 0600);
    if (h < 0) return (int)h;
    fs_close(h);
    put_num(uid, "next", id + 1);
    put_num(uid, "last", id);
    for (long old = id - max_convs; old > 0 && mem_exists(uid, old); old--) {
        path(p, uid, 0, old);
        fs_unlink(p);
    }
    return (int)id;
}

/* One record at the end of the log at path p; returns the log's new size. */
static long append(const char *p, char kind, const char *text, size_t n)
{
    char head[32];
    siefs_stat_t st;
    long h = fs_open(p, FS_WRONLY, 0);
    if (h < 0) return h;
    if (fs_fstat(h, &st) < 0) { fs_close(h); return -EIO; }
    head[0] = kind; head[1] = ' ';
    char *e = dec(head + 2, (long)n);
    *e++ = '\n';
    uint64_t off = st.size;
    long r = fs_write(h, off, head, e - head);
    for (size_t done = 0; r >= 0 && done < n; ) {           /* (FS_MAX per request) */
        size_t k = n - done < FS_MAX ? n - done : FS_MAX;
        r = fs_write(h, off + (e - head) + done, text + done, k);
        done += k;
    }
    if (r >= 0) r = fs_write(h, off + (e - head) + n, "\n", 1);
    fs_close(h);
    return r < 0 ? r : (long)(off + (e - head) + n + 1);
}

/* Add one record to conversation id; returns the log's new size, or -error. */
long mem_add(int uid, int id, char kind, const char *text, size_t n)
{
    char p[FS_PATH];
    path(p, uid, 0, id);
    long r = append(p, kind, text, n);
    if (r >= 0 && get_num(uid, "last") != id) put_num(uid, "last", id);
    return r;
}

static char *dup(const char *s, size_t n)
{
    char *t = malloc(n + 1);
    if (t) { memcpy(t, s, n); t[n] = 0; }
    return t;
}

void mem_free(conv_t *c)
{
    free(c->ctx); free(c->digest); free(c->title);
    for (int i = 0; i < c->nturn; i++) free((void *)c->turn[i].text);
    memset(c, 0, sizeof *c);
}

static void add_turn(conv_t *c, int role, char *text)
{
    if (c->nturn == MEM_MAXTURN) {                         /* (compaction should prevent it) */
        free((void *)c->turn[0].text); free((void *)c->turn[1].text);
        memmove(&c->turn[0], &c->turn[2], (c->nturn - 2) * sizeof c->turn[0]);
        c->nturn -= 2;
    }
    c->turn[c->nturn++] = (sia_turn_t){ role, text };
}

/* Read conversation id back. A damaged or cut-short record ends the reading
 * there: what came before it is kept (and the log said so). */
int mem_load(int uid, int id, conv_t *c)
{
    char p[FS_PATH];
    size_t n = 0;
    memset(c, 0, sizeof *c);
    path(p, uid, 0, id);
    char *f = file_get(p, &n);
    if (!f) return -ENOENT;
    size_t i = 0;
    while (i < n) {
        char k = f[i];
        size_t j = i + 2, len = 0;
        if (!strchr("UACST", k) || i + 2 > n || f[i + 1] != ' ') break;
        while (j < n && f[j] >= '0' && f[j] <= '9' && len < (1UL << 30)) len = len * 10 + (f[j++] - '0');
        if (j >= n || f[j] != '\n' || j + 1 + len + 1 > n || f[j + 1 + len] != '\n') break;
        char *t = dup(f + j + 1, len);
        if (!t) break;
        switch (k) {
        case 'U': add_turn(c, SIA_USER, t); break;
        case 'A': add_turn(c, SIA_ASSISTANT, t); break;
        case 'C': free(c->ctx); c->ctx = t; break;
        case 'T': free(c->title); c->title = t; break;
        case 'S':                                           /* everything before it is replaced */
            free(c->digest); c->digest = t;
            for (int q = 0; q < c->nturn; q++) free((void *)c->turn[q].text);
            c->nturn = 0;
            break;
        }
        i = j + 1 + len + 1;
    }
    if (i < n) sia_log("siad: conversation %d of uid %d: damaged after byte %lu, the rest is ignored\n", id, uid, i);
    free(f);
    return 0;
}

/* Conversation id written anew from c (after compaction): N.new, renamed
 * over N.log, so a crash leaves the old log or the new one, never a mix. */
int mem_rewrite(int uid, int id, const conv_t *c)
{
    char p[FS_PATH], t[FS_PATH + 8];
    path(p, uid, 0, id);
    strlcpy(t, p, FS_PATH);
    strlcpy(t + strlen(t), ".new", 8);
    long h = fs_open(t, FS_WRONLY | FS_CREAT | FS_TRUNC, 0600);
    if (h < 0) return (int)h;
    fs_close(h);
    long r = 0;
    struct { char k; const char *s; } rec[3] = { { 'T', c->title }, { 'C', c->ctx }, { 'S', c->digest } };
    for (int i = 0; i < 3 && r >= 0; i++)
        if (rec[i].s) r = append(t, rec[i].k, rec[i].s, strlen(rec[i].s));
    for (int i = 0; i < c->nturn && r >= 0; i++)
        r = append(t, c->turn[i].role == SIA_ASSISTANT ? 'A' : 'U', c->turn[i].text, strlen(c->turn[i].text));
    if (r < 0) { fs_unlink(t); return (int)r; }
    return (int)fs_rename(t, p);
}

/* The user's conversations, newest first: "id<TAB>turns<TAB>tokens<TAB>title". */
long mem_list(int uid, char *out, size_t cap, int (*tokens)(const conv_t *))
{
    size_t n = 0;
    long next = get_num(uid, "next");
    out[0] = 0;
    for (long id = next - 1; id > 0 && id >= next - max_convs; id--) {
        conv_t c;
        if (!mem_exists(uid, id) || mem_load(uid, id, &c) < 0) continue;
        char line[200], *p = dec(line, id);
        *p++ = '\t'; p = dec(p, c.nturn + (c.digest ? 1 : 0));
        *p++ = '\t'; p = dec(p, tokens ? tokens(&c) : 0);
        *p++ = '\t';
        const char *t = c.title ? c.title : c.nturn ? c.turn[0].text : "(empty)";
        size_t k = 0;
        while (t[k] && t[k] != '\n' && k < 60) k++;
        memcpy(p, t, k); p += k;
        *p++ = '\n'; *p = 0;
        if (n + (p - line) < cap) { memcpy(out + n, line, p - line + 1); n += p - line; }
        mem_free(&c);
    }
    return (long)n;
}

int mem_forget(int uid, int id)
{
    char p[FS_PATH];
    if (id > 0) {
        if (!mem_exists(uid, id)) return -ENOENT;
        path(p, uid, 0, id);
        return (int)fs_unlink(p);
    }
    for (long i = get_num(uid, "next") - 1; i > 0; i--)
        if (mem_exists(uid, i)) { path(p, uid, 0, i); fs_unlink(p); }
    path(p, uid, "last", 0);
    fs_unlink(p);
    return 0;
}

char *mem_facts(int uid)
{
    char p[FS_PATH];
    size_t n = 0;
    path(p, uid, "facts", 0);
    return file_get(p, &n);
}

int mem_facts_put(int uid, const char *text)
{
    char p[FS_PATH];
    if (folder(uid) < 0) return -EIO;
    path(p, uid, "facts", 0);
    return put(p, text, strlen(text));
}
