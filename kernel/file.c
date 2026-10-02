/*
 * file.c - Open file objects shared between file descriptors, and the
 * processes' descriptor tables.
 *
 * Locking: a file's references are counted atomically; the last one closes
 * it.  The table of files has a spin lock for finding a free entry.  A
 * process's descriptors are under its p_fdlock; a system call uses a file
 * through a reference (getf/releasef, or fd_file: held until the call
 * returns), so another LWP's close cannot free it meanwhile.  A file's
 * offset is under its f_offlock (held across the read or write that moves
 * it).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "fs.h"
#include "blkdev.h"
#include "mm.h"
#include "tty.h"
#include "poll.h"
#include "net.h"
#include "random.h"
#include "proc.h"

#define NFILE 8192                       /* open files in the whole system */

static struct file ftable[NFILE];
static struct spinlock ftable_lock;

struct file *file_alloc(void)
{
    static uint64_t gen;
    struct file *f = NULL;
    spin_lock(&ftable_lock);
    for (int i = 0; i < NFILE; i++) {
        if (ftable[i].ref == 0) {
            f = &ftable[i];
            memset(f, 0, sizeof(*f));
            f->ref = 1;
            f->gen = ++gen;
            break;
        }
    }
    spin_unlock(&ftable_lock);
    return f;
}

struct file *file_dup(struct file *f)
{
    __atomic_add_fetch(&f->ref, 1, __ATOMIC_RELAXED);
    return f;
}

void file_close(struct file *f)
{
    /* The last reference: the entry is reserved (ref -1) while it is closed
     * (that may sleep: the last iput can do disk I/O), or file_alloc could
     * hand it out meanwhile and we would clear someone else's file. */
    int r = __atomic_load_n(&f->ref, __ATOMIC_RELAXED);
    for (;;) {
        if (r <= 0)
            panic("file_close: bad refcount");
        int n = r == 1 ? -1 : r - 1;
        if (__atomic_compare_exchange_n(&f->ref, &r, n, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            break;
    }
    if (r > 1)
        return;
    struct file c = *f;
    if (c.ip)
        iput(c.ip);
    if (c.pdir)
        iput(c.pdir);
    kfree(c.pname);
    if (c.type == FD_PIPE)
        pipe_close(c.pipe, c.flags & O_ACCMODE);
    else if (c.type == FD_TTY && c.tty)
        pty_slave_close(c.tty);
    else if (c.type == FD_PTM)
        pty_master_close(c.pty);
    else if (c.type == FD_EVENTS)
        input_close();
    else if (c.type == FD_SOCKET && c.sock)
        socket_close(c.sock);
    else if (c.type == FD_UNIX && c.usock)
        unix_close(c.usock);
    else if (c.type == FD_OPS && c.ops->close)
        c.ops->close(&c);
    memset(f, 0, offsetof(struct file, ref));
    memset((char *)f + offsetof(struct file, ref) + sizeof(f->ref), 0,
           sizeof(*f) - offsetof(struct file, ref) - sizeof(f->ref));
    __atomic_store_n(&f->ref, 0, __ATOMIC_RELEASE);   /* free (ref 0), type FD_NONE */
}

/* ---------------- descriptor tables ---------------- */

/* The caller's file on fd, referenced (releasef), or NULL. */
struct file *getf(int fd)
{
    struct proc *p = current;
    if (fd < 0 || fd >= NOFILE)
        return NULL;
    mutex_enter(&p->p_fdlock);
    struct file *f = p->ofile[fd];
    if (f)
        file_dup(f);
    mutex_exit(&p->p_fdlock);
    return f;
}

void releasef(struct file *f)
{
    file_close(f);
}

/* Does the caller's descriptor fd still name f? */
bool fd_still(int fd, struct file *f)
{
    struct proc *p = current;
    if (fd < 0 || fd >= NOFILE)
        return false;
    mutex_enter(&p->p_fdlock);
    bool r = p->ofile[fd] == f;
    mutex_exit(&p->p_fdlock);
    return r;
}

/* The caller's file on fd, referenced until the system call returns (fd_release_held; once per file). */
struct file *fd_file(int fd)
{
    struct lwp *l = curlwp;
    struct file *f = getf(fd);
    if (!f)
        return NULL;
    for (int i = 0; i < l->nfheld; i++)
        if (l->fheld[i] == f) {
            releasef(f);                         /* (held already: the call's reference serves) */
            return f;
        }
    if (l->nfheld == (int)ARRAY_SIZE(l->fheld)) {
        releasef(f);
        panic("fd_file: more than %d files held by one system call", (int)ARRAY_SIZE(l->fheld));
    }
    l->fheld[l->nfheld++] = f;
    return f;
}

void fd_release_held(void)
{
    struct lwp *l = curlwp;
    while (l->nfheld > 0)
        releasef(l->fheld[--l->nfheld]);
}

/* A descriptor for f (the table's reference: the caller's), from `from` up; -EMFILE. */
int fd_alloc(struct proc *p, struct file *f, int from, int fdflags)
{
    uint64_t lim = p->rlim_cur[5];                   /* SIEOS_RLIMIT_NOFILE */
    int max = lim < NOFILE ? (int)lim : NOFILE;
    int r = -EMFILE;
    mutex_enter(&p->p_fdlock);
    for (int fd = from < 0 ? 0 : from; fd < max; fd++) {
        if (!p->ofile[fd]) {
            p->ofile[fd] = f;
            p->fdflags[fd] = fdflags;
            r = fd;
            break;
        }
    }
    mutex_exit(&p->p_fdlock);
    return r;
}

/* Put f on descriptor fd (dup2: its reference is the caller's); the file it replaced, for the caller to close. */
struct file *fd_replace(struct proc *p, int fd, struct file *f, int fdflags)
{
    mutex_enter(&p->p_fdlock);
    struct file *old = p->ofile[fd];
    p->ofile[fd] = f;
    p->fdflags[fd] = fdflags;
    mutex_exit(&p->p_fdlock);
    return old;
}

/* Take fd's file out of the table (its reference is the caller's now), or NULL. */
static struct file *fd_detach(struct proc *p, int fd)
{
    if (fd < 0 || fd >= NOFILE)
        return NULL;
    mutex_enter(&p->p_fdlock);
    struct file *f = p->ofile[fd];
    p->ofile[fd] = NULL;
    p->fdflags[fd] = 0;
    mutex_exit(&p->p_fdlock);
    return f;
}

/* A file that left a descriptor of p: its record locks of p go, and the reference. */
static void fd_drop(struct proc *p, struct file *f)
{
    if (f->ip)
        flock_release(f->ip, p->pid);
    if (f->type == FD_SOCKET && f->sock)
        socket_fd_closed(f->sock);               /* (a thread sleeping in accept on it sees it gone) */
    file_close(f);
}

/* Close descriptor fd of process p (its record locks on the file go too); false if it was not open. */
bool fd_close(struct proc *p, int fd)
{
    struct file *f = fd_detach(p, fd);
    if (!f)
        return false;
    fd_drop(p, f);
    return true;
}

int fd_getflags(struct proc *p, int fd)
{
    mutex_enter(&p->p_fdlock);
    int r = fd >= 0 && fd < NOFILE && p->ofile[fd] ? p->fdflags[fd] : -EBADF;
    mutex_exit(&p->p_fdlock);
    return r;
}

int fd_setflags(struct proc *p, int fd, int flags)
{
    mutex_enter(&p->p_fdlock);
    int r = fd >= 0 && fd < NOFILE && p->ofile[fd] ? 0 : -EBADF;
    if (!r)
        p->fdflags[fd] = flags;
    mutex_exit(&p->p_fdlock);
    return r;
}

/* fork: the child's descriptors are the parent's. */
void fd_copy_table(struct proc *np, struct proc *cp)
{
    mutex_enter(&cp->p_fdlock);
    for (int i = 0; i < NOFILE; i++)
        if (cp->ofile[i]) {
            np->ofile[i] = file_dup(cp->ofile[i]);
            np->fdflags[i] = cp->fdflags[i];
        }
    mutex_exit(&cp->p_fdlock);
}

/* Close every descriptor (exit), or those marked close-on-exec (exec). */
static void fd_close_some(struct proc *p, bool exec_only)
{
    for (int i = 0; i < NOFILE; i++) {
        mutex_enter(&p->p_fdlock);
        struct file *f = p->ofile[i];
        if (f && (!exec_only || (p->fdflags[i] & FD_CLOEXEC))) {
            p->ofile[i] = NULL;
            p->fdflags[i] = 0;
        } else {
            f = NULL;
        }
        mutex_exit(&p->p_fdlock);
        if (f)
            fd_drop(p, f);
    }
}

void fd_close_all(struct proc *p)
{
    fd_close_some(p, false);
}

void fd_close_exec(struct proc *p)
{
    fd_close_some(p, true);
}

/* The path of an open file, for /proc/<pid>/fd/<n>.  Returns its length. */
int file_path(struct file *f, char *buf, size_t size)
{
    char tmp[64];
    const char *s = NULL;
    switch (f->type) {
    case FD_TTY: {
        int n = f->tty ? pty_index(f->tty) : -1;
        if (n >= 0) {
            snprintf(tmp, sizeof(tmp), "/dev/pts/%d", n);
            s = tmp;
        } else {
            s = "/dev/console";
        }
        break;
    }
    case FD_PTM:
        s = "/dev/ptmx";
        break;
    case FD_PIPE:
        snprintf(tmp, sizeof(tmp), "pipe:[%lx]", (uint64_t)f->pipe & 0xFFFFFF);
        s = tmp;
        break;
    case FD_OPS:
        snprintf(tmp, sizeof(tmp), "anon_inode:[%s]", f->ops->name);
        s = tmp;
        break;
    case FD_SOCKET:
        snprintf(tmp, sizeof(tmp), "socket:[%lx]", (uint64_t)f->sock & 0xFFFFFF);
        s = tmp;
        break;
    case FD_UNIX:
        snprintf(tmp, sizeof(tmp), "socket:[%lx]", (uint64_t)f->usock & 0xFFFFFF);
        s = tmp;
        break;
    }
    if (!s && f->pname) {
        if (f->pname[0] == '/' || !f->pdir) {
            s = f->pname;
        } else {
            int r = vfs_dir_path(f->pdir, buf, size);
            if (r < 0)
                return r;
            size_t dl = r - 1;
            if (dl > 1)
                strlcat(buf, "/", size);
            strlcat(buf, f->pname, size);
            return strlen(buf);
        }
    }
    if (!s && f->ip && S_ISDIR(inode_mode(f->ip))) {
        int r = vfs_dir_path(f->ip, buf, size);
        return r < 0 ? r : r - 1;
    }
    if (!s)
        return -ENOENT;
    strlcpy(buf, s, size);
    return strlen(buf);
}

/* RLIMIT_FSIZE: writes past the limit fail with EFBIG and SIGXFSZ. */
static long fsize_check(struct file *f, uint64_t off, size_t *n)
{
    uint64_t lim = current->rlim_cur[1];             /* SIEOS_RLIMIT_FSIZE */
    if (lim == (uint64_t)-3 || !S_ISREG(inode_mode(f->ip)) || !*n)
        return 0;
    if (off >= lim) {
        signal_send(current, SIGXFSZ);
        return -EFBIG;
    }
    if (off + *n > lim)
        *n = lim - off;
    return 0;
}

/* The offset of a seekable file moves under its f_offlock. */
static long offset_io(struct file *f, void *buf, size_t n, bool write)
{
    mutex_enter(&f->f_offlock);
    long r;
    if (f->type == FD_BLK) {
        r = blk_file_io(f->minor, f->off, buf, n, write);
    } else if (!write) {
        r = readi(f->ip, buf, f->off, n);
    } else {
        if (f->flags & O_APPEND)
            f->off = inode_size(f->ip);
        r = fsize_check(f, f->off, &n);
        if (r == 0)
            r = writei(f->ip, buf, f->off, n);
    }
    if (r > 0)
        f->off += r;
    mutex_exit(&f->f_offlock);
    return r;
}

long file_read(struct file *f, void *buf, size_t n)
{
    if ((f->flags & O_ACCMODE) == O_WRONLY)
        return -EBADF;
    if ((f->flags & O_NONBLOCK_K) && f->type != FD_INODE && n && !(file_poll(f, POLLIN | POLLHUP)))
        return -EAGAIN;
    switch (f->type) {
    case FD_TTY:
        return tty_read(f->tty, buf, n);
    case FD_PTM:
        return pty_master_read(f->pty, buf, n);
    case FD_EVENTS:
        return input_read(buf, n);
    case FD_SOCKET:
        return socket_read(f->sock, buf, n, f->flags & O_NONBLOCK_K);
    case FD_UNIX:
        return unix_read(f, buf, n);
    case FD_FB:
        return -EINVAL;
    case FD_PIPE:
        return pipe_read(f->pipe, buf, n);
    case FD_OPS:
        return f->ops->read ? f->ops->read(f, buf, n) : -EINVAL;
    case FD_NULL:
        return 0;
    case FD_ZERO:
        memset(buf, 0, n);
        return n;
    case FD_RANDOM:
        random_bytes(buf, n);
        return n;
    case FD_BLK:
        return offset_io(f, buf, n, false);
    case FD_INODE:
        if (S_ISDIR(inode_mode(f->ip)))
            return -EISDIR;
        return offset_io(f, buf, n, false);
    }
    return -EBADF;
}

long file_write(struct file *f, const void *buf, size_t n)
{
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    if ((f->flags & O_NONBLOCK_K) && f->type != FD_INODE && n && !(file_poll(f, POLLOUT | POLLHUP)))
        return -EAGAIN;
    switch (f->type) {
    case FD_TTY:
        return tty_write(f->tty, buf, n);
    case FD_PTM:
        return pty_master_write(f->pty, buf, n);
    case FD_EVENTS:
    case FD_FB:
        return -EINVAL;
    case FD_SOCKET:
        return socket_write(f->sock, buf, n, f->flags & O_NONBLOCK_K);
    case FD_UNIX:
        return unix_write(f, buf, n);
    case FD_PIPE:
        return pipe_write(f->pipe, buf, n, f->flags & O_NONBLOCK_K);
    case FD_OPS:
        return f->ops->write ? f->ops->write(f, buf, n) : -EINVAL;
    case FD_NULL:
    case FD_ZERO:
        return n;
    case FD_BLK:
        return offset_io(f, (void *)buf, n, true);
    case FD_RANDOM:                       /* writes stir the pool */
        for (size_t i = 0; i + 8 <= n; i += 8)
            random_add_entropy(*(const uint64_t *)((const uint8_t *)buf + i));
        return n;
    case FD_INODE:
        return offset_io(f, (void *)buf, n, true);
    }
    return -EBADF;
}

long file_pread(struct file *f, void *buf, size_t n, uint64_t off)
{
    if ((f->flags & O_ACCMODE) == O_WRONLY)
        return -EBADF;
    if (f->type == FD_BLK)
        return blk_file_io(f->minor, off, buf, n, false);
    if (f->type != FD_INODE || !f->ip)
        return -ESPIPE;
    if (S_ISDIR(inode_mode(f->ip)))
        return -EISDIR;
    if (S_ISCHR(inode_mode(f->ip)))
        return -ESPIPE;
    return readi(f->ip, buf, off, n);
}

long file_pwrite(struct file *f, const void *buf, size_t n, uint64_t off)
{
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    if (f->type == FD_BLK)
        return blk_file_io(f->minor, off, (void *)buf, n, true);
    if (f->type != FD_INODE || !f->ip || S_ISCHR(inode_mode(f->ip)))
        return -ESPIPE;
    long e = fsize_check(f, off, &n);
    if (e < 0)
        return e;
    return writei(f->ip, buf, off, n);   /* O_APPEND does not move pwrite (as on Solaris) */
}

/* Is an open file (or the directory it was opened from) on fs? */
bool file_table_uses(struct fs *fs)
{
    for (int i = 0; i < NFILE; i++) {
        struct file *f = &ftable[i];
        if (f->ref && ((f->ip && f->ip->fs == fs) || (f->pdir && f->pdir->fs == fs)))
            return true;
    }
    return false;
}
