/*
 * check.c - siefs_check: verify a whole SieFS volume.
 *
 * It walks both trees from the superblock down. Reading a node verifies
 * its checksum; on top of that it checks:
 *   - every node: right tree, right level, keys in order, items inside the
 *     block, entry keys equal to their child's first key;
 *   - every block is used once, and the bitmap says "in use" exactly for
 *     the blocks the trees reach (anything else is damage or a leak);
 *   - every object: link count = number of names (directories: 2 + their
 *     subdirectories), a parent that agrees, contents consistent with its
 *     size, every name pointing at an existing object;
 *   - the attribute index matches the attributes, both ways;
 *   - with data != 0, every file block against its checksum.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdarg.h>
#include "siefs_int.h"

void vformat(void (*put)(void *, char), void *ctx, const char *fmt, va_list ap);   /* lib/fmt.c */
#define ISDIR_M(m) (((m) & SIEFS_IFMT) == SIEFS_IFDIR)

typedef struct {
    uint64_t ino, parent, dirparent, size, end;   /* end: past the last extent block */
    uint32_t mode, nlink, links, subdirs, flags, dtype;
    int seen, inl;
} obj_t;

typedef struct {
    siefs_t *fs;
    uint8_t *reach;                 /* blocks reached by the walk */
    obj_t *tab;                     /* objects, by ino (open addressing) */
    uint64_t cap, nobj, maxino;
    long errs;
    int data;
    void (*report)(void *, const char *);
    void *ctx;
    char msg[200];
    size_t mlen;
    uint8_t *blk;                   /* one block, for data checks */
} ck_t;

static void mput(void *c, char ch) { ck_t *k = c; if (k->mlen < sizeof k->msg - 1) k->msg[k->mlen++] = ch; }

static void bad(ck_t *c, const char *fmt, ...)
{
    va_list ap;
    c->errs++;
    if (!c->report) return;
    c->mlen = 0;
    va_start(ap, fmt);
    vformat(mput, c, fmt, ap);
    va_end(ap);
    c->msg[c->mlen] = 0;
    c->report(c->ctx, c->msg);
}

static obj_t *obj(ck_t *c, uint64_t ino)
{
    uint64_t i = ino * 0x9E3779B97F4A7C15ULL & (c->cap - 1);
    for (uint64_t tries = 0; tries < c->cap; tries++, i = (i + 1) & (c->cap - 1)) {
        if (c->tab[i].ino == ino) return &c->tab[i];
        if (!c->tab[i].ino) { c->tab[i].ino = ino; c->nobj++; return &c->tab[i]; }
    }
    return 0;
}

static void use(ck_t *c, uint64_t b, uint64_t n, const char *what)
{
    for (uint64_t i = b; i < b + n; i++) {
        if (i < c->fs->sb.data_start || i >= c->fs->sb.nblocks) { bad(c, "%s block %lu outside the data area", what, i); continue; }
        if (bit(c->reach, i)) bad(c, "%s block %lu is used twice", what, i);
        c->reach[i >> 3] |= 1 << (i & 7);
    }
}

/* One item of the file tree. */
static void item(ck_t *c, skey_t k, const uint8_t *v, unsigned len)
{
    siefs_t *fs = c->fs;
    int ty = KTYPE(k.off);
    uint64_t off = k.off & OFFMASK;
    obj_t *o;
    if (k.id & IDX_BIT) {
        if (ty != T_XINDEX || len) { bad(c, "bad index item %lx", k.id); return; }
        uint8_t b[MAXITEM];
        int n, found = 0;
        path_t pa;
        for (int r = t_seek(fs, &fs->ft, K(off, T_XATTR, 0), &pa); r > 0 && !found; r = t_next(fs, &pa)) {
            if (pkey(&pa).id != off || KTYPE(pkey(&pa).off) != T_XATTR) break;
            n = (int)plen(&pa);
            memcpy(b, pval(&pa), n);
            for (int p = 0; p + 3 <= n; p += 3 + b[p] + (b[p + 1] | b[p + 2] << 8))
                if (xindex_id((char *)b + p + 3, b[p], b + p + 3 + b[p], b[p + 1] | b[p + 2] << 8) == k.id) found = 1;
        }
        if (!found) bad(c, "index entry %lx for object %lu has no matching attribute", k.id, off);
        return;
    }
    if (!(o = obj(c, k.id))) { bad(c, "too many objects to check"); return; }
    if (k.id > c->maxino) c->maxino = k.id;
    switch (ty) {
    case T_INODE: {
        inode_t in;
        if (len != sizeof in || off) { bad(c, "object %lu: bad inode item", k.id); return; }
        memcpy(&in, v, len);
        o->seen = 1; o->mode = in.mode; o->nlink = in.nlink; o->parent = in.parent; o->size = in.size; o->flags = (uint32_t)in.flags;
        uint32_t t = in.mode & SIEFS_IFMT;
        if (t != SIEFS_IFDIR && t != SIEFS_IFREG && t != SIEFS_IFLNK) bad(c, "object %lu: unknown type %o", k.id, in.mode);
        break;
    }
    case T_INLINE:
        if (!o->seen || !(o->flags & F_INLINE)) bad(c, "object %lu: unexpected inline data", k.id);
        else if (len != o->size || off) bad(c, "object %lu: inline data of %u bytes, size %lu", k.id, len, o->size);
        o->inl = 1;
        break;
    case T_EXTENT: {
        extent_t x;
        if (len < 16 || len > sizeof x) { bad(c, "object %lu: bad extent item", k.id); return; }
        memcpy(&x, v, len);
        if (!x.n || x.n > EXT_MAX || len != EXT_SIZE(x.n) || off % BS) { bad(c, "object %lu: bad extent at %lu", k.id, off); return; }
        uint64_t s = off / BS;
        if (!o->seen || (o->flags & F_INLINE)) bad(c, "object %lu: unexpected extent", k.id);
        if (s < o->end) bad(c, "object %lu: extents overlap at block %lu", k.id, s);
        o->end = s + x.n;
        if (o->end > (o->size + BS - 1) / BS) bad(c, "object %lu: extent past the end of the file", k.id);
        use(c, x.blk, x.n, "data");
        for (uint32_t i = 0; c->data && i < x.n; i++)
            if (dev_read(fs, x.blk + i, 1, c->blk) || siefs_crc32c(0, c->blk, BS) != x.csum[i])
                bad(c, "object %lu: data block %lu (file block %lu) is damaged", k.id, x.blk + i, s + i);
        break;
    }
    case T_DIRENT:
        if (!o->seen || !ISDIR_M(o->mode)) bad(c, "object %lu: names in something that is not a directory", k.id);
        for (unsigned p = 0; p < len; ) {
            uint64_t ino;
            if (p + 10 > len || p + 10 + v[p + 9] > len || !v[p + 9]) { bad(c, "directory %lu: damaged entry list", k.id); break; }
            memcpy(&ino, v + p, 8);
            if (name_hash((const char *)v + p + 10, v[p + 9]) != off) bad(c, "directory %lu: a name filed under the wrong hash", k.id);
            obj_t *t = obj(c, ino);
            if (!t) { bad(c, "too many objects to check"); return; }
            t->links++;
            t->dtype = v[p + 8];
            if (v[p + 8] == SIEFS_IFDIR >> 12) { t->dirparent = k.id; obj(c, k.id)->subdirs++; }
            p += 10 + v[p + 9];
        }
        break;
    case T_XATTR:
        for (unsigned p = 0; p < len; ) {
            unsigned nl = v[p], vl = v[p + 1] | v[p + 2] << 8;
            if (p + 3 + nl + vl > len || !nl) { bad(c, "object %lu: damaged attribute list", k.id); break; }
            if (name_hash((const char *)v + p + 3, nl) != off) bad(c, "object %lu: an attribute filed under the wrong hash", k.id);
            if (vl <= 255) {
                uint8_t dummy;
                if (t_get(fs, &fs->ft, K(xindex_id((const char *)v + p + 3, nl, v + p + 3 + nl, vl), T_XINDEX, k.id), &dummy, 0) < 0)
                    bad(c, "object %lu: attribute missing from the index", k.id);
            }
            p += 3 + nl + vl;
        }
        break;
    default:
        bad(c, "object %lu: unknown item type %d", k.id, ty);
    }
}

/* Walk a (sub)tree. first: the key the parent filed it under; limit: the
 * next sibling's key (all keys here must be below it). The node is copied,
 * so the cache can be trimmed as we go and memory stays small. */
static void walk(ck_t *c, uint64_t tree, const bptr_t *p, int level, const skey_t *first, const skey_t *limit)
{
    siefs_t *fs = c->fs;
    node_t *cn;
    if (t_get_node(fs, p, &cn)) { bad(c, "tree %lu: node at block %lu unreadable or damaged", tree, p->blk); return; }
    node_t *n = fs->env.alloc(sizeof *n);
    if (!n) { bad(c, "out of memory"); return; }
    memcpy(n->d, cn->d, BS);
    cache_trim(fs);
    if (!(p->blk & TMP)) use(c, p->blk, 1, "node");
    nhdr_t *h = NH(n);
    int cnt = h->n;
    if (level >= 0 && h->level != level) bad(c, "tree %lu: node %lu at the wrong level", tree, p->blk);
    if (h->tree != tree) bad(c, "node %lu belongs to tree %lu, found in %lu", p->blk, h->tree, tree);
    if (!cnt && (level >= 0 || h->level)) bad(c, "tree %lu: empty node %lu", tree, p->blk);
    if (h->level ? cnt > IMAX : cnt * sizeof(litem_t) + sizeof(nhdr_t) > h->dstart) { bad(c, "tree %lu: node %lu overfull", tree, p->blk); cnt = 0; }
    for (int i = 0; i < cnt; i++) {
        skey_t k = h->level ? IE(n, i)->k : LI(n, i)->k;
        if (i && kcmp(h->level ? IE(n, i - 1)->k : LI(n, i - 1)->k, k) >= 0) bad(c, "tree %lu: node %lu: keys out of order", tree, p->blk);
        if (!i && first && kcmp(*first, k)) bad(c, "tree %lu: node %lu: parent key differs from its first key", tree, p->blk);
        if (limit && kcmp(k, *limit) >= 0) bad(c, "tree %lu: node %lu: key beyond its parent's range", tree, p->blk);
        if (h->level) { walk(c, tree, &IE(n, i)->p, h->level - 1, &IE(n, i)->k, i + 1 < cnt ? &IE(n, i + 1)->k : limit); continue; }
        litem_t *it = LI(n, i);
        if (it->off < h->dstart || it->off + it->len > BS || it->len > MAXITEM) { bad(c, "tree %lu: node %lu: item %d (%u bytes at %u, data from %u) outside the block", tree, p->blk, i, it->len, it->off, h->dstart); continue; }
        if (tree == 1) item(c, k, n->d + it->off, it->len);
        else if (KTYPE(k.off) != T_VOLUME || it->len != sizeof(vol_t)) bad(c, "root tree: unexpected item");
    }
    fs->env.free(n);
}

long siefs_check(siefs_t *fs, int data, void (*report)(void *, const char *), void *ctx)
{
    ck_t c = { .fs = fs, .data = data, .report = report, .ctx = ctx };
    uint64_t nb = fs->sb.nblocks;
    c.cap = 1024;
    while (c.cap < 4 * (fs->vol.inodes + 16)) c.cap *= 2;
    c.reach = fs->env.alloc(fs->sb.nbm * BS);
    c.tab = fs->env.alloc(c.cap * sizeof *c.tab);
    c.blk = fs->env.alloc(BS);
    if (!c.reach || !c.tab || !c.blk) { bad(&c, "out of memory"); goto out; }
    memset(c.reach, 0, fs->sb.nbm * BS);
    memset(c.tab, 0, c.cap * sizeof *c.tab);
    for (uint64_t b = 0; b < fs->sb.nbm * BPB; b++)          /* the layout and the end: in use */
        if (b < fs->sb.data_start || b >= nb) c.reach[b >> 3] |= 1 << (b & 7);
    if (fs->ndirty && report) report(ctx, "note: changes not committed yet: checking memory, not the disk");

    walk(&c, 0, &fs->rt.root, -1, 0, 0);
    walk(&c, 1, &fs->ft.root, -1, 0, 0);

    uint64_t freed = 0, leaked = 0;                          /* the bitmap must match the trees */
    for (uint64_t b = 0; b < nb; b++) {
        int r = bit(c.reach, b), m = bit(fs->map, b);
        if (r && !m && freed++ < 10) bad(&c, "block %lu is in use but marked free", b);
        if (!r && m && leaked++ < 10) bad(&c, "block %lu is marked in use but unused (leak)", b);
    }
    if (freed > 10) bad(&c, "... %lu blocks in use but marked free in all", freed);
    if (leaked > 10) bad(&c, "... %lu leaked blocks in all", leaked);

    uint64_t seen = 0;
    for (uint64_t i = 0; i < c.cap; i++) {
        obj_t *o = &c.tab[i];
        if (!o->ino) continue;
        if (!o->seen) { bad(&c, "object %lu has a name but does not exist", o->ino); continue; }
        seen++;
        int dir = ISDIR_M(o->mode);
        uint32_t want = dir ? 2 + o->subdirs : o->links;
        if (o->ino == SIEFS_ROOT) { if (o->links) bad(&c, "the root directory has a name"); }
        else if (!o->links) bad(&c, "object %lu has no name (orphan)", o->ino);
        else if (o->dtype != o->mode >> 12) bad(&c, "object %lu: its directory entry has the wrong type", o->ino);
        if (o->nlink != want) bad(&c, "object %lu: link count %u, should be %u", o->ino, o->nlink, want);
        if (dir && o->links > 1) bad(&c, "directory %lu has %u names", o->ino, o->links);
        if (dir && o->ino != SIEFS_ROOT && o->links && o->parent != o->dirparent) bad(&c, "directory %lu: wrong parent", o->ino);
        if ((o->flags & F_INLINE) && !o->inl && o->size) bad(&c, "object %lu: inline data missing", o->ino);
        if (dir && o->size) bad(&c, "directory %lu has a size", o->ino);
    }
    if (seen != fs->vol.inodes) bad(&c, "the volume counts %lu objects, found %lu", fs->vol.inodes, seen);
    if (c.maxino >= fs->vol.next_id) bad(&c, "object %lu is beyond the next free id %lu", c.maxino, fs->vol.next_id);
out:
    fs->env.free(c.reach); fs->env.free(c.tab); fs->env.free(c.blk);
    return c.errs;
}
