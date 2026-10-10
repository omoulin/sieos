/*
 * test.c - SieFS tests and measurements (make siefs-test).
 *
 *   1. unit tests of every operation and its errors;
 *   2. a model test: thousands of random operations, done both on SieFS and
 *      on a simple in-memory model, compared after every commit and remount,
 *      with a full check each time;
 *   3. crash tests: the disk "loses power" at every write of a commit, with
 *      unflushed writes randomly kept, dropped or torn; the remounted state
 *      must be exactly the old or the new one, and check clean;
 *   4. damage tests: broken superblocks, bit flips in data, nodes, bitmaps;
 *   5. measurements: speed, memory, blocks read per lookup, space overhead.
 *
 * The disk is in memory and remembers what was flushed, so a power cut
 * can be simulated precisely. ISO C only.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "siefs_int.h"

static long fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- Random numbers (deterministic). */
static uint64_t rs = 1;
static uint32_t rnd(void) { rs = rs * 6364136223846793005ULL + 1442695040888963407ULL; return (uint32_t)(rs >> 33); }
static uint32_t rn(uint32_t n) { return n ? rnd() % n : 0; }

/* ---- Memory accounting: how much the library holds. */
static long mem_now, mem_peak;
static void *xalloc(size_t n)
{
    size_t *p = malloc(n + 16);
    if (!p) return 0;
    *p = n; mem_now += (long)n;
    if (mem_now > mem_peak) mem_peak = mem_now;
    return p + 2;
}
static void xfree(void *q) { if (q) { size_t *p = (size_t *)q - 2; mem_now -= (long)*p; free(p); } }

/* ---- The memory disk. cur: what reads see. dur: what survived the last
 * flush. A power cut after write number `cut` loses everything after it;
 * the writes since the last flush are each kept, dropped or torn. */
typedef struct { uint64_t b; } pw_t;
typedef struct {
    uint8_t *cur, *dur;
    uint64_t nb;
    long writes, reads, flushes, cut;
    int dead;                   /* a write past the cut was attempted: the power is off */
    uint64_t *pend; size_t npend, cap;
} mem_t;
static int64_t clk;
static int64_t now(void) { return clk += 1000000; }

static int mrd(void *c, uint64_t b, uint32_t n, void *buf)
{
    mem_t *m = c;
    m->reads += n;
    memcpy(buf, m->cur + b * BS, (size_t)n * BS);
    return 0;
}
static int mwr(void *c, uint64_t b, uint32_t n, const void *buf)
{
    mem_t *m = c;
    if (m->dead || (m->cut >= 0 && m->writes >= m->cut)) { m->dead = 1; m->writes++; return 0; }   /* the power is off */
    m->writes++;
    memcpy(m->cur + b * BS, buf, (size_t)n * BS);
    for (uint32_t i = 0; i < n; i++) {
        if (m->npend == m->cap) { m->cap = m->cap ? m->cap * 2 : 256; m->pend = realloc(m->pend, m->cap * 8); }
        m->pend[m->npend++] = b + i;
    }
    return 0;
}
static int mfl(void *c)
{
    mem_t *m = c;
    if (m->dead) return 0;
    for (size_t i = 0; i < m->npend; i++) memcpy(m->dur + m->pend[i] * BS, m->cur + m->pend[i] * BS, BS);
    m->npend = 0;
    m->flushes++;
    return 0;
}
static void mem_init(mem_t *m, uint64_t nb)
{
    memset(m, 0, sizeof *m);
    m->nb = nb; m->cut = -1;
    m->cur = calloc(nb, BS); m->dur = calloc(nb, BS);
}
static void mem_free(mem_t *m) { free(m->cur); free(m->dur); free(m->pend); }
/* The power fails: what is on the disk now? */
static void mem_crash(mem_t *m)
{
    for (size_t i = 0; i < m->npend; i++) {
        uint64_t b = m->pend[i];
        uint32_t r = rn(10);
        if (r < 5) memcpy(m->dur + b * BS, m->cur + b * BS, BS);            /* reached the disk */
        else if (r < 6) memcpy(m->dur + b * BS, m->cur + b * BS, BS / 2);   /* torn in the middle */
    }
    memcpy(m->cur, m->dur, m->nb * BS);
    m->npend = 0; m->cut = -1; m->dead = 0;
}
static siefs_env_t env_of(mem_t *m)
{
    return (siefs_env_t){ .ctx = m, .nblocks = m->nb, .read = mrd, .write = mwr, .flush = mfl,
                          .alloc = xalloc, .free = xfree, .now = now, .commit_ns = (int64_t)1 << 62 };
}

static void quiet(void *ctx, const char *msg) { (void)ctx; printf("    check: %s\n", msg); }

/* A signature of the whole visible state: every path, type, mode, size,
 * contents, attribute. Two equal states give the same signature. */
static uint32_t sig_dir(siefs_t *fs, uint64_t dir, uint32_t h, int depth)
{
    siefs_dirent_t de;
    uint64_t cur = 2;
    static uint8_t buf[1 << 16];
    while (siefs_readdir(fs, dir, &cur, &de) == 1) {
        siefs_stat_t st;
        h = siefs_crc32c(h, de.name, strlen(de.name));
        if (siefs_stat(fs, de.ino, &st)) return h ^ 0xBAD;
        h = siefs_crc32c(h, &st.mode, 4);
        h = siefs_crc32c(h, &st.size, 8);
        h = siefs_crc32c(h, &st.nlink, 4);
        if (de.type == 4 && depth < 16) h = sig_dir(fs, de.ino, h, depth + 1);
        else if (de.type == 10) { long n = siefs_readlink(fs, de.ino, (char *)buf, sizeof buf); h = siefs_crc32c(h, buf, n > 0 ? (size_t)n : 0); }
        else for (uint64_t off = 0; ; ) {
            long n = siefs_read(fs, de.ino, off, buf, sizeof buf);
            if (n <= 0) { if (n < 0) h ^= 0xBAD; break; }
            h = siefs_crc32c(h, buf, (size_t)n); off += (uint64_t)n;
        }
        long n = siefs_getxattr(fs, de.ino, "tag", buf, 255);
        if (n > 0) h = siefs_crc32c(h, buf, (size_t)n);
    }
    return h;
}
static uint32_t sig(siefs_t *fs) { return sig_dir(fs, SIEFS_ROOT, 0, 0); }

/* ================================================================ 1. units */
static void unit(void)
{
    mem_t m;
    siefs_env_t env;
    siefs_t *fs;
    uint64_t a, b, d, x;
    char buf[8192];
    static uint8_t big[3 << 20], back[3 << 20];
    printf("unit tests\n");
    CHECK(siefs_crc32c(0, "123456789", 9) == 0xE3069283);   /* the standard check value */
    mem_init(&m, 4096);
    env = env_of(&m);
    CHECK(siefs_mount(&env, &fs) == -SIEFS_EINVAL);           /* not formatted */
    CHECK(siefs_format(&env, "unit") == 0);
    CHECK(siefs_mount(&env, &fs) == 0);

    CHECK(siefs_create(fs, SIEFS_ROOT, "a", SIEFS_IFREG | 0644, 1000, 100, &a) == 0);
    CHECK(siefs_create(fs, SIEFS_ROOT, "a", SIEFS_IFREG | 0644, 0, 0, &x) == -SIEFS_EEXIST);
    CHECK(siefs_create(fs, SIEFS_ROOT, "..", SIEFS_IFREG, 0, 0, &x) == -SIEFS_EINVAL);
    CHECK(siefs_create(fs, SIEFS_ROOT, "x/y", SIEFS_IFREG, 0, 0, &x) == -SIEFS_EINVAL);
    CHECK(siefs_create(fs, a, "z", SIEFS_IFREG, 0, 0, &x) == -SIEFS_ENOTDIR);
    CHECK(siefs_create(fs, SIEFS_ROOT, "d", SIEFS_IFDIR | 0755, 0, 0, &d) == 0);
    CHECK(siefs_walk(fs, "/d/../a", &x) == 0 && x == a);
    CHECK(siefs_walk(fs, "/nope", &x) == -SIEFS_ENOENT);

    CHECK(siefs_write(fs, a, 0, "hello", 5) == 5);            /* inline */
    CHECK(siefs_read(fs, a, 0, buf, sizeof buf) == 5 && !memcmp(buf, "hello", 5));
    CHECK(siefs_write(fs, a, 10, "!", 1) == 1);               /* a gap of zeros */
    CHECK(siefs_read(fs, a, 0, buf, sizeof buf) == 11 && buf[7] == 0 && buf[10] == '!');
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 7 + i / 4096);
    CHECK(siefs_write(fs, a, 0, big, sizeof big) == (long)sizeof big);   /* leaves the tree */
    CHECK(siefs_read(fs, a, 0, back, sizeof back) == (long)sizeof back && !memcmp(big, back, sizeof big));
    CHECK(siefs_write(fs, a, 5000, "XYZ", 3) == 3);           /* overwrite in the middle */
    big[5000] = 'X'; big[5001] = 'Y'; big[5002] = 'Z';
    CHECK(siefs_read(fs, a, 4090, buf, 20) == 20 && !memcmp(buf, big + 4090, 20));
    CHECK(siefs_truncate(fs, a, 10000) == 0);
    CHECK(siefs_read(fs, a, 0, back, sizeof back) == 10000 && !memcmp(big, back, 10000));
    CHECK(siefs_truncate(fs, a, 20000) == 0);                 /* grows with zeros */
    CHECK(siefs_read(fs, a, 9990, buf, 100) == 100 && !memcmp(buf, big + 9990, 10) && buf[10] == 0 && buf[99] == 0);
    CHECK(siefs_write(fs, a, (uint64_t)1 << 40, "far", 3) == 3);   /* sparse: 1 TiB */
    siefs_stat_t st;
    CHECK(siefs_stat(fs, a, &st) == 0 && st.size == ((uint64_t)1 << 40) + 3 && st.uid == 1000 && st.gid == 100);
    CHECK(siefs_read(fs, a, ((uint64_t)1 << 40) - 2, buf, 10) == 5 && buf[0] == 0 && !memcmp(buf + 2, "far", 3));
    CHECK(siefs_truncate(fs, a, 0) == 0);

    CHECK(siefs_link(fs, a, d, "a2") == 0);
    CHECK(siefs_stat(fs, a, &st) == 0 && st.nlink == 2);
    CHECK(siefs_link(fs, d, SIEFS_ROOT, "dd") == -SIEFS_EPERM);
    CHECK(siefs_unlink(fs, SIEFS_ROOT, "a") == 0);
    CHECK(siefs_stat(fs, a, &st) == 0 && st.nlink == 1);
    CHECK(siefs_rmdir(fs, SIEFS_ROOT, "d") == -SIEFS_ENOTEMPTY);
    CHECK(siefs_unlink(fs, SIEFS_ROOT, "d") == -SIEFS_EISDIR);
    CHECK(siefs_symlink(fs, d, "l", "../target", 0, 0, &b) == 0);
    CHECK(siefs_readlink(fs, b, buf, sizeof buf) == 9 && !memcmp(buf, "../target", 9));

    CHECK(siefs_create(fs, d, "sub", SIEFS_IFDIR | 0700, 0, 0, &x) == 0);
    CHECK(siefs_rename(fs, SIEFS_ROOT, "d", x, "inside") == -SIEFS_EINVAL);   /* into itself */
    CHECK(siefs_rename(fs, d, "sub", SIEFS_ROOT, "top") == 0);
    CHECK(siefs_walk(fs, "/top/..", &b) == 0 && b == SIEFS_ROOT);
    CHECK(siefs_stat(fs, d, &st) == 0 && st.nlink == 2);
    CHECK(siefs_stat(fs, SIEFS_ROOT, &st) == 0 && st.nlink == 4);
    CHECK(siefs_rename(fs, d, "a2", SIEFS_ROOT, "top") == -SIEFS_EISDIR);
    CHECK(siefs_rename(fs, d, "a2", d, "a3") == 0 && siefs_walk(fs, "/d/a3", &b) == 0 && b == a);

    CHECK(siefs_setxattr(fs, a, "project", "SIEOS", 5) == 0);
    CHECK(siefs_setxattr(fs, x, "project", "SIEOS", 5) == 0);
    CHECK(siefs_setxattr(fs, d, "project", "other", 5) == 0);
    CHECK(siefs_getxattr(fs, a, "project", buf, sizeof buf) == 5 && !memcmp(buf, "SIEOS", 5));
    CHECK(siefs_getxattr(fs, a, "nope", buf, sizeof buf) == -SIEFS_ENODATA);
    CHECK(siefs_listxattr(fs, a, buf, sizeof buf) == 8 && !strcmp(buf, "project"));
    uint64_t cur = 0, found = 0;
    while (siefs_find(fs, "project", "SIEOS", 5, &cur, &b) == 1) found++;
    CHECK(found == 2);
    CHECK(siefs_setxattr(fs, a, "project", "moved", 5) == 0);
    cur = found = 0;
    while (siefs_find(fs, "project", "SIEOS", 5, &cur, &b) == 1) found++;
    CHECK(found == 1);
    CHECK(siefs_removexattr(fs, d, "project") == 0 && siefs_removexattr(fs, d, "project") == -SIEFS_ENODATA);

    siefs_stat_t ch = { .mode = 0600, .uid = 7, .gid = 8 };
    CHECK(siefs_setattr(fs, a, &ch, SIEFS_SET_MODE | SIEFS_SET_UID | SIEFS_SET_GID) == 0);
    CHECK(siefs_stat(fs, a, &st) == 0 && st.mode == (SIEFS_IFREG | 0600) && st.uid == 7);
    CHECK(siefs_access(&st, 7, 0, 6) == 0 && siefs_access(&st, 9, 8, 4) == -SIEFS_EACCES && siefs_access(&st, 0, 0, 6) == 0);

    char name[32];                                       /* a big directory: many leaves */
    for (int i = 0; i < 3000; i++) { sprintf(name, "file-%04d", i); CHECK(siefs_create(fs, d, name, SIEFS_IFREG | 0644, 0, 0, &b) == 0); }
    siefs_dirent_t de;
    cur = 0; found = 0;
    while (siefs_readdir(fs, d, &cur, &de) == 1) found++;
    CHECK(found == 3000 + 2 + 2);                        /* + . .. l a3 */
    for (int i = 0; i < 3000; i += 2) { sprintf(name, "file-%04d", i); CHECK(siefs_unlink(fs, d, name) == 0); }
    CHECK(siefs_check(fs, 1, quiet, 0) == 0);
    uint32_t s1 = sig(fs);
    CHECK(siefs_unmount(fs) == 0);
    CHECK(siefs_mount(&env, &fs) == 0);
    CHECK(sig(fs) == s1);
    CHECK(siefs_check(fs, 1, quiet, 0) == 0);
    for (int i = 1; i < 3000; i += 2) { sprintf(name, "file-%04d", i); CHECK(siefs_unlink(fs, d, name) == 0); }
    CHECK(siefs_sync(fs) == 0 && siefs_check(fs, 1, quiet, 0) == 0);

    /* filling the disk: ENOSPC, then everything still works and checks */
    CHECK(siefs_create(fs, SIEFS_ROOT, "fill", SIEFS_IFREG | 0644, 0, 0, &b) == 0);
    long w = 0;
    for (uint64_t off = 0; (w = siefs_write(fs, b, off, big, sizeof big)) > 0; off += (uint64_t)w) ;
    CHECK(w == -SIEFS_ENOSPC);
    long u = siefs_unlink(fs, SIEFS_ROOT, "fill"), sy = siefs_sync(fs);
    if (u || sy) printf("  fill: write %ld, unlink %ld, sync %ld, broken %d\n", w, u, sy, fs->broken);
    CHECK(!u && !sy);
    CHECK(siefs_check(fs, 1, quiet, 0) == 0);
    CHECK(siefs_unmount(fs) == 0);
    mem_free(&m);
}

/* ================================================================ 2. model */
enum { MREG = 1, MDIR, MLNK };
typedef struct { int type, nlink; uint8_t *data; size_t len; char tag[8]; } mobj_t;
typedef struct { char path[64]; int obj; } ment_t;
typedef struct { mobj_t o[256]; ment_t e[256]; int no, ne; } model_t;

static const char *mdirs[] = { "", "/a", "/b", "/a/c" };     /* places where names may go */
static int mfind(model_t *m, const char *p) { for (int i = 0; i < m->ne; i++) if (!strcmp(m->e[i].path, p)) return i; return -1; }
static int mchildren(model_t *m, const char *p)     /* direct children */
{
    int n = 0;
    size_t l = strlen(p);
    for (int i = 0; i < m->ne; i++)
        if (!strncmp(m->e[i].path, p, l) && m->e[i].path[l] == '/' && !strchr(m->e[i].path + l + 1, '/')) n++;
    return n;
}
static const char *mnames[] = { "a", "b", "c", "f0", "f1", "f2", "x0", "x1" };
static void mpath(char *p, size_t n) { snprintf(p, n, "%s/%s", mdirs[rn(4)], mnames[rn(8)]); }
static void mdel(model_t *m, int i)
{
    mobj_t *o = &m->o[m->e[i].obj];
    if (!--o->nlink) { free(o->data); o->data = 0; o->type = 0; }
    m->e[i] = m->e[--m->ne];
}
static int mnew(model_t *m, int type)
{
    for (int i = 0; i < 256; i++) if (!m->o[i].type) { m->o[i] = (mobj_t){ type, 0, 0, 0, "" }; if (i >= m->no) m->no = i + 1; return i; }
    return -1;
}
static int split(siefs_t *fs, const char *path, uint64_t *dir, const char **name)
{
    const char *s = strrchr(path, '/');
    char d[64];
    memcpy(d, path, (size_t)(s - path)); d[s - path] = 0;
    *name = s + 1;
    return siefs_walk(fs, d[0] ? d : "/", dir);
}

/* Compare SieFS with the model, entry by entry. */
static void mcompare(siefs_t *fs, model_t *m)
{
    static uint8_t buf[1 << 18];
    uint64_t ino;
    for (int i = 0; i < m->ne; i++) {
        mobj_t *o = &m->o[m->e[i].obj];
        siefs_stat_t st;
        if (siefs_walk(fs, m->e[i].path, &ino) || siefs_stat(fs, ino, &st)) { CHECK(!"missing"); printf("  %s\n", m->e[i].path); continue; }
        CHECK((int)(st.mode >> 12) == (o->type == MDIR ? 4 : o->type == MREG ? 8 : 10));
        if (o->type != MDIR) CHECK(st.nlink == (uint32_t)o->nlink);
        if (o->type == MREG) {
            CHECK(st.size == o->len);
            long n = siefs_read(fs, ino, 0, buf, sizeof buf);
            CHECK(n == (long)o->len && !memcmp(buf, o->data, o->len));
        }
        if (o->type == MLNK) { long n = siefs_readlink(fs, ino, (char *)buf, sizeof buf); CHECK(n == (long)o->len && !memcmp(buf, o->data, o->len)); }
        long n = siefs_getxattr(fs, ino, "tag", buf, 8);
        CHECK(o->tag[0] ? n == (long)strlen(o->tag) && !memcmp(buf, o->tag, (size_t)n) : n == -SIEFS_ENODATA);
    }
    for (int i = 0; i < 4; i++) {                       /* nothing more than the model has */
        siefs_dirent_t de;
        uint64_t cur = 2;
        int n = 0;
        if (siefs_walk(fs, mdirs[i][0] ? mdirs[i] : "/", &ino)) continue;
        while (siefs_readdir(fs, ino, &cur, &de) == 1) n++;
        CHECK(n == mchildren(m, mdirs[i]));
    }
}

/* One random operation, on both. ok: should it succeed? */
static void mop(siefs_t *fs, model_t *m)
{
    static uint8_t data[200000];
    char path[64];
    const char *name;
    uint64_t dir, ino;
    int r = (int)rn(100), e = 0;
    mpath(path, sizeof path);
    int at = mfind(m, path);
    int pidx = -1;
    { char d[64]; strcpy(d, path); *strrchr(d, '/') = 0; pidx = d[0] ? mfind(m, d) : -2; }
    int parent_ok = pidx == -2 || (pidx >= 0 && m->o[m->e[pidx].obj].type == MDIR);
    if (!parent_ok || split(fs, path, &dir, &name)) return;
    if (r < 25 && at < 0) {                              /* create a file */
        int o = mnew(m, MREG);
        if (o < 0) return;
        CHECK((e = siefs_create(fs, dir, name, SIEFS_IFREG | 0644, 0, 0, &ino)) == 0);
        m->o[o].nlink = 1; strcpy(m->e[m->ne].path, path); m->e[m->ne++].obj = o;
    } else if (r < 32 && at < 0) {                       /* mkdir */
        int o = mnew(m, MDIR);
        if (o < 0) return;
        CHECK((e = siefs_create(fs, dir, name, SIEFS_IFDIR | 0755, 0, 0, &ino)) == 0);
        m->o[o].nlink = 1; strcpy(m->e[m->ne].path, path); m->e[m->ne++].obj = o;
    } else if (r < 36 && at < 0) {                       /* symlink */
        int o = mnew(m, MLNK);
        if (o < 0) return;
        size_t l = 1 + rn(40);
        for (size_t i = 0; i < l; i++) data[i] = (uint8_t)('a' + rn(26));
        data[l] = 0;
        CHECK((e = siefs_symlink(fs, dir, name, (char *)data, 0, 0, &ino)) == 0);
        m->o[o].nlink = 1; m->o[o].data = malloc(l); memcpy(m->o[o].data, data, l); m->o[o].len = l;
        strcpy(m->e[m->ne].path, path); m->e[m->ne++].obj = o;
    } else if (at < 0) return;
    else {
        mobj_t *o = &m->o[m->e[at].obj];
        if (siefs_walk(fs, path, &ino)) { CHECK(!"walk"); return; }
        if (r < 60 && o->type == MREG) {                 /* write */
            static const size_t sizes[] = { 1, 100, 1500, 3000, 4096, 9000, 70000, 190000 };
            size_t n = sizes[rn(8)] + rn(50);
            uint64_t off = rn((uint32_t)o->len + 9000);
            if (off + n > 199999) return;
            for (size_t i = 0; i < n; i++) data[i] = (uint8_t)rnd();
            CHECK(siefs_write(fs, ino, off, data, n) == (long)n);
            if (off + n > o->len) { o->data = realloc(o->data, off + n); memset(o->data + o->len, 0, off + n - o->len); o->len = off + n; }
            memcpy(o->data + off, data, n);
        } else if (r < 66 && o->type == MREG) {          /* truncate */
            size_t n = rn(3) ? rn((uint32_t)o->len + 1) : rn(100000);
            CHECK(siefs_truncate(fs, ino, n) == 0);
            o->data = realloc(o->data, n + 1);
            if (n > o->len) memset(o->data + o->len, 0, n - o->len);
            o->len = n;
        } else if (r < 74) {                             /* remove */
            if (o->type == MDIR) {
                CHECK(siefs_rmdir(fs, dir, name) == (mchildren(m, path) ? -SIEFS_ENOTEMPTY : 0));
                if (!mchildren(m, path)) mdel(m, at);
            } else { CHECK(siefs_unlink(fs, dir, name) == 0); mdel(m, at); }
        } else if (r < 82) {                             /* rename (to a free name, or a file over a file) */
            char to[64];
            uint64_t tdir;
            const char *tname;
            mpath(to, sizeof to);
            size_t pl = strlen(path);
            int t = mfind(m, to);
            char d[64]; strcpy(d, to); *strrchr(d, '/') = 0;
            int tp = d[0] ? mfind(m, d) : -2;
            if (!(tp == -2 || (tp >= 0 && m->o[m->e[tp].obj].type == MDIR)) || split(fs, to, &tdir, &tname)) return;
            if (o->type == MDIR) {                       /* a directory: only to a free name outside itself */
                if (t >= 0 || (!strncmp(to, path, pl) && to[pl] == '/')) return;
                CHECK(siefs_rename(fs, dir, name, tdir, tname) == 0);
                for (int i = 0; i < m->ne; i++)          /* everything inside moves too */
                    if (!strncmp(m->e[i].path, path, pl) && m->e[i].path[pl] == '/') {
                        char tmp[64];
                        snprintf(tmp, sizeof tmp, "%s%s", to, m->e[i].path + pl);
                        strcpy(m->e[i].path, tmp);
                    }
                strcpy(m->e[at].path, to);
                return;
            }
            if (t >= 0 && m->o[m->e[t].obj].type == MDIR) { CHECK(siefs_rename(fs, dir, name, tdir, tname) == -SIEFS_EISDIR); return; }
            CHECK(siefs_rename(fs, dir, name, tdir, tname) == 0);
            if (t == at) return;
            if (t >= 0) {
                if (m->e[t].obj == m->e[at].obj) return; /* same object: nothing happens */
                mdel(m, t);
                at = mfind(m, path);
            }
            strcpy(m->e[at].path, to);
        } else if (r < 88 && o->type == MREG) {          /* hard link */
            char to[64];
            uint64_t tdir;
            const char *tname;
            mpath(to, sizeof to);
            char d[64]; strcpy(d, to); *strrchr(d, '/') = 0;
            int tp = d[0] ? mfind(m, d) : -2;
            if (mfind(m, to) >= 0 || !(tp == -2 || (tp >= 0 && m->o[m->e[tp].obj].type == MDIR)) || split(fs, to, &tdir, &tname)) return;
            CHECK(siefs_link(fs, ino, tdir, tname) == 0);
            o->nlink++;
            strcpy(m->e[m->ne].path, to); m->e[m->ne++].obj = m->e[at].obj;
        } else if (r < 96) {                             /* tag */
            const char *v = (const char *[]){ "x", "yy", "zzz" }[rn(3)];
            CHECK(siefs_setxattr(fs, ino, "tag", v, strlen(v)) == 0);
            strcpy(o->tag, v);
        } else if (o->tag[0]) {
            CHECK(siefs_removexattr(fs, ino, "tag") == 0);
            o->tag[0] = 0;
        }
    }
    (void)e;
}

static void model(int rounds)
{
    mem_t mem;
    siefs_env_t env;
    siefs_t *fs;
    static model_t m;
    long ops = 0;
    printf("model test: %d rounds of 40 random operations\n", rounds);
    mem_init(&mem, 8192);                               /* 32 MiB */
    env = env_of(&mem);
    env.cache_nodes = 16;                               /* a tiny cache: more reading back from disk */
    CHECK(siefs_format(&env, "model") == 0 && siefs_mount(&env, &fs) == 0);
    for (int round = 0; round < rounds && !fails; round++) {
        for (int i = 0; i < 40; i++, ops++) mop(fs, &m);
        mcompare(fs, &m);
        CHECK(siefs_sync(fs) == 0);
        CHECK(siefs_check(fs, 1, quiet, 0) == 0);
        if (round % 10 == 9) {
            CHECK(siefs_unmount(fs) == 0 && siefs_mount(&env, &fs) == 0);
            mcompare(fs, &m);
        }
    }
    printf("  %ld operations, %d names at the end, %ld failures\n", ops, m.ne, fails);
    siefs_unmount(fs);
    for (int i = 0; i < m.no; i++) free(m.o[i].data);
    mem_free(&mem);
}

/* ================================================================ 3. crashes */
static void ops_batch(siefs_t *fs, uint64_t seed, int n)
{
    static uint8_t data[50000];
    char name[16];
    uint64_t ino, dir;
    rs = seed;
    for (int i = 0; i < n; i++) {
        sprintf(name, "n%u", rn(12));
        int r = (int)rn(10);
        dir = rn(3) ? SIEFS_ROOT : 0;
        if (!dir && siefs_walk(fs, "/dir", &dir)) dir = SIEFS_ROOT;
        if (r < 3) siefs_create(fs, dir, name, SIEFS_IFREG | 0644, 0, 0, &ino);
        else if (r < 6 && !siefs_lookup(fs, dir, name, &ino)) {
            size_t len = rn(sizeof data);
            for (size_t j = 0; j < len; j++) data[j] = (uint8_t)rnd();
            siefs_write(fs, ino, rn(20000), data, len);
        } else if (r < 7) siefs_unlink(fs, dir, name);
        else if (r < 8) { char to[16]; sprintf(to, "n%u", rn(12)); siefs_rename(fs, dir, name, SIEFS_ROOT, to); }
        else if (r < 9 && !siefs_lookup(fs, dir, name, &ino)) siefs_setxattr(fs, ino, "tag", name, strlen(name));
        else if (!siefs_lookup(fs, dir, name, &ino)) siefs_truncate(fs, ino, rn(30000));
    }
}

static void crash(int seeds)
{
    long runs = 0, newer = 0;
    printf("crash test: %d scenarios, a power cut at every write of a transaction\n", seeds);
    for (int s = 1; s <= seeds && !fails; s++) {
        mem_t m;
        siefs_env_t env;
        siefs_t *fs;
        uint64_t d;
        mem_init(&m, 2048);
        env = env_of(&m);
        clk = 0;
        CHECK(siefs_format(&env, "crash") == 0 && siefs_mount(&env, &fs) == 0);
        siefs_create(fs, SIEFS_ROOT, "dir", SIEFS_IFDIR | 0755, 0, 0, &d);
        ops_batch(fs, (uint64_t)s * 1000, 40);
        CHECK(siefs_unmount(fs) == 0);                  /* state A, committed */
        uint8_t *a_img = malloc(m.nb * BS);
        memcpy(a_img, m.cur, m.nb * BS);
        memcpy(m.dur, m.cur, m.nb * BS);
        uint32_t sa, sb;
        long w0, total;
        /* a dry run: state B, and how many writes its commit makes */
        CHECK(siefs_mount(&env, &fs) == 0);
        sa = sig(fs);
        int64_t c0 = clk;
        int nops = 10 + 6 * s;                           /* bigger and bigger transactions */
        w0 = m.writes;
        ops_batch(fs, (uint64_t)s * 7777, nops);
        sb = sig(fs);
        CHECK(siefs_sync(fs) == 0);
        total = m.writes - w0;                           /* every write: file data, nodes, bitmap, superblock */
        siefs_unmount(fs);
        for (long cut = 0; cut <= total && !fails; cut++)
            for (int v = 0; v < (cut < total ? 3 : 1); v++) {   /* 3 random fates for unflushed writes */
                memcpy(m.cur, a_img, m.nb * BS);
                memcpy(m.dur, a_img, m.nb * BS);
                m.npend = 0;
                CHECK(siefs_mount(&env, &fs) == 0);
                clk = c0;
                m.cut = m.writes + cut;                  /* the power fails at write number cut */
                ops_batch(fs, (uint64_t)s * 7777, nops);
                siefs_sync(fs);
                fs->broken = 1;                          /* forget the machine that lost power */
                siefs_unmount(fs);
                mem_crash(&m);
                siefs_t *fs2;
                if (siefs_mount(&env, &fs2)) { CHECK(!"mount after crash"); continue; }
                uint32_t sg = sig(fs2);
                CHECK(sg == sa || sg == sb);
                if (sg != sa && sg != sb) printf("  scenario %d, cut at write %ld/%ld: neither old nor new\n", s, cut, total);
                newer += sg == sb;
                if (cut == total) CHECK(sg == sb);
                CHECK(siefs_check(fs2, 1, quiet, 0) == 0);
                ops_batch(fs2, 99, 10);                  /* life goes on after the crash */
                CHECK(siefs_sync(fs2) == 0 && siefs_check(fs2, 1, quiet, 0) == 0);
                siefs_unmount(fs2);
                runs++;
            }
        free(a_img);
        mem_free(&m);
    }
    printf("  %ld crashes simulated: %ld came back with the new state, %ld with the old one, all checked clean\n",
           runs, newer, runs - newer);
}

/* ================================================================ 4. damage */
static void damage(void)
{
    mem_t m;
    siefs_env_t env;
    siefs_t *fs;
    uint64_t a;
    static uint8_t data[100000];
    printf("damage tests\n");
    mem_init(&m, 2048);
    env = env_of(&m);
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)rnd();
    CHECK(siefs_format(&env, "dmg") == 0 && siefs_mount(&env, &fs) == 0);
    CHECK(siefs_create(fs, SIEFS_ROOT, "f", SIEFS_IFREG | 0644, 0, 0, &a) == 0 && siefs_write(fs, a, 0, data, sizeof data) == sizeof data);
    CHECK(siefs_unmount(fs) == 0);
    CHECK(siefs_mount(&env, &fs) == 0);
    uint32_t s1 = sig(fs);
    CHECK(siefs_setxattr(fs, a, "tag", "new", 3) == 0);
    uint32_t s2 = sig(fs);
    int slot = (int)(fs->txn & 1);
    CHECK(siefs_unmount(fs) == 0);

    m.cur[(1 + slot) * BS + 100] ^= 1;                  /* the newest superblock damaged: the previous state */
    CHECK(siefs_mount(&env, &fs) == 0 && sig(fs) == s1);
    siefs_unmount(fs);
    m.cur[(1 + slot) * BS + 100] ^= 1;
    CHECK(siefs_mount(&env, &fs) == 0 && sig(fs) == s2);
    uint64_t root = fs->ft.root.blk, bm = fs->sb.bm[fs->sb.seq & 1];
    extent_t x;
    path_t pa;
    CHECK(t_seek(fs, &fs->ft, K(a, T_EXTENT, 0), &pa) == 1);
    memcpy(&x, pval(&pa), plen(&pa));
    siefs_unmount(fs);

    m.cur[x.blk * BS + 7] ^= 0x10;                      /* a bit flips in file data */
    CHECK(siefs_mount(&env, &fs) == 0);
    CHECK(siefs_read(fs, a, 0, data, 4096) == -SIEFS_EIO);
    CHECK(siefs_check(fs, 1, 0, 0) == 1);
    siefs_unmount(fs);
    m.cur[x.blk * BS + 7] ^= 0x10;

    m.cur[root * BS + 2000] ^= 4;                       /* a bit flips in the newest tree: the previous state */
    CHECK(siefs_mount(&env, &fs) == 0 && sig(fs) == s1);
    siefs_unmount(fs);
    m.cur[root * BS + 2000] ^= 4;

    m.cur[bm * BS + 5] ^= 1;                            /* the newest bitmap: the previous state */
    CHECK(siefs_mount(&env, &fs) == 0 && sig(fs) == s1);
    siefs_unmount(fs);
    m.cur[bm * BS + 5] ^= 1;

    m.cur[1 * BS + 8] ^= 1; m.cur[2 * BS + 8] ^= 1;     /* both superblocks: no file system */
    CHECK(siefs_mount(&env, &fs) == -SIEFS_EINVAL);
    mem_free(&m);
    printf("  damaged superblock, data, node and bitmap: all detected; fallback to the previous commit works\n");
}

/* ================================================================ 5. measurements */
static double secs(clock_t t) { return (double)(clock() - t) / CLOCKS_PER_SEC; }

static void measure(void)
{
    mem_t m;
    siefs_env_t env;
    siefs_t *fs;
    uint64_t d, ino, x;
    char name[32];
    static uint8_t data[128 << 10];
    const int N = 20000;
    printf("measurements (in-memory disk: the cost of SieFS itself, no device time)\n");
    mem_init(&m, 65536);                                /* 256 MiB */
    env = env_of(&m);
    env.commit_ns = 0;                                  /* default: 5 s */
    CHECK(siefs_format(&env, "perf") == 0);
    CHECK(siefs_mount(&env, &fs) == 0);
    siefs_statfs_t st;
    siefs_statfs(fs, &st);
    printf("  empty 256 MiB volume: %lu blocks used (%lu KiB, %.3f%%)\n", (unsigned long)(st.blocks - st.free),
           (unsigned long)(st.blocks - st.free) * 4, 100.0 * (double)(st.blocks - st.free) / (double)st.blocks);
    long base = mem_now;
    printf("  RAM of a mounted volume: %ld KiB (bitmaps %lu KiB, buffers %d KiB)\n", base / 1024,
           (unsigned long)(3 * fs->sb.nbm * BS) / 1024, EXT_MAX * BS / 1024);

    CHECK(siefs_create(fs, SIEFS_ROOT, "d", SIEFS_IFDIR | 0755, 0, 0, &d) == 0);
    clock_t t = clock();
    for (int i = 0; i < N; i++) { sprintf(name, "file-%06d", i); siefs_create(fs, d, name, SIEFS_IFREG | 0644, 0, 0, &ino); }
    siefs_sync(fs);
    printf("  create: %.0f files/s (%d in one directory, committed)\n", N / secs(t), N);
    t = clock();
    for (int i = 0; i < N; i++) { sprintf(name, "file-%06d", (int)rn(N)); siefs_lookup(fs, d, name, &ino); }
    printf("  lookup: %.0f/s (random names)\n", N / secs(t));
    t = clock();
    for (int i = 0; i < N; i++) { sprintf(name, "file-%06d", i); siefs_lookup(fs, d, name, &ino); siefs_write(fs, ino, 0, data, 1000); }
    siefs_sync(fs);
    printf("  small writes: %.0f files/s (1000 bytes each: stored inside the tree)\n", N / secs(t));
    CHECK(siefs_create(fs, SIEFS_ROOT, "big", SIEFS_IFREG | 0644, 0, 0, &x) == 0);
    t = clock();
    for (uint64_t off = 0; off < (64 << 20); off += sizeof data) siefs_write(fs, x, off, data, sizeof data);
    siefs_sync(fs);
    double ws = secs(t);
    t = clock();
    for (uint64_t off = 0; off < (64 << 20); off += sizeof data) siefs_read(fs, x, off, data, sizeof data);
    printf("  64 MiB file: write %.0f MiB/s, read %.0f MiB/s (checksums included)\n", 64 / ws, 64 / secs(t));
    siefs_statfs(fs, &st);
    printf("  RAM after this work: %ld KiB in use (cache: %u nodes), peak %ld KiB\n", mem_now / 1024,
           fs->env.cache_nodes, mem_peak / 1024);
    siefs_unmount(fs);

    long r0 = m.reads;
    t = clock();
    CHECK(siefs_mount(&env, &fs) == 0);
    printf("  mount: %.2f ms, %ld blocks read (superblocks, bitmap, root nodes)\n", secs(t) * 1000, m.reads - r0);
    r0 = m.reads;
    CHECK(siefs_walk(fs, "/d/file-012345", &ino) == 0);
    long cold = m.reads - r0;
    r0 = m.reads;
    CHECK(siefs_walk(fs, "/d/file-012346", &ino) == 0);
    printf("  path lookup /d/file-012345 among %d files: %ld blocks read cold, %ld warm\n", N, cold, m.reads - r0);
    siefs_unmount(fs);
    mem_free(&m);

    /* a small populated tree: overhead */
    mem_init(&m, 16384);
    env = env_of(&m);
    CHECK(siefs_format(&env, "small") == 0 && siefs_mount(&env, &fs) == 0);
    siefs_statfs(fs, &st);
    uint64_t used0 = st.blocks - st.free, bytes = 0;
    for (int i = 0; i < 20; i++) {
        sprintf(name, "dir%d", i);
        siefs_create(fs, SIEFS_ROOT, name, SIEFS_IFDIR | 0755, 0, 0, &d);
        for (int j = 0; j < 10; j++) {
            size_t len = (size_t)(rn(4) ? rn(2000) : rn(20000));
            sprintf(name, "f%d", j);
            siefs_create(fs, d, name, SIEFS_IFREG | 0644, 0, 0, &ino);
            siefs_write(fs, ino, 0, data, len);
            bytes += len;
        }
    }
    siefs_unmount(fs);
    CHECK(siefs_mount(&env, &fs) == 0);
    siefs_statfs(fs, &st);
    uint64_t used = st.blocks - st.free - st.pending - used0;
    printf("  small tree (20 dirs, 200 files, %lu KiB of contents): %lu KiB on disk beyond the empty volume\n",
           (unsigned long)(bytes / 1024), (unsigned long)used * 4);
    siefs_unmount(fs);
    mem_free(&m);
}

int main(int argc, char **argv)
{
    int quick = argc > 1 && !strcmp(argv[1], "quick");
    unit();
    model(quick ? 30 : 300);
    crash(quick ? 3 : 30);
    damage();
    if (!fails) measure();
    printf(fails ? "SieFS tests: %ld FAILURES\n" : "SieFS tests: all passed\n", fails);
    return fails != 0;
}
