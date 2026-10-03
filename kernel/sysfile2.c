/*
 * sysfile2.c - ABI v2 file calls: the *at family, descriptors, fcntl with
 * record locks, positioned and vector I/O, getdents with d_off cookies,
 * statvfs, and the terminal/pseudo-terminal ioctls.  The work is done by
 * fsys.c; this file translates the Solaris constants and structures.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "proc.h"
#include "mm.h"
#include "fs.h"
#include "tty.h"
#include "poll.h"
#include "display.h"
#include "net.h"
#include "abi2.h"
#include "sieos/syscall.h"
#include "sieos/errno.h"
#include "sieos/fcntl.h"
#include "sieos/stat.h"
#include "sieos/sysinfo.h"
#include "sieos/termios.h"
#include "sieos/time.h"
#include "sieos/socket.h"
#include "sieos/mount.h"
#include "sieos/lofi.h"
#include "sieos/dkio.h"
#include "sieos/power.h"
#include "power.h"
#include "blkdev.h"

/* ---------------- translations ---------------- */

static int dfd(long fd)
{
    return (int)fd == SIEOS_AT_FDCWD ? AT_FDCWD_K : (int)fd;
}

static int at_flags(long f, long allowed, bool *ok)
{
    *ok = !(f & ~allowed);
    int k = 0;
    if (f & SIEOS_AT_SYMLINK_NOFOLLOW) k |= AT_NOFOLLOW_K;
    if (f & SIEOS_AT_SYMLINK_FOLLOW)   k |= AT_FOLLOW_K;
    if (f & SIEOS_AT_REMOVEDIR)        k |= AT_REMOVEDIR_K;
    if (f & SIEOS_AT_EACCESS)          k |= AT_EACCESS_K;
    return k;
}

int sieos_oflags_to_k(long f, bool *ok)
{
    *ok = true;
    int acc = f & 3;
    if (acc == 3 || (f & (SIEOS_O_SEARCH | SIEOS_O_EXEC)))
        acc = O_RDONLY;                              /* search/exec opens: read access suffices here */
    int v = acc;
    if (f & SIEOS_O_APPEND) v |= O_APPEND;
    if (f & SIEOS_O_CREAT) v |= O_CREAT;
    if (f & SIEOS_O_TRUNC) v |= O_TRUNC;
    if (f & SIEOS_O_EXCL) v |= O_EXCL;
    if (f & SIEOS_O_NOCTTY) v |= O_NOCTTY;
    if (f & SIEOS_O_DIRECTORY) v |= O_DIRECTORY;
    if (f & (SIEOS_O_NONBLOCK | SIEOS_O_NDELAY)) v |= O_NONBLOCK_K;
    if (f & SIEOS_O_NOFOLLOW) v |= O_NOFOLLOW_K;
    if (f & SIEOS_O_CLOEXEC) v |= O_CLOEXEC_K;
    if (f & (SIEOS_O_SYNC | SIEOS_O_DSYNC | SIEOS_O_RSYNC)) v |= O_SYNC_K;
    long known = 3 | SIEOS_O_SEARCH | SIEOS_O_EXEC | SIEOS_O_APPEND | SIEOS_O_CREAT | SIEOS_O_TRUNC |
                 SIEOS_O_EXCL | SIEOS_O_NOCTTY | SIEOS_O_DIRECTORY | SIEOS_O_LARGEFILE | SIEOS_O_CLOEXEC |
                 SIEOS_O_NONBLOCK | SIEOS_O_NDELAY | SIEOS_O_SYNC | SIEOS_O_DSYNC | SIEOS_O_RSYNC |
                 SIEOS_O_NOFOLLOW;
    if (f & ~known)
        *ok = false;
    return v;
}

static long k_oflags_to_sieos(int k)
{
    long v = k & O_ACCMODE;
    if (k & O_APPEND) v |= SIEOS_O_APPEND;
    if (k & O_NONBLOCK_K) v |= SIEOS_O_NONBLOCK;
    if (k & O_SYNC_K) v |= SIEOS_O_SYNC;
    if (k & O_NOCTTY) v |= SIEOS_O_NOCTTY;
    return v | SIEOS_O_LARGEFILE;
}

static void to_sieos_stat(const struct kstat *k, struct sieos_stat *s)
{
    memset(s, 0, sizeof(*s));
    s->st_dev = SIEOS_MAKEDEV(k->dev_major, k->dev_minor);
    s->st_ino = k->ino;
    s->st_mode = k->mode;
    s->st_nlink = k->nlink;
    s->st_uid = k->uid;
    s->st_gid = k->gid;
    s->st_rdev = k->rdev ? SIEOS_MAKEDEV(MAJOR(k->rdev), MINOR(k->rdev)) : 0;
    s->st_size = k->size;
    s->st_atim.tv_sec = k->atime;
    s->st_atim.tv_nsec = k->atime_ns;
    s->st_mtim.tv_sec = k->mtime;
    s->st_mtim.tv_nsec = k->mtime_ns;
    s->st_ctim.tv_sec = k->ctime;
    s->st_ctim.tv_nsec = k->ctime_ns;
    s->st_blksize = k->blksize ? k->blksize : 4096;
    s->st_blocks = k->blocks;
    strlcpy(s->st_fstype, k->fstype ? k->fstype : "", sizeof(s->st_fstype));
}

static long do_fstatat(long fd, const char *upath, struct sieos_stat *ust, long flag)
{
    bool ok;
    int fl = at_flags(flag, SIEOS_AT_SYMLINK_NOFOLLOW, &ok);
    if (!ok)
        return -EINVAL;
    if (!user_ok(ust, sizeof(*ust), true))
        return -EFAULT;
    struct kstat k;
    long r = fsys_stat(upath ? dfd(fd) : (int)fd, upath, &k, fl);
    if (r < 0)
        return r;
    struct sieos_stat s;
    to_sieos_stat(&k, &s);
    memcpy(ust, &s, sizeof(s));
    return 0;
}

/* ---------------- getdents ---------------- */

struct v2_dents {
    uint8_t *buf;
    size_t size, used;
    bool small;
};

static int v2_fill(void *arg, const char *name, size_t len, uint64_t ino, int dtype, uint64_t next)
{
    UNUSED(dtype);
    struct v2_dents *a = arg;
    size_t reclen = (SIEOS_DIRENT_NAME_OFFSET + len + 1 + 7) & ~7UL;
    if (a->used + reclen > a->size) {
        a->small = a->used == 0;
        return 1;
    }
    struct sieos_dirent *e = (struct sieos_dirent *)(a->buf + a->used);
    memset(e, 0, reclen);
    e->d_ino = ino;
    e->d_off = next;
    e->d_reclen = reclen;
    memcpy(e->d_name, name, len);
    a->used += reclen;
    return 0;
}

static long do_getdents(long fd, uint8_t *ubuf, size_t n)
{
    struct file *f = fsys_file(fd);
    if (!f)
        return -EBADF;
    if (f->type != FD_INODE || !S_ISDIR(inode_mode(f->ip)))
        return -ENOTDIR;
    if (!user_ok(ubuf, n, true))
        return -EFAULT;
    struct v2_dents a = { ubuf, n, 0, false };
    int r = vfs_readdir(f->ip, &f->off, v2_fill, &a);
    if (r < 0)
        return r;
    return a.small ? -EINVAL : (long)a.used;
}

/* ---------------- I/O ---------------- */

static long do_rw(long fd, void *ubuf, size_t n, bool write, bool positioned, int64_t off)
{
    struct file *f = fsys_file(fd);
    if (!f)
        return -EBADF;
    if (!user_ok(ubuf, n, !write))
        return -EFAULT;
    if (positioned && off < 0)
        return -EINVAL;
    /* the file is held while the transfer may sleep: another thread closing
     * the descriptor meanwhile must not free it (its pipe, socket ...) */
    file_dup(f);
    long r;
    if (positioned)
        r = write ? file_pwrite(f, ubuf, n, off) : file_pread(f, ubuf, n, off);
    else
        r = write ? file_write(f, ubuf, n) : file_read(f, ubuf, n);
    file_close(f);
    return r;
}

static long do_rwv(long fd, const struct sieos_iovec *uiov, int cnt, bool write)
{
    if (cnt <= 0 || cnt > 1024)
        return -EINVAL;
    if (!user_ok(uiov, cnt * sizeof(*uiov), false))
        return -EFAULT;
    long total = 0;
    for (int i = 0; i < cnt; i++) {
        struct sieos_iovec v = uiov[i];
        if ((long)v.iov_len < 0)
            return -EINVAL;
        if (!v.iov_len)
            continue;
        long r = do_rw(fd, v.iov_base, v.iov_len, write, false, 0);
        if (r < 0)
            return total ? total : r;
        total += r;
        if ((size_t)r < v.iov_len)
            break;                                   /* short transfer: stop here */
    }
    return total;
}

/* preadv / pwritev: the buffers in turn from off; the file offset stays. */
static long do_prwv(long fd, const struct sieos_iovec *uiov, int cnt, int64_t off, bool write)
{
    if (cnt <= 0 || cnt > 1024 || off < 0)
        return -EINVAL;
    if (!user_ok(uiov, cnt * sizeof(*uiov), false))
        return -EFAULT;
    long total = 0;
    for (int i = 0; i < cnt; i++) {
        struct sieos_iovec v = uiov[i];
        if ((long)v.iov_len < 0)
            return -EINVAL;
        if (!v.iov_len)
            continue;
        long r = do_rw(fd, v.iov_base, v.iov_len, write, true, off + total);
        if (r < 0)
            return total ? total : r;
        total += r;
        if ((size_t)r < v.iov_len)
            break;
    }
    return total;
}

#define XFER_BUF 65536

/* Write all of buf (n bytes) to f, at *off if off; what was written. */
static long write_all(struct file *f, const void *buf, size_t n, int64_t *off)
{
    size_t done = 0;
    while (done < n) {
        long w = off ? file_pwrite(f, (const char *)buf + done, n - done, *off + done)
                     : file_write(f, (const char *)buf + done, n - done);
        if (w <= 0)
            return done ? (long)done : (w ? w : -EIO);
        done += w;
    }
    if (off)
        *off += done;
    return done;
}

/*
 * The data of copy_file_range and splice: up to len bytes from fin (at *uoff_in, or
 * its offset) to fout (at *uoff_out, or its offset), through a kernel buffer.  once:
 * stop after the first read (splice: what a pipe or socket has now).
 */
static long xfer(long fd_in, int64_t *uoff_in, long fd_out, int64_t *uoff_out, size_t len, bool once)
{
    struct file *fi = fsys_file(fd_in), *fo = fsys_file(fd_out);
    if (!fi || !fo)
        return -EBADF;
    if ((uoff_in && !user_ok(uoff_in, 8, true)) || (uoff_out && !user_ok(uoff_out, 8, true)))
        return -EFAULT;
    int64_t oi = uoff_in ? *uoff_in : 0, oo = uoff_out ? *uoff_out : 0;
    if (oi < 0 || oo < 0)
        return -EINVAL;
    char *buf = kmalloc(XFER_BUF);
    if (!buf)
        return -ENOMEM;
    file_dup(fi);
    file_dup(fo);
    long total = 0, r = 0;
    while ((size_t)total < len) {
        size_t n = MIN(len - total, (size_t)XFER_BUF);
        r = uoff_in ? file_pread(fi, buf, n, oi) : file_read(fi, buf, n);
        if (r <= 0)
            break;
        if (uoff_in)
            oi += r;
        long w = write_all(fo, buf, r, uoff_out ? &oo : NULL);
        if (w < 0) {
            r = w;
            break;
        }
        total += w;
        if (w < r || once || (size_t)r < n)
            break;
    }
    file_close(fi);
    file_close(fo);
    kfree(buf);
    if (uoff_in)
        *uoff_in = oi;
    if (uoff_out)
        *uoff_out = oo;
    return total ? total : r;
}

/* copy_file_range(fd_in, *off_in, fd_out, *off_out, len, flags): regular files only. */
static long do_copy_file_range(long fd_in, int64_t *off_in, long fd_out, int64_t *off_out, size_t len, long flags)
{
    struct file *fi = fsys_file(fd_in), *fo = fsys_file(fd_out);
    if (!fi || !fo)
        return -EBADF;
    if (flags)
        return -EINVAL;
    if (fi->type != FD_INODE || fo->type != FD_INODE || !fi->ip || !fo->ip)
        return -EINVAL;
    if (S_ISDIR(inode_mode(fi->ip)) || S_ISDIR(inode_mode(fo->ip)))
        return -EISDIR;
    if (!S_ISREG(inode_mode(fi->ip)) || !S_ISREG(inode_mode(fo->ip)))
        return -EINVAL;
    if ((fi->flags & O_ACCMODE) == O_WRONLY || (fo->flags & O_ACCMODE) == O_RDONLY || (fo->flags & O_APPEND))
        return -EBADF;
    return xfer(fd_in, off_in, fd_out, off_out, len, false);
}

/* splice(fd_in, *off_in, fd_out, *off_out, len, flags): one end a pipe (its offset NULL). */
static long do_splice(long fd_in, int64_t *off_in, long fd_out, int64_t *off_out, size_t len, long flags)
{
    struct file *fi = fsys_file(fd_in), *fo = fsys_file(fd_out);
    if (!fi || !fo)
        return -EBADF;
    if (flags & ~0xFL)                               /* SPLICE_F_MOVE, NONBLOCK, MORE, GIFT */
        return -EINVAL;
    if (fi->type != FD_PIPE && fo->type != FD_PIPE)
        return -EINVAL;
    if ((fi->type == FD_PIPE && off_in) || (fo->type == FD_PIPE && off_out))
        return -ESPIPE;
    if (len == 0)
        return 0;
    return xfer(fd_in, off_in, fd_out, off_out, len, true);
}

/* ---------------- fcntl ---------------- */

static long flock_range(struct file *f, const struct sieos_flock *u, struct kflock *k)
{
    int64_t base;
    switch (u->l_whence) {
    case SIEOS_SEEK_SET: base = 0; break;
    case SIEOS_SEEK_CUR: base = f->off; break;
    case SIEOS_SEEK_END: base = inode_size(f->ip); break;
    default: return -EINVAL;
    }
    int64_t start = base + u->l_start, len = u->l_len;
    if (len < 0) {
        start += len;
        len = -len;
    }
    if (start < 0)
        return -EINVAL;
    k->start = start;
    k->end = len ? (uint64_t)(start + len - 1) : UINT64_MAX;
    k->pid = current->pid;
    return 0;
}

static long do_fcntl(long fd, long cmd, uint64_t arg)
{
    struct file *f = fsys_file(fd);
    if (!f)
        return -EBADF;
    switch (cmd) {
    case SIEOS_F_DUPFD:
    case SIEOS_F_DUPFD_CLOEXEC:
        return fsys_dup(fd, (int)arg, cmd == SIEOS_F_DUPFD_CLOEXEC);
    case SIEOS_F_DUP2FD:
    case SIEOS_F_DUP2FD_CLOEXEC:
        return fsys_dup2(fd, (int)arg, cmd == SIEOS_F_DUP2FD_CLOEXEC);
    case SIEOS_F_GETFD: {
        int fl = fd_getflags(current, fd);
        return fl < 0 ? fl : (fl & FD_CLOEXEC) ? SIEOS_FD_CLOEXEC : 0;
    }
    case SIEOS_F_SETFD:
        return fd_setflags(current, fd, (arg & SIEOS_FD_CLOEXEC) ? FD_CLOEXEC : 0);
    case SIEOS_F_GETFL:
        return k_oflags_to_sieos(f->flags);
    case SIEOS_F_SETFL: {
        bool ok;
        int k = sieos_oflags_to_k(arg & ~3L, &ok);
        f->flags = (f->flags & ~O_SETFL_K) | (k & O_SETFL_K);
        return 0;
    }
    case SIEOS_F_GETOWN:
        return 0;
    case SIEOS_F_SETOWN:
        return 0;
    case SIEOS_F_GETLK:
    case SIEOS_F_SETLK:
    case SIEOS_F_SETLKW:
    case SIEOS_F_FREESP: {
        struct sieos_flock *ul = (struct sieos_flock *)arg;
        if (!user_ok(ul, sizeof(*ul), cmd == SIEOS_F_GETLK))
            return -EFAULT;
        if (f->type != FD_INODE || !f->ip || S_ISDIR(inode_mode(f->ip)))
            return -EINVAL;
        struct sieos_flock u = *ul;
        struct kflock k;
        long r = flock_range(f, &u, &k);
        if (r < 0)
            return r;
        if (cmd == SIEOS_F_FREESP) {
            if ((f->flags & O_ACCMODE) == O_RDONLY)
                return -EBADF;
            uint64_t size = inode_size(f->ip);
            if (k.end == UINT64_MAX || k.end + 1 >= size)
                return itruncate(f->ip, k.start);    /* the common case: truncate/extend to l_start */
            static const char zero[512];
            for (uint64_t o = k.start; o <= k.end;) {  /* a hole in the middle: zero-fill it */
                size_t n = MIN(sizeof(zero), k.end + 1 - o);
                long w = writei(f->ip, zero, o, n);
                if (w <= 0)
                    return w < 0 ? w : -EIO;
                o += w;
            }
            return 0;
        }
        switch (u.l_type) {
        case SIEOS_F_RDLCK: k.type = F_RDLCK_K; break;
        case SIEOS_F_WRLCK: k.type = F_WRLCK_K; break;
        case SIEOS_F_UNLCK: k.type = F_UNLCK_K; break;
        default: return -EINVAL;
        }
        if (cmd == SIEOS_F_GETLK) {
            if (k.type == F_UNLCK_K)
                return -EINVAL;
            flock_get(f->ip, &k);
            u.l_type = k.type == F_RDLCK_K ? SIEOS_F_RDLCK : k.type == F_WRLCK_K ? SIEOS_F_WRLCK : SIEOS_F_UNLCK;
            if (k.type != F_UNLCK_K) {
                u.l_whence = SIEOS_SEEK_SET;
                u.l_start = k.start;
                u.l_len = k.end == UINT64_MAX ? 0 : (int64_t)(k.end - k.start + 1);
                u.l_pid = k.pid;
                u.l_sysid = 0;
            }
            memcpy(ul, &u, sizeof(u));
            return 0;
        }
        int acc = f->flags & O_ACCMODE;
        if ((k.type == F_RDLCK_K && acc == O_WRONLY) || (k.type == F_WRLCK_K && acc == O_RDONLY))
            return -EBADF;
        return flock_set(f->ip, &k, cmd == SIEOS_F_SETLKW);
    }
    }
    return -EINVAL;
}

/* ---------------- ioctl ---------------- */

static void termios_to_v2(const struct termios *k, struct sieos_termios *t)
{
    memset(t, 0, sizeof(*t));
    t->c_iflag = k->c_iflag;                 /* the flag bits have the Solaris values */
    t->c_oflag = k->c_oflag;
    t->c_cflag = k->c_cflag;
    t->c_lflag = k->c_lflag;
    for (int i = 0; i < SIEOS_NCCS; i++)
        t->c_cc[i] = i < NCCS ? k->c_cc[i] : 0;
    bool canon = k->c_lflag & ICANON;
    t->c_cc[SIEOS_VEOF] = canon ? k->c_cc[VEOF] : k->c_cc[VMIN];     /* VMIN shares VEOF's slot */
    t->c_cc[SIEOS_VEOL] = canon ? 0 : k->c_cc[VTIME];                /* VTIME shares VEOL's */
}

static void termios_from_v2(const struct sieos_termios *t, struct termios *k)
{
    struct termios old = *k;
    k->c_iflag = t->c_iflag;
    k->c_oflag = t->c_oflag;
    k->c_cflag = t->c_cflag;
    k->c_lflag = t->c_lflag;
    for (int i = 0; i < NCCS && i < SIEOS_NCCS; i++)
        if (i != VEOF && i != VTIME && i != VMIN)
            k->c_cc[i] = t->c_cc[i];
    if (t->c_lflag & SIEOS_ICANON) {
        k->c_cc[VEOF] = t->c_cc[SIEOS_VEOF];
        k->c_cc[VMIN] = old.c_cc[VMIN];
        k->c_cc[VTIME] = old.c_cc[VTIME];
    } else {
        k->c_cc[VMIN] = t->c_cc[SIEOS_VMIN];
        k->c_cc[VTIME] = t->c_cc[SIEOS_VTIME];
        k->c_cc[VEOF] = old.c_cc[VEOF];
    }
}

static long tty_call(struct tty *t, unsigned long cmd, uint64_t arg)
{
    return tty_ioctl(t, cmd, arg);
}

/* ---------------- lofi ---------------- */

static long lofi_ioctl(unsigned long cmd, struct sieos_lofi_ioctl *uli)
{
    if (current->euid != 0)
        return -EPERM;
    if (!user_ok(uli, sizeof(*uli), true))
        return -EFAULT;
    struct sieos_lofi_ioctl *li = kmalloc(sizeof(*li));
    if (!li)
        return -ENOMEM;
    memcpy(li, uli, sizeof(*li));
    li->li_filename[sizeof(li->li_filename) - 1] = 0;
    long r;
    if (cmd == SIEOS_LOFI_MAP_FILE) {
        int err;
        struct inode *ip = namei(li->li_filename, &err);
        if (!ip) {
            r = err;
        } else if (!S_ISREG(inode_mode(ip)) || inode_size(ip) < 1024) {
            iput(ip);
            r = -EINVAL;
        } else {
            char abs[SIEOS_LOFI_PATH_MAX];
            strlcpy(abs, li->li_filename, sizeof(abs));
            r = blk_lofi_attach(ip, abs, li->li_readonly || inode_readonly(ip));
            iput(ip);
            if (r >= 0) {
                uli->li_minor = r - BLK_LOFI0 + 1;
                r = 0;
            }
        }
    } else if (li->li_minor < 1 || li->li_minor > NLOFI) {
        r = -ENXIO;
    } else if (cmd == SIEOS_LOFI_UNMAP_FILE_MINOR) {
        r = blk_lofi_detach(BLK_LOFI0 + li->li_minor - 1);
    } else {
        r = blk_lofi_file(BLK_LOFI0 + li->li_minor - 1, li->li_filename, sizeof(li->li_filename));
        if (r == 0)
            memcpy(uli->li_filename, li->li_filename, sizeof(li->li_filename));
    }
    kfree(li);
    return r;
}

/* ---------------- mount ---------------- */

struct nonempty { bool any; };

static int nonempty_cb(void *arg, const char *name, size_t len, uint64_t ino, int dtype, uint64_t next)
{
    UNUSED(ino);
    UNUSED(dtype);
    UNUSED(next);
    if (!(len == 1 && name[0] == '.') && !(len == 2 && name[0] == '.' && name[1] == '.')) {
        ((struct nonempty *)arg)->any = true;
        return 1;
    }
    return 0;
}

static long mount_at(const char *uspec, const char *udir, long mflag, const char *utype, char *dir);

/* mount(spec, dir, mflag, fstype, dataptr, datalen): tmpfs and proc. */
static long do_mount(const char *uspec, const char *udir, long mflag, const char *utype)
{
    char *dir = path_get();
    if (!dir)
        return -ENOMEM;
    long r = mount_at(uspec, udir, mflag, utype, dir);
    path_put(dir);
    return r;
}

static long mount_at(const char *uspec, const char *udir, long mflag, const char *utype, char *dir)
{
    char spec[64], type[32], abs[64];
    if (current->euid != 0)
        return -EPERM;
    if (mflag & ~(long)(SIEOS_MS_RDONLY | SIEOS_MS_FSS | SIEOS_MS_DATA | SIEOS_MS_REMOUNT | SIEOS_MS_NOSUID |
                        SIEOS_MS_OVERLAY | SIEOS_MS_OPTIONSTR))
        return -EINVAL;
    if (user_fetch_str(udir, dir, MAXPATH) < 0 || user_fetch_str(utype, type, sizeof(type)) < 0)
        return -EFAULT;
    if (!uspec)
        strlcpy(spec, type, sizeof(spec));
    else if (user_fetch_str(uspec, spec, sizeof(spec)) < 0)
        return -EFAULT;
    int err;
    struct inode *ip = namei(dir, &err);
    if (!ip)
        return err;
    struct fs *on = vfs_mounted_on(ip);
    if (mflag & SIEOS_MS_REMOUNT) {                   /* new flags for the mount on dir */
        iput(ip);
        if (!on || on == root_fs)
            return on ? -EBUSY : -EINVAL;
        on->rdonly = mflag & SIEOS_MS_RDONLY;
        on->nosuid = mflag & SIEOS_MS_NOSUID;
        return 0;
    }
    long r = 0;
    struct nonempty ne = { false };
    if (!S_ISDIR(inode_mode(ip)))
        r = -ENOTDIR;
    else if (on)
        r = -EBUSY;                                    /* already a mount point */
    else if (!(mflag & SIEOS_MS_OVERLAY) && ip->fs->ops->readdir) {
        uint64_t off = 0;
        vfs_readdir(ip, &off, nonempty_cb, &ne);
        if (ne.any)
            r = -EBUSY;                                /* covering files needs MS_OVERLAY */
    }
    if (r == 0 && (err = vfs_dir_path(ip, abs, sizeof(abs))) < 0)
        r = err;
    iput(ip);
    if (r < 0)
        return r;
    struct fs *fs;
    if (!strcmp(type, "ext4")) {                       /* spec: a block device node */
        struct inode *dp = namei(spec, &err);
        if (!dp)
            return err;
        uint16_t m = inode_mode(dp);
        uint32_t rd = inode_rdev(dp);
        iput(dp);
        if (!S_ISBLK(m) || MAJOR(rd) != DEV_BLK_MAJOR)
            return -ENOTBLK_K;
        int dev = MINOR(rd);
        if (!blk_present(dev))
            return -ENXIO;
        if (blk_in_use(dev))
            return -EBUSY;
        if (!(fs = ext4_mount(dev, mflag & SIEOS_MS_RDONLY)))
            return -EINVAL;                            /* not an ext4 file system we can use */
    } else if (!strcmp(type, "tmpfs"))
        fs = tmpfs_create();
    else if (!strcmp(type, "proc"))
        fs = procfs_create();
    else
        return -ENODEV;
    if (!fs)
        return -ENOMEM;
    fs->rdonly = mflag & SIEOS_MS_RDONLY;
    fs->nosuid = mflag & SIEOS_MS_NOSUID;
    strlcpy(fs->special, spec, sizeof(fs->special));
    if ((r = vfs_mount(fs, abs)) < 0)
        fs->ops->destroy(fs);
    return r;
}

static long do_umount2(const char *udir, long mflag)
{
    if (current->euid != 0)
        return -EPERM;
    if (mflag & ~(long)SIEOS_MS_FORCE)
        return -EINVAL;
    char *dir = path_get();
    if (!dir)
        return -ENOMEM;
    int err = -EFAULT;
    struct inode *ip = user_fetch_str(udir, dir, MAXPATH) < 0 ? NULL : namei(dir, &err);
    path_put(dir);
    if (!ip)
        return err;
    struct fs *fs = vfs_mounted_on(ip);
    iput(ip);
    if (!fs)
        return -EINVAL;                                /* not a mount point */
    return vfs_umount(fs);
}

static long do_ioctl(long fd, unsigned long cmd, uint64_t arg)
{
    struct file *f = fsys_file(fd);
    if (!f)
        return -EBADF;
    int *ip = (int *)arg;
    switch (cmd) {
    case SIEOS_FIOCLEX:
        return fd_setflags(current, fd, FD_CLOEXEC);
    case SIEOS_FIONCLEX:
        return fd_setflags(current, fd, 0);
    case SIEOS_FIONBIO:
        if (!user_ok(ip, sizeof(int), false))
            return -EFAULT;
        f->flags = *ip ? (f->flags | O_NONBLOCK_K) : (f->flags & ~O_NONBLOCK_K);
        return 0;
    case SIEOS_FIONREAD: {
        if (!user_ok(ip, sizeof(int), true))
            return -EFAULT;
        long n = 0;
        if (f->type == FD_INODE && f->ip && S_ISREG(inode_mode(f->ip)))
            n = inode_size(f->ip) > f->off ? (long)(inode_size(f->ip) - f->off) : 0;
        else if (f->type == FD_PIPE && f->pipe)
            n = pipe_nread(f->pipe);
        else if (f->type == FD_UNIX && f->usock)
            n = unix_nread(f->usock);
        else if (f->type == FD_SOCKET && f->sock)
            n = socket_nread(f->sock);
        else
            n = (file_poll(f, POLLIN) & POLLIN) ? 1 : 0;
        *ip = (int)MIN(n, 0x7FFFFFFF);
        return 0;
    }
    default:
        if (f->type == FD_OPS && f->ops->ioctl)
            return f->ops->ioctl(f, cmd, (void *)arg);   /* (a driver's device) */
        break;
    }
    switch (cmd) {
    case SIEOS_ISPTM:
    case SIEOS_UNLKPT:
    case SIEOS_PTSNAME:
        if (f->type != FD_PTM)
            return -ENOTTY;
        return pty_master_ioctl(f->pty, cmd & 0xFF, (char *)arg);
    case SIEOS_DKIOCINFO: {
        if (f->type != FD_BLK)
            return -ENOTTY;
        if (!user_ok((void *)arg, sizeof(struct sieos_dk_info), true))
            return -EFAULT;
        struct sieos_dk_info di;
        int r = blk_info(f->minor, &di);
        if (r == 0)
            memcpy((void *)arg, &di, sizeof(di));
        return r;
    }
    case SIEOS_POWER_GET:
    case SIEOS_POWER_SET: {
        if (f->type != FD_POWER)
            return -ENOTTY;
        size_t sz = cmd == SIEOS_POWER_GET ? sizeof(struct sieos_power_info) : sizeof(struct sieos_power_set);
        if (!user_ok((void *)arg, sz, cmd == SIEOS_POWER_GET))
            return -EFAULT;
        return power_ioctl(cmd, (void *)arg);
    }
    case SIEOS_DKIOCREREAD:
        if (f->type != FD_BLK)
            return -ENOTTY;
        return blk_reread(f->minor);
    case SIEOS_LOFI_MAP_FILE:
    case SIEOS_LOFI_UNMAP_FILE_MINOR:
    case SIEOS_LOFI_GET_FILENAME:
        return f->type == FD_LOFICTL ? lofi_ioctl(cmd, (struct sieos_lofi_ioctl *)arg) : -ENOTTY;
    case SIEOS_FBIOGET_INFO:
    case SIEOS_FBIOGET_DISPLAY:
    case SIEOS_FBIOGET_MODES:
    case SIEOS_FBIOSET_MODE:
        if (f->type != FD_FB)
            return -ENOTTY;
        return fb_ioctl(f, cmd, arg);
    case SIEOS_TIOCGWINSZ:
    case SIEOS_TIOCSWINSZ: {
        unsigned long kc = cmd == SIEOS_TIOCGWINSZ ? TIOCGWINSZ : TIOCSWINSZ;
        if (f->type == FD_PTM)
            return pty_winsize(f->pty, kc, arg);
        return f->type == FD_TTY ? tty_call(f->tty, kc, arg) : -ENOTTY;
    }
    }
    if (f->type != FD_TTY)
        return -ENOTTY;
    struct tty *t = f->tty;
    switch (cmd) {
    case SIEOS_TCGETS: {
        struct sieos_termios *u = (struct sieos_termios *)arg;
        if (!user_ok(u, sizeof(*u), true))
            return -EFAULT;
        struct sieos_termios v;
        struct termios kt;
        tty_get_termios(t, &kt);
        termios_to_v2(&kt, &v);
        memcpy(u, &v, sizeof(v));
        return 0;
    }
    case SIEOS_TCSETS:
    case SIEOS_TCSETSW:
    case SIEOS_TCSETSF: {
        const struct sieos_termios *u = (const struct sieos_termios *)arg;
        if (!user_ok(u, sizeof(*u), false))
            return -EFAULT;
        struct termios kt;
        tty_get_termios(t, &kt);
        termios_from_v2(u, &kt);
        unsigned long kc = cmd == SIEOS_TCSETS ? TCSETS : cmd == SIEOS_TCSETSW ? TCSETSW : TCSETSF;
        return tty_set_termios(t, &kt, kc);
    }
    case SIEOS_TCSBRK:
        return 0;
    case SIEOS_TCXONC:                               /* tcflow: TCOOFF..TCION, accepted (no flow control) */
        return arg <= 3 ? 0 : -EINVAL;
    case SIEOS_TCFLSH:
        return tty_flush(t, (int)arg);
    case SIEOS_TIOCGPGRP: return tty_call(t, TIOCGPGRP, arg);
    case SIEOS_TIOCSPGRP: return tty_call(t, TIOCSPGRP, arg);
    case SIEOS_TIOCSCTTY: return tty_call(t, TIOCSCTTY, arg);
    case SIEOS_TIOCNOTTY: return tty_call(t, TIOCNOTTY, arg);
    case SIEOS_TIOCGSID:
        if (!user_ok(ip, sizeof(int), true))
            return -EFAULT;
        if (!t->session)
            return -ENOTTY;
        *ip = t->session;
        return 0;
    }
    return -ENOTTY;
}

/* ---------------- statvfs ---------------- */

static long do_statvfs(long fd, const char *upath, struct sieos_statvfs *u)
{
    if (!user_ok(u, sizeof(*u), true))
        return -EFAULT;
    struct kstatvfs k;
    long r = fsys_statvfs(fd, upath, &k);
    if (r < 0)
        return r;
    struct sieos_statvfs v;
    memset(&v, 0, sizeof(v));
    v.f_bsize = v.f_frsize = k.bsize;
    v.f_blocks = k.blocks;
    v.f_bfree = v.f_bavail = k.bfree;
    v.f_files = k.files;
    v.f_ffree = v.f_favail = k.ffree;
    v.f_namemax = k.namemax;
    v.f_flag = k.rdonly ? SIEOS_ST_RDONLY : 0;
    /* identify the file system of the target */
    struct kstat st;
    if (fsys_stat(fd, upath, &st, 0) == 0) {
        v.f_fsid = ((uint64_t)st.dev_major << 32) | st.dev_minor;
        strlcpy(v.f_basetype, st.fstype ? st.fstype : "", sizeof(v.f_basetype));
    }
    memcpy(u, &v, sizeof(v));
    return 0;
}

/* ---------------- dispatch ---------------- */

long syscall_file_v2(struct trapframe *tf, bool *handled)
{
    uint64_t a1 = tf->rdi, a2 = tf->rsi, a3 = tf->rdx, a4 = tf->r10, a5 = tf->r8, a6 = tf->r9;
    bool ok;
    int fl, k;
    *handled = true;
    switch (tf->rax) {
    case SIEOS_SYS_read:      return do_rw(a1, (void *)a2, a3, false, false, 0);
    case SIEOS_SYS_write:     return do_rw(a1, (void *)a2, a3, true, false, 0);
    case SIEOS_SYS_pread:     return do_rw(a1, (void *)a2, a3, false, true, (int64_t)a4);
    case SIEOS_SYS_pwrite:    return do_rw(a1, (void *)a2, a3, true, true, (int64_t)a4);
    case SIEOS_SYS_readv:     return do_rwv(a1, (const struct sieos_iovec *)a2, (int)a3, false);
    case SIEOS_SYS_preadv:    return do_prwv(a1, (const struct sieos_iovec *)a2, (int)a3, (int64_t)a4, false);
    case SIEOS_SYS_pwritev:   return do_prwv(a1, (const struct sieos_iovec *)a2, (int)a3, (int64_t)a4, true);
    case SIEOS_SYS_copy_file_range:
        return do_copy_file_range(a1, (int64_t *)a2, a3, (int64_t *)a4, a5, (long)a6);
    case SIEOS_SYS_splice:    return do_splice(a1, (int64_t *)a2, a3, (int64_t *)a4, a5, (long)a6);
    case SIEOS_SYS_writev:    return do_rwv(a1, (const struct sieos_iovec *)a2, (int)a3, true);
    case SIEOS_SYS_close:
        if (!fd_close(current, a1))
            return -EBADF;
        return 0;
    case SIEOS_SYS_lseek:     return fsys_lseek(a1, (int64_t)a2, (int)a3);
    case SIEOS_SYS_openat:
        k = sieos_oflags_to_k(a3, &ok);
        if (!ok)
            return -EINVAL;
        return fsys_open(dfd(a1), (const char *)a2, k, a4);
    case SIEOS_SYS_fcntl:     return do_fcntl(a1, a2, a3);
    case SIEOS_SYS_ioctl:     return do_ioctl(a1, a2, a3);
    case SIEOS_SYS_mount:     return do_mount((const char *)a1, (const char *)a2, a3, (const char *)a4);
    case SIEOS_SYS_umount2:   return do_umount2((const char *)a1, a2);
    case SIEOS_SYS_getdents:  return do_getdents(a1, (uint8_t *)a2, a3);
    case SIEOS_SYS_fstatat:   return do_fstatat(a1, (const char *)a2, (struct sieos_stat *)a3, a4);
    case SIEOS_SYS_fchmodat:
        fl = at_flags(a4, SIEOS_AT_SYMLINK_NOFOLLOW, &ok);
        if (!ok)
            return -EINVAL;
        return fsys_chmod(a2 ? dfd(a1) : (int)a1, (const char *)a2, a3, fl);
    case SIEOS_SYS_fchownat:
        fl = at_flags(a5, SIEOS_AT_SYMLINK_NOFOLLOW, &ok);
        if (!ok)
            return -EINVAL;
        return fsys_chown(a2 ? dfd(a1) : (int)a1, (const char *)a2, (int)a3, (int)a4, fl);
    case SIEOS_SYS_faccessat:
        fl = at_flags(a4, SIEOS_AT_EACCESS | SIEOS_AT_SYMLINK_NOFOLLOW, &ok);
        if (!ok)
            return -EINVAL;
        return fsys_access(dfd(a1), (const char *)a2, a3, fl);
    case SIEOS_SYS_mkdirat:   return fsys_mkdir(dfd(a1), (const char *)a2, a3);
    case SIEOS_SYS_mknodat: {
        uint64_t dev = a4;
        uint32_t ma = dev >> SIEOS_NBITSMAJOR, mi = dev & SIEOS_MAXMIN;
        if (ma > 0xFFF || mi > 0xFF)
            return -EINVAL;
        return fsys_mknod(dfd(a1), (const char *)a2, a3, MKDEV(ma, mi));
    }
    case SIEOS_SYS_unlinkat:
        fl = at_flags(a3, SIEOS_AT_REMOVEDIR, &ok);
        if (!ok)
            return -EINVAL;
        return fsys_unlink(dfd(a1), (const char *)a2, fl);
    case SIEOS_SYS_renameat:  return fsys_rename(dfd(a1), (const char *)a2, dfd(a3), (const char *)a4);
    case SIEOS_SYS_linkat:
        fl = at_flags(a5, SIEOS_AT_SYMLINK_FOLLOW, &ok);
        if (!ok)
            return -EINVAL;
        return fsys_link(dfd(a1), (const char *)a2, dfd(a3), (const char *)a4, fl);
    case SIEOS_SYS_symlinkat: return fsys_symlink((const char *)a1, dfd(a2), (const char *)a3);
    case SIEOS_SYS_readlinkat: return fsys_readlink(dfd(a1), (const char *)a2, (char *)a3, a4);
    case SIEOS_SYS_utimensat: {
        fl = at_flags(a4, SIEOS_AT_SYMLINK_NOFOLLOW, &ok);
        if (!ok)
            return -EINVAL;
        const struct sieos_timespec *uts = (const struct sieos_timespec *)a3;
        int64_t ts[4];
        if (uts) {
            if (!user_ok(uts, 2 * sizeof(*uts), false))
                return -EFAULT;
            ts[0] = uts[0].tv_sec;
            ts[1] = uts[0].tv_nsec == SIEOS_UTIME_NOW ? -1 : uts[0].tv_nsec == SIEOS_UTIME_OMIT ? -2 : uts[0].tv_nsec;
            ts[2] = uts[1].tv_sec;
            ts[3] = uts[1].tv_nsec == SIEOS_UTIME_NOW ? -1 : uts[1].tv_nsec == SIEOS_UTIME_OMIT ? -2 : uts[1].tv_nsec;
            if ((uts[0].tv_nsec < 0 && ts[1] >= 0) || (uts[1].tv_nsec < 0 && ts[3] >= 0))
                return -EINVAL;
        }
        return fsys_utimens(a2 ? dfd(a1) : (int)a1, (const char *)a2, uts ? ts : NULL, fl);
    }
    case SIEOS_SYS_ftruncate: return fsys_ftruncate(a1, (int64_t)a2);
    case SIEOS_SYS_fdsync:
        if (!fsys_file(a1))
            return -EBADF;
        vfs_sync();
        return 0;
    case SIEOS_SYS_chdir:     return fsys_chdir((const char *)a1);
    case SIEOS_SYS_fchdir:    return fsys_fchdir(a1);
    case SIEOS_SYS_chroot:    return fsys_chroot((const char *)a1);
    case SIEOS_SYS_getcwd:    return fsys_getcwd((char *)a1, a2);
    case SIEOS_SYS_statvfs:   return do_statvfs(AT_FDCWD_K, (const char *)a1, (struct sieos_statvfs *)a2);
    case SIEOS_SYS_fstatvfs:  return do_statvfs(a1, NULL, (struct sieos_statvfs *)a2);
    case SIEOS_SYS_sync:      vfs_sync(); return 0;
    case SIEOS_SYS_pipe2: {
        if (a2 & ~(uint64_t)(SIEOS_O_CLOEXEC | SIEOS_O_NONBLOCK))
            return -EINVAL;
        int *ufds = (int *)a1;
        if (!user_ok(ufds, 2 * sizeof(int), true))
            return -EFAULT;
        struct file *rf, *wf;
        long r = pipe_create(&rf, &wf);
        if (r < 0)
            return r;
        if (a2 & SIEOS_O_NONBLOCK) {
            rf->flags |= O_NONBLOCK_K;
            wf->flags |= O_NONBLOCK_K;
        }
        int fdfl = (a2 & SIEOS_O_CLOEXEC) ? FD_CLOEXEC : 0;
        int fd0 = fd_alloc(current, rf, 0, fdfl);
        int fd1 = fd0 >= 0 ? fd_alloc(current, wf, 0, fdfl) : -EMFILE;
        if (fd0 < 0 || fd1 < 0) {
            if (fd0 >= 0)
                fd_close(current, fd0);
            else
                file_close(rf);
            file_close(wf);
            return -EMFILE;
        }
        ufds[0] = fd0;
        ufds[1] = fd1;
        return 0;
    }
    }
    *handled = false;
    return -ENOSYS;
}
