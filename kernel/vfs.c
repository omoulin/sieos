/*
 * vfs.c - The virtual file system layer.
 *
 * struct inode is the vnode: its attributes are kept in an ext4-layout inode
 * by every file system, and its operations dispatch on ip->fs->ops.  This
 * file holds the generic attribute code, the dispatchers, the mount table
 * and path reconstruction (getcwd).  Path lookup is in namei.c.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "fs.h"
#include "blkdev.h"
#include "proc.h"
#include "mm.h"

struct fs *root_fs;
static void now_ts(int64_t *s, long *ns);
static struct fs *mounts;             /* every mounted file system, root first */

/* ------------------------------------------------------------------ */
/* References                                                          */
/* ------------------------------------------------------------------ */

struct inode *idup(struct inode *ip)
{
    ip->ref++;
    return ip;
}

void iput(struct inode *ip)
{
    if (!ip)
        return;
    if (ip->ref <= 0)
        panic("iput: refcount underflow on inode %u", ip->ino);
    if (--ip->ref > 0)
        return;
    if (ip->fs && ip->fs->ops->release)
        ip->fs->ops->release(ip);
}

int iupdate(struct inode *ip)
{
    if (ip->fs->rdonly)
        return -EROFS;
    return ip->fs->ops->update ? ip->fs->ops->update(ip) : 0;
}

/* An inode for the in-memory file systems (tmpfs, procfs, devpts). */
struct inode *vfs_new_inode(struct fs *fs, uint32_t ino, uint16_t mode, int uid, int gid)
{
    struct inode *ip = kzalloc(sizeof(*ip));
    if (!ip)
        return NULL;
    ip->ino = ino;
    ip->ref = 1;
    ip->valid = true;
    ip->fs = fs;
    struct ext4_inode *di = DI(ip);
    di->i_mode = mode;
    di->i_links_count = S_ISDIR(mode) ? 2 : 1;
    di->i_uid = uid & 0xFFFF;
    di->i_uid_high = uid >> 16;
    di->i_gid = gid & 0xFFFF;
    di->i_gid_high = gid >> 16;
    di->i_extra_isize = 32;
    int64_t s;
    long ns;
    now_ts(&s, &ns);
    for (int w = 0; w < 3; w++)
        inode_time_set(ip, w, s, ns);
    return ip;
}

void vfs_free_inode(struct inode *ip)
{
    kfree(ip);
}

/* ------------------------------------------------------------------ */
/* Attributes                                                          */
/* ------------------------------------------------------------------ */

bool inode_readonly(struct inode *ip)
{
    return ip->fs->rdonly;
}

/* Device number of a character/block special file (old or new encoding). */
uint32_t inode_rdev(struct inode *ip)
{
    struct ext4_inode *di = DI(ip);
    if (di->i_block[0]) {
        uint32_t d = di->i_block[0];
        return MKDEV((d >> 8) & 0xFF, d & 0xFF);
    }
    uint32_t d = di->i_block[1];
    return MKDEV((d & 0xFFF00) >> 8, (d & 0xFF) | ((d >> 12) & 0xFFF00));
}

void inode_set_rdev(struct inode *ip, uint32_t dev)
{
    struct ext4_inode *di = DI(ip);
    uint32_t ma = MAJOR(dev), mi = MINOR(dev);
    di->i_block[0] = di->i_block[1] = 0;
    if (ma < 256 && mi < 256)
        di->i_block[0] = (ma << 8) | mi;
    else
        di->i_block[1] = (mi & 0xFF) | (ma << 8) | ((mi & ~0xFFU) << 12);
}

static bool has_extra_times(struct inode *ip)
{
    return DI(ip)->i_extra_isize >= 16;
}

/* ext4 timestamps: 32-bit seconds, extra = nanoseconds << 2 | epoch bits. */
void inode_time_set(struct inode *ip, int which, int64_t s, long ns)
{
    struct ext4_inode *di = DI(ip);
    uint32_t sec = (uint32_t)s;
    uint32_t extra = ((uint32_t)ns << 2) | (((uint64_t)(s - (int32_t)sec) >> 32) & 3);
    bool x = has_extra_times(ip);
    switch (which) {
    case 0:
        di->i_atime = sec;
        if (x)
            di->i_atime_extra = extra;
        break;
    case 1:
        di->i_mtime = sec;
        if (x)
            di->i_mtime_extra = extra;
        break;
    default:
        di->i_ctime = sec;
        if (x)
            di->i_ctime_extra = extra;
        break;
    }
}

static void time_get(struct inode *ip, uint32_t sec, uint32_t extra, int64_t *s, long *ns)
{
    *s = (int32_t)sec;
    *ns = 0;
    if (has_extra_times(ip)) {
        *s += (int64_t)(extra & 3) << 32;
        *ns = extra >> 2;
    }
}

static void now_ts(int64_t *s, long *ns)
{
    int64_t t = realtime_ns();
    *s = t / 1000000000L;
    *ns = t % 1000000000L;
}

/* Set mtime and/or ctime to now (in memory; the caller writes the inode). */
void inode_touch(struct inode *ip, bool m, bool c)
{
    int64_t s;
    long ns;
    now_ts(&s, &ns);
    if (m)
        inode_time_set(ip, 1, s, ns);
    if (c)
        inode_time_set(ip, 2, s, ns);
}

/* Change permission bits and/or ownership; -1 leaves a field unchanged. */
int inode_setattr(struct inode *ip, int mode, int uid, int gid)
{
    struct ext4_inode *di = DI(ip);
    if (ip->fs->rdonly)
        return -EROFS;
    if (mode >= 0)
        di->i_mode = (di->i_mode & S_IFMT) | (mode & 07777);
    if (uid != -1) {                             /* (IDs are 32-bit unsigned: -1 alone means "unchanged") */
        di->i_uid = (uint32_t)uid & 0xFFFF;
        di->i_uid_high = (uint32_t)uid >> 16;
    }
    if (gid != -1) {
        di->i_gid = (uint32_t)gid & 0xFFFF;
        di->i_gid_high = (uint32_t)gid >> 16;
    }
    inode_touch(ip, false, true);
    return iupdate(ip);
}

/* Set the access and modification times; a negative ns leaves that time alone. */
int inode_settimes(struct inode *ip, int64_t as, long ans, int64_t ms, long mns)
{
    if (ip->fs->rdonly)
        return -EROFS;
    if (ans >= 0)
        inode_time_set(ip, 0, as, ans);
    if (mns >= 0)
        inode_time_set(ip, 1, ms, mns);
    inode_touch(ip, false, true);
    return iupdate(ip);
}

void inode_getstat(struct inode *ip, struct kstat *st)
{
    struct ext4_inode *di = DI(ip);
    memset(st, 0, sizeof(*st));
    st->dev_major = ip->fs->dev_major;
    st->dev_minor = ip->fs->dev_minor;
    st->ino = ip->ino;
    st->mode = di->i_mode;
    st->nlink = di->i_links_count;
    st->uid = inode_uid(ip);
    st->gid = inode_gid(ip);
    st->size = inode_size(ip);
    uint64_t blocks = di->i_blocks_lo | ((uint64_t)di->i_blocks_high << 32);
    if (di->i_flags & EXT4_HUGE_FILE_FL)
        blocks *= ip->fs->bsize / 512;
    st->blocks = blocks;
    if (S_ISCHR(di->i_mode) || S_ISBLK(di->i_mode))
        st->rdev = inode_rdev(ip);
    time_get(ip, di->i_atime, di->i_atime_extra, &st->atime, &st->atime_ns);
    time_get(ip, di->i_mtime, di->i_mtime_extra, &st->mtime, &st->mtime_ns);
    time_get(ip, di->i_ctime, di->i_ctime_extra, &st->ctime, &st->ctime_ns);
    st->blksize = ip->fs->bsize ? ip->fs->bsize : PAGE_SIZE;
    st->fstype = ip->fs->ops->name;
}


/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

/* Copy between a byte range and the cached MAP_SHARED pages that overlap it. */
static void pcache_copy(struct inode *ip, uint8_t *buf, uint64_t off, size_t n, bool to_cache)
{
    for (size_t done = 0; done < n;) {
        uint64_t idx = (off + done) / PAGE_SIZE, po = (off + done) % PAGE_SIZE;
        size_t chunk = MIN(n - done, PAGE_SIZE - po);
        if (idx < ip->npcache && ip->pcache[idx]) {
            uint8_t *pg = (uint8_t *)P2V(ip->pcache[idx]) + po;
            if (to_cache)
                memcpy(pg, buf + done, chunk);
            else
                memcpy(buf + done, pg, chunk);
        }
        done += chunk;
    }
}

long readi(struct inode *ip, void *dst, uint64_t off, size_t n)
{
    if (!ip->fs->ops->read)
        return -EINVAL;
    long r = ip->fs->ops->read(ip, dst, off, n);
    if (r > 0 && ip->pcache)
        pcache_copy(ip, dst, off, r, false);     /* shared mappings may hold newer data */
    return r;
}

long writei(struct inode *ip, const void *src, uint64_t off, size_t n)
{
    if (ip->fs->rdonly)
        return -EROFS;
    if (!ip->fs->ops->write)
        return -EINVAL;
    long r = ip->fs->ops->write(ip, src, off, n);
    if (r > 0 && ip->pcache)
        pcache_copy(ip, (uint8_t *)src, off, r, true);
    return r;
}

/* ---------------- pages shared by MAP_SHARED mappings ---------------- */

int vfs_getpage(struct inode *ip, uint64_t idx, uint64_t *pa)
{
    if (ip->fs->ops->getpage)
        return ip->fs->ops->getpage(ip, idx, pa);
    if (idx >= ip->npcache) {
        size_t cap = ip->npcache ? ip->npcache : 8;
        while (cap <= idx)
            cap *= 2;
        uint64_t *n = kzalloc(cap * sizeof(uint64_t));
        if (!n)
            return -ENOMEM;
        if (ip->pcache)
            memcpy(n, ip->pcache, ip->npcache * sizeof(uint64_t));
        kfree(ip->pcache);
        ip->pcache = n;
        ip->npcache = cap;
    }
    if (!ip->pcache[idx]) {
        uint64_t f = pmm_alloc();                /* zero-filled */
        if (!f)
            return -ENOMEM;
        uint64_t size = inode_size(ip), fo = idx * PAGE_SIZE;
        if (fo < size && ip->fs->ops->read(ip, P2V(f), fo, MIN(PAGE_SIZE, size - fo)) < 0) {
            pmm_free(f);
            return -EIO;
        }
        ip->pcache[idx] = f;                     /* the cache's reference */
    }
    pmm_ref(ip->pcache[idx]);
    *pa = ip->pcache[idx];
    return 0;
}

/* Write a cached page back to the file (the part inside the file). */
void vfs_writeback(struct inode *ip, uint64_t idx)
{
    if (ip->fs->ops->getpage || idx >= ip->npcache || !ip->pcache[idx] || ip->fs->rdonly)
        return;
    uint64_t size = inode_size(ip), fo = idx * PAGE_SIZE;
    if (fo < size)
        ip->fs->ops->write(ip, P2V(ip->pcache[idx]), fo, MIN(PAGE_SIZE, size - fo));
}

/* Drop cached pages nobody maps any more. */
void vfs_pcache_trim(struct inode *ip)
{
    bool any = false;
    for (size_t i = 0; i < ip->npcache; i++) {
        if (ip->pcache[i] && pmm_refcount(ip->pcache[i]) <= 1) {
            pmm_unref(ip->pcache[i]);
            ip->pcache[i] = 0;
        }
        any |= ip->pcache[i] != 0;
    }
    if (!any) {
        kfree(ip->pcache);
        ip->pcache = NULL;
        ip->npcache = 0;
    }
}

int itruncate(struct inode *ip, uint64_t len)
{
    if (ip->fs->rdonly)
        return -EROFS;
    for (size_t i = (len + PAGE_SIZE - 1) / PAGE_SIZE; i < ip->npcache; i++)
        if (ip->pcache[i]) {                     /* mappings keep their frames */
            pmm_unref(ip->pcache[i]);
            ip->pcache[i] = 0;
        }
    if (!ip->fs->ops->truncate)
        return -EINVAL;
    return ip->fs->ops->truncate(ip, len);
}

int vfs_lookup(struct inode *dir, const char *name, size_t len, struct inode **out)
{
    if (!S_ISDIR(inode_mode(dir)))
        return -ENOTDIR;
    return dir->fs->ops->lookup(dir, name, len, out);
}

int vfs_readdir(struct inode *dir, uint64_t *off, filldir_t fill, void *arg)
{
    if (!S_ISDIR(inode_mode(dir)))
        return -ENOTDIR;
    return dir->fs->ops->readdir(dir, off, fill, arg);
}

/* Checks shared by the operations that add a name to a directory. */
static int may_modify(struct inode *dir, bool have_op)
{
    if (!S_ISDIR(inode_mode(dir)))
        return -ENOTDIR;
    if (dir->fs->rdonly)
        return -EROFS;
    return have_op ? 0 : -EPERM;
}

int dir_name_ok(const char *name, size_t *len)
{
    *len = strlen(name);
    if (*len == 0)
        return -ENOENT;
    if (*len > 255)
        return -ENAMETOOLONG;
    return 0;
}

int vfs_create(struct inode *dir, const char *name, uint16_t mode, uint32_t rdev, int uid, int gid,
               struct inode **out)
{
    int r = may_modify(dir, dir->fs->ops->create != NULL);
    return r < 0 ? r : dir->fs->ops->create(dir, name, mode, rdev, uid, gid, out);
}

int vfs_mkdir(struct inode *dir, const char *name, uint16_t mode, int uid, int gid)
{
    int r = may_modify(dir, dir->fs->ops->mkdir != NULL);
    return r < 0 ? r : dir->fs->ops->mkdir(dir, name, mode, uid, gid);
}

int vfs_unlink(struct inode *dir, const char *name, bool is_dir)
{
    int r = may_modify(dir, dir->fs->ops->unlink != NULL);
    return r < 0 ? r : dir->fs->ops->unlink(dir, name, is_dir);
}

int vfs_rename(struct inode *od, const char *on, struct inode *nd, const char *nn)
{
    if (od->fs != nd->fs)
        return -EXDEV;
    int r = may_modify(od, od->fs->ops->rename != NULL);
    return r < 0 ? r : od->fs->ops->rename(od, on, nd, nn);
}

int vfs_link(struct inode *dir, const char *name, struct inode *ip)
{
    if (dir->fs != ip->fs)
        return -EXDEV;
    int r = may_modify(dir, dir->fs->ops->link != NULL);
    return r < 0 ? r : dir->fs->ops->link(dir, name, ip);
}

int vfs_symlink(struct inode *dir, const char *name, const char *target, int uid, int gid)
{
    int r = may_modify(dir, dir->fs->ops->symlink != NULL);
    return r < 0 ? r : dir->fs->ops->symlink(dir, name, target, uid, gid);
}

/* Read a symbolic link's target (not NUL-terminated). */
long vfs_readlink(struct inode *ip, char *buf, size_t n)
{
    if (!S_ISLNK(inode_mode(ip)))
        return -EINVAL;
    return readi(ip, buf, 0, n);
}

int vfs_statvfs(struct inode *ip, struct kstatvfs *sv)
{
    memset(sv, 0, sizeof(*sv));
    sv->bsize = ip->fs->bsize ? ip->fs->bsize : PAGE_SIZE;
    sv->namemax = 255;
    if (ip->fs->ops->statvfs)
        ip->fs->ops->statvfs(ip->fs, sv);
    sv->rdonly = sv->rdonly || ip->fs->rdonly;
    return 0;
}

void vfs_sync(void)
{
    for (struct fs *fs = mounts; fs; fs = fs->next)
        if (fs->ops->sync)
            fs->ops->sync(fs);
}

/* ------------------------------------------------------------------ */
/* Mounts                                                              */
/* ------------------------------------------------------------------ */

struct fs *vfs_mounts(void)
{
    return mounts;
}

struct inode *vfs_root(void)
{
    return idup(root_fs->root);
}

struct inode *vfs_covering(struct inode *ip)
{
    struct fs *top = NULL;
    for (struct fs *fs = mounts; fs; fs = fs->next)
        if (fs->covered && same_inode(fs->covered, ip))
            top = fs;                            /* the most recent mount wins */
    return top ? idup(top->root) : NULL;
}

int vfs_mount(struct fs *fs, const char *path)
{
    int err;
    struct inode *ip = namei(path, &err);
    if (!ip)
        return err;
    if (!S_ISDIR(inode_mode(ip))) {
        iput(ip);
        return -ENOTDIR;
    }
    fs->covered = ip;
    strlcpy(fs->mntpoint, path, sizeof(fs->mntpoint));
    if (!fs->special[0])
        strlcpy(fs->special, fs->ops->name, sizeof(fs->special));
    fs->mount_time = kernel_time();
    struct fs **pp = &mounts;
    while (*pp)
        pp = &(*pp)->next;
    *pp = fs;
    return 0;
}

struct fs *vfs_mounted_on(struct inode *ip)
{
    for (struct fs *fs = mounts; fs; fs = fs->next)
        if (fs->root == ip)
            return fs;
    return NULL;
}

int vfs_umount(struct fs *fs)
{
    if (fs == root_fs || !fs->ops->destroy)
        return -EBUSY;
    for (struct fs *o = mounts; o; o = o->next)
        if (o->covered && o->covered->fs == fs)
            return -EBUSY;                       /* something is mounted inside */
    if (file_table_uses(fs) || proc_table_uses(fs))
        return -EBUSY;
    for (struct fs **pp = &mounts; *pp; pp = &(*pp)->next)
        if (*pp == fs) {
            *pp = fs->next;
            break;
        }
    iput(fs->covered);
    fs->ops->destroy(fs);
    return 0;
}

long vfs_mnttab(char *buf, size_t size)
{
    size_t n = 0;
    for (struct fs *fs = mounts; fs; fs = fs->next) {
        char line[224];
        int k = snprintf(line, sizeof(line), "%s\t%s\t%s\t%s%s,dev=%x\t%ld\n",
                         fs->special[0] ? fs->special : fs->ops->name, fs == root_fs ? "/" : fs->mntpoint,
                         fs->ops->name, fs->rdonly ? "ro" : "rw", fs->nosuid ? ",nosuid" : "",
                         (fs->dev_major << 18) | fs->dev_minor, (long)fs->mount_time);
        if (n + k < size)
            memcpy(buf + n, line, k);
        n += k;
    }
    if (size)
        buf[n < size ? n : size - 1] = 0;
    return n;
}

void vfs_init(void)
{
    mounts = root_fs;
    root_fs->next = NULL;
}

/* Create a missing mount point or device node on the root file system. */
static void ensure(const char *path, uint16_t mode, uint32_t rdev)
{
    int err;
    struct inode *ip = namei(path, &err);
    if (ip) {
        iput(ip);
        return;
    }
    if (root_fs->rdonly)
        return;
    char name[256];
    struct inode *dir = nameiparent(path, name, &err);
    if (!dir)
        return;
    if (S_ISDIR(mode))
        err = vfs_mkdir(dir, name, mode & 07777, 0, 0);
    else
        err = vfs_create(dir, name, mode, rdev, 0, 0, NULL);
    iput(dir);
    if (err < 0)
        kprintf("vfs: cannot create %s (%d)\n", path, err);
}

/*
 * The /dev/dsk nodes of the block devices there are: created, or made
 * again where the name now stands for another device (after a disk's
 * partitions were read again).
 */
void vfs_blk_nodes(void)
{
    ensure("/dev/dsk", S_IFDIR | 0755, 0);
    for (int u = 0; u < NBLKDEV; u++) {
        if (!blk_present(u) || !blk_name(u))
            continue;
        char path[48];
        snprintf(path, sizeof(path), "/dev/dsk/%s", blk_name(u));
        int err;
        struct inode *ip = namei(path, &err);
        if (ip) {
            bool same = S_ISBLK(inode_mode(ip)) && inode_rdev(ip) == (uint32_t)MKDEV(DEV_BLK_MAJOR, u);
            iput(ip);
            if (same)
                continue;
            char name[64];
            struct inode *dir = nameiparent(path, name, &err);
            if (dir) {
                vfs_unlink(dir, name, false);
                iput(dir);
            }
        }
        ensure(path, S_IFBLK | 0600, MKDEV(DEV_BLK_MAJOR, u));
    }
}

void vfs_mount_all(void)
{
    if (blk_is_ramdisk())
        strlcpy(root_fs->special, "/dev/ramdisk", sizeof(root_fs->special));
    else
        snprintf(root_fs->special, sizeof(root_fs->special), "/dev/dsk/%s", blk_name(blk_root()));
    root_fs->mount_time = kernel_time();
    ensure("/tmp", S_IFDIR | 01777, 0);
    ensure("/proc", S_IFDIR | 0555, 0);
    ensure("/dev/pts", S_IFDIR | 0755, 0);
    ensure("/dev/ptmx", S_IFCHR | 0666, MKDEV(DEV_TTY_MAJOR, 2));
    /* block devices: the disks there are, and the lofi devices */
    vfs_blk_nodes();
    ensure("/dev/lofictl", S_IFCHR | 0600, MKDEV(DEV_LOFI_MAJOR, 0));
    ensure("/dev/power", S_IFCHR | 0666, MKDEV(DEV_POWER_MAJOR, 0));
    ensure("/dev/lofi", S_IFDIR | 0755, 0);
    for (int n = 1; n <= NLOFI; n++) {
        char name[32];
        snprintf(name, sizeof(name), "/dev/lofi/%d", n);
        ensure(name, S_IFBLK | 0600, MKDEV(DEV_BLK_MAJOR, BLK_LOFI0 + n - 1));
    }
    ensure("/dev/shm", S_IFDIR | 01777, 0);
    struct { struct fs *fs; const char *path; } m[] = {
        { tmpfs_create(), "/tmp" },
        { procfs_create(), "/proc" },
        { devpts_create(), "/dev/pts" },
        { tmpfs_create(), "/dev/shm" },
    };
    for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); i++) {
        int r = m[i].fs ? vfs_mount(m[i].fs, m[i].path) : -ENOMEM;
        if (r < 0)
            kprintf("vfs: cannot mount %s (%d)\n", m[i].path, r);
    }
}

/* ------------------------------------------------------------------ */
/* getcwd                                                              */
/* ------------------------------------------------------------------ */

struct findname {
    uint64_t ino;
    char *out;
    bool found;
};

static int findname_cb(void *arg, const char *name, size_t len, uint64_t ino, int dtype, uint64_t next)
{
    UNUSED(dtype);
    UNUSED(next);
    struct findname *a = arg;
    if (ino != a->ino || (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return 0;
    memcpy(a->out, name, len);
    a->out[len] = 0;
    a->found = true;
    return 1;
}

_Static_assert(MAXPATH == PAGE_SIZE, "path_get hands out pages");

char *path_get(void)
{
    uint64_t pa = pmm_alloc();
    return pa ? P2V(pa) : NULL;
}

void path_put(char *p)
{
    if (p)
        pmm_free(V2P(p));
}

static int dir_path(struct inode *dir, char *buf, size_t size, char *tmp);

/*
 * Build the absolute path of a directory by walking "..", crossing mount
 * points upwards, and stopping at the process's root.
 */
int vfs_dir_path(struct inode *dir, char *buf, size_t size)
{
    char *tmp = path_get();
    if (!tmp)
        return -ENOMEM;
    int r = dir_path(dir, buf, size, tmp);
    path_put(tmp);
    return r;
}

static int dir_path(struct inode *dir, char *buf, size_t size, char *tmp)
{
    char name[256];
    size_t pos = MAXPATH - 1;
    tmp[pos] = 0;
    struct inode *root = current && current->root ? current->root : root_fs->root;
    struct inode *cur = idup(dir);
    int r = 0;
    for (int depth = 0; !same_inode(cur, root); depth++) {
        if (depth > MAXPATH / 2) {                 /* (a path has at most that many directories) */
            r = -ELOOP;
            break;
        }
        if (same_inode(cur, cur->fs->root)) {
            if (!cur->fs->covered)
                break;                           /* the global root, outside a chroot */
            struct inode *up = idup(cur->fs->covered);
            iput(cur);
            cur = up;
            continue;
        }
        struct inode *parent;
        if ((r = vfs_lookup(cur, "..", 2, &parent)) < 0)
            break;
        struct findname a = { cur->ino, name, false };
        uint64_t off = 0;
        r = vfs_readdir(parent, &off, findname_cb, &a);
        iput(cur);
        cur = parent;
        if (r < 0)
            break;
        if (!a.found) {
            r = -ENOENT;                         /* the directory was removed */
            break;
        }
        size_t n = strlen(name);
        if (n + 1 > pos) {
            r = -ENAMETOOLONG;
            break;
        }
        pos -= n;
        memcpy(tmp + pos, name, n);
        tmp[--pos] = '/';
    }
    iput(cur);
    if (r < 0)
        return r;
    if (pos == MAXPATH - 1)
        tmp[--pos] = '/';
    size_t len = MAXPATH - 1 - pos;
    if (len + 1 > size)
        return -ERANGE;
    memcpy(buf, tmp + pos, len + 1);
    return len + 1;
}
