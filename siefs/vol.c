/*
 * vol.c - Snapshots: taking, deleting, restoring and listing them.
 *
 * A snapshot is a volume record whose tree root is the one its head (the
 * volume it pictures) had at a commit: nothing is copied. From then on the
 * head lets go of shared blocks onto its dead list instead of freeing them
 * (blk_release in space.c). The snapshots of a head, oldest first, and the
 * head itself each own a dead list: the blocks freed since the snapshot
 * before them while an older one still uses them.
 *
 * Deleting snapshot S (P the one before it, N the one after it or the
 * head): N's dead list holds what left the trees between S and N. Of
 * those, the blocks written after P were only S's: they are freed. The
 * others P still uses: they join S's dead list, which becomes N's. (ZFS
 * deletes snapshots the same way.) Each of these operations lands whole in
 * one commit, like every other.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "siefs_int.h"

static int begin(siefs_t *fs) { fs->opmod = 0; return fs->broken ? E(EIO) : 0; }
static int end(siefs_t *fs, int r)
{
    if (r == E(EIO) || r == E(ENOMEM) || r == E(ENOSPC)) { if (fs->opmod) fs->broken = 1; }
    cache_trim(fs);
    return r;
}

/* The head volume of the object g (a snapshot's head, for an object in one). */
static int head_of(siefs_t *fs, uint64_t g, volx_t **h)
{
    volx_t *v = vol_get(fs, VOLOF(g));
    if (v && (v->v.flags & V_SNAP)) v = vol_get(fs, v->v.parent);
    if (!v) return E(ENOENT);
    if (v->locked) return E(EACCES);
    *h = v;
    return 0;
}

static volx_t *snap_of(siefs_t *fs, volx_t *h, const char *name)
{
    for (uint32_t i = 0; i < fs->nvols; i++) {
        volx_t *v = fs->vols[i];
        if ((v->v.flags & V_SNAP) && v->v.parent == h->id && !strcmp(v->name, name)) return v;
    }
    return 0;
}

/* The snapshot of h just before (dir < 0) or after (dir > 0) id s. */
static volx_t *snap_near(siefs_t *fs, volx_t *h, uint64_t s, int dir)
{
    volx_t *best = 0;
    for (uint32_t i = 0; i < fs->nvols; i++) {
        volx_t *v = fs->vols[i];
        if (!(v->v.flags & V_SNAP) || v->v.parent != h->id) continue;
        if (dir < 0 ? v->id < s && (!best || v->id > best->id) : v->id > s && (!best || v->id < best->id)) best = v;
    }
    return best;
}

static int name_ok(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 63 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return E(EINVAL);
    for (size_t i = 0; i < n; i++) if (s[i] == '/') return E(EINVAL);
    return 0;
}

int siefs_snapshot(siefs_t *fs, uint64_t g, const char *name)
{
    volx_t *h, *s;
    int e;
    if ((e = begin(fs)) || (e = head_of(fs, g, &h)) || (e = name_ok(name))) return end(fs, e);
    if (VOLOF(g) != h->id) return end(fs, E(EROFS));    /* (not of a snapshot) */
    if (snap_of(fs, h, name)) return end(fs, E(EEXIST));
    if ((e = space_reserve(fs, 16)) || (e = commit(fs, 0))) return end(fs, e);   /* its tree, on the disk */
    if (!h->v.dl) h->v.dl = fs->next_vid++;
    vol_t sv = h->v;
    sv.flags = V_SNAP | (h->v.flags & V_ENC);
    sv.root = h->ft.root;
    sv.parent = h->id; sv.gen = fs->sb.seq; sv.created = fs->env.now();
    if (!(s = vol_add(fs, fs->next_vid++, &sv))) return end(fs, E(ENOMEM));
    memcpy(s->name, name, strlen(name) + 1);
    s->dirty = 1;
    if (h->key) {                                       /* the snapshot shares the key */
        uint8_t k[32];
        memcpy(k, ((aead_t *)h->key)->key, 32);
        e = vol_secret(fs, h, k);
        memset(k, 0, sizeof k);
        if (e) return end(fs, e);
    }
    fs->opmod = 1;
    if ((e = t_put(fs, &fs->rt, K(s->id, T_VOLNAME, 0), name, (unsigned)strlen(name)))) return end(fs, e);
    h->v.dl = fs->next_vid++;                           /* from now on: what s still uses */
    h->v.gen = fs->sb.seq;
    h->dirty = 1;
    fs->changed = 1;
    return end(fs, commit(fs, 1));
}

static int dead_next(siefs_t *fs, uint64_t dl, uint64_t *b, uint64_t d[2])
{
    path_t pa;
    int r = t_seek(fs, &fs->rt, K(dl, T_DEAD, 0), &pa);
    if (r <= 0) return r;
    if (pkey(&pa).id != dl || KTYPE(pkey(&pa).off) != T_DEAD || plen(&pa) != 16) return 0;
    *b = pkey(&pa).off & OFFMASK;
    memcpy(d, pval(&pa), 16);
    return 1;
}

static void forget(siefs_t *fs, uint64_t b, uint64_t n)
{
    for (uint64_t i = b; i < b + n; i++) cache_forget(fs, i);
    blk_free(fs, b, n);
}

static int snap_del(siefs_t *fs, volx_t *h, volx_t *s)
{
    volx_t *p = snap_near(fs, h, s->id, -1), *n = snap_near(fs, h, s->id, 1);
    if (!n) n = h;
    uint64_t b, d[2];
    int r, e;
    if ((e = commit(fs, 0))) return e;                  /* (dead-list entries still in memory: on the disk) */
    if (!s->v.dl) s->v.dl = fs->next_vid++;
    fs->opmod = 1;
    while ((r = dead_next(fs, n->v.dl, &b, d)) > 0) {
        if ((e = t_delete(fs, &fs->rt, K(n->v.dl, T_DEAD, b)))) return e;
        if (p && d[1] <= p->v.gen) { if ((e = t_insert(fs, &fs->rt, K(s->v.dl, T_DEAD, b), d, sizeof d))) return e; }
        else forget(fs, b, d[0]);                       /* only s used it */
    }
    if (r < 0) return r;
    n->v.dl = s->v.dl;
    n->dirty = 1;
    if (n == h) h->v.gen = p ? p->v.gen : 0;            /* the newest snapshot is now p (or none) */
    if ((e = t_delete(fs, &fs->rt, K(s->id, T_VOLUME, 0))) || ((e = t_delete(fs, &fs->rt, K(s->id, T_VOLNAME, 0))) && e != E(ENOENT)))
        return e;
    vol_drop(fs, s);
    fs->changed = 1;
    return commit(fs, 1);
}

int siefs_snapshot_delete(siefs_t *fs, uint64_t g, const char *name)
{
    volx_t *h, *s;
    int e;
    if ((e = begin(fs)) || (e = head_of(fs, g, &h))) return end(fs, e);
    if (!(s = snap_of(fs, h, name))) return end(fs, E(ENOENT));
    if ((e = space_reserve(fs, 64))) return end(fs, e);
    return end(fs, snap_del(fs, h, s));
}

/* Free what a head's tree holds that was written after commit sg (with
 * all: everything). Under a node written by sg or before, all is shared. */
static int free_newer(siefs_t *fs, const bptr_t *p, uint64_t sg, int all)
{
    node_t *cn, *n;
    int e;
    if (!all && p->gen <= sg) return 0;
    if ((e = t_get_node(fs, p, &cn))) return e;
    if (!(n = fs->env.alloc(sizeof *n))) return E(ENOMEM);
    memcpy(n->d, cn->d, BS);
    for (int i = 0; i < NH(n)->n && !e; i++) {
        if (NH(n)->level) { e = free_newer(fs, &IE(n, i)->p, sg, all); continue; }
        extent_t x;
        skey_t k = LI(n, i)->k;
        if (KTYPE(k.off) != T_EXTENT || (k.id & IDX_BIT)) continue;
        if ((e = ext_dec(n->d + LI(n, i)->off, LI(n, i)->len, &x))) break;
        if (all || x.gen > sg) blk_free(fs, x.blk, x.n);
    }
    fs->env.free(n);
    if (!e) forget(fs, p->blk, 1);
    return e;
}

int siefs_snapshot_restore(siefs_t *fs, uint64_t g, const char *name)
{
    volx_t *h, *s, *t;
    uint64_t b, d[2];
    int e, r;
    if ((e = begin(fs)) || (e = head_of(fs, g, &h))) return end(fs, e);
    if (!(s = snap_of(fs, h, name))) return end(fs, E(ENOENT));
    if ((e = space_reserve(fs, 64))) return end(fs, e);
    while ((t = snap_near(fs, h, s->id, 1)))            /* newer snapshots go first */
        if ((e = snap_del(fs, h, t))) return end(fs, e);
    if ((e = commit(fs, 0))) return end(fs, e);
    fs->opmod = 1;
    if ((e = free_newer(fs, &h->ft.root, s->v.gen, 0))) return end(fs, e);
    while ((r = dead_next(fs, h->v.dl, &b, d)) > 0)     /* s uses those again: they stay */
        if ((e = t_delete(fs, &fs->rt, K(h->v.dl, T_DEAD, b)))) return end(fs, e);
    if (r < 0) return end(fs, r);
    h->v.root = h->ft.root = s->v.root;
    h->v.next_id = s->v.next_id; h->v.inodes = s->v.inodes;
    h->dirty = 1;
    fs->changed = 1;
    return end(fs, commit(fs, 1));
}

/* A volume that is gone: every block of its tree (no snapshot left). */
int vol_free_tree(siefs_t *fs, volx_t *v) { return free_newer(fs, &v->ft.root, 0, 1); }

int siefs_snapshot_list(siefs_t *fs, uint64_t g, uint64_t *cursor, siefs_snap_t *out)
{
    volx_t *h, *s;
    int e;
    if ((e = begin(fs)) || (e = head_of(fs, g, &h))) return end(fs, e);
    if (!(s = snap_near(fs, h, *cursor, 1))) return end(fs, 0);
    memcpy(out->name, s->name, sizeof out->name);
    out->ino = s->id << VSHIFT | SIEFS_ROOT;
    out->created = s->v.created;
    out->commit = s->v.gen;
    *cursor = s->id;
    return end(fs, 1);
}

int siefs_volume_next(siefs_t *fs, uint64_t *cursor, uint64_t *root)
{
    volx_t *best = 0;
    for (uint32_t i = 0; i < fs->nvols; i++) {
        volx_t *v = fs->vols[i];
        if (!(v->v.flags & V_SNAP) && !v->locked && v->id > *cursor && (!best || v->id < best->id)) best = v;
    }
    if (!best) return 0;
    *cursor = best->id;
    *root = best->id == 1 ? SIEFS_ROOT : best->id << VSHIFT | SIEFS_ROOT;
    return 1;
}

int siefs_volume_root(siefs_t *fs, uint64_t g, uint64_t *root)   /* (a locked one too: to unlock it) */
{
    volx_t *h = vol_get(fs, VOLOF(g));
    if (h && (h->v.flags & V_SNAP)) h = vol_get(fs, h->v.parent);
    if (!h) return E(ENOENT);
    *root = h->id == 1 ? SIEFS_ROOT : h->id << VSHIFT | SIEFS_ROOT;
    return 0;
}
