/*
 * fsys.c - File system calls shared by ABI v1 (syscall.c) and ABI v2
 * (syscall2.c).  Paths are user pointers; dirfd is a directory descriptor
 * or AT_FDCWD_K; flags use the kernel's O_* and AT_*_K values.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "proc.h"
#include "mm.h"
#include "fs.h"
#include "blkdev.h"
#include "tty.h"
#include "poll.h"
#include "display.h"
#include "abi2.h"

struct file *fsys_file(int fd)
{
    if (fd < 0 || fd >= NOFILE)
        return NULL;
    return current->ofile[fd];
}

int fsys_fdalloc(struct file *f, int from)
{
    uint64_t lim = current->rlim_cur[5];             /* SIEOS_RLIMIT_NOFILE */
    int max = lim < NOFILE ? (int)lim : NOFILE;
    for (int fd = from < 0 ? 0 : from; fd < max; fd++) {
        if (!current->ofile[fd]) {
            current->ofile[fd] = f;
            current->fdflags[fd] = 0;
            return fd;
        }
    }
    return -EMFILE;
}

static bool is_root(void)
{
    return current->euid == 0;
}

/* Copy the path in and find the directory relative paths start from. */
static int prep(int dirfd, const char *upath, char *path, struct inode **start)
{
    int r = user_fetch_str(upath, path, MAXPATH);
    if (r < 0)
        return r;
    *start = NULL;
    if (path[0] == '/' || dirfd == AT_FDCWD_K)
        return 0;
    struct file *f = fsys_file(dirfd);
    if (!f)
        return -EBADF;
    if (!f->ip || !S_ISDIR(inode_mode(f->ip)))
        return -ENOTDIR;
    *start = f->ip;
    return 0;
}

static struct inode *resolve(int dirfd, const char *upath, int atflags, int *err)
{
    char *path = path_get();
    struct inode *start, *ip = NULL;
    if (!path)
        *err = -ENOMEM;
    else if ((*err = prep(dirfd, upath, path, &start)) >= 0)
        ip = namei_at(start, path, (atflags & AT_NOFOLLOW_K) ? NAMEI_NOFOLLOW : 0, err);
    path_put(path);
    return ip;
}

/* The inode a call targets: a path, or the descriptor itself if upath is NULL. */
static struct inode *target(int dirfd, const char *upath, int atflags, int *err)
{
    if (upath)
        return resolve(dirfd, upath, atflags, err);
    struct file *f = fsys_file(dirfd);
    if (!f) {
        *err = -EBADF;
        return NULL;
    }
    if (!f->ip) {
        *err = -EINVAL;
        return NULL;
    }
    return idup(f->ip);
}

static struct inode *resolve_parent(int dirfd, const char *upath, char *name, int *err)
{
    char *path = path_get();
    struct inode *start, *ip = NULL;
    if (!path)
        *err = -ENOMEM;
    else if ((*err = prep(dirfd, upath, path, &start)) >= 0)
        ip = nameiparent_at(start, path, name, err);
    path_put(path);
    return ip;
}

/* Group of a new file: the directory's with set-gid, else the caller's. */
static int new_gid(struct inode *dir)
{
    return (inode_mode(dir) & S_ISGID) ? inode_gid(dir) : current->egid;
}

/* ------------------------------------------------------------------ */
/* open                                                                */
/* ------------------------------------------------------------------ */

/* Turn an opened character-special inode into a device file. */
static int open_device(struct file *f, struct inode *ip, int flags)
{
    uint32_t dev = inode_rdev(ip);
    bool acquire = current->sid == current->pid && !(flags & O_NOCTTY) && !tty_of_session(current->sid);
    if (MAJOR(dev) == DEV_TTY_MAJOR && MINOR(dev) == 1) {
        f->type = FD_TTY;
        f->tty = &console_tty;
        if (acquire)
            tty_ioctl(&console_tty, TIOCSCTTY, 0);   /* session leader acquires ctty */
        return 0;
    }
    if (MAJOR(dev) == DEV_TTY_MAJOR && MINOR(dev) == 0) {   /* /dev/tty: controlling tty */
        struct tty *t = tty_of_session(current->sid);
        if (!t)
            return -ENXIO;
        f->type = FD_TTY;
        f->tty = t;
        pty_slave_ref(t);
        return 0;
    }
    if (MAJOR(dev) == DEV_TTY_MAJOR && MINOR(dev) == 2)     /* /dev/ptmx */
        return pty_open_master(f);
    if (MAJOR(dev) == DEV_PTS_MAJOR) {
        int r = pty_open_slave(MINOR(dev), f);
        if (r == 0 && acquire)
            tty_ioctl(f->tty, TIOCSCTTY, 0);
        return r;
    }
    if (MAJOR(dev) == DEV_INPUT_MAJOR && MINOR(dev) == 0) {
        f->type = FD_EVENTS;
        return input_open();
    }
    if (MAJOR(dev) == DEV_FB_MAJOR && MINOR(dev) < DISPLAY_MAX) {
        int r = fb_open(MINOR(dev));
        if (r == 0) {
            f->type = FD_FB;
            f->minor = MINOR(dev);
        }
        return r;
    }
    if (MAJOR(dev) == DEV_POWER_MAJOR && MINOR(dev) == 0) {
        f->type = FD_POWER;
        return 0;
    }
    if (MAJOR(dev) == DEV_LOFI_MAJOR && MINOR(dev) == 0) {
        f->type = FD_LOFICTL;
        return 0;
    }
    if (MAJOR(dev) == DEV_MEM_MAJOR && MINOR(dev) == 3) {
        f->type = FD_NULL;
        return 0;
    }
    if (MAJOR(dev) == DEV_MEM_MAJOR && MINOR(dev) == 5) {
        f->type = FD_ZERO;
        return 0;
    }
    if (MAJOR(dev) == DEV_MEM_MAJOR && (MINOR(dev) == 8 || MINOR(dev) == 9)) {
        f->type = FD_RANDOM;
        return 0;
    }
    return -ENXIO;
}

/* An opened block-special inode: the device itself, read and written in bytes (root). */
static int open_blk(struct file *f, struct inode *ip, int flags)
{
    uint32_t dev = inode_rdev(ip);
    if (MAJOR(dev) != DEV_BLK_MAJOR)
        return -ENXIO;
    if (current->euid != 0)
        return -EACCES;
    int r = blk_file_open(MINOR(dev), (flags & O_ACCMODE) != O_RDONLY);
    if (r == 0) {
        f->type = FD_BLK;
        f->minor = MINOR(dev);
        f->off = 0;
    }
    return r;
}

static char *kstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = kmalloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

/*
 * open(O_CREAT) found a symbolic link at dir/name: follow it (and any link
 * it leads to); a target that does not exist is created where the link
 * points.  *dirp is consumed.  0 with *ipp, and *created if it was made.
 */
static int create_through_link(struct inode *dir, char *name, int mode, struct inode **ipp, bool *created)
{
    char *t = kmalloc(SYMLINK_MAX + 1);
    if (!t) {
        iput(dir);
        return -ENOMEM;
    }
    int r = 0;
    for (int links = 0;; links++) {
        struct inode *l;
        if (links >= MAXSYMLINKS) {
            r = -ELOOP;
            break;
        }
        if ((r = vfs_lookup(dir, name, strlen(name), &l)) < 0)
            break;
        long n = vfs_readlink(l, t, SYMLINK_MAX);
        iput(l);
        if (n <= 0) {
            r = n < 0 ? n : -ENOENT;
            break;
        }
        t[n] = 0;
        int err;
        struct inode *d2 = nameiparent_at(dir, t, name, &err);
        iput(dir);
        dir = d2;
        if (!dir) {
            r = err;
            break;
        }
        struct inode *ip;
        r = vfs_lookup(dir, name, strlen(name), &ip);
        if (r == 0) {
            struct inode *m;
            while ((m = vfs_covering(ip))) {
                iput(ip);
                ip = m;
            }
            if (S_ISLNK(inode_mode(ip))) {
                iput(ip);
                continue;
            }
            *ipp = ip;
            break;
        }
        if (r != -ENOENT)
            break;
        if ((r = inode_permission(dir, W_OK | X_OK)) == 0)
            r = vfs_create(dir, name, S_IFREG | ((mode & 07777) & ~current->umask), 0, current->euid,
                           new_gid(dir), ipp);
        if (r == 0)
            *created = true;
        break;
    }
    if (dir)
        iput(dir);
    kfree(t);
    return r;
}

static long open_at(int dirfd, const char *upath, int flags, int mode, char *path);

long fsys_open(int dirfd, const char *upath, int flags, int mode)
{
    char *path = path_get();
    if (!path)
        return -ENOMEM;
    long r = open_at(dirfd, upath, flags, mode, path);
    path_put(path);
    return r;
}

static long open_at(int dirfd, const char *upath, int flags, int mode, char *path)
{
    char name[256];
    struct inode *start, *ip = NULL;
    int r, err;
    bool created = false;
    if ((r = prep(dirfd, upath, path, &start)) < 0)
        return r;

    if (flags & O_CREAT) {
        struct inode *dir = nameiparent_at(start, path, name, &err);
        if (!dir)
            return err;
        r = vfs_lookup(dir, name, strlen(name), &ip);
        if (r == 0) {
            struct inode *m;
            while ((m = vfs_covering(ip))) {
                iput(ip);
                ip = m;
            }
            if (flags & O_EXCL) {
                iput(ip);
                iput(dir);
                return -EEXIST;
            }
            if (S_ISLNK(inode_mode(ip))) {
                iput(ip);
                ip = NULL;
                if (flags & O_NOFOLLOW_K) {
                    iput(dir);
                    return -ELOOP;
                }
                if ((r = create_through_link(dir, name, mode, &ip, &created)) < 0)
                    return r;
            } else {
                iput(dir);
            }
        } else if (r == -ENOENT) {
            size_t pl = strlen(path);
            if (pl && path[pl - 1] == '/') {             /* "new/": not a file to create (as Linux) */
                iput(dir);
                return -EISDIR;
            }
            if ((r = inode_permission(dir, W_OK | X_OK)) == 0)
                r = vfs_create(dir, name, S_IFREG | ((mode & 07777) & ~current->umask), 0, current->euid,
                               new_gid(dir), &ip);
            iput(dir);
            if (r < 0)
                return r;
            created = true;
        } else {
            iput(dir);
            return r;
        }
    } else if (!(ip = namei_at(start, path, (flags & O_NOFOLLOW_K) ? NAMEI_NOFOLLOW : 0, &err))) {
        return err;
    }

    uint16_t m = inode_mode(ip);
    int acc = flags & O_ACCMODE;
    if (S_ISLNK(m))
        r = -ELOOP;                                  /* O_NOFOLLOW on a symbolic link */
    else if (S_ISDIR(m) && acc != O_RDONLY)
        r = -EISDIR;
    else if ((flags & O_DIRECTORY) && !S_ISDIR(m))
        r = -ENOTDIR;
    else if (!created) {
        int want = (acc == O_RDONLY ? R_OK : acc == O_WRONLY ? W_OK : R_OK | W_OK);
        if ((flags & O_TRUNC) && acc == O_RDONLY)
            want |= W_OK;
        r = inode_permission(ip, want);
    }
    if (r == 0 && (flags & O_TRUNC) && S_ISREG(m) && acc != O_RDONLY && inode_size(ip) > 0)
        r = itruncate(ip, 0);
    if (r < 0) {
        iput(ip);
        return r;
    }
    struct file *f = file_alloc();
    if (!f) {
        iput(ip);
        return -ENFILE;
    }
    f->type = FD_INODE;
    f->ip = ip;
    f->flags = flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC_K | O_NOFOLLOW_K);
    f->off = 0;
    f->pname = kstrdup(path);
    if (path[0] != '/')
        f->pdir = idup(start ? start : current->cwd);
    if ((S_ISCHR(m) && (r = open_device(f, ip, flags)) < 0) || (S_ISBLK(m) && (r = open_blk(f, ip, flags)) < 0) || (S_ISFIFO(m) && (r = fifo_open(f, ip, flags)) < 0)) {
        file_close(f);
        return r;
    }
    int fd = fsys_fdalloc(f, 0);
    if (fd < 0)
        file_close(f);
    else if (flags & O_CLOEXEC_K)
        current->fdflags[fd] = FD_CLOEXEC;
    return fd;
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

long fsys_mknod(int dirfd, const char *upath, int mode, uint32_t rdev)
{
    char name[256];
    int r, err;
    uint16_t type = mode & S_IFMT;
    if (!type)
        type = S_IFREG;
    if (type != S_IFREG && type != S_IFIFO && type != S_IFCHR && type != S_IFBLK && type != S_IFSOCK)
        return -EINVAL;
    if ((type == S_IFCHR || type == S_IFBLK) && !is_root())
        return -EPERM;
    struct inode *dir = resolve_parent(dirfd, upath, name, &err);
    if (!dir)
        return err;
    if ((r = inode_permission(dir, W_OK | X_OK)) == 0)
        r = vfs_create(dir, name, type | ((mode & 07777) & ~current->umask), rdev, current->euid, new_gid(dir),
                       NULL);
    iput(dir);
    return r;
}

long fsys_mkdir(int dirfd, const char *upath, int mode)
{
    char name[256];
    int r, err;
    struct inode *dir = resolve_parent(dirfd, upath, name, &err);
    if (!dir)
        return err;
    r = inode_permission(dir, W_OK | X_OK);
    struct inode *ip;
    if (r < 0 && inode_permission(dir, X_OK) == 0 && vfs_lookup(dir, name, strlen(name), &ip) == 0) {
        iput(ip);
        r = -EEXIST;                             /* (it exists: that first, as elsewhere; mkdir -p relies on it) */
    }
    if (r == 0) {
        mode = (mode & 07777) & ~current->umask;
        if (inode_mode(dir) & S_ISGID)
            mode |= S_ISGID;
        r = vfs_mkdir(dir, name, mode, current->euid, new_gid(dir));
    }
    iput(dir);
    return r;
}

long fsys_unlink(int dirfd, const char *upath, int flags)
{
    char name[256];
    int r, err;
    struct inode *dir = resolve_parent(dirfd, upath, name, &err);
    if (!dir)
        return err;
    struct inode *victim, *m;
    r = vfs_lookup(dir, name, strlen(name), &victim);
    if (r == 0) {
        if ((m = vfs_covering(victim))) {
            iput(m);
            r = -EBUSY;
        } else {
            r = may_delete(dir, victim);
        }
        iput(victim);
        if (r == 0)
            r = vfs_unlink(dir, name, flags & AT_REMOVEDIR_K);
    }
    iput(dir);
    return r;
}

long fsys_rename(int ofd, const char *uold, int nfd, const char *unew)
{
    char oldname[256], newname[256];
    int r, err;
    struct inode *od = resolve_parent(ofd, uold, oldname, &err);
    if (!od)
        return err;
    struct inode *nd = resolve_parent(nfd, unew, newname, &err);
    if (!nd) {
        iput(od);
        return err;
    }
    struct inode *ip = NULL, *tip = NULL, *m;
    if ((r = vfs_lookup(od, oldname, strlen(oldname), &ip)) < 0)
        goto out;
    if ((m = vfs_covering(ip))) {
        iput(m);
        r = -EBUSY;
        goto out;
    }
    if ((r = may_delete(od, ip)) < 0 || (r = inode_permission(nd, W_OK | X_OK)) < 0)
        goto out;
    if (vfs_lookup(nd, newname, strlen(newname), &tip) == 0) {
        if (!same_inode(tip, ip)) {
            if ((m = vfs_covering(tip))) {
                iput(m);
                r = -EBUSY;
                goto out;
            }
            if ((r = may_delete(nd, tip)) < 0)
                goto out;
        }
        iput(tip);
        tip = NULL;
    }
    /* Moving a directory to another parent rewrites its "..": needs write access. */
    if (S_ISDIR(inode_mode(ip)) && !same_inode(od, nd) && (r = inode_permission(ip, W_OK)) < 0)
        goto out;
    iput(ip);
    ip = NULL;
    r = vfs_rename(od, oldname, nd, newname);
out:
    if (tip)
        iput(tip);
    if (ip)
        iput(ip);
    iput(nd);
    iput(od);
    return r;
}

long fsys_link(int ofd, const char *uold, int nfd, const char *unew, int flags)
{
    char name[256];
    int r, err;
    struct inode *ip = resolve(ofd, uold, (flags & AT_FOLLOW_K) ? 0 : AT_NOFOLLOW_K, &err);
    if (!ip)
        return err;
    struct inode *dir = resolve_parent(nfd, unew, name, &err);
    if (!dir) {
        iput(ip);
        return err;
    }
    if (S_ISDIR(inode_mode(ip)))
        r = -EPERM;
    else if ((r = inode_permission(dir, W_OK | X_OK)) == 0)
        r = vfs_link(dir, name, ip);
    iput(dir);
    iput(ip);
    return r;
}

long fsys_symlink(const char *utarget, int dirfd, const char *upath)
{
    char name[256];
    char *tgt = kmalloc(SYMLINK_MAX + 1);
    int r, err;
    if (!tgt)
        return -ENOMEM;
    if ((r = user_fetch_str(utarget, tgt, SYMLINK_MAX + 1)) < 0)
        goto out;
    r = -ENOENT;
    if (!tgt[0])
        goto out;
    struct inode *dir = resolve_parent(dirfd, upath, name, &err);
    if (!dir) {
        r = err;
        goto out;
    }
    if ((r = inode_permission(dir, W_OK | X_OK)) == 0)
        r = vfs_symlink(dir, name, tgt, current->euid, new_gid(dir));
    iput(dir);
out:
    kfree(tgt);
    return r;
}

long fsys_readlink(int dirfd, const char *upath, char *ubuf, size_t n)
{
    int err;
    if ((long)n <= 0)
        return -EINVAL;
    struct inode *ip = resolve(dirfd, upath, AT_NOFOLLOW_K, &err);
    if (!ip)
        return err;
    char *buf = kmalloc(SYMLINK_MAX + 1);
    if (!buf) {
        iput(ip);
        return -ENOMEM;
    }
    long r = vfs_readlink(ip, buf, MIN(n, (size_t)SYMLINK_MAX + 1));
    iput(ip);
    if (r >= 0 && !user_ok(ubuf, r, true))
        r = -EFAULT;
    if (r > 0)
        memcpy(ubuf, buf, r);
    kfree(buf);
    return r;
}

/* ------------------------------------------------------------------ */
/* Attributes                                                          */
/* ------------------------------------------------------------------ */

long fsys_stat(int dirfd, const char *upath, struct kstat *st, int flags)
{
    int err;
    if (!upath) {
        struct file *f = fsys_file(dirfd);
        if (!f)
            return -EBADF;
        if (f->ip) {
            inode_getstat(f->ip, st);
            return 0;
        }
        memset(st, 0, sizeof(*st));
        bool sock = f->type == FD_SOCKET || f->type == FD_UNIX;
        st->mode = f->type == FD_PIPE ? (S_IFIFO | 0600) : sock ? (S_IFSOCK | 0777)
                                                                                : (S_IFCHR | 0620);
        st->nlink = 1;
        st->uid = current->euid;
        st->gid = current->egid;
        st->blksize = PAGE_SIZE;
        st->fstype = f->type == FD_PIPE ? "fifofs" : sock ? "sockfs" : "specfs";
        if (f->type == FD_TTY) {
            int n = f->tty ? pty_index(f->tty) : -1;
            st->rdev = n >= 0 ? MKDEV(DEV_PTS_MAJOR, n) : MKDEV(DEV_TTY_MAJOR, 1);
        }
        return 0;
    }
    struct inode *ip = resolve(dirfd, upath, flags, &err);
    if (!ip)
        return err;
    inode_getstat(ip, st);
    iput(ip);
    return 0;
}

long fsys_chmod(int dirfd, const char *upath, int mode, int flags)
{
    int err;
    struct inode *ip = target(dirfd, upath, flags, &err);
    if (!ip)
        return err;
    long r;
    if (!inode_owner_or_root(ip))
        r = -EPERM;
    else if (S_ISLNK(inode_mode(ip))) {
        r = inode_setattr(ip, mode & 0777, -1, -1);  /* lchmod: kept, as BSD's (links' access ignores it) */
    } else {
        if (!is_root() && !cred_in_group(current, inode_gid(ip)))
            mode &= ~S_ISGID;
        if (!is_root() && !S_ISDIR(inode_mode(ip)))
            mode &= ~S_ISVTX;
        r = inode_setattr(ip, mode & 07777, -1, -1);
    }
    iput(ip);
    return r;
}

/*
 * chown follows the restricted-chown rule (as in Solaris with rstchown=1):
 * only root may give files away; an owner may change the group to one of
 * its own groups.  A non-root chown clears the set-ID bits.
 */
long fsys_chown(int dirfd, const char *upath, int uid, int gid, int flags)
{
    int err;
    struct inode *ip = target(dirfd, upath, flags, &err);
    if (!ip)
        return err;
    long r = 0;
    if (uid == inode_uid(ip))
        uid = -1;
    if (gid == inode_gid(ip))
        gid = -1;
    if (!is_root() && (uid != -1 || !inode_owner_or_root(ip) || (gid != -1 && !cred_in_group(current, gid))))
        r = -EPERM;
    if (r == 0) {
        int mode = -1;
        if (!is_root() && (uid != -1 || gid != -1) && !S_ISDIR(inode_mode(ip)))
            mode = inode_mode(ip) & ~(S_ISUID | S_ISGID) & 07777;
        if (uid != -1 || gid != -1 || mode != -1)
            r = inode_setattr(ip, mode, uid, gid);
    }
    iput(ip);
    return r;
}

/* access() checks with the real IDs unless AT_EACCESS is given. */
long fsys_access(int dirfd, const char *upath, int mode, int flags)
{
    int err, euid = current->euid, egid = current->egid;
    if (mode & ~7)
        return -EINVAL;
    if (!(flags & AT_EACCESS_K)) {
        current->euid = current->uid;
        current->egid = current->gid;
    }
    struct inode *ip = resolve(dirfd, upath, flags & AT_NOFOLLOW_K, &err);
    long r = ip ? (mode ? inode_permission(ip, mode & 7) : 0) : err;
    if (ip)
        iput(ip);
    current->euid = euid;
    current->egid = egid;
    return r;
}

/* ts: {atime sec, atime ns, mtime sec, mtime ns} with ns = -1 now, -2 omit; NULL: both now. */
long fsys_utimens(int dirfd, const char *upath, const int64_t *ts, int flags)
{
    int err;
    int64_t t[4], rt = realtime_ns(), now = rt / 1000000000L;
    long now_ns = rt % 1000000000L;
    bool explicit_time = false;
    for (int i = 0; i < 4; i += 2) {
        long ns = ts ? ts[i + 1] : -1;
        if (ns == -1) {
            t[i] = now;
            t[i + 1] = now_ns;
        } else if (ns == -2) {
            t[i] = 0;
            t[i + 1] = -1;
        } else if (ns < 0 || ns > 999999999) {
            return -EINVAL;
        } else {
            t[i] = ts[i];
            t[i + 1] = ns;
            explicit_time = true;
        }
    }
    struct inode *ip = target(dirfd, upath, flags, &err);
    if (!ip)
        return err;
    long r = 0;
    if (!inode_owner_or_root(ip)) {
        r = explicit_time ? -EPERM : inode_permission(ip, W_OK);
        if (r == -EACCES && explicit_time)
            r = -EPERM;
    }
    if (r == 0 && (t[1] >= 0 || t[3] >= 0))
        r = inode_settimes(ip, t[0], t[1], t[2], t[3]);
    iput(ip);
    return r;
}

long fsys_ftruncate(int fd, int64_t len)
{
    struct file *f = fsys_file(fd);
    if (!f)
        return -EBADF;
    if (len < 0 || f->type != FD_INODE || !S_ISREG(inode_mode(f->ip)))
        return -EINVAL;
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    return itruncate(f->ip, len);
}

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */

static long set_dir(struct inode *ip, struct inode **slot)
{
    long r = 0;
    if (!S_ISDIR(inode_mode(ip)))
        r = -ENOTDIR;
    else
        r = inode_permission(ip, X_OK);
    if (r < 0) {
        iput(ip);
        return r;
    }
    iput(*slot);
    *slot = ip;
    return 0;
}

long fsys_chdir(const char *upath)
{
    int err;
    struct inode *ip = resolve(AT_FDCWD_K, upath, 0, &err);
    return ip ? set_dir(ip, &current->cwd) : err;
}

long fsys_fchdir(int fd)
{
    struct file *f = fsys_file(fd);
    if (!f)
        return -EBADF;
    if (!f->ip)
        return -ENOTDIR;
    return set_dir(idup(f->ip), &current->cwd);
}

long fsys_chroot(const char *upath)
{
    int err;
    if (!is_root())
        return -EPERM;
    struct inode *ip = resolve(AT_FDCWD_K, upath, 0, &err);
    return ip ? set_dir(ip, &current->root) : err;
}

long fsys_getcwd(char *ubuf, size_t n)
{
    char *path = path_get();
    if (!path)
        return -ENOMEM;
    long len = vfs_dir_path(current->cwd, path, MAXPATH);
    if (len >= 0 && n < (size_t)len)
        len = -ERANGE;
    else if (len >= 0 && !user_ok(ubuf, len, true))
        len = -EFAULT;
    else if (len >= 0)
        memcpy(ubuf, path, len);
    path_put(path);
    return len;
}

long fsys_statvfs(int fd, const char *upath, struct kstatvfs *sv)
{
    int err;
    struct file *f = upath ? NULL : fsys_file(fd);
    if (f && !f->ip) {                               /* pipes and sockets: fifofs/sockfs */
        memset(sv, 0, sizeof(*sv));
        sv->bsize = PAGE_SIZE;
        sv->namemax = 255;
        return 0;
    }
    struct inode *ip = target(fd, upath, 0, &err);
    if (!ip)
        return err;
    vfs_statvfs(ip, sv);
    iput(ip);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Descriptors                                                         */
/* ------------------------------------------------------------------ */

long fsys_lseek(int fd, int64_t off, int whence)
{
    struct file *f = fsys_file(fd);
    if (!f)
        return -EBADF;
    if (f->type == FD_BLK) {                         /* a disk: its size is the end */
        int64_t end = (int64_t)blk_sectors(f->minor) * 512;
        int64_t b = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->off : whence == SEEK_END ? end : -1;
        if (b < 0 || b + off < 0)
            return -EINVAL;
        f->off = b + off;
        return f->off;
    }
    if (f->type == FD_NULL || f->type == FD_ZERO || f->type == FD_RANDOM)
        return whence >= SEEK_SET && whence <= SEEK_END ? 0 : -EINVAL;   /* (as Linux: always at 0) */
    if (f->type != FD_INODE)
        return -ESPIPE;
    int64_t base, size = inode_size(f->ip);
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = f->off; break;
    case SEEK_END: base = size; break;
    case 3:                                          /* SEEK_DATA: no holes are reported */
    case 4:                                          /* SEEK_HOLE: the end of the file */
        if (off < 0 || off >= size)
            return -ENXIO;
        f->off = whence == 3 ? off : size;
        return f->off;
    default: return -EINVAL;
    }
    if (base + off < 0)
        return -EINVAL;
    f->off = base + off;                             /* directories: a getdents cookie */
    return f->off;
}

long fsys_dup(int fd, int from, bool cloexec)
{
    struct file *f = fsys_file(fd);
    if (!f)
        return -EBADF;
    if (from < 0 || from >= NOFILE)
        return -EINVAL;
    int nfd = fsys_fdalloc(f, from);
    if (nfd >= 0) {
        file_dup(f);
        current->fdflags[nfd] = cloexec ? FD_CLOEXEC : 0;
    }
    return nfd;
}

long fsys_dup2(int fd, int to, bool cloexec)
{
    struct file *f = fsys_file(fd);
    if (!f || to < 0 || to >= NOFILE)
        return -EBADF;
    if (fd == to)
        return to;
    file_dup(f);
    fd_close(current, to);
    current->ofile[to] = f;
    current->fdflags[to] = cloexec ? FD_CLOEXEC : 0;
    return to;
}
