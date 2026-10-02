/*
 * tmpfs.c - A memory file system (mounted on /tmp).
 *
 * Each node is an in-memory inode whose tnode holds the directory entries
 * or the data pages (files and symbolic links).  A node lives as long as it
 * has a link or a reference.  Directory offsets are per-entry cookies that
 * never change, so readdir resumes correctly around insertions/removals.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "fs.h"
#include "proc.h"
#include "mm.h"

struct tdirent {
    struct tdirent *next;
    struct inode *ip;
    uint64_t cookie;
    uint8_t len;
    char name[];
};

struct tnode {
    struct inode *parent;         /* directories */
    struct tdirent *ents;
    uint64_t next_cookie;
    uint64_t *pages;              /* data: physical addresses, 0 = hole */
    size_t npages;
    size_t nresident;             /* pages[] entries that are not holes */
};

struct tmpfs {
    uint32_t next_ino;
    uint64_t used_pages, max_pages;
    uint64_t nodes;
};

static const struct fs_ops tmpfs_ops;

static struct tnode *TN(struct inode *ip) { return ip->priv; }
static struct tmpfs *TFS(struct inode *ip) { return ip->fs->priv; }

static struct inode *tnode_new(struct fs *fs, uint16_t mode, int uid, int gid)
{
    struct tmpfs *t = fs->priv;
    struct inode *ip = vfs_new_inode(fs, t->next_ino, mode, uid, gid);
    struct tnode *tn = kzalloc(sizeof(*tn));
    if (!ip || !tn) {
        if (ip)
            vfs_free_inode(ip);
        kfree(tn);
        return NULL;
    }
    t->next_ino++;
    t->nodes++;
    tn->next_cookie = 2;
    ip->priv = tn;
    return ip;
}

/*
 * A file of tmpfs fs that no directory names (memfd_create): freed with its
 * last reference.  NULL if fs is not a tmpfs or memory is short.
 */
struct inode *tmpfs_unnamed(struct fs *fs, int uid, int gid)
{
    if (!fs || fs->ops != &tmpfs_ops)
        return NULL;
    fs_enter(fs);
    struct inode *ip = tnode_new(fs, S_IFREG | 0600, uid, gid);
    if (ip)
        DI(ip)->i_links_count = 0;
    fs_exit(fs);
    return ip;
}

static void data_free_from(struct inode *ip, size_t first)
{
    struct tnode *tn = TN(ip);
    for (size_t i = first; i < tn->npages && tn->nresident; i++)
        if (tn->pages[i]) {
            pmm_unref(tn->pages[i]);             /* shared mappings may still hold it */
            tn->pages[i] = 0;
            tn->nresident--;
            TFS(ip)->used_pages--;
        }
}

static void tmpfs_release(struct inode *ip)
{
    if (DI(ip)->i_links_count)
        return;                                  /* still named in a directory */
    struct tnode *tn = TN(ip);
    data_free_from(ip, 0);
    kfree(tn->pages);
    for (struct tdirent *d = tn->ents, *n; d; d = n) {
        n = d->next;
        kfree(d);
    }
    if (tn->parent)
        iput(tn->parent);
    kfree(tn);
    TFS(ip)->nodes--;
    vfs_free_inode(ip);
}

static void set_blocks(struct inode *ip)
{
    uint64_t n = (uint64_t)TN(ip)->nresident * (PAGE_SIZE / 512);   /* (a count: files may be huge and sparse) */
    DI(ip)->i_blocks_lo = n & 0xFFFFFFFF;
    DI(ip)->i_blocks_high = n >> 32;
}

/* ---------------- data ---------------- */

static long tmpfs_read(struct inode *ip, void *dst, uint64_t off, size_t n)
{
    struct tnode *tn = TN(ip);
    uint64_t size = inode_size(ip);
    if (S_ISDIR(inode_mode(ip)))
        return -EISDIR;
    if (off >= size)
        return 0;
    n = MIN(n, size - off);
    size_t done = 0;
    while (done < n) {
        uint64_t pg = (off + done) / PAGE_SIZE, po = (off + done) % PAGE_SIZE;
        size_t chunk = MIN(n - done, PAGE_SIZE - po);
        if (pg < tn->npages && tn->pages[pg])
            memcpy((uint8_t *)dst + done, (uint8_t *)P2V(tn->pages[pg]) + po, chunk);
        else
            memset((uint8_t *)dst + done, 0, chunk);
        done += chunk;
    }
    return done;
}

static int grow_table(struct tnode *tn, size_t need)
{
    if (need <= tn->npages)
        return 0;
    size_t cap = tn->npages ? tn->npages : 4;
    while (cap < need)
        cap *= 2;
    uint64_t *np = kzalloc(cap * sizeof(uint64_t));
    if (!np)
        return -ENOSPC;
    if (tn->pages)
        memcpy(np, tn->pages, tn->npages * sizeof(uint64_t));
    kfree(tn->pages);
    tn->pages = np;
    tn->npages = cap;
    return 0;
}

static long tmpfs_write(struct inode *ip, const void *src, uint64_t off, size_t n)
{
    struct tnode *tn = TN(ip);
    struct tmpfs *t = TFS(ip);
    if (off + n < off || off + n > (1UL << 40))
        return -EFBIG;
    if (grow_table(tn, (off + n + PAGE_SIZE - 1) / PAGE_SIZE) < 0)
        return -ENOSPC;
    size_t done = 0;
    while (done < n) {
        uint64_t pg = (off + done) / PAGE_SIZE, po = (off + done) % PAGE_SIZE;
        size_t chunk = MIN(n - done, PAGE_SIZE - po);
        if (!tn->pages[pg]) {
            uint64_t pa = t->used_pages < t->max_pages ? pmm_alloc() : 0;
            if (!pa)
                break;
            memset(P2V(pa), 0, PAGE_SIZE);
            tn->pages[pg] = pa;
            tn->nresident++;
            t->used_pages++;
        }
        memcpy((uint8_t *)P2V(tn->pages[pg]) + po, (const uint8_t *)src + done, chunk);
        done += chunk;
    }
    if (off + done > inode_size(ip))
        inode_set_size(ip, off + done);
    if (done)
        inode_touch(ip, true, true);
    set_blocks(ip);
    return done ? (long)done : -ENOSPC;
}

static int tmpfs_truncate(struct inode *ip, uint64_t len)
{
    struct tnode *tn = TN(ip);
    if (len > (1UL << 40))
        return -EFBIG;
    if (len < inode_size(ip)) {
        size_t keep = (len + PAGE_SIZE - 1) / PAGE_SIZE;
        data_free_from(ip, keep);
        if (len % PAGE_SIZE && keep - 1 < tn->npages && tn->pages[keep - 1])
            memset((uint8_t *)P2V(tn->pages[keep - 1]) + len % PAGE_SIZE, 0, PAGE_SIZE - len % PAGE_SIZE);
    }
    inode_set_size(ip, len);
    inode_touch(ip, true, true);
    set_blocks(ip);
    return 0;
}

/* ---------------- directories ---------------- */

static struct tdirent *dfind(struct inode *dir, const char *name, size_t len, struct tdirent ***prevp)
{
    struct tdirent **pp = &TN(dir)->ents;
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->len == len && !memcmp((*pp)->name, name, len)) {
            if (prevp)
                *prevp = pp;
            return *pp;
        }
    return NULL;
}

static int dadd(struct inode *dir, const char *name, size_t len, struct inode *ip)
{
    struct tdirent *d = kmalloc(sizeof(*d) + len + 1);
    if (!d)
        return -ENOSPC;
    d->next = NULL;
    d->ip = ip;
    d->len = len;
    memcpy(d->name, name, len);
    d->name[len] = 0;
    struct tnode *tn = TN(dir);
    d->cookie = tn->next_cookie++;
    struct tdirent **pp = &tn->ents;
    while (*pp)
        pp = &(*pp)->next;
    *pp = d;
    inode_set_size(dir, inode_size(dir) + 1);
    inode_touch(dir, true, true);
    return 0;
}

static int tmpfs_lookup(struct inode *dir, const char *name, size_t len, struct inode **out)
{
    if (len == 1 && name[0] == '.') {
        *out = idup(dir);
        return 0;
    }
    if (len == 2 && name[0] == '.' && name[1] == '.') {
        *out = idup(TN(dir)->parent ? TN(dir)->parent : dir);
        return 0;
    }
    struct tdirent *d = dfind(dir, name, len, NULL);
    if (!d)
        return -ENOENT;
    *out = idup(d->ip);
    return 0;
}

static int tmpfs_readdir(struct inode *dir, uint64_t *off, filldir_t fill, void *arg)
{
    struct tnode *tn = TN(dir);
    if (*off == 0) {
        if (fill(arg, ".", 1, dir->ino, DT_DIR, 1))
            return 0;
        *off = 1;
    }
    if (*off == 1) {
        struct inode *p = tn->parent ? tn->parent : dir;
        if (fill(arg, "..", 2, p->ino, DT_DIR, 2))
            return 0;
        *off = 2;
    }
    for (struct tdirent *d = tn->ents; d; d = d->next) {
        if (d->cookie < *off)
            continue;
        uint16_t m = inode_mode(d->ip);
        int dt = S_ISDIR(m) ? DT_DIR : S_ISREG(m) ? DT_REG : S_ISLNK(m) ? DT_LNK : S_ISCHR(m) ? DT_CHR
               : S_ISBLK(m) ? DT_BLK : S_ISFIFO(m) ? DT_FIFO : DT_SOCK;
        if (fill(arg, d->name, d->len, d->ip->ino, dt, d->cookie + 1))
            return 0;
        *off = d->cookie + 1;
    }
    *off = tn->next_cookie;
    return 0;
}

static int check_new(struct inode *dir, const char *name, size_t *len)
{
    int r = dir_name_ok(name, len);
    if (r < 0)
        return r;
    if ((*len == 1 && name[0] == '.') || (*len == 2 && !memcmp(name, "..", 2)) || dfind(dir, name, *len, NULL))
        return -EEXIST;
    return 0;
}

static int tmpfs_mknod(struct inode *dir, const char *name, uint16_t mode, uint32_t rdev, int uid, int gid,
                        struct inode **out)
{
    size_t len;
    int r = check_new(dir, name, &len);
    if (r < 0)
        return r;
    struct inode *ip = tnode_new(dir->fs, mode, uid, gid);
    if (!ip)
        return -ENOSPC;
    if (S_ISCHR(mode) || S_ISBLK(mode))
        inode_set_rdev(ip, rdev);
    if ((r = dadd(dir, name, len, ip)) < 0) {
        DI(ip)->i_links_count = 0;
        iput(ip);
        return r;
    }
    if (out)
        *out = ip;
    else
        iput(ip);
    return 0;
}

static int tmpfs_mkdir(struct inode *dir, const char *name, uint16_t mode, int uid, int gid)
{
    size_t len;
    int r = check_new(dir, name, &len);
    if (r < 0)
        return r;
    struct inode *ip = tnode_new(dir->fs, S_IFDIR | (mode & 07777), uid, gid);
    if (!ip)
        return -ENOSPC;
    TN(ip)->parent = idup(dir);
    if ((r = dadd(dir, name, len, ip)) < 0) {
        DI(ip)->i_links_count = 0;
        iput(ip);
        return r;
    }
    DI(dir)->i_links_count++;
    iput(ip);
    return 0;
}

static void dremove(struct inode *dir, struct tdirent **pp);

static int tmpfs_symlink(struct inode *dir, const char *name, const char *target, int uid, int gid)
{
    struct inode *ip;
    size_t tlen = strlen(target);
    if (tlen >= PAGE_SIZE)
        return -ENAMETOOLONG;
    int r = tmpfs_mknod(dir, name, S_IFLNK | 0777, 0, uid, gid, &ip);
    if (r < 0)
        return r;
    if (tmpfs_write(ip, target, 0, tlen) != (long)tlen) {
        struct tdirent **pp;
        if (dfind(dir, name, strlen(name), &pp))
            dremove(dir, pp);
        r = -ENOSPC;
    }
    iput(ip);
    return r;
}

static int tmpfs_link(struct inode *dir, const char *name, struct inode *ip)
{
    size_t len;
    if (S_ISDIR(inode_mode(ip)))
        return -EPERM;
    int r = check_new(dir, name, &len);
    if (r < 0)
        return r;
    if ((r = dadd(dir, name, len, ip)) == 0) {
        DI(ip)->i_links_count++;
        inode_touch(ip, false, true);
    }
    return r;
}

/* Remove the entry and drop its link (the node goes when unreferenced). */
static void dremove(struct inode *dir, struct tdirent **pp)
{
    struct tdirent *d = *pp;
    struct inode *ip = d->ip;
    *pp = d->next;
    kfree(d);
    inode_set_size(dir, inode_size(dir) - 1);
    inode_touch(dir, true, true);
    if (S_ISDIR(inode_mode(ip))) {
        DI(ip)->i_links_count = 0;
        DI(dir)->i_links_count--;
    } else if (DI(ip)->i_links_count) {
        DI(ip)->i_links_count--;
    }
    inode_touch(ip, false, true);
    idup(ip);
    iput(ip);                                    /* frees it if unlinked and unreferenced */
}

static int tmpfs_unlink(struct inode *dir, const char *name, bool is_dir)
{
    size_t len = strlen(name);
    if ((len == 1 && name[0] == '.') || (len == 2 && !memcmp(name, "..", 2)))
        return is_dir ? -EINVAL : -EISDIR;
    struct tdirent **pp;
    struct tdirent *d = dfind(dir, name, len, &pp);
    if (!d)
        return -ENOENT;
    bool isd = S_ISDIR(inode_mode(d->ip));
    if (is_dir && !isd)
        return -ENOTDIR;
    if (!is_dir && isd)
        return -EISDIR;
    if (isd && TN(d->ip)->ents)
        return -ENOTEMPTY;
    dremove(dir, pp);
    return 0;
}

static int tmpfs_rename(struct inode *od, const char *on, struct inode *nd, const char *nn)
{
    size_t ol = strlen(on), nl;
    int r = dir_name_ok(nn, &nl);
    if (r < 0)
        return r;
    if ((ol == 1 && on[0] == '.') || (ol == 2 && !memcmp(on, "..", 2)) ||
        (nl == 1 && nn[0] == '.') || (nl == 2 && !memcmp(nn, "..", 2)))
        return -EINVAL;
    struct tdirent **opp, **npp;
    struct tdirent *d = dfind(od, on, ol, &opp);
    if (!d)
        return -ENOENT;
    struct inode *ip = d->ip;
    bool isd = S_ISDIR(inode_mode(ip));
    if (isd)
        for (struct inode *a = nd; a; a = TN(a)->parent)     /* not into itself */
            if (a == ip)
                return -EINVAL;
    struct tdirent *t = dfind(nd, nn, nl, &npp);
    if (t) {
        if (t->ip == ip)
            return 0;
        bool tdir = S_ISDIR(inode_mode(t->ip));
        if (isd && !tdir)
            return -ENOTDIR;
        if (!isd && tdir)
            return -EISDIR;
        if (tdir && TN(t->ip)->ents)
            return -ENOTEMPTY;
    }
    idup(ip);
    DI(ip)->i_links_count++;                     /* keep it alive while it has no entry */
    *opp = d->next;
    kfree(d);
    inode_set_size(od, inode_size(od) - 1);
    inode_touch(od, true, true);
    if (t) {
        dfind(nd, nn, nl, &npp);                 /* the list may have changed */
        dremove(nd, npp);
    }
    r = dadd(nd, nn, nl, ip);
    DI(ip)->i_links_count--;
    if (isd && od != nd) {
        iput(TN(ip)->parent);
        TN(ip)->parent = idup(nd);
        DI(od)->i_links_count--;
        DI(nd)->i_links_count++;
    }
    inode_touch(ip, false, true);
    iput(ip);
    return r;
}

/* MAP_SHARED maps the file's own pages. */
static int tmpfs_getpage(struct inode *ip, uint64_t idx, uint64_t *pa)
{
    struct tnode *tn = TN(ip);
    struct tmpfs *t = TFS(ip);
    if (grow_table(tn, idx + 1) < 0)
        return -ENOMEM;
    if (!tn->pages[idx]) {
        uint64_t f = t->used_pages < t->max_pages ? pmm_alloc() : 0;
        if (!f)
            return -ENOMEM;
        tn->pages[idx] = f;
        tn->nresident++;
        t->used_pages++;
        set_blocks(ip);
    }
    pmm_ref(tn->pages[idx]);
    *pa = tn->pages[idx];
    return 0;
}

static void tmpfs_statvfs(struct fs *fs, struct kstatvfs *sv)
{
    struct tmpfs *t = fs->priv;
    sv->bsize = PAGE_SIZE;
    sv->blocks = t->max_pages;
    sv->bfree = t->max_pages - t->used_pages;
    sv->files = 65536;
    sv->ffree = t->nodes < 65536 ? 65536 - t->nodes : 0;
}

/* Unmounted: remove every entry, deepest first, then the root. */
static void purge(struct inode *dir)
{
    struct tnode *tn = TN(dir);
    while (tn->ents) {
        struct inode *ip = tn->ents->ip;
        if (S_ISDIR(inode_mode(ip)))
            purge(ip);
        dremove(dir, &tn->ents);
    }
}

static void tmpfs_destroy(struct fs *fs)
{
    purge(fs->root);
    DI(fs->root)->i_links_count = 0;
    iput(fs->root);                              /* the file system's reference: frees it */
    kfree(fs->priv);
    kfree(fs);
}

static const struct fs_ops tmpfs_ops = {
    .name = "tmpfs",
    .read = tmpfs_read,
    .write = tmpfs_write,
    .truncate = tmpfs_truncate,
    .lookup = tmpfs_lookup,
    .readdir = tmpfs_readdir,
    .create = tmpfs_mknod,
    .mkdir = tmpfs_mkdir,
    .unlink = tmpfs_unlink,
    .rename = tmpfs_rename,
    .link = tmpfs_link,
    .symlink = tmpfs_symlink,
    .release = tmpfs_release,
    .statvfs = tmpfs_statvfs,
    .getpage = tmpfs_getpage,
    .destroy = tmpfs_destroy,
};

struct fs *tmpfs_create(void)
{
    static uint32_t instance;
    struct fs *fs = kzalloc(sizeof(*fs));
    struct tmpfs *t = kzalloc(sizeof(*t));
    if (!fs || !t)
        return NULL;
    fs_lock_init(fs);
    t->next_ino = 2;
    t->max_pages = pmm_total_pages() / 2;
    fs->ops = &tmpfs_ops;
    fs->priv = t;
    fs->dev_major = 20;
    fs->dev_minor = __atomic_fetch_add(&instance, 1, __ATOMIC_RELAXED);
    fs->bsize = PAGE_SIZE;
    fs->root = tnode_new(fs, S_IFDIR | 01777, 0, 0);
    if (!fs->root)
        return NULL;
    DI(fs->root)->i_links_count = 2;
    return fs;
}
