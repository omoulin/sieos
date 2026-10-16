/*
 * odb.c - git's object store, refs, index and trees, in git's formats.
 *
 * An object is "type size\0" + its content, named by the SHA-1 of that.
 * Loose objects are one zlib-compressed file each, .git/objects/xx/yyyy...
 * Packs hold many: .pack (entries: a type and size, then zlib data; a
 * "delta" entry is a recipe to build the object from another one, given
 * by its offset in the pack or by its name) and .idx v2 (the names sorted,
 * where each entry starts, a CRC of each). Refs are files holding a name
 * (.git/refs/heads/main), or lines of .git/packed-refs; HEAD names the
 * branch ("ref: refs/heads/main"). The index (.git/index, "DIRC" v2) is
 * the next commit's files: path, mode, object, and the file's size and
 * time, to see quickly what changed. A tree lists one folder: mode, name
 * and object of each entry; commit_t reads a commit.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "git.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void wbe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static const uint8_t zero[20];
static void *zalloc(size_t n) { void *p = malloc(n); if (p) memset(p, 0, n); return p; }

static const char *const tnames[] = { "?", "commit", "tree", "blob", "tag" };
const char *type_name(int t) { return tnames[t >= 1 && t <= 4 ? t : 0]; }

void obj_hash(int type, const void *p, size_t n, oid_t id)
{
    char h[32];
    int hl = sfmt(h, sizeof h, "%s %lu", type_name(type), (unsigned long)n) + 1;
    sha1_t s;
    sha1_init(&s);
    sha1_update(&s, h, (size_t)hl);
    sha1_update(&s, p, n);
    sha1_final(&s, id);
}

static void loose_path(char *path, const oid_t id)
{
    char h[41];
    hex(h, id);
    sfmt(path, 64, ".git/objects/%c%c/%s", h[0], h[1], h + 2);
}

int obj_write(int type, const void *p, size_t n, oid_t id)
{
    obj_hash(type, p, n, id);
    if (obj_has(id)) return 0;
    char h[32], path[64];
    size_t hl = (size_t)sfmt(h, sizeof h, "%s %lu", type_name(type), (unsigned long)n) + 1;
    uint8_t *raw = malloc(hl + n + 1), *z = malloc(ZBOUND(hl + n));
    if (!raw || !z) { free(raw); free(z); return -ENOMEM; }
    memcpy(raw, h, hl);
    memcpy(raw + hl, p, n);
    size_t zn = zdeflate(raw, hl + n, z);
    loose_path(path, id);
    int e = put_file(path, z, zn, 0644);
    free(raw); free(z);
    return e;
}

/* ---- packs: the .idx files at the first need, a .pack's data at its first read */
typedef struct { uint8_t *idx, *data; size_t datan; uint32_t n; char path[96]; } pack_t;
static pack_t packs[32];
static int npacks = -1;
/* the pack being indexed (pack_store): its objects named so far */
static struct { const uint8_t *pk; size_t n; int cnt; uint8_t (*ids)[20]; size_t *offs; char *done; } cur;

#define NCACHE 64                    /* delta bases used again (one base, many deltas): the last ones kept */
static struct { const uint8_t *pk; size_t off, len; int type; uint8_t *d; } cache[NCACHE];

void odb_reload(void)
{
    for (int i = 0; i < npacks; i++) { free(packs[i].idx); free(packs[i].data); }
    npacks = -1;
    for (int i = 0; i < NCACHE; i++) { free(cache[i].d); cache[i].d = 0; }   /* (keyed by the packs' memory) */
}

static void load_packs(void)
{
    if (npacks >= 0) return;
    npacks = 0;
    long h = fs_open(".git/objects/pack", FS_RDONLY | FS_DIRECTORY, 0);
    if (h < 0) return;
    static char buf[4096];
    char name[256];
    uint64_t cs = 0, ino;
    long n;
    int type;
    while ((n = fs_readdir(h, &cs, buf, sizeof buf)) > 0)
        for (const char *p = buf; fs_dirent(&p, buf + n, &ino, &type, name) && npacks < 32; ) {
            size_t l = strlen(name);
            if (l < 5 || strcmp(name + l - 4, ".idx") || l > 70) continue;
            pack_t *k = &packs[npacks];
            size_t len;
            sfmt(k->path, sizeof k->path, ".git/objects/pack/%s", name);
            k->idx = (uint8_t *)get_file(k->path, &len);
            if (!k->idx || len < 8 + 1024 + 40 || be32(k->idx) != 0xff744f63 || be32(k->idx + 4) != 2) { free(k->idx); continue; }
            k->n = be32(k->idx + 8 + 255 * 4);
            strlcpy(k->path + strlen(k->path) - 4, ".pack", 6);
            k->data = 0;
            npacks++;
        }
    fs_close(h);
}

static int idx_find(pack_t *k, const oid_t id, size_t *off)
{
    const uint8_t *fan = k->idx + 8, *ids = fan + 1024;
    uint32_t lo = id[0] ? be32(fan + 4 * (id[0] - 1)) : 0, hi = be32(fan + 4 * id[0]);
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        int c = memcmp(ids + 20 * m, id, 20);
        if (!c) {
            const uint8_t *o = ids + 24 * k->n + 4 * m;
            uint32_t v = be32(o);
            if (v & 0x80000000) {
                const uint8_t *b = ids + 28 * k->n + 8 * (v & 0x7fffffff);
                *off = (size_t)((uint64_t)be32(b) << 32 | be32(b + 4));
            } else *off = v;
            return 1;
        }
        if (c < 0) lo = m + 1; else hi = m;
    }
    return 0;
}

/* One entry of a pack: its type, size, delta base, and its data inflated. */
typedef struct { int type; size_t size, end, base; const uint8_t *bid; uint8_t *data; } raw_t;
static int entry_raw(const uint8_t *pk, size_t n, size_t off, raw_t *r)
{
    size_t p = off, used;
    if (p >= n) return -1;
    uint8_t c = pk[p++];
    uint64_t sz = c & 15;
    r->type = c >> 4 & 7;
    if (!r->type || r->type == 5) return -1;
    for (int sh = 4; c & 0x80; sh += 7) { if (p >= n) return -1; c = pk[p++]; sz |= (uint64_t)(c & 0x7f) << sh; }
    if (r->type == OBJ_OFS) {
        if (p >= n) return -1;
        c = pk[p++];
        uint64_t o = c & 127;
        while (c & 128) { if (p >= n) return -1; c = pk[p++]; o = (o + 1) << 7 | (c & 127); }
        if (o > off) return -1;
        r->base = off - o;
    } else if (r->type == OBJ_REF) { r->bid = pk + p; p += 20; }
    if (p > n || !(r->data = malloc(sz + 1))) return -1;
    if (zinflate(pk + p, n - p, r->data, sz, &used) != (long)sz || !used) { free(r->data); return -1; }
    r->data[sz] = 0;
    r->size = sz;
    r->end = p + used;
    return 0;
}

static uint64_t varint(const uint8_t **p, const uint8_t *e)
{
    uint64_t v = 0;
    for (int s = 0; *p < e; s += 7) { uint8_t c = *(*p)++; v |= (uint64_t)(c & 127) << s; if (!(c & 128)) break; }
    return v;
}

/* A delta: the base's size, the result's, then copies from the base
 * (offset and size, their bytes given by the flags) and inserted bytes. */
static uint8_t *apply(const uint8_t *base, size_t bn, const uint8_t *d, size_t dn, size_t *len)
{
    const uint8_t *p = d, *e = d + dn;
    uint64_t sn = varint(&p, e), tn = varint(&p, e);
    uint8_t *o = sn == bn ? malloc(tn + 1) : 0;
    size_t k = 0;
    if (!o) return 0;
    while (p < e) {
        uint8_t op = *p++;
        if (op & 0x80) {
            uint64_t co = 0, cn = 0;
            for (int i = 0; i < 4; i++) if (op & 1 << i) co |= (uint64_t)(p < e ? *p++ : 0) << 8 * i;
            for (int i = 0; i < 3; i++) if (op & 16 << i) cn |= (uint64_t)(p < e ? *p++ : 0) << 8 * i;
            if (!cn) cn = 0x10000;
            if (co + cn > bn || k + cn > tn) break;
            memcpy(o + k, base + co, cn);
            k += cn;
        } else if (op && op <= e - p && k + op <= tn) { memcpy(o + k, p, op); p += op; k += op; }
        else break;
    }
    if (p != e || k != tn) { free(o); return 0; }
    o[tn] = 0;
    *len = tn;
    return o;
}

static uint8_t *dup_obj(const uint8_t *d, size_t n) { uint8_t *c = malloc(n + 1); if (c) { memcpy(c, d, n); c[n] = 0; } return c; }

uint8_t *pack_entry(const uint8_t *pk, size_t n, size_t off, int *type, size_t *len, size_t *end)
{
    unsigned ci = (unsigned)((off ^ (uintptr_t)pk >> 4) % NCACHE);
    if (!end && cache[ci].d && cache[ci].pk == pk && cache[ci].off == off) {
        *type = cache[ci].type; *len = cache[ci].len;
        return dup_obj(cache[ci].d, *len);
    }
    raw_t r;
    if (entry_raw(pk, n, off, &r)) return 0;
    if (end) *end = r.end;
    if (r.type != OBJ_OFS && r.type != OBJ_REF) { *type = r.type; *len = r.size; return r.data; }
    int bt;
    size_t bl;
    uint8_t *b = r.type == OBJ_OFS ? pack_entry(pk, n, r.base, &bt, &bl, 0) : obj_read(r.bid, &bt, &bl);
    uint8_t *o = b ? apply(b, bl, r.data, r.size, len) : 0;
    free(r.data);
    if (o && bl < (1 << 20)) {                       /* keep the base */
        unsigned bi = (unsigned)(((r.type == OBJ_OFS ? r.base : off) ^ (uintptr_t)pk >> 4) % NCACHE);
        if (r.type == OBJ_OFS) {
            free(cache[bi].d);
            cache[bi].pk = pk; cache[bi].off = r.base; cache[bi].len = bl; cache[bi].type = bt; cache[bi].d = b;
            b = 0;
        }
    }
    free(b);
    *type = bt;
    return o;
}

static int loose_read(const oid_t id, uint8_t **out, int *type, size_t *len)
{
    char path[64], hdr[64];
    size_t n, used;
    loose_path(path, id);
    char *z = get_file(path, &n);
    if (!z) return -1;
    long h = zinflate(z, n, hdr, sizeof hdr - 1, &used);
    char *sp = 0, *nul = 0;
    for (long i = 0; i < h && !nul; i++) if (hdr[i] == ' ' && !sp) sp = hdr + i; else if (!hdr[i]) nul = hdr + i;
    *out = 0;
    if (sp && nul > sp) {
        *sp = 0;
        long sz = strnum(sp + 1);
        size_t hl = (size_t)(nul - hdr) + 1;
        for (*type = 1; *type <= 4 && strcmp(hdr, tnames[*type]); (*type)++) ;
        uint8_t *o = sz >= 0 && *type <= 4 ? malloc(hl + (size_t)sz + 1) : 0;
        if (o && zinflate(z, n, o, hl + (size_t)sz, &used) == (long)(hl + (size_t)sz) && used) {
            memmove(o, o + hl, (size_t)sz);
            o[sz] = 0;
            *len = (size_t)sz;
            *out = o;
        } else free(o);
    }
    free(z);
    return *out ? 0 : -1;
}

uint8_t *obj_read(const oid_t id, int *type, size_t *len)
{
    uint8_t *o;
    if (!loose_read(id, &o, type, len)) return o;
    load_packs();
    size_t off;
    for (int i = 0; i < npacks; i++)
        if (idx_find(&packs[i], id, &off)) {
            pack_t *k = &packs[i];
            if (!k->data && !(k->data = (uint8_t *)get_file(k->path, &k->datan))) return 0;
            return pack_entry(k->data, k->datan, off, type, len, 0);
        }
    for (int i = 0; i < cur.cnt; i++)
        if (cur.done[i] && !memcmp(cur.ids[i], id, 20)) return pack_entry(cur.pk, cur.n, cur.offs[i], type, len, 0);
    return 0;
}

int obj_has(const oid_t id)
{
    char path[64];
    siefs_stat_t st;
    size_t off;
    loose_path(path, id);
    if (!fs_stat(path, &st, 0)) return 1;
    load_packs();
    for (int i = 0; i < npacks; i++) if (idx_find(&packs[i], id, &off)) return 1;
    return 0;
}

/* Objects whose name starts with these hex digits -> how many (the first in id). */
static int abbrev(const char *s, oid_t id)
{
    size_t l = strlen(s);
    int found = 0;
    char h[41], dir[32];
    load_packs();
    for (int i = 0; i < npacks; i++)
        for (uint32_t k = 0; k < packs[i].n; k++) {
            hex(h, packs[i].idx + 8 + 1024 + 20 * k);
            if (!memcmp(h, s, l) && (!found || memcmp(id, packs[i].idx + 8 + 1024 + 20 * k, 20))) { found++; memcpy(id, packs[i].idx + 8 + 1024 + 20 * k, 20); }
        }
    sfmt(dir, sizeof dir, ".git/objects/%c%c", s[0], s[1]);
    long d = fs_open(dir, FS_RDONLY | FS_DIRECTORY, 0);
    if (d < 0) return found;
    static char buf[4096];
    char name[256];
    uint64_t cs = 0, ino;
    long n;
    int type;
    while ((n = fs_readdir(d, &cs, buf, sizeof buf)) > 0)
        for (const char *p = buf; fs_dirent(&p, buf + n, &ino, &type, name); ) {
            h[0] = s[0]; h[1] = s[1];
            strlcpy(h + 2, name, 39);
            oid_t x;
            if (!memcmp(h, s, l) && !unhex(x, h) && (!found || memcmp(id, x, 20))) { found++; memcpy(id, x, 20); }
        }
    fs_close(d);
    return found;
}

/* ---- a pack received: name every object (deltas resolved, against this
 * pack or our objects), add the objects a "thin" pack's deltas use but do
 * not carry, then write it and its index. */
static int id_cmp(const void *a, const void *b) { return memcmp(a, b, 20); }

int pack_store(buf_t *pb, int *count)
{
    uint8_t *pk = (uint8_t *)pb->p;
    size_t n = pb->n;
    oid_t sum;
    if (n < 32 || memcmp(pk, "PACK", 4) || (be32(pk + 4) != 2 && be32(pk + 4) != 3)) return fail("not a pack");
    sha1(sum, pk, n - 20);
    if (memcmp(sum, pk + n - 20, 20)) return fail("the pack's checksum is wrong");
    int cnt = (int)be32(pk + 8), extra = 0, left = 0;
    cur.ids = malloc(20 * (size_t)cnt + 20); cur.offs = malloc(sizeof(size_t) * ((size_t)cnt + 1));
    cur.done = malloc((size_t)cnt + 1);
    size_t *ends = malloc(sizeof(size_t) * ((size_t)cnt + 1)), off = 12;
    if (!cur.ids || !cur.offs || !cur.done || !ends) return fail("out of memory");
    cur.pk = pk; cur.n = n; cur.cnt = cnt;
    for (int i = 0; i < cnt; i++) {                  /* whole objects: named now */
        raw_t r;
        cur.offs[i] = off;
        if (entry_raw(pk, n, off, &r)) { cur.cnt = 0; return fail("bad pack entry %d", i); }
        cur.done[i] = r.type != OBJ_OFS && r.type != OBJ_REF;
        if (cur.done[i]) obj_hash(r.type, r.data, r.size, cur.ids[i]); else left++;
        free(r.data);
        ends[i] = off = r.end;
    }
    if (off != n - 20) { cur.cnt = 0; return fail("bad pack: %lu bytes too many", (unsigned long)(n - 20 - off)); }
    for (int progress = 1; left && progress; ) {     /* deltas: until all are named */
        progress = 0;
        for (int i = 0; i < cnt; i++) {
            if (cur.done[i]) continue;
            int t;
            size_t l;
            uint8_t *o = pack_entry(pk, n, cur.offs[i], &t, &l, 0);
            if (!o) continue;
            obj_hash(t, o, l, cur.ids[i]);
            free(o);
            cur.done[i] = 1; left--; progress = 1;
        }
    }
    if (left) { cur.cnt = 0; return fail("%d objects of the pack cannot be built (missing bases)", left); }
    set_t in = { 0 }, add = { 0 };
    for (int i = 0; i < cnt; i++) set_add(&in, cur.ids[i]);
    for (int i = 0; i < cnt; i++) {                  /* a thin pack: bases we have, not in it */
        size_t p = cur.offs[i];
        while (pk[p] & 0x80) p++;                    /* (past the type and size) */
        if ((pk[cur.offs[i]] >> 4 & 7) == OBJ_REF && !set_has(&in, pk + p + 1)) set_add(&add, pk + p + 1);
    }
    cur.cnt = 0;
    for (int i = 0; i < add.cap; i++) {
        if (!memcmp(add.id[i], zero, 20)) continue;
        int t;
        size_t l;
        uint8_t *o = obj_read(add.id[i], &t, &l), hdr[16];
        if (!o) return fail("a delta's base is missing");
        size_t hn = 0;
        hdr[hn++] = (uint8_t)(t << 4 | (l & 15) | (l > 15 ? 0x80 : 0));
        for (size_t v = l >> 4; v; v >>= 7) hdr[hn++] = (uint8_t)((v & 127) | (v > 127 ? 0x80 : 0));
        uint8_t *z = malloc(ZBOUND(l));
        size_t zn = zdeflate(o, l, z);
        cur.ids = realloc(cur.ids, 20 * (size_t)(cnt + extra + 1));
        cur.offs = realloc(cur.offs, sizeof(size_t) * (size_t)(cnt + extra + 1));
        ends = realloc(ends, sizeof(size_t) * (size_t)(cnt + extra + 1));
        pb->n -= 20;
        cur.offs[cnt + extra] = pb->n;
        bput(pb, hdr, hn); bput(pb, z, zn);
        ends[cnt + extra] = pb->n;
        memcpy(cur.ids[cnt + extra], add.id[i], 20);
        bput(pb, zero, 20);                          /* (the checksum, again below) */
        extra++;
        free(o); free(z);
    }
    pk = (uint8_t *)pb->p; n = pb->n;
    if (extra) {
        wbe32(pk + 8, (uint32_t)(cnt + extra));
        sha1(pk + n - 20, pk, n - 20);
        outf("a thin pack: %d delta base(s) of ours added to it\n", extra);
    }
    cnt += extra;
    /* the index: names sorted (each with its place), fan-out, CRCs, offsets */
    uint8_t *ord = malloc((size_t)cnt * 24);
    for (int i = 0; i < cnt; i++) { memcpy(ord + 24 * i, cur.ids[i], 20); wbe32(ord + 24 * i + 20, (uint32_t)i); }
    sort(ord, cnt, 24, id_cmp);
    buf_t x = { 0 };
    uint8_t w[4];
    bput(&x, "\377tOc\0\0\0\2", 8);
    for (int b = 0, i = 0; b < 256; b++) { while (i < cnt && ord[24 * i] <= b) i++; wbe32(w, (uint32_t)i); bput(&x, w, 4); }
    for (int i = 0; i < cnt; i++) bput(&x, ord + 24 * i, 20);
    for (int i = 0; i < cnt; i++) { int k = (int)be32(ord + 24 * i + 20); wbe32(w, crc32(0, pk + cur.offs[k], ends[k] - cur.offs[k])); bput(&x, w, 4); }
    for (int i = 0; i < cnt; i++) {
        size_t o = cur.offs[be32(ord + 24 * i + 20)];
        if (o >> 31) return fail("packs over 2 GiB are not supported");
        wbe32(w, (uint32_t)o); bput(&x, w, 4);
    }
    bput(&x, pk + n - 20, 20);
    oid_t xs;
    sha1(xs, x.p, x.n);
    bput(&x, xs, 20);
    char h[41], path[96];
    hex(h, pk + n - 20);
    sfmt(path, sizeof path, ".git/objects/pack/pack-%s.pack", h);
    int e = put_file(path, pk, n, 0644);
    sfmt(path, sizeof path, ".git/objects/pack/pack-%s.idx", h);
    if (!e) e = put_file(path, x.p, x.n, 0644);
    free(ord); free(x.p); free(ends); free(cur.ids); free(cur.offs); free(cur.done); free(in.id); free(add.id);
    cur.ids = 0;
    odb_reload();
    *count = cnt;
    return e ? fail("cannot write %s", path) : 0;
}

/* ---- sets of names: open addressing, the all-zero name marks a free slot */
static unsigned slot(const set_t *s, const oid_t id) { return (unsigned)be32(id) & (unsigned)(s->cap - 1); }
int set_has(const set_t *s, const oid_t id)
{
    if (!s->cap) return 0;
    for (unsigned i = slot(s, id); ; i = (i + 1) & (unsigned)(s->cap - 1)) {
        if (!memcmp(s->id[i], id, 20)) return 1;
        if (!memcmp(s->id[i], zero, 20)) return 0;
    }
}
int set_add(set_t *s, const oid_t id)
{
    if (set_has(s, id)) return 0;
    if (2 * (s->n + 1) > s->cap) {                   /* grow: twice the slots, everything again */
        int cap = s->cap ? 2 * s->cap : 64;
        set_t g = { zalloc(20 * (size_t)cap), 0, cap };
        for (int i = 0; i < s->cap; i++) if (memcmp(s->id[i], zero, 20)) set_add(&g, s->id[i]);
        free(s->id);
        *s = g;
    }
    unsigned i = slot(s, id);
    while (memcmp(s->id[i], zero, 20)) i = (i + 1) & (unsigned)(s->cap - 1);
    memcpy(s->id[i], id, 20);
    s->n++;
    return 1;
}

/* ---- entries, sorted by path */
static int ents_at(const ents_t *l, const char *path, int *found)
{
    int lo = 0, hi = l->n;
    while (lo < hi) {
        int m = (lo + hi) / 2, c = strcmp(l->e[m].path, path);
        if (!c) { *found = 1; return m; }
        if (c < 0) lo = m + 1; else hi = m;
    }
    *found = 0;
    return lo;
}
ent_t *ents_find(const ents_t *l, const char *path)
{
    int f, i = ents_at(l, path, &f);
    return f ? &l->e[i] : 0;
}
ent_t *ents_put(ents_t *l, const char *path)
{
    int f, i = ents_at(l, path, &f);
    if (f) return &l->e[i];
    if (l->n == l->cap) { l->cap = l->cap ? 2 * l->cap : 64; l->e = realloc(l->e, sizeof(ent_t) * (size_t)l->cap); }
    memmove(l->e + i + 1, l->e + i, sizeof(ent_t) * (size_t)(l->n - i));
    l->n++;
    memset(&l->e[i], 0, sizeof(ent_t));
    size_t pl = strlen(path) + 1;
    l->e[i].path = malloc(pl);
    memcpy(l->e[i].path, path, pl);
    return &l->e[i];
}
void ents_del(ents_t *l, ent_t *e)
{
    free(e->path);
    memmove(e, e + 1, sizeof(ent_t) * (size_t)(l->e + l->n - e - 1));
    l->n--;
}

/* ---- trees */
static int flat(const oid_t t, const char *pre, ents_t *o)
{
    int type;
    size_t n;
    uint8_t *d = obj_read(t, &type, &n);
    if (!d || type != OBJ_TREE) { free(d); return -1; }
    for (const uint8_t *p = d; p < d + n; ) {
        uint32_t mode = 0;
        for (; *p >= '0' && *p <= '7'; p++) mode = mode * 8 + (uint32_t)(*p - '0');
        const char *name = (const char *)++p;
        p += strlen(name) + 1;
        if (p + 20 > d + n) break;
        char path[FS_PATH];
        sfmt(path, sizeof path, "%s%s", pre, name);
        if (mode == 040000) {
            strlcpy(path + strlen(path), "/", 2);
            if (flat(p, path, o)) { free(d); return -1; }
        } else { ent_t *e = ents_put(o, path); e->mode = mode; memcpy(e->id, p, 20); }
        p += 20;
    }
    free(d);
    return 0;
}
int tree_flat(const oid_t tree, ents_t *out) { return flat(tree, "", out); }

/* The files of l from *i on whose path starts with pre (pl bytes): one tree.
 * (Sorted paths are in the order git wants for a tree's entries.) */
static int wtree(const ents_t *l, int *i, const char *pre, size_t pl, oid_t out)
{
    buf_t b = { 0 };
    while (*i < l->n && strlen(l->e[*i].path) > pl && !memcmp(l->e[*i].path, pre, pl)) {
        const char *name = l->e[*i].path + pl, *sl = strchr(name, '/');
        char m[16];
        if (sl) {
            oid_t sub;
            if (wtree(l, i, l->e[*i].path, (size_t)(sl - l->e[*i].path) + 1, sub)) return -1;
            bput(&b, "40000 ", 6); bput(&b, name, (size_t)(sl - name)); bput(&b, "", 1); bput(&b, sub, 20);
        } else {
            bput(&b, m, (size_t)sfmt(m, sizeof m, "%o ", l->e[*i].mode));
            bput(&b, name, strlen(name) + 1); bput(&b, l->e[*i].id, 20);
            (*i)++;
        }
    }
    int e = obj_write(OBJ_TREE, b.p, b.n, out);
    free(b.p);
    return e;
}
int tree_write(const ents_t *l, oid_t out) { int i = 0; return wtree(l, &i, "", 0, out); }

/* ---- the index (v2; v3's extended flags read too) */
int index_read(ents_t *ix)
{
    size_t n;
    uint8_t *d = (uint8_t *)get_file(".git/index", &n), *p, sum[20];
    if (!d) return 0;                                /* (none yet: empty) */
    if (n < 32 || memcmp(d, "DIRC", 4) || be32(d + 4) < 2 || be32(d + 4) > 3) { free(d); return fail("unsupported index (version 2 and 3 only)"); }
    sha1(sum, d, n - 20);
    if (memcmp(sum, d + n - 20, 20)) { free(d); return fail("the index's checksum is wrong"); }
    uint32_t cnt = be32(d + 8);
    p = d + 12;
    for (uint32_t i = 0; i < cnt; i++) {
        if (p + 64 > d + n) break;
        uint16_t fl = (uint16_t)(p[60] << 8 | p[61]);
        const char *name = (const char *)p + 62 + (fl & 0x4000 ? 2 : 0);
        size_t len = 62 + (fl & 0x4000 ? 2 : 0) + strlen(name);
        if (!(fl & 0x3000)) {                        /* (merge stages: not ours) */
            ent_t *e = ents_put(ix, name);
            e->mtime = (int64_t)be32(p + 8) * 1000000000 + be32(p + 12);
            e->mode = be32(p + 24); e->size = be32(p + 36);
            memcpy(e->id, p + 40, 20);
        }
        p += (len + 8) & ~(size_t)7;
    }
    free(d);
    return 0;
}

int index_write(const ents_t *ix)
{
    buf_t b = { 0 };
    uint8_t h[62], sum[20];
    memcpy(h, "DIRC\0\0\0\2", 8); wbe32(h + 8, (uint32_t)ix->n);
    bput(&b, h, 12);
    for (int i = 0; i < ix->n; i++) {
        const ent_t *e = &ix->e[i];
        size_t nl = strlen(e->path);
        memset(h, 0, sizeof h);
        wbe32(h, (uint32_t)(e->mtime / 1000000000)); wbe32(h + 4, (uint32_t)(e->mtime % 1000000000));
        memcpy(h + 8, h, 8);                         /* ctime = mtime */
        wbe32(h + 24, e->mode); wbe32(h + 36, e->size);
        memcpy(h + 40, e->id, 20);
        h[60] = (uint8_t)((nl < 0xfff ? nl : 0xfff) >> 8); h[61] = (uint8_t)(nl < 0xfff ? nl : 0xfff);
        bput(&b, h, 62);
        bput(&b, e->path, nl);
        bput(&b, "\0\0\0\0\0\0\0\0", 8 - (62 + nl) % 8);
    }
    sha1(sum, b.p, b.n);
    bput(&b, sum, 20);
    int e = put_file(".git/index", b.p, b.n, 0644);
    free(b.p);
    return e;
}

/* ---- commits */
int commit_read(const oid_t id, commit_t *c)
{
    int type;
    size_t n;
    memset(c, 0, sizeof *c);
    char *d = (char *)obj_read(id, &type, &n);
    if (!d || type != OBJ_COMMIT) { free(d); return -1; }
    c->raw = (uint8_t *)d;
    for (char *p = d; *p && *p != '\n'; ) {          /* the header lines, to the empty one */
        char *e = strchr(p, '\n');
        if (!e) break;
        *e = 0;
        if (!memcmp(p, "tree ", 5)) unhex(c->tree, p + 5);
        else if (!memcmp(p, "parent ", 7) && c->np < 2) unhex(c->parent[c->np++], p + 7);
        else if (!memcmp(p, "author ", 7)) c->author = p + 7;
        else if (!memcmp(p, "committer ", 10)) {
            c->committer = p + 10;
            char *gt = e;
            while (gt > p && *gt != '>') gt--;
            for (gt += 2; *gt >= '0' && *gt <= '9'; gt++) c->time = c->time * 10 + *gt - '0';
        }
        p = e + 1;
        if (*p == '\n') { c->msg = p + 1; break; }
    }
    if (!c->msg) c->msg = "";
    return 0;
}

int commit_tree(const oid_t id, oid_t tree)
{
    commit_t c;
    if (commit_read(id, &c)) return -1;
    memcpy(tree, c.tree, 20);
    free(c.raw);
    return 0;
}

/* Breadth first from `from` (a queue of names in a buffer): the first
 * commit in `stop` -> 0 (in found), else -1; every commit seen goes in seen. */
int walk(const oid_t from, const set_t *stop, set_t *seen, oid_t found)
{
    buf_t q = { 0 };
    int r = -1;
    set_add(seen, from);
    bput(&q, from, 20);
    for (size_t h = 0; h < q.n && r; h += 20) {
        const uint8_t *id = (uint8_t *)q.p + h;
        commit_t c;
        if (stop && set_has(stop, id)) { memcpy(found, id, 20); r = 0; continue; }
        if (commit_read(id, &c)) continue;
        for (int i = 0; i < c.np; i++) if (set_add(seen, c.parent[i])) bput(&q, c.parent[i], 20);
        free(c.raw);
    }
    free(q.p);
    return r;
}
int merge_base(const oid_t a, const oid_t b, oid_t base)
{
    set_t sa = { 0 }, sb = { 0 };
    walk(a, 0, &sa, base);
    int r = walk(b, &sa, &sb, base);
    free(sa.id); free(sb.id);
    return r;
}
int is_ancestor(const oid_t a, const oid_t b)
{
    oid_t m;
    return !merge_base(b, a, m) && !memcmp(m, a, 20);
}

/* ---- refs */
static int ref_get(const char *name, oid_t id, int depth)
{
    char path[FS_PATH];
    sfmt(path, sizeof path, ".git/%s", name);
    char *s = get_file(path, 0);
    if (s) {
        char *e = strchr(s, '\n');
        if (e) *e = 0;
        int r = !memcmp(s, "ref: ", 5) ? depth < 5 ? ref_get(s + 5, id, depth + 1) : -1 : unhex(id, s);
        free(s);
        return r;
    }
    s = get_file(".git/packed-refs", 0);
    int r = -1;
    size_t nl = strlen(name);
    for (char *p = s; p && *p && r; ) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        if (strlen(p) == 41 + nl && p[40] == ' ' && !strcmp(p + 41, name)) r = unhex(id, p);
        p = e ? e + 1 : p + strlen(p);
    }
    free(s);
    return r;
}
int ref_read(const char *name, oid_t id) { return ref_get(name, id, 0); }

const char *head_ref(void)
{
    static char r[256];
    char *s = get_file(".git/HEAD", 0);
    int sym = s && !memcmp(s, "ref: ", 5);
    if (sym) { strlcpy(r, s + 5, sizeof r); char *e = strchr(r, '\n'); if (e) *e = 0; }
    free(s);
    return sym ? r : 0;
}

int ref_write(const char *name, const oid_t id)
{
    char path[FS_PATH], h[42];
    if (!strcmp(name, "HEAD") && head_ref()) name = head_ref();
    sfmt(path, sizeof path, ".git/%s", name);
    hex(h, id);
    h[40] = '\n';
    return put_file(path, h, 41, 0644);
}

int ref_delete(const char *name)
{
    char path[FS_PATH];
    sfmt(path, sizeof path, ".git/%s", name);
    int gone = !fs_unlink(path);
    size_t n, nl = strlen(name);
    char *s = get_file(".git/packed-refs", &n);
    if (!s) return gone ? 0 : -1;
    buf_t b = { 0 };
    for (char *p = s; *p; ) {                        /* packed-refs again, without it (and its peeled line) */
        char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) + 1 : strlen(p);
        int it = l >= 41 + nl && p[40] == ' ' && !memcmp(p + 41, name, nl) && (p[41 + nl] == '\n' || !p[41 + nl]);
        if (it) { gone = 1; p += l; if (*p == '^') { e = strchr(p, '\n'); p = e ? e + 1 : p + strlen(p); } continue; }
        bput(&b, p, l);
        p += l;
    }
    if (b.n != n) put_file(".git/packed-refs", b.p, b.n, 0644);
    free(s); free(b.p);
    return gone ? 0 : -1;
}

static void ref_dir(const char *dir, ents_t *l)
{
    long h = fs_open(dir, FS_RDONLY | FS_DIRECTORY, 0);
    if (h < 0) return;
    char *buf = malloc(4096), name[256], path[FS_PATH];
    uint64_t cs = 0, ino;
    long n;
    int type;
    while ((n = fs_readdir(h, &cs, buf, 4096)) > 0)
        for (const char *p = buf; fs_dirent(&p, buf + n, &ino, &type, name); ) {
            if (name[0] == '.') continue;            /* ("." and "..") */
            sfmt(path, sizeof path, "%s/%s", dir, name);
            if (type == 4) ref_dir(path, l);
            else { oid_t id; if (!ref_read(path + 5, id)) memcpy(ents_put(l, path + 5)->id, id, 20); }
        }
    free(buf);
    fs_close(h);
}

void ref_each(const char *prefix, ref_fn *fn, void *ctx)
{
    ents_t l = { 0 };
    char *s = get_file(".git/packed-refs", 0);
    for (char *p = s; p && *p; ) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        if (strlen(p) > 41 && p[40] == ' ') unhex(ents_put(&l, p + 41)->id, p);
        p = e ? e + 1 : p + strlen(p);
    }
    free(s);
    char dir[FS_PATH];
    sfmt(dir, sizeof dir, ".git/%s", prefix);
    if (dir[strlen(dir) - 1] == '/') dir[strlen(dir) - 1] = 0;
    ref_dir(dir, &l);
    size_t pl = strlen(prefix);
    for (int i = 0; i < l.n; i++) if (!memcmp(l.e[i].path, prefix, pl)) fn(l.e[i].path, l.e[i].id, ctx);
    for (int i = 0; i < l.n; i++) free(l.e[i].path);
    free(l.e);
}

/* A name for a commit: HEAD, a branch, a tag, a remote's branch, a hash or
 * its first digits; then ~N (N first parents back) or ^ / ^2 (a parent). */
int rev_parse(const char *s, oid_t id)
{
    char base[256];
    size_t bl = 0;
    while (s[bl] && s[bl] != '~' && s[bl] != '^') bl++;
    if (bl >= sizeof base) return -1;
    memcpy(base, s, bl);
    base[bl] = 0;
    static const char *const pre[] = { "", "refs/", "refs/heads/", "refs/tags/", "refs/remotes/" };
    int ok = 0;
    char ref[300];
    if (!strcmp(base, "HEAD") || !strcmp(base, "@")) ok = !ref_read("HEAD", id);
    for (int i = 0; !ok && i < 5 && strcmp(base, "@"); i++) { sfmt(ref, sizeof ref, "%s%s", pre[i], base); ok = !ref_read(ref, id); }
    if (!ok) { sfmt(ref, sizeof ref, "refs/remotes/%s/HEAD", base); ok = !ref_read(ref, id); }
    if (!ok && bl >= 4) {
        int h = 1;
        for (size_t i = 0; i < bl; i++) h &= (base[i] >= '0' && base[i] <= '9') || (base[i] >= 'a' && base[i] <= 'f');
        ok = h && (bl == 40 ? !unhex(id, base) : abbrev(base, id) == 1);
    }
    if (!ok) return -1;
    for (int peel = 0; peel < 5; peel++) {           /* an annotated tag: the commit it names */
        int t;
        size_t n;
        uint8_t *d = obj_read(id, &t, &n);
        int tag = d && t == OBJ_TAG && !memcmp(d, "object ", 7) && !unhex(id, (char *)d + 7);
        free(d);
        if (!tag) break;
    }
    for (const char *p = s + bl; *p; ) {
        char op = *p++;
        long k = 0;
        int digits = 0;
        for (; *p >= '0' && *p <= '9'; p++, digits++) k = k * 10 + *p - '0';
        if (!digits) k = 1;
        for (long j = 0; j < (op == '~' ? k : 1); j++) {
            commit_t c;
            int which = op == '^' ? (int)k - 1 : 0;
            if (op == '^' && !k) break;
            if (commit_read(id, &c)) return -1;
            int has = which < c.np;
            if (has) memcpy(id, c.parent[which], 20);
            free(c.raw);
            if (!has) return -1;
        }
    }
    return 0;
}
