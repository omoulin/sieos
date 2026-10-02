/*
 * devpts.c - /dev/pts: one character device per pseudo-terminal opened
 * through /dev/ptmx (136,n).  Nodes are made on lookup and dropped when
 * unreferenced; the directory lists the pseudo-terminals in use.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "fs.h"
#include "proc.h"
#include "tty.h"
#include "mm.h"

static int parse_num(const char *s, size_t len)
{
    if (!len || len > 4)
        return -1;
    int n = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        n = n * 10 + s[i] - '0';
    }
    return n;
}

static int devpts_lookup(struct inode *dir, const char *name, size_t len, struct inode **out)
{
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) {
        *out = idup(dir);
        return 0;
    }
    int n = parse_num(name, len), uid, gid;
    if (n < 0 || !pty_slot(n, &uid, &gid))
        return -ENOENT;
    struct inode *ip = vfs_new_inode(dir->fs, n + 2, S_IFCHR | 0620, uid, gid);
    if (!ip)
        return -ENOMEM;
    inode_set_rdev(ip, MKDEV(DEV_PTS_MAJOR, n));
    *out = ip;
    return 0;
}

static int devpts_readdir(struct inode *dir, uint64_t *off, filldir_t fill, void *arg)
{
    if (*off == 0) {
        if (fill(arg, ".", 1, dir->ino, DT_DIR, 1))
            return 0;
        *off = 1;
    }
    if (*off == 1) {
        if (fill(arg, "..", 2, dir->ino, DT_DIR, 2))
            return 0;
        *off = 2;
    }
    for (int n = *off - 2; n < pty_count(); n++) {
        int uid, gid;
        if (!pty_slot(n, &uid, &gid))
            continue;
        char name[8];
        int len = snprintf(name, sizeof(name), "%d", n);
        if (fill(arg, name, len, n + 2, DT_CHR, n + 3))
            return 0;
        *off = n + 3;
    }
    *off = pty_count() + 2;
    return 0;
}

static void devpts_release(struct inode *ip)
{
    if (ip != ip->fs->root)
        vfs_free_inode(ip);
}

static int devpts_update(struct inode *ip)
{
    UNUSED(ip);
    return 0;                                    /* chmod/chown (grantpt) apply to this node only */
}

static const struct fs_ops devpts_ops = {
    .name = "devpts",
    .lookup = devpts_lookup,
    .readdir = devpts_readdir,
    .update = devpts_update,
    .release = devpts_release,
};

struct fs *devpts_create(void)
{
    struct fs *fs = kzalloc(sizeof(*fs));
    if (!fs)
        return NULL;
    fs_lock_init(fs);
    fs->ops = &devpts_ops;
    fs->dev_major = 22;
    fs->bsize = 1024;
    fs->root = vfs_new_inode(fs, 1, S_IFDIR | 0755, 0, 0);
    return fs->root ? fs : NULL;
}
