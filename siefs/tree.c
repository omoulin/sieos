/*
 * tree.c - The copy-on-write B+tree that holds everything in SieFS.
 *
 * A tree is a root pointer. Reading walks down from it, checking each
 * node's checksum (held by its parent) the first time it is read. Changing
 * a node never touches its block: the node is copied in memory ("dirty"),
 * and so is every node above it, up to the root ("path copying"). Dirty
 * nodes have no block yet: they are named TMP | slot, an index in
 * fs->dirty. At commit, t_flush writes them bottom-up to new blocks, each
 * parent receiving its children's new places and checksums; the old blocks
 * were freed when the copies were made.
 *
 * Invariants: keys are sorted; every leaf is at the same depth; an internal
 * entry's key is the first key of its child (so a search for the item at or
 * just before a key ends in the right leaf).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "siefs_int.h"

/* ---- The cache of clean nodes: a hash table by block number, and a list
 * from most to least recently used. Dirty nodes are not in it. */
static uint32_t hslot(siefs_t *fs, uint64_t b) { return (uint32_t)(b * 0x9E3779B97F4A7C15ULL >> (64 - fs->hbits)); }
static void lru_unlink(node_t *n) { n->prev->next = n->next; n->next->prev = n->prev; }
static void lru_front(siefs_t *fs, node_t *n)
{
    n->next = fs->lru.next; n->prev = &fs->lru;
    n->next->prev = n; fs->lru.next = n;
}
static node_t *cache_find(siefs_t *fs, uint64_t b)
{
    for (node_t *n = fs->hash[hslot(fs, b)]; n; n = n->hnext)
        if (n->blk == b) { lru_unlink(n); lru_front(fs, n); return n; }
    return 0;
}
static void cache_add(siefs_t *fs, node_t *n)
{
    uint32_t h = hslot(fs, n->blk);
    n->hnext = fs->hash[h]; fs->hash[h] = n;
    lru_front(fs, n);
    fs->nclean++;
}
static void cache_del(siefs_t *fs, node_t *n)
{
    node_t **pp = &fs->hash[hslot(fs, n->blk)];
    while (*pp != n) pp = &(*pp)->hnext;
    *pp = n->hnext;
    lru_unlink(n);
    fs->nclean--;
}
/* Drop the least recently used clean nodes beyond the cache size. Called
 * only between operations: during one, paths point at cached nodes. */
void cache_trim(siefs_t *fs)
{
    while (fs->nclean > fs->env.cache_nodes) {
        node_t *n = fs->lru.prev;
        cache_del(fs, n);
        fs->env.free(n);
    }
}

void nodes_free(siefs_t *fs)
{
    fs->env.cache_nodes = 0;
    cache_trim(fs);
    for (uint32_t i = 0; i < fs->dcap; i++) if (fs->dirty[i]) fs->env.free(fs->dirty[i]);
}

/* ---- Dirty nodes: a slot each in fs->dirty. */
static int dslot(siefs_t *fs, node_t *n)
{
    if (fs->ndirty == fs->dcap) {                       /* full: double the table */
        uint32_t cap = fs->dcap ? fs->dcap * 2 : 64;
        node_t **d = fs->env.alloc(cap * sizeof *d);
        if (!d) return E(ENOMEM);
        memset(d, 0, cap * sizeof *d);
        if (fs->dcap) memcpy(d, fs->dirty, fs->dcap * sizeof *d);
        fs->env.free(fs->dirty);
        fs->dirty = d; fs->dcap = cap;
    }
    while (fs->dirty[fs->dnext]) fs->dnext = (fs->dnext + 1) % fs->dcap;
    fs->dirty[fs->dnext] = n;
    fs->ndirty++;
    return (int)fs->dnext;
}

static void changed(siefs_t *fs)
{
    fs->opmod = 1;
    if (!fs->changed) { fs->changed = 1; fs->first_change = fs->env.now(); }
}

static node_t *nnew(siefs_t *fs, int level, uint64_t tree)
{
    node_t *n = fs->env.alloc(sizeof *n);
    int s;
    if (!n) return 0;
    if ((s = dslot(fs, n)) < 0) { fs->env.free(n); return 0; }
    memset(n->d, 0, BS);
    n->dirty = 1; n->blk = TMP | (uint64_t)s;
    *NH(n) = (nhdr_t){ .magic = NODE_MAGIC, .level = (uint16_t)level, .dstart = BS, .tree = tree };
    changed(fs);
    return n;
}

static void ndrop(siefs_t *fs, node_t *n)               /* a dirty node is no longer needed */
{
    fs->dirty[n->blk & ~TMP] = 0;
    fs->ndirty--;
    fs->env.free(n);
}

/* The node a pointer designates: dirty, cached, or read and checked. */
int t_get_node(siefs_t *fs, const bptr_t *p, node_t **out)
{
    node_t *n;
    if (p->blk & TMP) { *out = fs->dirty[p->blk & ~TMP]; return 0; }
    if ((n = cache_find(fs, p->blk))) { *out = n; return 0; }
    if (p->blk >= fs->sb.nblocks || !(n = fs->env.alloc(sizeof *n))) return p->blk >= fs->sb.nblocks ? E(EIO) : E(ENOMEM);
    if (dev_read(fs, p->blk, 1, n->d) || siefs_crc32c(0, n->d, BS) != p->csum ||
        NH(n)->magic != NODE_MAGIC || NH(n)->gen != p->gen || NH(n)->level >= MAXH) {
        fs->env.free(n);
        return E(EIO);                                  /* unreadable, or not what the parent wrote */
    }
    n->blk = p->blk; n->dirty = 0;
    cache_add(fs, n);
    *out = n;
    return 0;
}

/* Copy-on-write: make node n changeable. ref is the pointer to it (in its
 * parent, already dirty, or the tree's root): it now names the copy. The
 * node's old block will be free once this transaction is committed. */
static int make_dirty(siefs_t *fs, node_t *n, bptr_t *ref)
{
    int s;
    if (n->dirty) return 0;
    if ((s = dslot(fs, n)) < 0) return s;
    cache_del(fs, n);
    blk_free(fs, n->blk, 1);
    n->dirty = 1; n->blk = TMP | (uint64_t)s;
    ref->blk = n->blk;
    changed(fs);
    return 0;
}

/* ---- Searching. */
static int ifind(node_t *n, skey_t k)        /* internal: the last entry with key <= k (or 0) */
{
    int lo = 0, hi = NH(n)->n - 1, r = 0;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (kcmp(IE(n, m)->k, k) <= 0) { r = m; lo = m + 1; } else hi = m - 1;
    }
    return r;
}
static int lfind(node_t *n, skey_t k, int *found)   /* leaf: the first item with key >= k */
{
    int lo = 0, hi = NH(n)->n;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (kcmp(LI(n, m)->k, k) < 0) lo = m + 1; else hi = m;
    }
    *found = lo < NH(n)->n && !kcmp(LI(n, lo)->k, k);
    return lo;
}

/* Walk down to the leaf where key k is or would be, filling the path.
 * With cow, every node on the way is made dirty first. Returns 1 if k is
 * there (at pa->i[0]), 0 if not (pa->i[0] is where it would go). */
static int search(siefs_t *fs, tree_t *t, skey_t k, path_t *pa, int cow)
{
    bptr_t *ref = &t->root;
    node_t *n;
    int e, found;
    if ((e = t_get_node(fs, ref, &n))) return e;
    int lv = NH(n)->level;
    pa->h = lv + 1;
    for (;;) {
        if (cow && (e = make_dirty(fs, n, ref))) return e;
        pa->n[lv] = n;
        if (!lv) break;
        if (!NH(n)->n) return E(EIO);
        pa->i[lv] = ifind(n, k);
        ref = &IE(n, pa->i[lv])->p;
        if ((e = t_get_node(fs, ref, &n))) return e;
        if (NH(n)->level != --lv) return E(EIO);
    }
    pa->i[0] = lfind(n, k, &found);
    return found;
}

static skey_t first(node_t *n) { return NH(n)->level ? IE(n, 0)->k : LI(n, 0)->k; }

/* The first key of the node at level lv changed: update the entry keys
 * above it, as far as it is the first child. */
static void fix_keys(path_t *pa, int lv)
{
    for (; lv + 1 < pa->h && NH(pa->n[lv])->n; lv++) {
        IE(pa->n[lv + 1], pa->i[lv + 1])->k = first(pa->n[lv]);
        if (pa->i[lv + 1]) break;
    }
}

/* ---- Leaf items: a table at the front, the values packed at the back. */
static unsigned lused(node_t *n) { return NH(n)->n * sizeof(litem_t) + BS - NH(n)->dstart; }
static unsigned lfree(node_t *n) { return BS - sizeof(nhdr_t) - lused(n); }

static void leaf_put(node_t *n, int i, skey_t k, const void *v, unsigned len)
{
    nhdr_t *h = NH(n);
    memmove(LI(n, i + 1), LI(n, i), (h->n - i) * sizeof(litem_t));
    h->dstart -= len;
    if (len) memcpy(n->d + h->dstart, v, len);
    *LI(n, i) = (litem_t){ k, h->dstart, (uint16_t)len, 0 };
    h->n++;
}

static void leaf_del(node_t *n, int i)
{
    nhdr_t *h = NH(n);
    litem_t it = *LI(n, i);
    memmove(n->d + h->dstart + it.len, n->d + h->dstart, it.off - h->dstart);   /* close the gap */
    h->dstart += it.len;
    for (int j = 0; j < h->n; j++)
        if (!LI(n, j)->len) LI(n, j)->off = h->dstart;  /* empty values point at the data start */
        else if (LI(n, j)->off < it.off) LI(n, j)->off += it.len;
    memmove(LI(n, i), LI(n, i + 1), (h->n - i - 1) * sizeof(litem_t));
    h->n--;
}

/* Put a new entry (key k, child) after pa->i[lv] in the node at level lv,
 * splitting it if full; at the top, grow a new root. */
static int ins_parent(siefs_t *fs, tree_t *t, path_t *pa, int lv, skey_t k, node_t *child)
{
    ientry_t e = { k, { .blk = child->blk } };
    if (lv == pa->h) {                                  /* the root was split */
        node_t *old = pa->n[lv - 1], *root;
        if (lv >= MAXH || !(root = nnew(fs, lv, t->id))) return E(ENOMEM);
        *IE(root, 0) = (ientry_t){ first(old), { .blk = old->blk } };
        *IE(root, 1) = e;
        NH(root)->n = 2;
        t->root = (bptr_t){ .blk = root->blk };
        pa->n[lv] = root; pa->i[lv] = 0; pa->h++;
        return 0;
    }
    node_t *p = pa->n[lv], *r;
    int at = pa->i[lv] + 1, n = NH(p)->n;
    if (n < IMAX) {
        memmove(IE(p, at + 1), IE(p, at), (n - at) * sizeof e);
        *IE(p, at) = e;
        NH(p)->n++;
        return 0;
    }
    ientry_t all[IMAX + 1];                             /* full: split in two halves */
    memcpy(all, IE(p, 0), at * sizeof e);
    all[at] = e;
    memcpy(all + at + 1, IE(p, at), (n - at) * sizeof e);
    if (!(r = nnew(fs, lv, t->id))) return E(ENOMEM);
    int m = (n + 1) / 2;
    memcpy(IE(p, 0), all, m * sizeof e);          NH(p)->n = m;
    memcpy(IE(r, 0), all + m, (n + 1 - m) * sizeof e); NH(r)->n = n + 1 - m;
    return ins_parent(fs, t, pa, lv + 1, IE(r, 0)->k, r);
}

/* The leaf is too full for the new item: share the items (with the new
 * one) between it and a new right neighbour, as evenly as possible. */
static int split_leaf(siefs_t *fs, tree_t *t, path_t *pa, skey_t k, const void *v, unsigned len)
{
    enum { MAXN = (BS - sizeof(nhdr_t)) / sizeof(litem_t) + 1 };
    node_t *l = pa->n[0], *r;
    int at = pa->i[0], n = NH(l)->n + 1, m = -1;
    skey_t kk[MAXN]; const uint8_t *vv[MAXN]; unsigned ll[MAXN];
    uint8_t *tmp = fs->env.alloc(BS);
    if (!tmp) return E(ENOMEM);
    memcpy(tmp, l->d, BS);
    unsigned total = 0, cap = BS - sizeof(nhdr_t), left = 0, best = ~0u;
    for (int j = 0; j < n; j++) {
        if (j == at) { kk[j] = k; vv[j] = v; ll[j] = len; }
        else {
            litem_t *it = (litem_t *)(tmp + sizeof(nhdr_t)) + (j < at ? j : j - 1);
            kk[j] = it->k; vv[j] = tmp + it->off; ll[j] = it->len;
        }
        total += ll[j] + sizeof(litem_t);
    }
    for (int j = 1; j < n; j++) {                       /* both halves must fit; prefer even */
        left += ll[j - 1] + sizeof(litem_t);
        unsigned right = total - left, d = left > right ? left - right : right - left;
        if (left <= cap && right <= cap && d < best) { best = d; m = j; }
    }
    if (m < 0 || !(r = nnew(fs, 0, t->id))) { fs->env.free(tmp); return E(ENOMEM); }
    NH(l)->n = 0; NH(l)->dstart = BS;
    for (int j = 0; j < n; j++) {
        node_t *d = j < m ? l : r;
        leaf_put(d, NH(d)->n, kk[j], vv[j], ll[j]);
    }
    fs->env.free(tmp);
    if (!at) fix_keys(pa, 0);
    return ins_parent(fs, t, pa, 1, LI(r, 0)->k, r);
}

/* Put an item at the place search found. */
static int place(siefs_t *fs, tree_t *t, path_t *pa, skey_t k, const void *v, unsigned len)
{
    node_t *l = pa->n[0];
    if (lfree(l) < len + sizeof(litem_t)) return split_leaf(fs, t, pa, k, v, len);
    leaf_put(l, pa->i[0], k, v, len);
    if (!pa->i[0]) fix_keys(pa, 0);
    return 0;
}

/* ---- Removing: a node that becomes small is merged with a neighbour when
 * both fit in one; an empty one disappears; a root with a single child
 * gives way to it. All leaves stay at the same depth. */
static int cow_child(siefs_t *fs, node_t *p, int i, node_t **out)
{
    int e = t_get_node(fs, &IE(p, i)->p, out);
    return e ? e : make_dirty(fs, *out, &IE(p, i)->p);
}
static unsigned nused(node_t *n) { return NH(n)->level ? NH(n)->n * sizeof(ientry_t) : lused(n); }
static void ent_del(node_t *p, int i)
{
    memmove(IE(p, i), IE(p, i + 1), (NH(p)->n - i - 1) * sizeof(ientry_t));
    NH(p)->n--;
}

static int rebalance(siefs_t *fs, tree_t *t, path_t *pa, int lv)
{
    node_t *n = pa->n[lv];
    int e;
    if (lv == pa->h - 1) {                              /* the root */
        while (NH(n)->level && NH(n)->n <= 1) {
            node_t *c;
            if (!NH(n)->n) {                            /* nothing left: an empty leaf */
                NH(n)->level = 0; NH(n)->dstart = BS;
                break;
            }
            if ((e = cow_child(fs, n, 0, &c))) return e;
            t->root = (bptr_t){ .blk = c->blk };
            ndrop(fs, n);
            n = c;
        }
        return 0;
    }
    node_t *p = pa->n[lv + 1], *s;
    int pi = pa->i[lv + 1];
    if (!NH(n)->n) {                                    /* empty: unhook it */
        ndrop(fs, n);
        ent_del(p, pi);
        pa->i[lv + 1] = 0;
        fix_keys(pa, lv + 1);
        return rebalance(fs, t, pa, lv + 1);
    }
    fix_keys(pa, lv);
    if (nused(n) >= (BS - sizeof(nhdr_t)) / 4 || NH(p)->n < 2) return 0;
    int si = pi + 1 < NH(p)->n ? pi + 1 : pi - 1;       /* the right neighbour, or the left one */
    if ((e = cow_child(fs, p, si, &s))) return e;
    node_t *a = si > pi ? n : s, *b = si > pi ? s : n;
    int bi = si > pi ? si : pi;
    if (NH(a)->level ? NH(a)->n + NH(b)->n > IMAX : lused(a) + lused(b) > BS - sizeof(nhdr_t)) return 0;
    if (NH(a)->level) {                                 /* merge b into a */
        memcpy(IE(a, NH(a)->n), IE(b, 0), NH(b)->n * sizeof(ientry_t));
        NH(a)->n += NH(b)->n;
    } else for (int j = 0; j < NH(b)->n; j++)
        leaf_put(a, NH(a)->n, LI(b, j)->k, b->d + LI(b, j)->off, LI(b, j)->len);
    ndrop(fs, b);
    ent_del(p, bi);
    pa->n[lv] = a; pa->i[lv + 1] = bi - 1;
    return rebalance(fs, t, pa, lv + 1);
}

/* ---- The operations. */
int t_get(siefs_t *fs, tree_t *t, skey_t k, void *buf, unsigned max)
{
    path_t pa;
    int f = search(fs, t, k, &pa, 0);
    if (f <= 0) return f ? f : E(ENOENT);
    unsigned len = plen(&pa);
    memcpy(buf, pval(&pa), len < max ? len : max);
    return (int)len;
}

int t_insert(siefs_t *fs, tree_t *t, skey_t k, const void *v, unsigned len)
{
    path_t pa;
    if (len > MAXITEM) return E(E2BIG);
    int f = search(fs, t, k, &pa, 1);
    if (f) return f < 0 ? f : E(EEXIST);
    return place(fs, t, &pa, k, v, len);
}

static int replace(siefs_t *fs, tree_t *t, path_t *pa, skey_t k, const void *v, unsigned len)
{
    if (plen(pa) == len) { memcpy(pval(pa), v, len); return 0; }
    leaf_del(pa->n[0], pa->i[0]);
    return place(fs, t, pa, k, v, len);
}

int t_update(siefs_t *fs, tree_t *t, skey_t k, const void *v, unsigned len)
{
    path_t pa;
    if (len > MAXITEM) return E(E2BIG);
    int f = search(fs, t, k, &pa, 1);
    if (f <= 0) return f ? f : E(ENOENT);
    return replace(fs, t, &pa, k, v, len);
}

int t_put(siefs_t *fs, tree_t *t, skey_t k, const void *v, unsigned len)     /* insert or replace */
{
    path_t pa;
    if (len > MAXITEM) return E(E2BIG);
    int f = search(fs, t, k, &pa, 1);
    if (f < 0) return f;
    return f ? replace(fs, t, &pa, k, v, len) : place(fs, t, &pa, k, v, len);
}

int t_delete(siefs_t *fs, tree_t *t, skey_t k)
{
    path_t pa;
    int f = search(fs, t, k, &pa, 1);
    if (f <= 0) return f ? f : E(ENOENT);
    leaf_del(pa.n[0], pa.i[0]);
    return rebalance(fs, t, &pa, 0);
}

/* Iterating (read-only: the path must not be kept across changes).
 * t_seek: the first item >= k; t_floor: the last item <= k; t_next: the
 * following one. Each returns 1 when positioned on an item, 0 if none. */
int t_next(siefs_t *fs, path_t *pa)
{
    if (++pa->i[0] < NH(pa->n[0])->n) return 1;
    int lv = 1, e;
    while (lv < pa->h && pa->i[lv] + 1 >= NH(pa->n[lv])->n) lv++;
    if (lv >= pa->h) return 0;
    pa->i[lv]++;
    for (; lv > 0; lv--) {
        if ((e = t_get_node(fs, &IE(pa->n[lv], pa->i[lv])->p, &pa->n[lv - 1]))) return e;
        pa->i[lv - 1] = 0;
    }
    return NH(pa->n[0])->n > 0;
}

int t_seek(siefs_t *fs, tree_t *t, skey_t k, path_t *pa)
{
    int f = search(fs, t, k, pa, 0);
    if (f < 0) return f;
    if (pa->i[0] < NH(pa->n[0])->n) return 1;
    pa->i[0]--;                                         /* past this leaf's end: the next one */
    return t_next(fs, pa);
}

int t_floor(siefs_t *fs, tree_t *t, skey_t k, path_t *pa)
{
    int f = search(fs, t, k, pa, 0);
    if (f) return f;
    if (!pa->i[0]) return 0;    /* k is before this leaf's first key: only in the leftmost leaf */
    pa->i[0]--;
    return 1;
}

int t_new_root(siefs_t *fs, tree_t *t)                  /* an empty tree */
{
    node_t *n = nnew(fs, 0, t->id);
    if (!n) return E(ENOMEM);
    t->root = (bptr_t){ .blk = n->blk };
    return 0;
}

/* ---- Commit: write the dirty nodes under p, children first, to new
 * blocks. Consecutive blocks are gathered in fs->io and written together. */
int t_wflush(siefs_t *fs)
{
    int e = fs->wn ? dev_write(fs, fs->wstart, (uint32_t)fs->wn, fs->io) : 0;
    fs->wn = 0;
    return e;
}

int t_flush(siefs_t *fs, bptr_t *p)
{
    if (!(p->blk & TMP)) return 0;
    uint32_t s = (uint32_t)(p->blk & ~TMP);
    node_t *n = fs->dirty[s];
    int e;
    if (NH(n)->level)
        for (int i = 0; i < NH(n)->n; i++) if ((e = t_flush(fs, &IE(n, i)->p))) return e;
    uint64_t b = commit_block(fs);
    if (!b) return E(ENOSPC);
    if (fs->wn && (b != fs->wstart + fs->wn || fs->wn == EXT_MAX) && (e = t_wflush(fs))) return e;
    if (!fs->wn) fs->wstart = b;
    NH(n)->gen = fs->txn;
    memcpy(fs->io + fs->wn++ * BS, n->d, BS);
    fs->dirty[s] = 0; fs->ndirty--;
    *p = (bptr_t){ .blk = b, .gen = fs->txn, .csum = siefs_crc32c(0, n->d, BS) };
    n->blk = b; n->dirty = 0;
    cache_add(fs, n);
    return 0;
}
