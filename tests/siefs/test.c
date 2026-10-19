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
static uint64_t rs2 = 77;                               /* (its own sequence: the model's stays as it was) */
static int mrand(void *b, size_t n)
{
    for (size_t i = 0; i < n; i++) { rs2 = rs2 * 6364136223846793005ULL + 1; ((uint8_t *)b)[i] = (uint8_t)(rs2 >> 56); }
    return 0;
}
static siefs_env_t env_of(mem_t *m)
{
    return (siefs_env_t){ .ctx = m, .nblocks = m->nb, .read = mrd, .write = mwr, .flush = mfl,
                          .alloc = xalloc, .free = xfree, .now = now, .commit_ns = (int64_t)1 << 62, .random = mrand };
}
static const siefs_kdf_t KDF = { "0123456789abcdef", 3, 64 };
static const uint8_t KEK[32] = "the key-encryption key for tests", KEK2[32] = "a new one, after a password chg";

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
static const char *mpre = "";                           /* where the model's "/" is (a volume, a snapshot) */
static int mwalk(siefs_t *fs, const char *p, uint64_t *ino)
{
    char b[160];
    snprintf(b, sizeof b, "%s%s", mpre, p);
    return siefs_walk(fs, b[0] ? b : "/", ino);
}
static int split(siefs_t *fs, const char *path, uint64_t *dir, const char **name)
{
    const char *s = strrchr(path, '/');
    char d[64];
    memcpy(d, path, (size_t)(s - path)); d[s - path] = 0;
    *name = s + 1;
    return mwalk(fs, d, dir);
}

/* Compare SieFS with the model, entry by entry. */
static void mcompare(siefs_t *fs, model_t *m)
{
    static uint8_t buf[1 << 18];
    uint64_t ino;
    for (int i = 0; i < m->ne; i++) {
        mobj_t *o = &m->o[m->e[i].obj];
        siefs_stat_t st;
        if (mwalk(fs, m->e[i].path, &ino) || siefs_stat(fs, ino, &st)) { CHECK(!"missing"); printf("  %s%s\n", mpre, m->e[i].path); continue; }
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
        if (mwalk(fs, mdirs[i], &ino)) continue;
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
        if (mwalk(fs, path, &ino)) { CHECK(!"walk"); return; }
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

/* ================================================================ 2b. snapshots
 * The model test again, inside a mounted volume (/home), while snapshots
 * are taken, deleted and restored at random: each snapshot must keep
 * showing the model as it was when taken, and the whole volume must check
 * clean (blocks, dead lists, no leaks) after every commit. */
static void mcopy(model_t *to, const model_t *from)
{
    *to = *from;
    for (int i = 0; i < from->no; i++)
        if (from->o[i].data) { to->o[i].data = malloc(from->o[i].len + 1); memcpy(to->o[i].data, from->o[i].data, from->o[i].len); }
}
static void mclear(model_t *m) { for (int i = 0; i < m->no; i++) free(m->o[i].data); memset(m, 0, sizeof *m); }

static void snapshots(int rounds, int enc)
{
    enum { NS = 6 };
    static model_t m, sm[NS];
    static char sname[NS][16];
    static long sseq[NS];
    long seq = 0, taken = 0, deleted = 0, restored = 0;
    mem_t mem;
    siefs_env_t env;
    siefs_t *fs;
    uint64_t home, ino, a, b;
    siefs_statfs_t st0, st1;
    printf("snapshot test%s: %d rounds, snapshots taken, deleted and restored at random\n", enc ? " (encrypted volume)" : "", rounds);
    mem_init(&mem, 8192);
    env = env_of(&mem);
    env.cache_nodes = 16;
    CHECK(siefs_format(&env, "snap") == 0 && siefs_mount(&env, &fs) == 0);
    CHECK(siefs_statfs(fs, &st0) == 0);

    /* volumes: one mounted at /home; names do not cross volumes */
    CHECK(siefs_mkvol(fs, SIEFS_ROOT, "home", 0755, 1000, 1000, &home) == 0 && home >> 48 > 1);
    CHECK(siefs_walk(fs, "/home", &ino) == 0 && ino == home);
    CHECK(siefs_create(fs, home, "f", SIEFS_IFREG | 0644, 0, 0, &a) == 0 && a >> 48 == home >> 48);
    CHECK(siefs_create(fs, SIEFS_ROOT, "g", SIEFS_IFREG | 0644, 0, 0, &b) == 0 && b >> 48 == 0);
    CHECK(siefs_walk(fs, "/home/..", &ino) == 0 && ino == SIEFS_ROOT);
    CHECK(siefs_rename(fs, home, "f", SIEFS_ROOT, "f") == -SIEFS_EXDEV);
    CHECK(siefs_link(fs, a, SIEFS_ROOT, "h") == -SIEFS_EXDEV);
    CHECK(siefs_rmdir(fs, SIEFS_ROOT, "home") == -SIEFS_EBUSY);
    CHECK(siefs_rmvol(fs, SIEFS_ROOT, "home") == -SIEFS_ENOTEMPTY);
    CHECK(siefs_unlink(fs, home, "f") == 0 && siefs_unlink(fs, SIEFS_ROOT, "g") == 0);
    CHECK(siefs_sync(fs) == 0 && siefs_check(fs, 1, quiet, 0) == 0);
    CHECK(siefs_rmvol(fs, SIEFS_ROOT, "home") == 0);
    CHECK(siefs_walk(fs, "/home", &ino) == -SIEFS_ENOENT);
    CHECK(siefs_sync(fs) == 0 && siefs_sync(fs) == 0 && siefs_sync(fs) == 0 && siefs_check(fs, 1, quiet, 0) == 0);
    CHECK((enc ? siefs_mkvol_enc(fs, SIEFS_ROOT, "home", 0755, 1000, 1000, &KDF, KEK, &home)
               : siefs_mkvol(fs, SIEFS_ROOT, "home", 0755, 1000, 1000, &home)) == 0);

    mpre = "/home";
    for (int round = 0; round < rounds && !fails; round++) {
        for (int i = 0; i < 40; i++) mop(fs, &m);
        int r = (int)rn(10), k = (int)rn(NS);
        if (r < 4 && !sname[k][0]) {                    /* take one */
            snprintf(sname[k], sizeof sname[k], "s%d", round);
            CHECK(siefs_snapshot(fs, home, sname[k]) == 0);
            mcopy(&sm[k], &m); sseq[k] = ++seq; taken++;
        } else if (r < 6 && sname[k][0]) {              /* delete one */
            CHECK(siefs_snapshot_delete(fs, home, sname[k]) == 0);
            mclear(&sm[k]); sname[k][0] = 0; deleted++;
        } else if (r == 6 && sname[k][0]) {             /* go back to one: the newer ones go */
            CHECK(siefs_snapshot_restore(fs, home, sname[k]) == 0);
            mclear(&m); mcopy(&m, &sm[k]); restored++;
            for (int j = 0; j < NS; j++) if (sname[j][0] && sseq[j] > sseq[k]) { mclear(&sm[j]); sname[j][0] = 0; }
        }
        mcompare(fs, &m);
        for (int j = 0; j < NS; j++) {                  /* every snapshot shows its time */
            char p[64];
            if (!sname[j][0]) continue;
            snprintf(p, sizeof p, "/home/.snapshots/%s", sname[j]);
            mpre = p; mcompare(fs, &sm[j]); mpre = "/home";
        }
        CHECK(siefs_sync(fs) == 0);
        CHECK(siefs_check(fs, 1, quiet, 0) == 0);
        if (round % 10 == 9) {
            CHECK(siefs_unmount(fs) == 0 && siefs_mount(&env, &fs) == 0);
            if (enc) CHECK(siefs_unlock(fs, home, KEK) == 0);   /* (locked again after a mount) */
            mcompare(fs, &m);
        }
    }
    for (int j = 0; j < NS; j++)                        /* a snapshot is read only */
        if (sname[j][0] && sm[j].ne) {
            char p[96];
            snprintf(p, sizeof p, "/home/.snapshots/%s", sname[j]);
            CHECK(siefs_walk(fs, p, &ino) == 0);
            CHECK(siefs_create(fs, ino, "new", SIEFS_IFREG | 0644, 0, 0, &a) == -SIEFS_EROFS);
            break;
        }
    uint64_t cur = 0;
    siefs_snap_t sn;
    int listed = 0, live = 0;
    while (siefs_snapshot_list(fs, home, &cur, &sn) == 1) listed++;
    for (int j = 0; j < NS; j++) live += sname[j][0] != 0;
    CHECK(listed == live);
    for (int j = 0; j < NS; j++) if (sname[j][0]) { CHECK(siefs_snapshot_delete(fs, home, sname[j]) == 0); mclear(&sm[j]); sname[j][0] = 0; }
    mcompare(fs, &m);
    CHECK(siefs_sync(fs) == 0 && siefs_check(fs, 1, quiet, 0) == 0);
    for (int i = 0; i < m.ne; i++) {                    /* empty the volume, remove it: all the space comes back */
        int d = 0;
        for (int j = 0; j < m.ne; j++) if (strlen(m.e[j].path) > strlen(m.e[d].path)) d = j;
        const char *name;
        uint64_t dir;
        if (split(fs, m.e[d].path, &dir, &name)) { CHECK(!"split"); break; }
        CHECK((m.o[m.e[d].obj].type == MDIR ? siefs_rmdir(fs, dir, name) : siefs_unlink(fs, dir, name)) == 0);
        mdel(&m, d); i = -1;
    }
    CHECK(siefs_rmvol(fs, SIEFS_ROOT, "home") == 0);
    for (int i = 0; i < 3; i++) CHECK(siefs_sync(fs) == 0);
    CHECK(siefs_check(fs, 1, quiet, 0) == 0);
    CHECK(siefs_statfs(fs, &st1) == 0);
    CHECK(st1.free + st1.pending == st0.free + st0.pending);
    printf("  %ld taken, %ld deleted, %ld restored; all space back at the end: %s\n", taken, deleted, restored,
           st1.free + st1.pending == st0.free + st0.pending ? "yes" : "NO");
    mpre = "";
    siefs_unmount(fs);
    mclear(&m);
    mem_free(&mem);
}

/* ================================================================ 2c. encryption
 * An encrypted volume: nothing of it in clear on the disk, locked after a
 * mount (its root still stats as its owner's), the wrong key refused, a
 * new password, and the checker without the key. */
static int on_disk(mem_t *m, const char *s)
{
    size_t n = strlen(s);
    for (uint64_t i = 0; i + n <= m->nb * BS; i++) if (m->cur[i] == (uint8_t)s[0] && !memcmp(m->cur + i, s, n)) return 1;
    return 0;
}
static void encryption(void)
{
    mem_t mem;
    siefs_env_t env;
    siefs_t *fs;
    uint64_t sec, pub, a, ino;
    siefs_stat_t st;
    siefs_dirent_t de;
    static char data[20000];
    printf("encryption test\n");
    mem_init(&mem, 4096);
    env = env_of(&mem);
    CHECK(siefs_format(&env, "enc") == 0 && siefs_mount(&env, &fs) == 0);
    CHECK(siefs_mkvol_enc(fs, SIEFS_ROOT, "secret", 0700, 1000, 1000, &KDF, KEK, &sec) == 0);
    CHECK(siefs_mkvol(fs, SIEFS_ROOT, "public", 0755, 1000, 1000, &pub) == 0);
    for (int i = 0; i < 20000; i++) data[i] = "ENCRYPTED-MARKER "[i % 17];
    CHECK(siefs_create(fs, sec, "hidden-name.txt", SIEFS_IFREG | 0600, 1000, 1000, &a) == 0);
    CHECK(siefs_write(fs, a, 0, data, sizeof data) == (long)sizeof data);
    CHECK(siefs_create(fs, sec, "small", SIEFS_IFREG | 0600, 1000, 1000, &a) == 0);
    CHECK(siefs_write(fs, a, 0, "SMALL-INLINE-SECRET", 19) == 19);
    CHECK(siefs_setxattr(fs, a, "topic", "XATTR-SECRET", 12) == 0);
    CHECK(siefs_create(fs, pub, "visible-name.txt", SIEFS_IFREG | 0644, 1000, 1000, &a) == 0);
    CHECK(siefs_write(fs, a, 0, "PLAIN-MARKER", 12) == 12);
    CHECK(siefs_snapshot(fs, sec, "s") == 0);
    CHECK(siefs_sync(fs) == 0 && siefs_check(fs, 1, quiet, 0) == 0);
    CHECK(!on_disk(&mem, "ENCRYPTED-MARKER") && !on_disk(&mem, "hidden-name") && !on_disk(&mem, "SMALL-INLINE") &&
          !on_disk(&mem, "XATTR-SECRET"));
    CHECK(on_disk(&mem, "PLAIN-MARKER") && on_disk(&mem, "visible-name"));   /* (the test can see) */
    CHECK(siefs_unmount(fs) == 0 && siefs_mount(&env, &fs) == 0);

    /* locked: the root stats as its owner's directory; nothing else */
    uint64_t cur = 2;
    CHECK(siefs_walk(fs, "/secret", &ino) == 0 && ino == sec);
    CHECK(siefs_stat(fs, sec, &st) == 0 && st.uid == 1000 && (st.mode & 07777) == 0700 && (st.mode >> 12) == 4);
    CHECK(siefs_readdir(fs, sec, &cur, &de) == -SIEFS_EACCES);
    CHECK(siefs_walk(fs, "/secret/small", &ino) == -SIEFS_EACCES);
    CHECK(siefs_walk(fs, "/secret/.snapshots/s/small", &ino) == -SIEFS_EACCES);
    CHECK(siefs_check(fs, 1, 0, 0) == 0);                /* the rest checks; leaks are not looked for */
    uint64_t vr;
    CHECK(siefs_volume_root(fs, sec, &vr) == 0 && vr == sec);   /* (found while locked: to unlock it) */
    CHECK(siefs_unlock(fs, sec, KEK2) == -SIEFS_EACCES);
    siefs_kdf_t k;
    CHECK(siefs_kdf(fs, sec, &k) == 0 && !memcmp(k.salt, KDF.salt, 16) && k.t == KDF.t && k.m_kib == KDF.m_kib);
    CHECK(siefs_kdf(fs, pub, &k) == -SIEFS_EINVAL);
    CHECK(siefs_unlock(fs, sec, KEK) == 0);
    char got[32];
    CHECK(siefs_walk(fs, "/secret/small", &ino) == 0 && siefs_read(fs, ino, 0, got, 32) == 19 && !memcmp(got, "SMALL-INLINE-SECRET", 19));
    CHECK(siefs_walk(fs, "/secret/hidden-name.txt", &ino) == 0 && siefs_read(fs, ino, 0, data, sizeof data) == (long)sizeof data &&
          !memcmp(data, "ENCRYPTED-MARKER", 16));
    CHECK(siefs_walk(fs, "/secret/.snapshots/s/small", &ino) == 0 && siefs_read(fs, ino, 0, got, 32) == 19);
    CHECK(siefs_check(fs, 1, quiet, 0) == 0);

    /* a new password: the same key, wrapped anew */
    siefs_kdf_t k2 = { "fedcba9876543210", 3, 64 };
    CHECK(siefs_rekey(fs, sec, &k2, KEK2) == 0);
    CHECK(siefs_unmount(fs) == 0 && siefs_mount(&env, &fs) == 0);
    CHECK(siefs_unlock(fs, sec, KEK) == -SIEFS_EACCES && siefs_unlock(fs, sec, KEK2) == 0);
    CHECK(siefs_walk(fs, "/secret/small", &ino) == 0 && siefs_read(fs, ino, 0, got, 32) == 19);

    /* locked again, while mounted */
    CHECK(siefs_lock(fs, sec) == 0 && siefs_walk(fs, "/secret/small", &ino) == -SIEFS_EACCES);
    CHECK(siefs_unlock(fs, sec, KEK2) == 0 && siefs_walk(fs, "/secret/small", &ino) == 0);

    /* tampering: a byte of a sealed data block changed, its checksum fixed: the tag says no */
    path_t pa;
    extent_t x;
    volx_t *v = vol_get(fs, sec >> 48);
    CHECK(siefs_walk(fs, "/secret/hidden-name.txt", &a) == 0);
    fs->cv = v;
    CHECK(t_seek(fs, &v->ft, K(a & LMASK, T_EXTENT, 0), &pa) == 1 && ext_dec(pval(&pa), plen(&pa), &x) == 0 && (x.flags & X_ENC));
    static uint8_t blk[BS];
    memcpy(blk, mem.cur + x.blk * BS, BS);              /* the sealed block opens with its tag ... */
    CHECK(data_open(fs, v, x.blk, x.salt, blk, x.mac[0]) == 0 && !memcmp(blk, "ENCRYPTED-MARKER", 16));
    memcpy(blk, mem.cur + x.blk * BS, BS);
    blk[100] ^= 1;                                      /* ... and not with one bit changed, */
    CHECK(data_open(fs, v, x.blk, x.salt, blk, x.mac[0]) == -SIEFS_EIO);
    memcpy(blk, mem.cur + x.blk * BS, BS);              /* nor at another place on the disk */
    CHECK(data_open(fs, v, x.blk + 1, x.salt, blk, x.mac[0]) == -SIEFS_EIO);
    siefs_unmount(fs);
    printf("  nothing in clear on the disk; locked after a mount; wrong key refused; new password; tampering seen\n");
    mem_free(&mem);
}

/* ================================================================ 3. crashes */
static int snaps, encd;                                 /* crash scenarios with snapshots in them; /dir encrypted */
static int mount_open(siefs_env_t *env, siefs_t **fs)  /* (mount, and unlock /dir when it is encrypted) */
{
    uint64_t d;
    int e = siefs_mount(env, fs);
    if (!e && encd && !siefs_walk(*fs, "/dir", &d)) e = siefs_unlock(*fs, d, KEK);
    return e;
}
static uint32_t steps[512]; static int nsteps = -1;     /* (dry run: the state after each operation: a snapshot's commits can land between) */
static uint32_t sig(siefs_t *fs);
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
        if (nsteps >= 0 && nsteps < 512) steps[nsteps++] = sig(fs);
        if (snaps && !rn(6)) {                          /* snapshots too: taken, deleted, restored */
            char sn[8];
            sprintf(sn, "s%u", rn(4));
            int q = (int)rn(4);
            if (q < 2) siefs_snapshot(fs, SIEFS_ROOT, sn);
            else if (q < 3) siefs_snapshot_delete(fs, SIEFS_ROOT, sn);
            else siefs_snapshot_restore(fs, SIEFS_ROOT, sn);
        }
        if (nsteps >= 0 && nsteps < 512) steps[nsteps++] = sig(fs);
    }
}

static void crash(int seeds)
{
    long runs = 0, newer = 0;
    printf("crash test: %d scenarios, a power cut at every write of a transaction (half with snapshots, a third encrypted)\n", seeds);
    for (int s = 1; s <= seeds && !fails; s++) {
        snaps = s % 2 == 0;
        encd = s % 3 == 0;
        mem_t m;
        siefs_env_t env;
        siefs_t *fs;
        uint64_t d;
        mem_init(&m, 2048);
        env = env_of(&m);
        clk = 0;
        CHECK(siefs_format(&env, "crash") == 0 && siefs_mount(&env, &fs) == 0);
        if (encd) CHECK(siefs_mkvol_enc(fs, SIEFS_ROOT, "dir", 0755, 0, 0, &KDF, KEK, &d) == 0);
        else siefs_create(fs, SIEFS_ROOT, "dir", SIEFS_IFDIR | 0755, 0, 0, &d);
        ops_batch(fs, (uint64_t)s * 1000, 40);
        CHECK(siefs_unmount(fs) == 0);                  /* state A, committed */
        uint8_t *a_img = malloc(m.nb * BS);
        memcpy(a_img, m.cur, m.nb * BS);
        memcpy(m.dur, m.cur, m.nb * BS);
        uint32_t sa, sb;
        long w0, total;
        /* a dry run: state B, and how many writes its commit makes */
        CHECK(mount_open(&env, &fs) == 0);
        sa = sig(fs);
        int64_t c0 = clk;
        int nops = 10 + 6 * s;                           /* bigger and bigger transactions */
        w0 = m.writes;
        nsteps = 0;                                      /* (snapshots commit by themselves: any step can land) */
        ops_batch(fs, (uint64_t)s * 7777, nops);
        int nst = nsteps;
        nsteps = -1;
        sb = sig(fs);
        CHECK(siefs_sync(fs) == 0);
        total = m.writes - w0;                           /* every write: file data, nodes, bitmap, superblock */
        siefs_unmount(fs);
        for (long cut = 0; cut <= total && !fails; cut++)
            for (int v = 0; v < (cut < total ? 3 : 1); v++) {   /* 3 random fates for unflushed writes */
                memcpy(m.cur, a_img, m.nb * BS);
                memcpy(m.dur, a_img, m.nb * BS);
                m.npend = 0;
                CHECK(mount_open(&env, &fs) == 0);
                clk = c0;
                m.cut = m.writes + cut;                  /* the power fails at write number cut */
                ops_batch(fs, (uint64_t)s * 7777, nops);
                siefs_sync(fs);
                fs->broken = 1;                          /* forget the machine that lost power */
                siefs_unmount(fs);
                mem_crash(&m);
                siefs_t *fs2;
                if (mount_open(&env, &fs2)) { CHECK(!"mount after crash"); continue; }
                uint32_t sg = sig(fs2);
                int mid = 0;
                for (int i = 0; snaps && i < nst; i++) mid |= sg == steps[i];
                CHECK(sg == sa || sg == sb || mid);
                if (sg != sa && sg != sb && !mid) printf("  scenario %d, cut at write %ld/%ld: neither old nor new\n", s, cut, total);
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

/* Growing (siefs_grow): a small volume on a bigger device grows twice (the
 * bitmap copies move each time), with a power cut at every write of each
 * grow: the volume must come back old or grown, same files, checked clean,
 * and grow afterwards; then files fill the new space and read back. */
static void grow(void)
{
    static const uint64_t size[3] = { 600, 34000, 70000 };     /* bitmap: 1, 2, then 3 blocks a copy */
    mem_t m;
    siefs_env_t env;
    siefs_t *fs;
    siefs_statfs_t st;
    long runs = 0;
    mem_init(&m, size[2]);
    env = env_of(&m);
    env.nblocks = size[0];
    clk = 0; encd = snaps = 0;
    CHECK(siefs_format(&env, "grow") == 0 && siefs_mount(&env, &fs) == 0);
    siefs_create(fs, SIEFS_ROOT, "dir", SIEFS_IFDIR | 0755, 0, 0, &(uint64_t){ 0 });
    ops_batch(fs, 4242, 60);
    CHECK(siefs_unmount(fs) == 0);
    for (int g = 1; g <= 2 && !fails; g++) {
        env.nblocks = size[g];
        uint8_t *a_img = malloc(m.nb * BS);
        memcpy(a_img, m.cur, m.nb * BS);
        CHECK(siefs_mount(&env, &fs) == 0);
        uint32_t sa = sig(fs);
        long w0 = m.writes, total;
        CHECK(siefs_grow(fs, size[g]) == 0);
        total = m.writes - w0;
        siefs_unmount(fs);
        for (long cut = 0; cut <= total && !fails; cut++)
            for (int v = 0; v < (cut < total ? 3 : 1); v++) {
                memcpy(m.cur, a_img, m.nb * BS); memcpy(m.dur, a_img, m.nb * BS); m.npend = 0;
                CHECK(siefs_mount(&env, &fs) == 0);
                m.cut = m.writes + cut;
                siefs_grow(fs, size[g]);
                fs->broken = 1;
                siefs_unmount(fs);
                mem_crash(&m);
                if (siefs_mount(&env, &fs)) { CHECK(!"mount after a cut grow"); continue; }
                siefs_statfs(fs, &st);
                CHECK(sig(fs) == sa && (st.blocks == size[g - 1] || st.blocks == size[g]));
                if (cut == total) CHECK(st.blocks == size[g]);
                CHECK(siefs_check(fs, 1, quiet, 0) == 0);
                CHECK(siefs_grow(fs, size[g]) == 0 && siefs_check(fs, 1, quiet, 0) == 0);   /* grown, then */
                ops_batch(fs, 99, 10);
                CHECK(siefs_sync(fs) == 0 && siefs_check(fs, 1, quiet, 0) == 0);
                siefs_unmount(fs);
                runs++;
            }
        memcpy(m.cur, a_img, m.nb * BS); memcpy(m.dur, a_img, m.nb * BS);
        free(a_img);
        CHECK(siefs_mount(&env, &fs) == 0 && siefs_grow(fs, size[g]) == 0);
        CHECK(siefs_grow(fs, size[g]) == 0);                       /* (twice: nothing to do) */
        siefs_unmount(fs);
    }
    /* fill the grown volume: files land past the old ends, and read back */
    CHECK(siefs_mount(&env, &fs) == 0);
    siefs_statfs(fs, &st);
    CHECK(st.blocks == size[2] && st.free > size[2] - size[1]);
    static uint8_t blk[EXT_MAX * BS];
    uint64_t ino, n = 0;
    char name[16];
    for (int f = 0; !fails; f++) {
        sprintf(name, "big%d", f);
        if (siefs_create(fs, SIEFS_ROOT, name, SIEFS_IFREG | 0644, 0, 0, &ino)) break;
        long r = 0;
        for (uint64_t off = 0; off < (64 << 20); off += sizeof blk) {
            memset(blk, f + 1, sizeof blk);
            if ((r = siefs_write(fs, ino, off, blk, sizeof blk)) < 0) break;
            n += sizeof blk;
        }
        if (r < 0) break;
    }
    siefs_statfs(fs, &st);
    CHECK(n > (size[2] - size[1]) * BS && st.free < 1000 && siefs_sync(fs) == 0);   /* (past both old ends) */
    siefs_unmount(fs);
    CHECK(siefs_mount(&env, &fs) == 0 && siefs_check(fs, 1, quiet, 0) == 0);
    CHECK(siefs_walk(fs, "/big3", &ino) == 0 && siefs_read(fs, ino, 1 << 20, blk, BS) == BS && blk[0] == 4 && blk[BS - 1] == 4);
    siefs_unmount(fs);
    mem_free(&m);
    printf("grow test: %lu -> %lu -> %lu blocks, %ld power cuts during the grows, all came back whole; %lu MiB written after\n",
           (unsigned long)size[0], (unsigned long)size[1], (unsigned long)size[2], runs, (unsigned long)(n >> 20));
}

/* ================================================================ 4. damage */
/* Two writers on one disk (a host tool on the image a virtual machine has
 * mounted): the one that did not commit last must notice before writing
 * anything, and the other's commit must stay whole. */
static void two_writers(void)
{
    mem_t m;
    siefs_env_t env;
    siefs_t *a, *b;
    uint64_t f, g;
    mem_init(&m, 4096);
    env = env_of(&m);
    clk = 0;
    CHECK(siefs_format(&env, "two") == 0 && siefs_mount(&env, &a) == 0);
    CHECK(siefs_create(a, SIEFS_ROOT, "a", SIEFS_IFREG | 0644, 0, 0, &f) == 0 && siefs_write(a, f, 0, "A", 1) == 1);
    CHECK(siefs_sync(a) == 0);
    CHECK(siefs_mount(&env, &b) == 0);                  /* the second writer: commits twice */
    CHECK(siefs_create(b, SIEFS_ROOT, "b", SIEFS_IFREG | 0644, 0, 0, &g) == 0 && siefs_write(b, g, 0, "BB", 2) == 2);
    CHECK(siefs_sync(b) == 0 && siefs_write(b, g, 2, "B", 1) == 1 && siefs_sync(b) == 0);
    uint32_t sb_ = sig(b);
    CHECK(!siefs_shared(a));
    uint8_t *img = malloc(m.nb * BS);
    memcpy(img, m.cur, m.nb * BS);
    long w0 = m.writes;
    CHECK(siefs_write(a, f, 1, "AAAA", 4) == E(EBUSY));    /* the first: stops before writing */
    CHECK(siefs_shared(a) && m.writes == w0 && !memcmp(img, m.cur, m.nb * BS));
    CHECK(siefs_write(a, f, 1, "A", 1) == E(EIO) && siefs_sync(a) != 0);
    siefs_unmount(a);
    CHECK(m.writes == w0 && !memcmp(img, m.cur, m.nb * BS));
    siefs_unmount(b);
    CHECK(siefs_mount(&env, &b) == 0);                  /* what the second wrote is all there */
    CHECK(sig(b) == sb_ && siefs_check(b, 1, quiet, 0) == 0);
    siefs_unmount(b);

    /* The other commits while a change waits for its commit (checked when
     * the change began): the commit checks again. */
    CHECK(siefs_mount(&env, &a) == 0 && siefs_mount(&env, &b) == 0);
    siefs_stat_t st = { .mode = 0600 };
    CHECK(siefs_setattr(a, g, &st, SIEFS_SET_MODE) == 0);
    CHECK(siefs_write(b, g, 0, "C", 1) == 1 && siefs_sync(b) == 0);
    memcpy(img, m.cur, m.nb * BS);
    w0 = m.writes;
    CHECK(siefs_sync(a) == E(EBUSY) && siefs_shared(a) && m.writes == w0 && !memcmp(img, m.cur, m.nb * BS));
    siefs_unmount(a);
    siefs_unmount(b);
    CHECK(siefs_mount(&env, &b) == 0 && siefs_check(b, 1, quiet, 0) == 0 && siefs_sync(b) == 0);
    CHECK(siefs_write(b, g, 0, "D", 1) == 1 && siefs_sync(b) == 0 && !siefs_shared(b));   /* alone again: writes */
    siefs_unmount(b);
    free(img);
    mem_free(&m);
    printf("two writers: the late one stopped before writing, the other's commits whole\n");
}

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
    uint64_t root = vol_get(fs, 1)->ft.root.blk, bm = fs->sb.bm[fs->sb.seq & 1];
    extent_t x;
    path_t pa;
    CHECK(t_seek(fs, &vol_get(fs, 1)->ft, K(a, T_EXTENT, 0), &pa) == 1);
    CHECK(ext_dec(pval(&pa), plen(&pa), &x) == 0);
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
    if (argc > 2 && !strcmp(argv[1], "snapshots")) {    /* (more seeds: siefs-test snapshots SEED) */
        rs = strtoull(argv[2], 0, 10);
        snapshots(300, argc > 3);
        printf(fails ? "%ld FAILURES\n" : "passed\n", fails);
        return fails != 0;
    }
    unit();
    model(quick ? 30 : 300);
    snapshots(quick ? 40 : 300, 0);
    snapshots(quick ? 40 : 150, 1);
    encryption();
    crash(quick ? 3 : 30);
    grow();
    two_writers();
    damage();
    if (!fails) measure();
    printf(fails ? "SieFS tests: %ld FAILURES\n" : "SieFS tests: all passed\n", fails);
    return fails != 0;
}
