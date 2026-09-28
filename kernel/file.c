/*
 * file.c - Open file objects shared between file descriptors.
 */
#include "fs.h"
#include "mm.h"
#include "tty.h"
#include "poll.h"
#include "net.h"
#include "random.h"
#include "proc.h"

#define NFILE 512

static struct file ftable[NFILE];

struct file *file_alloc(void)
{
    for (int i = 0; i < NFILE; i++) {
        if (ftable[i].ref == 0) {
            memset(&ftable[i], 0, sizeof(ftable[i]));
            ftable[i].ref = 1;
            return &ftable[i];
        }
    }
    return NULL;
}

struct file *file_dup(struct file *f)
{
    f->ref++;
    return f;
}

void file_close(struct file *f)
{
    if (f->ref <= 0)
        panic("file_close: bad refcount");
    if (--f->ref > 0)
        return;
    if (f->ip)
        iput(f->ip);
    if (f->pdir)
        iput(f->pdir);
    kfree(f->pname);
    f->pdir = NULL;
    f->pname = NULL;
    if (f->type == FD_PIPE)
        pipe_close(f->pipe, f->flags & O_ACCMODE);
    else if (f->type == FD_TTY && f->tty)
        pty_slave_close(f->tty);
    else if (f->type == FD_PTM)
        pty_master_close(f->pty);
    else if (f->type == FD_EVENTS)
        input_close();
    else if (f->type == FD_SOCKET && f->sock)
        socket_close(f->sock);
    else if (f->type == FD_UNIX && f->usock)
        unix_close(f->usock);
    f->type = FD_NONE;
    f->ip = NULL;
    f->pipe = NULL;
    f->pty = NULL;
    f->tty = NULL;
    f->sock = NULL;
    f->usock = NULL;
}

/* Close descriptor fd of process p (its record locks on the file go too). */
void fd_close(struct proc *p, int fd)
{
    struct file *f = p->ofile[fd];
    if (!f)
        return;
    p->ofile[fd] = NULL;
    p->fdflags[fd] = 0;
    if (f->ip)
        flock_release(f->ip, p->pid);
    file_close(f);
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
        return socket_read(f->sock, buf, n);
    case FD_UNIX:
        return unix_read(f, buf, n);
    case FD_FB:
        return -EINVAL;
    case FD_PIPE:
        return pipe_read(f->pipe, buf, n);
    case FD_NULL:
        return 0;
    case FD_ZERO:
        memset(buf, 0, n);
        return n;
    case FD_RANDOM:
        random_bytes(buf, n);
        return n;
    case FD_INODE: {
        if (S_ISDIR(inode_mode(f->ip)))
            return -EISDIR;
        long r = readi(f->ip, buf, f->off, n);
        if (r > 0)
            f->off += r;
        return r;
    }
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
        return socket_write(f->sock, buf, n);
    case FD_UNIX:
        return unix_write(f, buf, n);
    case FD_PIPE:
        return pipe_write(f->pipe, buf, n);
    case FD_NULL:
    case FD_ZERO:
        return n;
    case FD_RANDOM:                       /* writes stir the pool */
        for (size_t i = 0; i + 8 <= n; i += 8)
            random_add_entropy(*(const uint64_t *)((const uint8_t *)buf + i));
        return n;
    case FD_INODE: {
        if (f->flags & O_APPEND)
            f->off = inode_size(f->ip);
        long e = fsize_check(f, f->off, &n);
        if (e < 0)
            return e;
        long r = writei(f->ip, buf, f->off, n);
        if (r > 0)
            f->off += r;
        return r;
    }
    }
    return -EBADF;
}

long file_pread(struct file *f, void *buf, size_t n, uint64_t off)
{
    if ((f->flags & O_ACCMODE) == O_WRONLY)
        return -EBADF;
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
