/*
 * fs.c - Files, directories and attributes, built on the tree.
 *
 * An object (file, directory, symlink) is a group of items with the same
 * id: its inode, then its contents (one INLINE item for small files, or
 * EXTENT items: runs of up to 32 blocks, each block with its checksum),
 * its directory entries if it is a directory, its extended attributes.
 * Every public operation checks its arguments first, then changes things:
 * an error after a change can only be the disk or memory failing, and then
 * the mount stops (fs->broken) rather than commit a half-done operation.
 *
 * Volumes: an object's number carries its volume (volume << 48 | its id;
 * volume 1's are just the id). Each public operation picks the volume of
 * the object it is given (fs->cv), and the code below works in it with
 * local ids. A directory entry or a parent naming an object of another
 * volume (a volume mounted there, or a volume root's mount point) holds
 * its whole number, the volume always written (1 << 48 for volume 1).
 * Snapshots are read only; a volume's root has a hidden ".snapshots"
 * directory (local id 1, made up on the fly) listing them.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "siefs_int.h"

#define MAXBLK (OFFMASK / BS)                           /* files up to 2^56 bytes */
#define FT  (&fs->cv->ft)                               /* the current volume's file tree */
#define VOL (fs->cv->v)                                 /* ... and its record */
#define ISDIR(m) (((m) & SIEFS_IFMT) == SIEFS_IFDIR)
#define ISREG(m) (((m) & SIEFS_IFMT) == SIEFS_IFREG)
#define ISLNK(m) (((m) & SIEFS_IFMT) == SIEFS_IFLNK)

/* ---- Every public operation is wrapped in begin/end. end commits when
 * enough has changed (between operations, so commits are always whole). */
static int begin(siefs_t *fs) { fs->opmod = 0; return fs->broken ? E(EIO) : 0; }
static long end(siefs_t *fs, long r)
{
    if (r == E(EIO) || r == E(ENOMEM) || r == E(ENOSPC)) { if (fs->opmod) fs->broken = 1; }
    else if (fs->changed && (fs->ndirty > fs->env.dirty_limit || fs->env.now() - fs->first_change >= fs->env.commit_ns)) {
        int e = commit(fs, 0);
        if (e && r >= 0) r = e;
    }
    cache_trim(fs);
    return r;
}
#define BEGIN(fs) do { int b_ = begin(fs); if (b_) return b_; } while (0)

#define SNAPDIR 1                                       /* a volume's ".snapshots" (no item: made up) */
static uint64_t G(siefs_t *fs, uint64_t l) { return fs->cv->id == 1 ? l : fs->cv->id << VSHIFT | l; }
static uint64_t ref_dec(siefs_t *fs, uint64_t x)        /* a stored reference -> an object's number */
{
    return !(x >> VSHIFT) ? G(fs, x) : x >> VSHIFT == 1 ? x & LMASK : x;
}
static uint64_t ref_enc(siefs_t *fs, uint64_t g)        /* ... and back, from the current volume */
{
    return VOLOF(g) == fs->cv->id ? g & LMASK : VOLOF(g) << VSHIFT | (g & LMASK);
}
static int pick(siefs_t *fs, uint64_t g, uint64_t *l)   /* work in g's volume */
{
    volx_t *v = vol_get(fs, VOLOF(g));
    if (!v) return E(ENOENT);
    if (v->locked) return E(EACCES);
    fs->cv = v;
    *l = g & LMASK;
    return 0;
}
static int ro(siefs_t *fs, uint64_t l) { return (fs->cv->v.flags & V_SNAP) || l == SNAPDIR ? E(EROFS) : 0; }
#define PICK(g, l) do { int p_ = pick(fs, g, &(l)); if (p_) return end(fs, p_); } while (0)

static int iget(siefs_t *fs, uint64_t ino, inode_t *in)
{
    if (ino == SNAPDIR && !(fs->cv->v.flags & V_SNAP)) {   /* ".snapshots": read only, like the root */
        inode_t r;
        int e = iget(fs, SIEFS_ROOT, &r);
        if (e) return e;
        *in = (inode_t){ .mode = SIEFS_IFDIR | 0555, .uid = r.uid, .gid = r.gid, .nlink = 2, .parent = SIEFS_ROOT,
                         .atime = r.btime, .mtime = r.btime, .ctime = r.btime, .btime = r.btime };
        return 0;
    }
    int n = t_get(fs, FT, K(ino, T_INODE, 0), in, sizeof *in);
    if (n >= 0 && ino == SIEFS_ROOT && (fs->cv->v.flags & V_SNAP))   /* a snapshot's root: in its head's ".snapshots" */
        in->parent = fs->cv->v.parent << VSHIFT | SNAPDIR;
    return n < 0 ? n : n == sizeof *in ? 0 : E(EIO);
}
static int iput(siefs_t *fs, uint64_t ino, inode_t *in) { return t_update(fs, FT, K(ino, T_INODE, 0), in, sizeof *in); }

/* ---- Hashes: a 64-bit FNV-1a with a final mix. Directory entries and
 * attributes are found by the hash of their name (56 bits; names that
 * collide share an item). Later, encrypted volumes will use a keyed hash
 * so names cannot be guessed from the tree's order. */
static uint64_t hash2(const void *a, size_t na, const void *b, size_t nb)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < na; i++) h = (h ^ ((const uint8_t *)a)[i]) * 0x100000001b3ULL;
    h = (h ^ 0x100) * 0x100000001b3ULL;
    for (size_t i = 0; i < nb; i++) h = (h ^ ((const uint8_t *)b)[i]) * 0x100000001b3ULL;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
    return h;
}
uint64_t name_hash(const char *s, size_t n)
{
    uint64_t h = hash2(s, n, 0, 0) & OFFMASK;
    return h < 2 ? h + 2 : h;                           /* 0 and 1: readdir's "." and ".." */
}
uint64_t xindex_id(const char *name, size_t nl, const void *v, size_t vl) { return IDX_BIT | hash2(name, nl, v, vl) >> 1; }

static int name_ok(const char *s, size_t *len)
{
    size_t n = strlen(s);
    if (!n || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return E(EINVAL);
    if (n > SIEFS_NAME_MAX) return E(ENAMETOOLONG);
    for (size_t i = 0; i < n; i++) if (s[i] == '/') return E(EINVAL);
    *len = n;
    return 0;
}

/* ---- Directory entries: in item (dir, DIRENT, hash), a list of
 *   u64 ino | u8 type | u8 name length | name */
static int dir_find(siefs_t *fs, uint64_t dir, const char *name, size_t len, uint64_t *ino, int *type)
{
    uint8_t b[MAXITEM];
    int n = t_get(fs, FT, K(dir, T_DIRENT, name_hash(name, len)), b, sizeof b);
    for (int p = 0; n > 0 && p + 10 <= n; p += 10 + b[p + 9])
        if (b[p + 9] == len && !memcmp(b + p + 10, name, len)) {
            memcpy(ino, b + p, 8);
            if (type) *type = b[p + 8];
            return 0;
        }
    return n < 0 && n != E(ENOENT) ? n : E(ENOENT);
}

static int dir_add(siefs_t *fs, uint64_t dir, const char *name, size_t len, uint64_t ino, int type)
{
    uint8_t b[MAXITEM];
    skey_t k = K(dir, T_DIRENT, name_hash(name, len));
    int n = t_get(fs, FT, k, b, sizeof b);
    if (n == E(ENOENT)) n = 0;
    if (n < 0) return n;
    if (n + 10 + len > MAXITEM) return E(ENOSPC);       /* ~7 names with one 56-bit hash: never seen */
    memcpy(b + n, &ino, 8);
    b[n + 8] = (uint8_t)type;
    b[n + 9] = (uint8_t)len;
    memcpy(b + n + 10, name, len);
    return t_put(fs, FT, k, b, n + 10 + len);
}

static int dir_del(siefs_t *fs, uint64_t dir, const char *name, size_t len)
{
    uint8_t b[MAXITEM];
    skey_t k = K(dir, T_DIRENT, name_hash(name, len));
    int n = t_get(fs, FT, k, b, sizeof b);
    for (int p = 0; n > 0 && p + 10 <= n; p += 10 + b[p + 9])
        if (b[p + 9] == len && !memcmp(b + p + 10, name, len)) {
            int sz = 10 + b[p + 9];
            memmove(b + p, b + p + sz, n - p - sz);
            n -= sz;
            return n ? t_update(fs, FT, k, b, n) : t_delete(fs, FT, k);
        }
    return n < 0 ? n : E(ENOENT);
}

static int dir_empty(siefs_t *fs, uint64_t dir)
{
    path_t pa;
    int r = t_seek(fs, FT, K(dir, T_DIRENT, 0), &pa);
    if (r < 0) return r;
    return !(r && pkey(&pa).id == dir && KTYPE(pkey(&pa).off) == T_DIRENT);
}

/* ---- Names. */
/* The snapshot of the current volume called name (n bytes). */
static volx_t *snap_named(siefs_t *fs, const char *name, size_t n)
{
    for (uint32_t i = 0; i < fs->nvols; i++) {
        volx_t *v = fs->vols[i];
        if ((v->v.flags & V_SNAP) && v->v.parent == fs->cv->id && strlen(v->name) == n && !memcmp(v->name, name, n)) return v;
    }
    return 0;
}

/* Look name up in dir (local): the object's number (global) in *ino. */
static int lookup(siefs_t *fs, uint64_t dir, const char *name, uint64_t *ino)
{
    inode_t d;
    int e = iget(fs, dir, &d);
    size_t n = strlen(name);
    uint64_t x;
    if (e) return e;
    if (!ISDIR(d.mode)) return E(ENOTDIR);
    if (n > SIEFS_NAME_MAX) return E(ENAMETOOLONG);
    if (!n || (n == 1 && name[0] == '.')) { *ino = G(fs, dir); return 0; }
    if (n == 2 && name[0] == '.' && name[1] == '.') { *ino = ref_dec(fs, d.parent); return 0; }
    if (dir == SNAPDIR) {
        volx_t *s = snap_named(fs, name, n);
        if (!s) return E(ENOENT);
        *ino = s->id << VSHIFT | SIEFS_ROOT;
        return 0;
    }
    if (dir == SIEFS_ROOT && n == 10 && !memcmp(name, ".snapshots", 10) && !(fs->cv->v.flags & V_SNAP)) { *ino = G(fs, SNAPDIR); return 0; }
    if ((e = dir_find(fs, dir, name, n, &x, 0))) return e;
    *ino = ref_dec(fs, x);
    return 0;
}

int siefs_lookup(siefs_t *fs, uint64_t dir, const char *name, uint64_t *ino)
{
    uint64_t l;
    BEGIN(fs);
    PICK(dir, l);
    return (int)end(fs, lookup(fs, l, name, ino));
}

int siefs_walk(siefs_t *fs, const char *path, uint64_t *ino)
{
    char c[SIEFS_NAME_MAX + 1];
    uint64_t cur = SIEFS_ROOT, l;
    int e = 0;
    BEGIN(fs);
    while (*path && !e) {
        size_t n = 0;
        while (*path == '/') path++;
        while (path[n] && path[n] != '/') n++;
        if (!n) break;
        if (n > SIEFS_NAME_MAX) { e = E(ENAMETOOLONG); break; }
        memcpy(c, path, n); c[n] = 0;
        path += n;
        if (!(e = pick(fs, cur, &l))) e = lookup(fs, l, c, &cur);
    }
    if (!e) *ino = cur;
    return (int)end(fs, e);
}

/* Remove an object entirely: all its items, its blocks, its index entries. */
static void unindex(siefs_t *fs, uint64_t ino, const uint8_t *b, int n);
static int free_object(siefs_t *fs, uint64_t ino)
{
    for (;;) {
        path_t pa;
        int r = t_seek(fs, FT, K(ino, 0, 0), &pa);
        if (r < 0) return r;
        if (!r || pkey(&pa).id != ino) break;
        skey_t k = pkey(&pa);
        int ty = KTYPE(k.off);
        if (ty == T_EXTENT) {
            extent_t x;
            if ((r = ext_dec(pval(&pa), plen(&pa), &x))) return r;
            blk_release(fs, fs->cv->id, x.blk, x.n, x.gen);
        }
        if (ty == T_XATTR) { uint8_t b[MAXITEM]; int n = (int)plen(&pa); memcpy(b, pval(&pa), n); unindex(fs, ino, b, n); }
        if ((r = t_delete(fs, FT, k))) return r;
    }
    VOL.inodes--;
    fs->cv->dirty = 1;
    return 0;
}

static int mkobj(siefs_t *fs, uint64_t dir, const char *name, uint32_t mode, uint32_t uid, uint32_t gid,
                 const char *link, uint64_t *out)
{
    size_t len, ll = link ? strlen(link) : 0;
    uint32_t type = mode & SIEFS_IFMT;
    inode_t d;
    uint64_t x;
    int e = name_ok(name, &len);
    if (e) return e;
    if (type != SIEFS_IFREG && type != SIEFS_IFDIR && type != SIEFS_IFLNK) return E(EINVAL);
    if (ll > INLINE_MAX) return E(ENAMETOOLONG);
    if ((e = iget(fs, dir, &d))) return e;
    if (!ISDIR(d.mode)) return E(ENOTDIR);
    if (!(e = dir_find(fs, dir, name, len, &x, 0))) return E(EEXIST);
    if (e != E(ENOENT) || (e = space_reserve(fs, 8))) return e;

    uint64_t ino = VOL.next_id++;
    int64_t t = fs->env.now();
    inode_t in = { .mode = mode, .uid = uid, .gid = gid, .nlink = type == SIEFS_IFDIR ? 2 : 1,
                   .parent = dir, .atime = t, .mtime = t, .ctime = t, .btime = t };
    if (link) { in.size = ll; in.flags = F_INLINE; }
    if ((e = t_insert(fs, FT, K(ino, T_INODE, 0), &in, sizeof in)) ||
        (link && (e = t_insert(fs, FT, K(ino, T_INLINE, 0), link, (unsigned)ll))) ||
        (e = dir_add(fs, dir, name, len, ino, type >> 12))) return e;
    if (type == SIEFS_IFDIR) d.nlink++;
    d.mtime = d.ctime = t;
    if ((e = iput(fs, dir, &d))) return e;
    VOL.inodes++;
    fs->cv->dirty = 1;
    *out = ino;
    return 0;
}

int siefs_create(siefs_t *fs, uint64_t dir, const char *name, uint32_t mode, uint32_t uid, uint32_t gid, uint64_t *ino)
{
    uint64_t l, o;
    int e;
    BEGIN(fs);
    PICK(dir, l);
    if (ISLNK(mode)) return (int)end(fs, E(EINVAL));
    if (!(e = ro(fs, l)) && !(e = mkobj(fs, l, name, mode, uid, gid, 0, &o))) *ino = G(fs, o);
    return (int)end(fs, e);
}

int siefs_symlink(siefs_t *fs, uint64_t dir, const char *name, const char *target, uint32_t uid, uint32_t gid, uint64_t *ino)
{
    uint64_t l, o;
    int e;
    BEGIN(fs);
    PICK(dir, l);
    if (!(e = ro(fs, l)) && !(e = mkobj(fs, l, name, SIEFS_IFLNK | 0777, uid, gid, target, &o))) *ino = G(fs, o);
    return (int)end(fs, e);
}

/* A new volume, mounted at dir/name: its root directory is that name's
 * object. Its root's parent, and the entry naming it, cross volumes. */
int siefs_mkvol(siefs_t *fs, uint64_t g, const char *name, uint32_t mode, uint32_t uid, uint32_t gid, uint64_t *ino)
{
    inode_t d;
    uint64_t dir, x;
    size_t len;
    int e;
    BEGIN(fs);
    PICK(g, dir);
    volx_t *pv = fs->cv;
    if ((e = ro(fs, dir)) || (e = name_ok(name, &len)) || (e = iget(fs, dir, &d))) return (int)end(fs, e);
    if (!ISDIR(d.mode)) return (int)end(fs, E(ENOTDIR));
    if (!(e = dir_find(fs, dir, name, len, &x, 0))) return (int)end(fs, E(EEXIST));
    if (e != E(ENOENT) || (e = space_reserve(fs, 16))) return (int)end(fs, e);
    int64_t t = fs->env.now();
    vol_t nv = { .next_id = 16, .inodes = 1, .created = t };
    volx_t *v = vol_add(fs, fs->next_vid++, &nv);
    if (!v) return (int)end(fs, E(ENOMEM));
    v->dirty = 1;
    fs->cv = v;
    inode_t root = { .mode = SIEFS_IFDIR | (mode & 07777), .uid = uid, .gid = gid, .nlink = 2,
                     .parent = pv->id << VSHIFT | dir, .atime = t, .mtime = t, .ctime = t, .btime = t };
    if ((e = t_new_root(fs, FT)) || (e = t_insert(fs, FT, K(SIEFS_ROOT, T_INODE, 0), &root, sizeof root))) return (int)end(fs, e);
    fs->cv = pv;
    if ((e = dir_add(fs, dir, name, len, ref_enc(fs, v->id << VSHIFT | SIEFS_ROOT), SIEFS_IFDIR >> 12))) return (int)end(fs, e);
    d.nlink++;
    d.mtime = d.ctime = t;
    if ((e = iput(fs, dir, &d))) return (int)end(fs, e);
    *ino = v->id << VSHIFT | SIEFS_ROOT;
    return (int)end(fs, 0);
}

/* Remove a mounted volume: its root must be empty, and it must have no
 * snapshots left. */
int siefs_rmvol(siefs_t *fs, uint64_t g, const char *name)
{
    inode_t d;
    uint64_t dir, x;
    size_t len;
    int e;
    BEGIN(fs);
    PICK(g, dir);
    volx_t *pv = fs->cv, *v;
    if ((e = ro(fs, dir)) || (e = name_ok(name, &len)) || (e = iget(fs, dir, &d)) || (e = dir_find(fs, dir, name, len, &x, 0)))
        return (int)end(fs, e);
    if (!(x >> VSHIFT) || !(v = vol_get(fs, VOLOF(x))) || v == pv) return (int)end(fs, E(EINVAL));   /* not a volume */
    if (v->locked) return (int)end(fs, E(EACCES));
    for (uint32_t i = 0; i < fs->nvols; i++)
        if ((fs->vols[i]->v.flags & V_SNAP) && fs->vols[i]->v.parent == v->id) return (int)end(fs, E(EBUSY));
    fs->cv = v;
    if ((e = dir_empty(fs, SIEFS_ROOT)) <= 0) return (int)end(fs, e ? e : E(ENOTEMPTY));
    if ((e = space_reserve(fs, 16)) || (e = commit(fs, 0)) || (e = vol_free_tree(fs, v))) return (int)end(fs, e);
    fs->cv = pv;
    fs->opmod = 1;
    if ((e = dir_del(fs, dir, name, len))) return (int)end(fs, e);
    d.nlink--;
    d.mtime = d.ctime = fs->env.now();
    if ((e = iput(fs, dir, &d)) || (e = t_delete(fs, &fs->rt, K(v->id, T_VOLUME, 0)))) return (int)end(fs, e);
    vol_drop(fs, v);
    fs->changed = 1;
    return (int)end(fs, 0);
}

long siefs_readlink(siefs_t *fs, uint64_t g, char *buf, size_t size)
{
    inode_t in;
    uint8_t b[INLINE_MAX];
    uint64_t ino;
    BEGIN(fs);
    PICK(g, ino);
    int e = iget(fs, ino, &in);
    if (e) return end(fs, e);
    if (!ISLNK(in.mode)) return end(fs, E(EINVAL));
    if ((e = t_get(fs, FT, K(ino, T_INLINE, 0), b, sizeof b)) < 0) return end(fs, e);
    size_t n = (size_t)e < size ? (size_t)e : size;
    memcpy(buf, b, n);
    return end(fs, (long)n);
}

int siefs_link(siefs_t *fs, uint64_t gi, uint64_t gd, const char *name)
{
    inode_t in, d;
    uint64_t x, ino, dir;
    size_t len;
    int e;
    BEGIN(fs);
    PICK(gi, ino);
    volx_t *v = fs->cv;
    PICK(gd, dir);
    if (fs->cv != v) return (int)end(fs, E(EXDEV));
    if ((e = ro(fs, dir)) || (e = name_ok(name, &len)) || (e = iget(fs, ino, &in)) || (e = iget(fs, dir, &d))) return (int)end(fs, e);
    if (ISDIR(in.mode)) return (int)end(fs, E(EPERM));
    if (!ISDIR(d.mode)) return (int)end(fs, E(ENOTDIR));
    if (in.nlink >= 0xFFFF) return (int)end(fs, E(EMLINK));
    if (!(e = dir_find(fs, dir, name, len, &x, 0))) return (int)end(fs, E(EEXIST));
    if (e != E(ENOENT) || (e = space_reserve(fs, 8))) return (int)end(fs, e);
    int64_t t = fs->env.now();
    in.nlink++; in.ctime = t;
    d.mtime = d.ctime = t;
    if (!(e = dir_add(fs, dir, name, len, ino, (int)(in.mode >> 12))) && !(e = iput(fs, ino, &in))) e = iput(fs, dir, &d);
    return (int)end(fs, e);
}

/* Remove a name. A file disappears with its last name; a directory must
 * be empty. (There are no "open but unlinked" files here: the server keeps
 * those alive itself if it wants to.) */
static int remove_name(siefs_t *fs, uint64_t dir, const char *name, int wantdir)
{
    inode_t d, in;
    uint64_t ino;
    size_t len;
    int e, type;
    if ((e = name_ok(name, &len)) || (e = iget(fs, dir, &d))) return e;
    if (!ISDIR(d.mode)) return E(ENOTDIR);
    if ((e = dir_find(fs, dir, name, len, &ino, &type))) return e;
    if (ino >> VSHIFT) return E(EBUSY);                 /* a volume is mounted there */
    if ((e = iget(fs, ino, &in))) return e;
    if (wantdir && !ISDIR(in.mode)) return E(ENOTDIR);
    if (!wantdir && ISDIR(in.mode)) return E(EISDIR);
    if (wantdir && (e = dir_empty(fs, ino)) <= 0) return e ? e : E(ENOTEMPTY);
    if ((e = space_reserve(fs, 8)) || (e = dir_del(fs, dir, name, len))) return e;
    int64_t t = fs->env.now();
    if (wantdir) { d.nlink--; e = free_object(fs, ino); }
    else if (--in.nlink) { in.ctime = t; e = iput(fs, ino, &in); }
    else e = free_object(fs, ino);
    d.mtime = d.ctime = t;
    return e ? e : iput(fs, dir, &d);
}

static int unname(siefs_t *fs, uint64_t g, const char *name, int wantdir)
{
    uint64_t l;
    int e;
    BEGIN(fs);
    PICK(g, l);
    return (int)end(fs, (e = ro(fs, l)) ? e : remove_name(fs, l, name, wantdir));
}
int siefs_unlink(siefs_t *fs, uint64_t dir, const char *name) { return unname(fs, dir, name, 0); }
int siefs_rmdir(siefs_t *fs, uint64_t dir, const char *name)  { return unname(fs, dir, name, 1); }

/* Rename: atomic like everything else, since the whole operation lands in
 * one commit. A target that exists is replaced (a directory only by a
 * directory, and only if empty). */
static int do_rename(siefs_t *fs, uint64_t sdir, const char *sname, uint64_t ddir, const char *dname)
{
    inode_t sd, dd, in, din;
    uint64_t ino, dino;
    size_t sl, dl;
    int e, ty, dty;
    if ((e = name_ok(sname, &sl)) || (e = name_ok(dname, &dl)) || (e = iget(fs, sdir, &sd)) || (e = iget(fs, ddir, &dd))) return e;
    if (!ISDIR(sd.mode) || !ISDIR(dd.mode)) return E(ENOTDIR);
    if ((e = dir_find(fs, sdir, sname, sl, &ino, &ty))) return e;
    if (ino >> VSHIFT) return E(EBUSY);                 /* a mounted volume stays where it is */
    if ((e = iget(fs, ino, &in))) return e;
    int isd = ISDIR(in.mode), exists = !(e = dir_find(fs, ddir, dname, dl, &dino, &dty));
    if (!exists && e != E(ENOENT)) return e;
    if (exists && dino >> VSHIFT) return E(EBUSY);
    if (exists && dino == ino) return 0;                /* two names of the same object */
    for (uint64_t p = ddir; isd; ) {                    /* a directory cannot move into itself */
        inode_t pi;
        if (p == ino) return E(EINVAL);
        if (p == SIEFS_ROOT) break;
        if ((e = iget(fs, p, &pi))) return e;
        p = pi.parent;
    }
    if (exists) {
        if ((e = iget(fs, dino, &din))) return e;
        if (isd && !ISDIR(din.mode)) return E(ENOTDIR);
        if (!isd && ISDIR(din.mode)) return E(EISDIR);
        if (ISDIR(din.mode) && (e = dir_empty(fs, dino)) <= 0) return e ? e : E(ENOTEMPTY);
    }
    if ((e = space_reserve(fs, 16))) return e;

    int64_t t = fs->env.now();
    inode_t *ps = &sd, *pd = sdir == ddir ? &sd : &dd;
    if (exists) {
        if ((e = dir_del(fs, ddir, dname, dl))) return e;
        if (ISDIR(din.mode)) { pd->nlink--; e = free_object(fs, dino); }
        else if (--din.nlink) { din.ctime = t; e = iput(fs, dino, &din); }
        else e = free_object(fs, dino);
        if (e) return e;
    }
    if ((e = dir_del(fs, sdir, sname, sl)) || (e = dir_add(fs, ddir, dname, dl, ino, ty))) return e;
    in.ctime = t;
    if (sdir != ddir) {                 /* a file's parent follows it too: finding its path stays cheap */
        in.parent = ddir;
        if (isd) { ps->nlink--; pd->nlink++; }
    }
    ps->mtime = ps->ctime = pd->mtime = pd->ctime = t;
    if ((e = iput(fs, ino, &in)) || (e = iput(fs, sdir, ps))) return e;
    return sdir != ddir ? iput(fs, ddir, pd) : 0;
}

int siefs_rename(siefs_t *fs, uint64_t gs, const char *sname, uint64_t gd, const char *dname)
{
    uint64_t sdir, ddir;
    int e;
    BEGIN(fs);
    PICK(gs, sdir);
    volx_t *v = fs->cv;
    PICK(gd, ddir);
    if (fs->cv != v) return (int)end(fs, E(EXDEV));
    if ((e = ro(fs, sdir)) || (e = ro(fs, ddir))) return (int)end(fs, e);
    return (int)end(fs, do_rename(fs, sdir, sname, ddir, dname));
}

/* readdir: "." and "..", then the entries in hash order. The cursor is
 * the position of the next entry: (hash << 8 | index in its item). */
int siefs_readdir(siefs_t *fs, uint64_t g, uint64_t *cursor, siefs_dirent_t *de)
{
    inode_t d;
    path_t pa;
    uint64_t dir;
    int e;
    BEGIN(fs);
    PICK(g, dir);
    if ((e = iget(fs, dir, &d))) return (int)end(fs, e);
    if (!ISDIR(d.mode)) return (int)end(fs, E(ENOTDIR));
    if (*cursor < 2) {
        de->ino = *cursor ? ref_dec(fs, d.parent) : G(fs, dir);
        de->type = SIEFS_IFDIR >> 12;
        de->name[0] = '.';
        de->name[1] = *cursor ? '.' : 0;
        de->name[2] = 0;
        (*cursor)++;
        return (int)end(fs, 1);
    }
    if (dir == SNAPDIR) {                               /* the snapshots, oldest first */
        volx_t *best = 0;
        uint64_t after = *cursor == 2 ? 0 : *cursor - 2;   /* (the last one listed: its id) */
        for (uint32_t i = 0; i < fs->nvols; i++) {
            volx_t *v = fs->vols[i];
            if ((v->v.flags & V_SNAP) && v->v.parent == fs->cv->id && v->id > after && (!best || v->id < best->id)) best = v;
        }
        if (!best) return (int)end(fs, 0);
        de->ino = best->id << VSHIFT | SIEFS_ROOT;
        de->type = SIEFS_IFDIR >> 12;
        memcpy(de->name, best->name, strlen(best->name) + 1);
        *cursor = best->id + 2;
        return (int)end(fs, 1);
    }
    uint64_t ch = *cursor == 2 ? 0 : *cursor >> 8, ci = *cursor == 2 ? 0 : *cursor & 0xFF;
    for (int r = t_seek(fs, FT, K(dir, T_DIRENT, ch), &pa); ; r = t_next(fs, &pa)) {
        if (r <= 0) return (int)end(fs, r);
        skey_t k = pkey(&pa);
        if (k.id != dir || KTYPE(k.off) != T_DIRENT) return (int)end(fs, 0);
        uint64_t h = k.off & OFFMASK;
        uint8_t *b = pval(&pa);
        int n = (int)plen(&pa), p = 0, idx = 0;
        for (; p + 10 <= n; p += 10 + b[p + 9], idx++) {
            if (h == ch && (uint64_t)idx < ci) continue;
            memcpy(&de->ino, b + p, 8);
            de->ino = ref_dec(fs, de->ino);
            de->type = b[p + 8];
            memcpy(de->name, b + p + 10, b[p + 9]);
            de->name[b[p + 9]] = 0;
            *cursor = h << 8 | (uint64_t)(idx + 1);
            return (int)end(fs, 1);
        }
    }
}

/* ---- File contents. An extent item, from and to its disk form (see
 * siefs_int.h). */
int ext_dec(const void *v, unsigned len, extent_t *x)
{
    const uint8_t *b = v;
    if (len < 16) return E(EIO);
    memcpy(&x->blk, b, 8); memcpy(&x->n, b + 8, 4); memcpy(&x->flags, b + 12, 4);
    if (!x->n || x->n > EXT_MAX || ((x->flags & X_ENC) && !(x->flags & X_GEN))) return E(EIO);
    unsigned p = 16 + 4 * x->n;
    if (len != p + (x->flags & X_GEN ? 8 : 0) + (x->flags & X_ENC ? 8 + 16 * x->n : 0)) return E(EIO);
    memcpy(x->csum, b + 16, 4 * x->n);
    x->gen = 0; x->salt = 0;
    if (x->flags & X_GEN) { memcpy(&x->gen, b + p, 8); p += 8; }
    if (x->flags & X_ENC) { memcpy(&x->salt, b + p, 4); memcpy(x->mac, b + p + 8, 16 * x->n); }
    return 0;
}
unsigned ext_enc(const extent_t *x, void *v)
{
    uint8_t *b = v;
    unsigned p = 16 + 4 * x->n;
    memcpy(b, &x->blk, 8); memcpy(b + 8, &x->n, 4); memcpy(b + 12, &x->flags, 4);
    memcpy(b + 16, x->csum, 4 * x->n);
    if (x->flags & X_GEN) { memcpy(b + p, &x->gen, 8); p += 8; }
    if (x->flags & X_ENC) { memcpy(b + p, &x->salt, 4); memset(b + p + 4, 0, 4); memcpy(b + p + 8, x->mac, 16 * x->n); p += 8 + 16 * x->n; }
    return p;
}
static int ext_store(siefs_t *fs, uint64_t ino, uint64_t at, const extent_t *x, int update)
{
    uint8_t b[MAXITEM];
    unsigned n = ext_enc(x, b);
    return update ? t_update(fs, FT, K(ino, T_EXTENT, at * BS), b, n) : t_insert(fs, FT, K(ino, T_EXTENT, at * BS), b, n);
}

/* The last blocks of an extent from block d on (when its head is cut off). */
static void ext_tail(extent_t *y, const extent_t *x, uint32_t d)
{
    *y = *x;
    y->blk = x->blk + d; y->n = x->n - d;
    memmove(y->csum, x->csum + d, 4 * y->n);
    memmove(y->mac, x->mac + d, 16 * y->n);
}

static int ext_get(path_t *pa, extent_t *x, uint64_t *start)
{
    int e = ext_dec(pval(pa), plen(pa), x);
    if (e) return e;
    *start = (pkey(pa).off & OFFMASK) / BS;
    return 0;
}

/* The extent holding file block b: 1 (in *x, starting at file block *s), 0 for a hole. */
static int ext_find(siefs_t *fs, uint64_t ino, uint64_t b, extent_t *x, uint64_t *s)
{
    path_t pa;
    int r = t_floor(fs, FT, K(ino, T_EXTENT, b * BS), &pa);
    if (r <= 0) return r;
    if (pkey(&pa).id != ino || KTYPE(pkey(&pa).off) != T_EXTENT) return 0;
    if ((r = ext_get(&pa, x, s))) return r;
    return b < *s + x->n;
}

/* Read n blocks of an extent, from block `from` on, checking each one. */
static int rd_ext(siefs_t *fs, const extent_t *x, uint32_t from, uint32_t n, uint8_t *dst)
{
    int e = dev_read(fs, x->blk + from, n, dst);
    for (uint32_t i = 0; !e && i < n; i++)
        if (siefs_crc32c(0, dst + i * BS, BS) != x->csum[from + i]) e = E(EIO);
    return e;
}

static int rd_block(siefs_t *fs, uint64_t ino, uint64_t b, uint8_t *dst)
{
    extent_t x;
    uint64_t s;
    int r = ext_find(fs, ino, b, &x, &s);
    if (r <= 0) { memset(dst, 0, BS); return r; }
    return rd_ext(fs, &x, (uint32_t)(b - s), 1, dst);
}

long siefs_read(siefs_t *fs, uint64_t g, uint64_t off, void *buf, size_t n)
{
    inode_t in;
    uint8_t *out = buf;
    int e;
    BEGIN(fs);
    uint64_t ino;
    PICK(g, ino);
    if ((e = iget(fs, ino, &in))) return end(fs, e);
    if (ISDIR(in.mode)) return end(fs, E(EISDIR));
    if (off >= in.size) return end(fs, 0);
    if (n > in.size - off) n = (size_t)(in.size - off);
    if (in.flags & F_INLINE) {
        uint8_t b[INLINE_MAX];
        memset(b, 0, sizeof b);
        if ((e = t_get(fs, FT, K(ino, T_INLINE, 0), b, sizeof b)) < 0) return end(fs, e);
        memcpy(out, b + off, n);
        return end(fs, (long)n);
    }
    for (size_t done = 0; done < n; ) {
        uint64_t pos = off + done, b = pos / BS, at = pos % BS, s;
        extent_t x;
        size_t c;
        int r = ext_find(fs, ino, b, &x, &s);
        if (r < 0) return end(fs, r);
        if (!r) {                                       /* a hole reads as zeros */
            c = BS - at < n - done ? BS - at : n - done;
            memset(out + done, 0, c);
        } else {
            uint64_t want = (at + (n - done) + BS - 1) / BS, cnt = x.n - (b - s);
            if (cnt > want) cnt = want;
            if ((e = rd_ext(fs, &x, (uint32_t)(b - s), (uint32_t)cnt, fs->io))) return end(fs, e);
            c = cnt * BS - at < n - done ? cnt * BS - at : n - done;
            memcpy(out + done, fs->io + at, c);
        }
        done += c;
    }
    return end(fs, (long)n);
}

/* Forget the file's blocks in [b0, b1): extents inside go, extents across
 * the edges are cut. */
static int punch(siefs_t *fs, uint64_t ino, uint64_t b0, uint64_t b1)
{
    for (uint64_t cur = b0; cur < b1; ) {
        extent_t x, y;
        uint64_t s;
        int r = ext_find(fs, ino, cur, &x, &s), e;
        if (r < 0) return r;
        if (!r) {                                       /* in a hole: go to the next extent */
            path_t pa;
            if ((r = t_seek(fs, FT, K(ino, T_EXTENT, cur * BS), &pa)) <= 0) return r;
            if (pkey(&pa).id != ino || KTYPE(pkey(&pa).off) != T_EXTENT) return 0;
            if ((r = ext_get(&pa, &x, &s))) return r;
            if (s >= b1) return 0;
        }
        uint64_t o0 = s > b0 ? s : b0, o1 = s + x.n < b1 ? s + x.n : b1;
        if ((e = t_delete(fs, FT, K(ino, T_EXTENT, s * BS)))) return e;
        blk_release(fs, fs->cv->id, x.blk + (o0 - s), o1 - o0, x.gen);
        if (s < o0) {                                   /* keep the head */
            y = x; y.n = (uint32_t)(o0 - s);
            if ((e = ext_store(fs, ino, s, &y, 0))) return e;
        }
        if (s + x.n > o1) {                             /* keep the tail */
            ext_tail(&y, &x, (uint32_t)(o1 - s));
            if ((e = ext_store(fs, ino, o1, &y, 0))) return e;
        }
        cur = o1;
    }
    return 0;
}

/* Record that file blocks [b, b+n) are now at disk blocks [blk, blk+n),
 * written by this transaction, extending the previous extent when it
 * continues on disk and was written by this transaction too (an extent has
 * one gen; encrypted ones one salt, so they are never extended). */
static int ext_put(siefs_t *fs, uint64_t ino, uint64_t b, const extent_t *nx)
{
    extent_t x;
    uint64_t s;
    if (b && !(nx->flags & X_ENC)) {
        int r = ext_find(fs, ino, b - 1, &x, &s);
        if (r < 0) return r;
        if (r && s + x.n == b && x.blk + x.n == nx->blk && x.n + nx->n <= EXT_MAX && x.gen == fs->txn && !(x.flags & X_ENC)) {
            memcpy(x.csum + x.n, nx->csum, 4 * nx->n);
            x.n += nx->n;
            return ext_store(fs, ino, s, &x, 1);
        }
    }
    return ext_store(fs, ino, b, nx, 0);
}

/* Write file bytes [off, off+n) (src 0: zeros) into new blocks, up to
 * EXT_MAX at a time. Partly written blocks keep the rest of their old
 * contents (read first). Copy-on-write: the old blocks are freed. */
static int wblocks(siefs_t *fs, uint64_t ino, uint64_t off, const uint8_t *src, size_t n)
{
    uint64_t b = off / BS, last = (off + n - 1) / BS;
    while (b <= last) {
        uint64_t got, want = last - b + 1 < EXT_MAX ? last - b + 1 : EXT_MAX;
        uint64_t at = blk_alloc(fs, want, &got);
        extent_t nx = { .blk = at, .n = (uint32_t)got, .flags = X_GEN, .gen = fs->txn };
        int e;
        if (!got) return E(ENOSPC);
        for (uint64_t i = 0; i < got; i++) {
            uint8_t *d = fs->io + i * BS;
            uint64_t lo = (b + i) * BS, hi = lo + BS;
            uint64_t c0 = off > lo ? off : lo, c1 = off + n < hi ? off + n : hi;
            if ((c0 > lo || c1 < hi) && (e = rd_block(fs, ino, b + i, d))) return e;
            if (src) memcpy(d + (c0 - lo), src + (c0 - off), c1 - c0);
            else memset(d + (c0 - lo), 0, c1 - c0);
            nx.csum[i] = siefs_crc32c(0, d, BS);
        }
        if ((e = dev_write(fs, at, (uint32_t)got, fs->io)) || (e = punch(fs, ino, b, b + got)) ||
            (e = ext_put(fs, ino, b, &nx))) return e;
        b += got;
    }
    return 0;
}

/* A small file leaves the tree for blocks. */
static int uninline(siefs_t *fs, uint64_t ino, inode_t *in)
{
    uint8_t b[INLINE_MAX];
    int e = t_get(fs, FT, K(ino, T_INLINE, 0), b, sizeof b);
    if (e < 0 && e != E(ENOENT)) return e;
    if (e >= 0 && (e = t_delete(fs, FT, K(ino, T_INLINE, 0)))) return e;
    in->flags &= ~F_INLINE;
    return in->size ? wblocks(fs, ino, 0, b, (size_t)in->size) : 0;
}

long siefs_write(siefs_t *fs, uint64_t g, uint64_t off, const void *buf, size_t n)
{
    inode_t in;
    int e;
    BEGIN(fs);
    uint64_t ino;
    PICK(g, ino);
    { int r_ = ro(fs, ino); if (r_) return end(fs, r_); }
    if ((e = iget(fs, ino, &in))) return end(fs, e);
    if (!ISREG(in.mode)) return end(fs, ISDIR(in.mode) ? E(EISDIR) : E(EINVAL));
    if (!n) return end(fs, 0);
    if (off + n < off || off + n > MAXBLK * BS) return end(fs, E(EFBIG));
    uint64_t size = off + n > in.size ? off + n : in.size;
    if ((e = space_reserve(fs, (n + BS - 1) / BS + 2 + 4 * (n / (EXT_MAX * BS) + 2)))) return end(fs, e);
    if (size <= INLINE_MAX && (in.flags & F_INLINE || !in.size)) {   /* small: stays in the tree */
        uint8_t b[INLINE_MAX];
        memset(b, 0, sizeof b);
        if (in.size && (e = t_get(fs, FT, K(ino, T_INLINE, 0), b, sizeof b)) < 0) return end(fs, e);
        memcpy(b + off, buf, n);
        e = t_put(fs, FT, K(ino, T_INLINE, 0), b, (unsigned)size);
        in.flags |= F_INLINE;
    } else if (!(in.flags & F_INLINE) || !(e = uninline(fs, ino, &in)))
        e = wblocks(fs, ino, off, buf, n);
    if (e) return end(fs, e);
    in.size = size;
    in.mtime = in.ctime = fs->env.now();
    return end(fs, (e = iput(fs, ino, &in)) ? e : (long)n);
}

static int do_truncate(siefs_t *fs, uint64_t ino, inode_t *in, uint64_t size)
{
    extent_t x;
    uint64_t s;
    int e = 0;
    if (!ISREG(in->mode)) return ISDIR(in->mode) ? E(EISDIR) : E(EINVAL);
    if (size > MAXBLK * BS) return E(EFBIG);
    if ((e = space_reserve(fs, 4))) return e;
    if (in->flags & F_INLINE) {
        if (size > INLINE_MAX) e = uninline(fs, ino, in);
        else if (!size) { e = t_delete(fs, FT, K(ino, T_INLINE, 0)); in->flags &= ~F_INLINE; }
        else {
            uint8_t b[INLINE_MAX];
            memset(b, 0, sizeof b);
            if ((e = t_get(fs, FT, K(ino, T_INLINE, 0), b, sizeof b)) >= 0) {
                if (size > in->size) memset(b + in->size, 0, size - in->size);
                e = t_update(fs, FT, K(ino, T_INLINE, 0), b, (unsigned)size);
            }
        }
    } else if (size < in->size) {
        e = punch(fs, ino, (size + BS - 1) / BS, MAXBLK);
        if (!e && size % BS && (e = ext_find(fs, ino, size / BS, &x, &s)) > 0)   /* zero the last block's tail */
            e = wblocks(fs, ino, size, 0, BS - size % BS);
    }
    if (e < 0) return e;
    in->size = size;
    in->mtime = in->ctime = fs->env.now();
    return iput(fs, ino, in);
}

int siefs_truncate(siefs_t *fs, uint64_t g, uint64_t size)
{
    inode_t in;
    BEGIN(fs);
    uint64_t ino;
    PICK(g, ino);
    { int r_ = ro(fs, ino); if (r_) return end(fs, r_); }
    int e = iget(fs, ino, &in);
    return (int)end(fs, e ? e : do_truncate(fs, ino, &in, size));
}

int siefs_stat(siefs_t *fs, uint64_t g, siefs_stat_t *st)
{
    inode_t in;
    BEGIN(fs);
    uint64_t ino;
    PICK(g, ino);
    int e = iget(fs, ino, &in);
    if (!e) *st = (siefs_stat_t){ G(fs, ino), in.size, (in.flags & F_INLINE) ? 0 : (in.size + BS - 1) / BS, ref_dec(fs, in.parent),
                                  in.mode, in.uid, in.gid, in.nlink, in.atime, in.mtime, in.ctime, in.btime };
    return (int)end(fs, e);
}

int siefs_setattr(siefs_t *fs, uint64_t g, const siefs_stat_t *st, unsigned mask)
{
    inode_t in;
    int e;
    BEGIN(fs);
    uint64_t ino;
    PICK(g, ino);
    { int r_ = ro(fs, ino); if (r_) return end(fs, r_); }
    if ((e = iget(fs, ino, &in))) return (int)end(fs, e);
    if ((mask & SIEFS_SET_SIZE) && (e = do_truncate(fs, ino, &in, st->size))) return (int)end(fs, e);
    if ((e = space_reserve(fs, 2))) return (int)end(fs, e);
    if (mask & SIEFS_SET_MODE)  in.mode = (in.mode & SIEFS_IFMT) | (st->mode & 07777);
    if (mask & SIEFS_SET_UID)   in.uid = st->uid;
    if (mask & SIEFS_SET_GID)   in.gid = st->gid;
    if (mask & SIEFS_SET_ATIME) in.atime = st->atime;
    if (mask & SIEFS_SET_MTIME) in.mtime = st->mtime;
    in.ctime = fs->env.now();
    return (int)end(fs, iput(fs, ino, &in));
}

int siefs_access(const siefs_stat_t *st, uint32_t uid, uint32_t gid, int want)
{
    if (!uid) return !(want & 1) || st->mode & 0111 || ISDIR(st->mode) ? 0 : E(EACCES);   /* root */
    int shift = uid == st->uid ? 6 : gid == st->gid ? 3 : 0;
    return ((int)(st->mode >> shift) & want) == want ? 0 : E(EACCES);
}

/* ---- Extended attributes: in item (ino, XATTR, hash of the name), a list of
 *   u8 name length | u16 value length | name | value
 * Values up to 255 bytes are also indexed: an item (index id, XINDEX, ino)
 * per attribute, so siefs_find walks just the matching objects. */
static int xscan(const uint8_t *b, int n, const char *name, size_t nl, int *pos, int *vl)
{
    for (int p = 0; p + 3 <= n; ) {
        int l = b[p], v = b[p + 1] | b[p + 2] << 8;
        if ((size_t)l == nl && !memcmp(b + p + 3, name, nl)) { *pos = p; *vl = v; return 1; }
        p += 3 + l + v;
    }
    return 0;
}

static void unindex(siefs_t *fs, uint64_t ino, const uint8_t *b, int n)
{
    for (int p = 0; p + 3 <= n; ) {
        int l = b[p], v = b[p + 1] | b[p + 2] << 8;
        if (v <= 255) t_delete(fs, FT, K(xindex_id((const char *)b + p + 3, l, b + p + 3 + l, v), T_XINDEX, ino));
        p += 3 + l + v;
    }
}

static int xget(siefs_t *fs, uint64_t ino, const char *name, size_t nl, uint8_t *b, int *n, int *pos, int *vl)
{
    *n = t_get(fs, FT, K(ino, T_XATTR, name_hash(name, nl)), b, MAXITEM);
    if (*n == E(ENOENT)) *n = 0;
    if (*n < 0) return *n;
    return xscan(b, *n, name, nl, pos, vl) ? 0 : E(ENODATA);
}

long siefs_getxattr(siefs_t *fs, uint64_t g, const char *name, void *buf, size_t size)
{
    uint8_t b[MAXITEM];
    size_t nl = strlen(name);
    int n, pos, vl, e;
    inode_t in;
    BEGIN(fs);
    uint64_t ino;
    PICK(g, ino);
    if (!nl || nl > 255) return end(fs, E(EINVAL));
    if ((e = iget(fs, ino, &in)) || (e = xget(fs, ino, name, nl, b, &n, &pos, &vl))) return end(fs, e);
    if (!size) return end(fs, vl);
    if (size < (size_t)vl) return end(fs, E(ERANGE));
    memcpy(buf, b + pos + 3 + nl, vl);
    return end(fs, vl);
}

static int xchange(siefs_t *fs, uint64_t ino, const char *name, const void *val, size_t len, int remove)
{
    uint8_t b[MAXITEM];
    size_t nl = strlen(name);
    int n, pos, vl, e;
    inode_t in;
    skey_t k = K(ino, T_XATTR, name_hash(name, nl));
    if (!nl) return E(EINVAL);
    if (nl > 255) return E(ENAMETOOLONG);
    if (len > SIEFS_XVAL_MAX) return E(E2BIG);
    if ((e = iget(fs, ino, &in))) return e;
    e = xget(fs, ino, name, nl, b, &n, &pos, &vl);
    if (e && (e != E(ENODATA) || remove)) return e;
    int have = !e, after = n - (have ? 3 + (int)nl + vl : 0) + (remove ? 0 : 3 + (int)(nl + len));
    if (after > MAXITEM) return E(E2BIG);
    if ((e = space_reserve(fs, 4))) return e;
    if (have) {                                         /* take the old one out */
        if (vl <= 255) t_delete(fs, FT, K(xindex_id(name, nl, b + pos + 3 + nl, vl), T_XINDEX, ino));
        memmove(b + pos, b + pos + 3 + nl + vl, n - pos - 3 - nl - vl);
        n -= 3 + (int)nl + vl;
    }
    if (!remove) {
        b[n] = (uint8_t)nl; b[n + 1] = (uint8_t)len; b[n + 2] = (uint8_t)(len >> 8);
        memcpy(b + n + 3, name, nl);
        if (len) memcpy(b + n + 3 + nl, val, len);
        n += 3 + (int)(nl + len);
        if (len <= 255 && (e = t_put(fs, FT, K(xindex_id(name, nl, val, len), T_XINDEX, ino), b, 0))) return e;
    }
    if ((e = n ? t_put(fs, FT, k, b, n) : t_delete(fs, FT, k))) return e;
    in.ctime = fs->env.now();
    return iput(fs, ino, &in);
}

int siefs_setxattr(siefs_t *fs, uint64_t g, const char *name, const void *val, size_t len)
{
    BEGIN(fs);
    uint64_t ino;
    PICK(g, ino);
    { int r_ = ro(fs, ino); if (r_) return end(fs, r_); }
    return (int)end(fs, xchange(fs, ino, name, val, len, 0));
}

int siefs_removexattr(siefs_t *fs, uint64_t g, const char *name)
{
    BEGIN(fs);
    uint64_t ino;
    PICK(g, ino);
    { int r_ = ro(fs, ino); if (r_) return end(fs, r_); }
    return (int)end(fs, xchange(fs, ino, name, 0, 0, 1));
}

long siefs_listxattr(siefs_t *fs, uint64_t g, char *buf, size_t size)
{
    path_t pa;
    long total = 0;
    inode_t in;
    int r;
    BEGIN(fs);
    uint64_t ino;
    PICK(g, ino);
    if ((r = iget(fs, ino, &in))) return end(fs, r);
    for (r = t_seek(fs, FT, K(ino, T_XATTR, 0), &pa); r > 0; r = t_next(fs, &pa)) {
        if (pkey(&pa).id != ino || KTYPE(pkey(&pa).off) != T_XATTR) break;
        uint8_t *b = pval(&pa);
        int n = (int)plen(&pa);
        for (int p = 0; p + 3 <= n; p += 3 + b[p] + (b[p + 1] | b[p + 2] << 8)) {
            if (size) {
                if ((size_t)total + b[p] + 1 > size) return end(fs, E(ERANGE));
                memcpy(buf + total, b + p + 3, b[p]);
                buf[total + b[p]] = 0;
            }
            total += b[p] + 1;
        }
    }
    return end(fs, r < 0 ? r : total);
}

/* Every volume that is not a snapshot and is open, in id order: the
 * cursor is (volume << 48 | the next object id). */
int siefs_find(siefs_t *fs, const char *name, const void *val, size_t len, uint64_t *cursor, uint64_t *ino)
{
    uint8_t b[MAXITEM];
    path_t pa;
    size_t nl = strlen(name);
    int r;
    BEGIN(fs);
    if (!nl || nl > 255 || len > 255) return (int)end(fs, E(EINVAL));
    uint64_t id = xindex_id(name, nl, val, len), vid = VOLOF(*cursor), at = *cursor & LMASK;
    for (;;) {
        volx_t *v = 0;                                  /* the volume vid, or the next one */
        for (uint32_t i = 0; i < fs->nvols; i++) {
            volx_t *w = fs->vols[i];
            if (w->id >= vid && !(w->v.flags & V_SNAP) && !w->locked && (!v || w->id < v->id)) v = w;
        }
        if (!v) return (int)end(fs, 0);
        if (v->id != vid) { vid = v->id; at = 0; }
        fs->cv = v;
        for (;; at++) {
            if ((r = t_seek(fs, FT, K(id, T_XINDEX, at), &pa)) < 0) return (int)end(fs, r);
            if (!r || pkey(&pa).id != id || KTYPE(pkey(&pa).off) != T_XINDEX) break;
            at = pkey(&pa).off & OFFMASK;
            int n, pos, vl;                             /* the hash matched: check the real value */
            if (!xget(fs, at, name, nl, b, &n, &pos, &vl) && (size_t)vl == len && !memcmp(b + pos + 3 + nl, val, len)) {
                *ino = G(fs, at);
                *cursor = vid << VSHIFT | (at + 1);
                return (int)end(fs, 1);
            }
        }
        vid++; at = 0;
    }
}
