/*
 * unix.c - AF_UNIX sockets (ABI v2): stream and datagram sockets named by
 * socket inodes that bind creates in the file system, socketpair, and
 * descriptor passing with SCM_RIGHTS.  There is no abstract namespace.
 *
 * A stream socket receives into its own ring buffer; the peer writes into
 * it.  Descriptors sent with SCM_RIGHTS are attached to the byte position
 * of the message that carried them, and a read stops at such a boundary so
 * the descriptors arrive with the first byte of their message.  A datagram
 * socket has a queue of messages.  Writers wait on the receiving socket.
 *
 * Locking: unix_lock covers every AF_UNIX socket (sleepers wait on their
 * socket with it as the interlock).  Descriptors in messages that go away
 * are closed after it is released (unix_unlock), as closing one may close
 * a socket; descriptors are installed without it.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "proc.h"
#include "fs.h"
#include "mm.h"
#include "poll.h"
#include "abi2.h"
#include "sieos/syscall.h"
#include "sieos/socket.h"

#define UNIX_BUF       32768                 /* stream receive buffer */
#define UNIX_DGRAM_MAX 65536                 /* largest datagram */
#define UNIX_QUEUE     (4 * UNIX_DGRAM_MAX)  /* datagram bytes a socket may queue */
#define UNIX_MAXFDS    253                   /* descriptors per message */
#define UNIX_BACKLOG   128
#define ADDR_HDR       2                     /* offsetof(sockaddr_un, sun_path) */

struct urights {
    struct urights *next;
    uint64_t pos;                            /* stream: first byte of the message */
    int n;
    struct file *f[];
};

struct umsg {
    struct umsg *next;
    size_t len;
    struct urights *rights;
    struct sieos_sockaddr_un from;
    unsigned int fromlen;
    uint8_t data[];
};

enum { US_NEW, US_LISTEN, US_CONNECTED };

struct usock {
    int type;                                /* SIEOS_SOCK_STREAM or SIEOS_SOCK_DGRAM */
    int state;
    int refs;                                /* the file, plus callers sleeping on it */
    bool closed;
    struct usock *peer;                      /* connected: the other end, NULL once it closed */
    bool peer_gone;                          /* ... and it did */
    bool shut_rd, shut_wr, peer_shut_wr;
    /* stream: what the peer wrote to us */
    uint8_t *buf;
    uint64_t rn, wn;
    struct urights *rights, **rtail;
    /* datagram: messages sent to us */
    struct umsg *q, **qtail;
    size_t qbytes;
    /* datagram: the default destination set by connect */
    bool has_dest;
    struct sieos_sockaddr_un dest;
    /* listening */
    struct usock *backlog[UNIX_BACKLOG];
    int nback, maxback;
    /* name */
    bool bound;
    struct fs *fs;
    uint32_t ino;
    struct sieos_sockaddr_un addr;
    unsigned int addrlen;
    struct usock *next;                      /* bound sockets */
};

static struct usock *bound_list;
static kmutex_t unix_lock;

/* Files to close once unix_lock is released. */
static struct dclose {
    struct dclose *next;
    struct urights *r;
    struct file *f;
} *dclose_list;
static kmutex_t dclose_lock = MUTEX_SPIN_INITIALIZER;

static void unix_sleep(const void *chan)
{
    sleepq_block(chan, &unix_lock, true);
    mutex_enter(&unix_lock);
}

static void unix_unlock(void)
{
    mutex_exit(&unix_lock);
    mutex_enter(&dclose_lock);
    struct dclose *d = dclose_list;
    dclose_list = NULL;
    mutex_exit(&dclose_lock);
    while (d) {
        struct dclose *next = d->next;
        for (struct urights *r = d->r, *rn; r; r = rn) {
            rn = r->next;
            for (int i = 0; i < r->n; i++)
                if (r->f[i])
                    file_close(r->f[i]);
            kfree(r);
        }
        if (d->f)
            file_close(d->f);
        kfree(d);
        d = next;
    }
}

static void defer_close(struct urights *r, struct file *f)
{
    struct dclose *d = kmalloc(sizeof(*d));
    if (!d)
        panic("unix: no memory to close a descriptor");
    d->r = r;
    d->f = f;
    mutex_enter(&dclose_lock);
    d->next = dclose_list;
    dclose_list = d;
    mutex_exit(&dclose_lock);
}

/* ------------------------------------------------------------------ */
/* Sockets and descriptors                                             */
/* ------------------------------------------------------------------ */

static struct usock *usock_new(int type)
{
    struct usock *u = kzalloc(sizeof(*u));
    if (!u)
        return NULL;
    if (type == SIEOS_SOCK_STREAM && !(u->buf = kmalloc(UNIX_BUF))) {
        kfree(u);
        return NULL;
    }
    u->type = type;
    u->refs = 1;
    u->rtail = &u->rights;
    u->qtail = &u->q;
    u->addr.sun_family = SIEOS_AF_UNIX;
    u->addrlen = ADDR_HDR;
    return u;
}

static void usock_put(struct usock *u)
{
    if (--u->refs == 0) {
        kfree(u->buf);
        kfree(u);
    }
}

/* unix_lock held: the descriptors are closed after it is released. */
static void rights_free(struct urights *r)
{
    if (r)
        defer_close(r, NULL);
}

static void wake(struct usock *u)
{
    sleepq_wakeup(u, -1);
    poll_wakeup();
}

static void close_locked(struct usock *u);

/* The socket's file is gone. */
void unix_close(struct usock *u)
{
    mutex_enter(&unix_lock);
    close_locked(u);
    unix_unlock();
}

/* unix_lock held: the socket's file is gone (or a pending connection is dropped). */
static void close_locked(struct usock *u)
{
    u->closed = true;
    if (u->bound)
        for (struct usock **pp = &bound_list; *pp; pp = &(*pp)->next)
            if (*pp == u) {
                *pp = u->next;
                break;
            }
    u->bound = false;
    if (u->peer) {
        u->peer->peer = NULL;
        u->peer->peer_gone = true;
        wake(u->peer);
        u->peer = NULL;
    }
    for (int i = 0; i < u->nback; i++)
        close_locked(u->backlog[i]);
    u->nback = 0;
    u->state = US_NEW;
    rights_free(u->rights);
    u->rights = NULL;
    while (u->q) {
        struct umsg *m = u->q;
        u->q = m->next;
        rights_free(m->rights);
        kfree(m);
    }
    wake(u);
    usock_put(u);
}

static struct file *unix_file(struct usock *u)
{
    struct file *f = file_alloc();
    if (!f)
        return NULL;
    f->type = FD_UNIX;
    f->flags = O_RDWR;
    f->usock = u;
    return f;
}

/* (without unix_lock) */
static long install(struct usock *u)
{
    struct file *f = unix_file(u);
    if (!f) {
        unix_close(u);
        return -ENFILE;
    }
    int fd = fsys_fdalloc(f, 0);
    if (fd < 0)
        file_close(f);
    return fd < 0 ? -EMFILE : fd;
}

long unix_socket(int type)
{
    if (type != SIEOS_SOCK_STREAM && type != SIEOS_SOCK_DGRAM)
        return -EPROTONOSUPPORT;
    struct usock *u = usock_new(type);
    return u ? install(u) : -ENOMEM;
}

long unix_socketpair(int type, int *usv)
{
    if (type != SIEOS_SOCK_STREAM && type != SIEOS_SOCK_DGRAM)
        return -EPROTONOSUPPORT;
    if (!user_ok(usv, 2 * sizeof(int), true))
        return -EFAULT;
    struct usock *a = usock_new(type), *b = a ? usock_new(type) : NULL;
    if (!b) {
        if (a)
            usock_put(a);
        return -ENOMEM;
    }
    a->peer = b;
    b->peer = a;
    a->state = b->state = US_CONNECTED;
    long fa = install(a);
    if (fa < 0) {
        unix_close(b);
        return fa;
    }
    long fb = install(b);
    if (fb < 0) {
        fd_close(current, fa);
        return fb;
    }
    usv[0] = fa;
    usv[1] = fb;
    return 0;
}

static struct usock *ufd(long fd, struct file **fp)
{
    struct file *f = fd_file((int)fd);
    if (!f || f->type != FD_UNIX)
        return NULL;
    if (fp)
        *fp = f;
    return f->usock;
}

bool unix_fd(long fd)
{
    return ufd(fd, NULL) != NULL;
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

/* Copy a user sockaddr_un and return the path length (without the NUL). */
static long get_path(const void *uaddr, long len, struct sieos_sockaddr_un *a)
{
    if (len < ADDR_HDR + 1 || len > (long)sizeof(*a))
        return len > (long)sizeof(*a) ? -EINVAL : -EINVAL;
    if (!user_ok(uaddr, len, false))
        return -EFAULT;
    memset(a, 0, sizeof(*a));
    memcpy(a, uaddr, len);
    if (a->sun_family != SIEOS_AF_UNIX)
        return -EAFNOSUPPORT;
    size_t n = 0;
    while (n < (size_t)len - ADDR_HDR && a->sun_path[n])
        n++;
    if (n == 0)
        return -EINVAL;                      /* no abstract or unnamed addresses */
    if (n >= SIEOS_UNIX_PATH_MAX)
        return -ENAMETOOLONG;
    a->sun_path[n] = 0;
    return n;
}

static int put_name(void *uaddr, unsigned int *ulen, const struct sieos_sockaddr_un *a, unsigned int alen)
{
    if (!uaddr)
        return 0;
    if (!ulen || !user_ok(ulen, sizeof(*ulen), true))
        return -EFAULT;
    unsigned int n = *ulen < alen ? *ulen : alen;
    if (!user_ok(uaddr, n, true))
        return -EFAULT;
    memcpy(uaddr, a, n);
    *ulen = alen;
    return 0;
}

static unsigned int name_len(const struct sieos_sockaddr_un *a)
{
    return ADDR_HDR + strlen(a->sun_path) + 1;
}

/* The bound socket a path names. */
static long lookup(const struct sieos_sockaddr_un *a, struct usock **out)
{
    int err;
    struct inode *ip = namei(a->sun_path, &err);
    if (!ip)
        return err;
    long r = 0;
    if ((inode_mode(ip) & S_IFMT) != S_IFSOCK)
        r = -ECONNREFUSED;
    else if ((r = inode_permission(ip, W_OK)) == 0) {
        struct usock *u;
        for (u = bound_list; u; u = u->next)
            if (u->fs == ip->fs && u->ino == ip->ino)
                break;
        *out = u;
        if (!u)
            r = -ECONNREFUSED;
    }
    iput(ip);
    return r;
}

static long do_bind(struct usock *u, const void *uaddr, long len)
{
    struct sieos_sockaddr_un a;
    long n = get_path(uaddr, len, &a);
    if (n < 0)
        return n;
    if (u->bound || u->state == US_LISTEN)
        return -EINVAL;
    char name[256];
    int err;
    struct inode *dir = nameiparent(a.sun_path, name, &err), *ip = NULL;
    if (!dir)
        return err;
    int r = inode_permission(dir, W_OK | X_OK);
    if (r == 0)
        r = vfs_create(dir, name, S_IFSOCK | (0777 & ~current->umask), 0, current->euid, current->egid, &ip);
    iput(dir);
    if (r == -EEXIST)
        return -EADDRINUSE;
    if (r < 0)
        return r;
    u->fs = ip->fs;
    u->ino = ip->ino;
    iput(ip);
    u->addr = a;
    u->addrlen = name_len(&a);
    u->bound = true;
    u->next = bound_list;
    bound_list = u;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Connections                                                         */
/* ------------------------------------------------------------------ */

static long do_listen(struct usock *u, long backlog)
{
    if (u->type != SIEOS_SOCK_STREAM)
        return -EOPNOTSUPP;
    if (u->state == US_CONNECTED || !u->bound)
        return -EINVAL;
    u->state = US_LISTEN;
    u->maxback = backlog < 1 ? 1 : backlog > UNIX_BACKLOG ? UNIX_BACKLOG : backlog;
    wake(u);
    return 0;
}

static long do_connect(struct usock *u, struct file *f, const void *uaddr, long len)
{
    struct sieos_sockaddr_un a;
    long r = get_path(uaddr, len, &a);
    if (r < 0)
        return r;
    struct usock *l;
    if ((r = lookup(&a, &l)) < 0)
        return r;
    if (l->type != u->type)
        return -EPROTOTYPE_K;
    if (u->type == SIEOS_SOCK_DGRAM) {           /* a default destination, checked at each send */
        u->dest = a;
        u->has_dest = true;
        return 0;
    }
    if (u->state == US_CONNECTED)
        return -EISCONN;
    if (u->state == US_LISTEN)
        return -EINVAL;
    if (l->state != US_LISTEN)
        return -ECONNREFUSED;
    l->refs++;
    while (!l->closed && l->state == US_LISTEN && l->nback >= l->maxback) {
        if (f->flags & O_NONBLOCK_K)
            r = -EAGAIN;
        else if (signal_pending(current))
            r = -ERESTART;
        if (r)
            break;
        unix_sleep(l);
    }
    if (!r && (l->closed || l->state != US_LISTEN))
        r = -ECONNREFUSED;
    struct usock *n = r ? NULL : usock_new(SIEOS_SOCK_STREAM);
    if (!r && !n)
        r = -ENOMEM;
    if (r) {
        usock_put(l);
        return r;
    }
    n->state = US_CONNECTED;
    n->addr = l->addr;                           /* the accepted socket has the listener's name */
    n->addrlen = l->addrlen;
    n->peer = u;
    u->peer = n;
    u->state = US_CONNECTED;
    l->backlog[l->nback++] = n;
    wake(l);
    usock_put(l);
    return 0;
}

static long do_accept(struct usock *u, struct file *f, void *uaddr, unsigned int *ulen, long flags)
{
    if (u->type != SIEOS_SOCK_STREAM)
        return -EOPNOTSUPP;
    if (u->state != US_LISTEN)
        return -EINVAL;
    while (u->nback == 0) {
        if (f->flags & O_NONBLOCK_K)
            return -EAGAIN;
        if (signal_pending(current))
            return -ERESTART;
        unix_sleep(u);
        if (u->state != US_LISTEN)
            return -EINVAL;
    }
    struct usock *n = u->backlog[0];
    memmove(u->backlog, u->backlog + 1, --u->nback * sizeof(*u->backlog));
    wake(u);                                     /* room in the backlog */
    struct sieos_sockaddr_un none = { .sun_family = SIEOS_AF_UNIX };
    struct usock *p = n->peer;
    int r = put_name(uaddr, ulen, p ? &p->addr : &none, p ? p->addrlen : ADDR_HDR);
    if (r < 0) {
        close_locked(n);
        return r;
    }
    unix_unlock();                               /* (installing may close: without the lock) */
    long fd = install(n);
    mutex_enter(&unix_lock);
    struct file *nf = fd >= 0 ? fd_file((int)fd) : NULL;
    if (nf) {
        if (flags & (SIEOS_SOCK_NONBLOCK | SIEOS_SOCK_NDELAY))
            nf->flags |= O_NONBLOCK_K;
        if (flags & SIEOS_SOCK_CLOEXEC)
            fd_setflags(current, (int)fd, FD_CLOEXEC);
    }
    return fd;
}

static long do_shutdown(struct usock *u, long how)
{
    if (how < SIEOS_SHUT_RD || how > SIEOS_SHUT_RDWR)
        return -EINVAL;
    if (u->state != US_CONNECTED)
        return -ENOTCONN;
    if (how != SIEOS_SHUT_WR)
        u->shut_rd = true;
    if (how != SIEOS_SHUT_RD) {
        u->shut_wr = true;
        if (u->peer) {
            u->peer->peer_shut_wr = true;
            wake(u->peer);
        }
    }
    wake(u);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Data                                                                */
/* ------------------------------------------------------------------ */

/* A user I/O vector being walked. */
struct uio {
    const struct sieos_iovec *iov;
    int cnt, i;
    size_t off, resid;
};

static int uio_init(struct uio *u, const struct sieos_iovec *iov, int cnt, bool write)
{
    if (cnt < 0 || cnt > 1024 || (cnt > 1 && !user_ok(iov, cnt * sizeof(*iov), false)))
        return -EINVAL;
    u->iov = iov;
    u->cnt = cnt;
    u->i = 0;
    u->off = 0;
    u->resid = 0;
    for (int i = 0; i < cnt; i++) {
        if (iov[i].iov_len && !user_ok(iov[i].iov_base, iov[i].iov_len, write))
            return -EFAULT;
        u->resid += iov[i].iov_len;
    }
    return 0;
}

/* Move up to n bytes between kbuf and the vector. */
static size_t uio_move(struct uio *u, void *kbuf, size_t n, bool to_user)
{
    size_t done = 0;
    while (done < n && u->i < u->cnt) {
        const struct sieos_iovec *v = &u->iov[u->i];
        size_t k = v->iov_len - u->off;
        if (k > n - done)
            k = n - done;
        uint8_t *p = (uint8_t *)v->iov_base + u->off;
        if (to_user)
            memcpy(p, (uint8_t *)kbuf + done, k);
        else
            memcpy((uint8_t *)kbuf + done, p, k);
        done += k;
        u->off += k;
        if (u->off == v->iov_len) {
            u->i++;
            u->off = 0;
        }
    }
    u->resid -= done;
    return done;
}

/* SCM_RIGHTS in a user control buffer -> an urights holding the files. */
static long get_rights(const void *ctl, unsigned int clen, struct urights **out)
{
    *out = NULL;
    if (!ctl || clen == 0)
        return 0;
    if (!user_ok(ctl, clen, false))
        return -EFAULT;
    const uint8_t *p = ctl, *end = p + clen;
    while (end - p >= (long)sizeof(struct sieos_cmsghdr)) {
        const struct sieos_cmsghdr *c = (const void *)p;
        if (c->cmsg_len < sizeof(*c) || c->cmsg_len > (size_t)(end - p))
            break;
        if (c->cmsg_level == SIEOS_SOL_SOCKET && c->cmsg_type == SIEOS_SCM_RIGHTS) {
            int n = (c->cmsg_len - sizeof(*c)) / sizeof(int);
            const int *fds = (const int *)(c + 1);
            int had = *out ? (*out)->n : 0;          /* (several SCM_RIGHTS messages: one set) */
            if (had + n > UNIX_MAXFDS) {
                rights_free(*out);
                *out = NULL;
                return -EINVAL;
            }
            if (n == 0)
                goto next;
            struct urights *r = kzalloc(sizeof(*r) + (had + n) * sizeof(struct file *));
            if (!r) {
                rights_free(*out);
                *out = NULL;
                return -ENOMEM;
            }
            if (*out) {
                memcpy(r->f, (*out)->f, had * sizeof(struct file *));
                r->n = had;
                kfree(*out);
                *out = NULL;
            }
            for (int i = 0; i < n; i++) {
                struct file *pf = getf(fds[i]);
                if (!pf) {
                    rights_free(r);
                    *out = NULL;
                    return -EBADF;
                }
                r->f[r->n++] = pf;               /* (getf's reference: the message's now) */
            }
            *out = r;
        }
    next:
        p += (c->cmsg_len + 7) & ~7u;
    }
    return 0;
}

/* Install received descriptors into a user control buffer. */
static void put_rights(struct sieos_msghdr *um, struct urights *r, int *mflags)
{
    unsigned int space = um->msg_control ? um->msg_controllen : 0, used = 0;
    if (r) {
        int fit = space >= sizeof(struct sieos_cmsghdr) ? (space - sizeof(struct sieos_cmsghdr)) / sizeof(int) : 0;
        if (fit > r->n)
            fit = r->n;
        if (fit < r->n)
            *mflags |= SIEOS_MSG_CTRUNC;
        if (fit > 0 && user_ok(um->msg_control, space, true)) {
            struct sieos_cmsghdr *c = um->msg_control;
            int *fds = (int *)(c + 1), k = 0;
            for (int i = 0; i < fit; i++) {
                int fd = fsys_fdalloc(r->f[i], 0);
                if (fd < 0) {
                    *mflags |= SIEOS_MSG_CTRUNC;
                    break;
                }
                r->f[i] = NULL;                  /* the descriptor owns the reference now */
                fds[k++] = fd;
            }
            if (k) {
                c->cmsg_len = sizeof(*c) + k * sizeof(int);
                c->__pad = 0;
                c->cmsg_level = SIEOS_SOL_SOCKET;
                c->cmsg_type = SIEOS_SCM_RIGHTS;
                used = (c->cmsg_len + 7) & ~7u;
                if (used > space)
                    used = space;
            }
        }
        r->next = NULL;
        rights_free(r);                          /* what did not fit is closed */
    }
    if (um->msg_control)
        um->msg_controllen = used;
}

static bool nonblocking(struct file *f, long flags)
{
    return (f->flags & O_NONBLOCK_K) || (flags & SIEOS_MSG_DONTWAIT);
}

static long broken_pipe(long flags)
{
    if (!(flags & SIEOS_MSG_NOSIGNAL))
        signal_send(current, SIGPIPE);
    return -EPIPE;
}

static long stream_send(struct usock *u, struct file *f, struct uio *io, struct urights *rights, long flags)
{
    size_t done = 0, total = io->resid;
    long r = 0;
    if (total == 0) {                            /* nothing to carry descriptors */
        rights_free(rights);
        return u->state == US_CONNECTED ? 0 : -ENOTCONN;
    }
    while (done < total) {
        struct usock *p = u->peer;
        if (u->shut_wr || u->peer_gone) {
            r = broken_pipe(flags);
            break;
        }
        if (u->state != US_CONNECTED || !p) {
            r = -ENOTCONN;
            break;
        }
        if (p->shut_rd) {
            r = broken_pipe(flags);
            break;
        }
        size_t space = UNIX_BUF - (p->wn - p->rn);
        if (space == 0) {
            if (nonblocking(f, flags))
                r = -EAGAIN;
            else if (signal_pending(current))
                r = -ERESTART;
            if (r)
                break;
            unix_sleep(p);
            continue;
        }
        if (rights) {                            /* the descriptors go with this message's first byte */
            rights->pos = p->wn;
            *p->rtail = rights;
            p->rtail = &rights->next;
            rights = NULL;
        }
        size_t k = total - done < space ? total - done : space;
        for (size_t moved = 0; moved < k;) {     /* at most two pieces of the ring */
            size_t at = p->wn % UNIX_BUF, piece = UNIX_BUF - at;
            if (piece > k - moved)
                piece = k - moved;
            uio_move(io, p->buf + at, piece, false);
            p->wn += piece;
            moved += piece;
        }
        done += k;
        wake(p);
    }
    rights_free(rights);
    if (done)
        return done;
    return r;
}

static long stream_recv(struct usock *u, struct file *f, struct uio *io, struct sieos_msghdr *um, long flags,
                        int *mflags)
{
    long r = 0;
    while (u->rn == u->wn) {
        if (u->peer_gone || u->peer_shut_wr || u->shut_rd)
            return 0;
        if (u->state != US_CONNECTED)
            return -ENOTCONN;
        if (nonblocking(f, flags))
            return -EAGAIN;
        if (signal_pending(current))
            return -ERESTART;
        unix_sleep(u);
    }
    bool peek = flags & SIEOS_MSG_PEEK;
    uint64_t limit = u->wn;
    struct urights *take = NULL;
    if (u->rights && u->rights->pos == u->rn) {
        if (!peek) {
            take = u->rights;
            u->rights = take->next;
            if (!u->rights)
                u->rtail = &u->rights;
        }
        if ((peek ? u->rights->next : u->rights))
            limit = (peek ? u->rights->next : u->rights)->pos;
    } else if (u->rights) {
        limit = u->rights->pos;                  /* stop before the next descriptors */
    }
    uint64_t at = u->rn;
    size_t want = io->resid, got = 0;
    while (got < want && at < limit) {
        size_t off = at % UNIX_BUF, piece = UNIX_BUF - off;
        if (piece > limit - at)
            piece = limit - at;
        if (piece > want - got)
            piece = want - got;
        uio_move(io, u->buf + off, piece, true);
        at += piece;
        got += piece;
    }
    if (!peek) {
        u->rn = at;
        wake(u);                                 /* room for the writer */
    }
    if (um)
        put_rights(um, take, mflags);
    else
        rights_free(take);
    r = got;
    return r;
}

static long dgram_send(struct usock *u, struct file *f, struct uio *io, const void *uaddr, long alen,
                       struct urights *rights, long flags)
{
    long r;
    struct usock *t = NULL;
    if (u->shut_wr) {
        rights_free(rights);
        return broken_pipe(flags);
    }
    if (uaddr) {
        struct sieos_sockaddr_un a;
        if ((r = get_path(uaddr, alen, &a)) < 0 || (r = lookup(&a, &t)) < 0) {
            rights_free(rights);
            return r;
        }
    } else if (u->state == US_CONNECTED) {
        if (!(t = u->peer)) {
            rights_free(rights);
            return -ECONNREFUSED;
        }
    } else if (u->has_dest) {
        if ((r = lookup(&u->dest, &t)) < 0) {
            rights_free(rights);
            return r;
        }
    } else {
        rights_free(rights);
        return -EDESTADDRREQ;
    }
    if (t->type != SIEOS_SOCK_DGRAM) {
        rights_free(rights);
        return -EPROTOTYPE_K;
    }
    size_t len = io->resid;
    if (len > UNIX_DGRAM_MAX) {
        rights_free(rights);
        return -EMSGSIZE;
    }
    struct umsg *m = kmalloc(sizeof(*m) + len);
    if (!m) {
        rights_free(rights);
        return -ENOBUFS;
    }
    m->next = NULL;
    m->len = len;
    m->rights = rights;
    m->from = u->addr;
    m->fromlen = u->addrlen;
    uio_move(io, m->data, len, false);
    t->refs++;
    r = 0;
    while (!t->closed && t->qbytes + len > UNIX_QUEUE && t->q) {
        if (nonblocking(f, flags))
            r = -EAGAIN;
        else if (signal_pending(current))
            r = -ERESTART;
        if (r)
            break;
        unix_sleep(t);
    }
    if (!r && t->closed)
        r = -ECONNREFUSED;
    if (r) {
        rights_free(m->rights);
        kfree(m);
    } else {
        *t->qtail = m;
        t->qtail = &m->next;
        t->qbytes += len;
        wake(t);
        r = len;
    }
    usock_put(t);
    return r;
}

static long dgram_recv(struct usock *u, struct file *f, struct uio *io, void *uaddr, unsigned int *ulen,
                       struct sieos_msghdr *um, long flags, int *mflags)
{
    while (!u->q) {
        if (u->shut_rd || u->peer_gone)
            return 0;
        if (nonblocking(f, flags))
            return -EAGAIN;
        if (signal_pending(current))
            return -ERESTART;
        unix_sleep(u);
    }
    struct umsg *m = u->q;
    size_t n = m->len < io->resid ? m->len : io->resid;
    uio_move(io, m->data, n, true);
    if (n < m->len)
        *mflags |= SIEOS_MSG_TRUNC;
    int r = put_name(uaddr, ulen, &m->from, m->fromlen);
    if (r < 0)
        return r;
    if (flags & SIEOS_MSG_PEEK)
        return n;
    u->q = m->next;
    if (!u->q)
        u->qtail = &u->q;
    u->qbytes -= m->len;
    if (um)
        put_rights(um, m->rights, mflags);
    else
        rights_free(m->rights);
    kfree(m);
    wake(u);
    return n;
}

/* sendto/sendmsg and recvfrom/recvmsg over a vector. */
static long do_send(struct usock *u, struct file *f, const struct sieos_iovec *iov, int cnt, const void *uaddr,
                    long alen, const void *ctl, unsigned int clen, long flags)
{
    struct uio io;
    struct urights *rights;
    long r;
    if (flags & ~(long)(SIEOS_MSG_DONTWAIT | SIEOS_MSG_NOSIGNAL | SIEOS_MSG_EOR | SIEOS_MSG_WAITALL))
        return -EOPNOTSUPP;
    if ((r = uio_init(&io, iov, cnt, false)) < 0)
        return r;
    if ((r = get_rights(ctl, clen, &rights)) < 0)
        return r;
    if (u->type == SIEOS_SOCK_STREAM)
        return stream_send(u, f, &io, rights, flags);
    return dgram_send(u, f, &io, uaddr, alen, rights, flags);
}

static long do_recv(struct usock *u, struct file *f, const struct sieos_iovec *iov, int cnt, void *uaddr,
                    unsigned int *ulen, struct sieos_msghdr *um, long flags)
{
    struct uio io;
    long r;
    int mflags = 0;
    if (flags & ~(long)(SIEOS_MSG_DONTWAIT | SIEOS_MSG_PEEK | SIEOS_MSG_WAITALL))
        return -EOPNOTSUPP;
    if ((r = uio_init(&io, iov, cnt, true)) < 0)
        return r;
    if (u->type == SIEOS_SOCK_STREAM) {
        size_t want = io.resid;
        long got = 0;
        do {
            r = stream_recv(u, f, &io, um, flags, &mflags);
            if (r > 0)
                got += r;
        } while (r > 0 && (flags & SIEOS_MSG_WAITALL) && !(flags & SIEOS_MSG_PEEK) && (size_t)got < want &&
                 !(mflags & SIEOS_MSG_CTRUNC) && (!um || !um->msg_control || um->msg_controllen == 0));
        if (got)
            r = got;
        if (r >= 0 && uaddr) {                   /* a stream has its peer's name */
            struct sieos_sockaddr_un none = { .sun_family = SIEOS_AF_UNIX };
            struct usock *p = u->peer;
            put_name(uaddr, ulen, p ? &p->addr : &none, p ? p->addrlen : ADDR_HDR);
        }
    } else {
        r = dgram_recv(u, f, &io, uaddr, ulen, um, flags, &mflags);
    }
    if (um && r >= 0) {
        if (!um->msg_control || (u->type == SIEOS_SOCK_STREAM && r == 0))
            um->msg_controllen = 0;
        um->msg_flags = mflags;
    }
    return r;
}

/* read(2) and write(2) on the descriptor. */
long unix_read(struct file *f, void *buf, size_t n)
{
    struct sieos_iovec v = { buf, n };
    mutex_enter(&unix_lock);
    long r = do_recv(f->usock, f, &v, 1, NULL, NULL, NULL, 0);
    unix_unlock();
    return r;
}

long unix_write(struct file *f, const void *buf, size_t n)
{
    struct sieos_iovec v = { (void *)buf, n };
    mutex_enter(&unix_lock);
    long r = do_send(f->usock, f, &v, 1, NULL, 0, NULL, 0, 0);
    unix_unlock();
    return r;
}

static short poll_locked(struct usock *u);

short unix_poll(struct usock *u)
{
    mutex_enter(&unix_lock);
    short r = poll_locked(u);
    mutex_exit(&unix_lock);
    return r;
}

static short poll_locked(struct usock *u)
{
    short r = 0;
    if (u->state == US_LISTEN)
        return u->nback ? POLLIN : 0;
    if (u->type == SIEOS_SOCK_DGRAM) {
        if (u->q || u->shut_rd)
            r |= POLLIN;
        r |= POLLOUT;
        if (u->state == US_CONNECTED && u->peer_gone)
            r |= POLLHUP;
        return r;
    }
    if (u->state != US_CONNECTED)
        return POLLOUT | POLLHUP;
    if (u->rn != u->wn || u->peer_gone || u->peer_shut_wr || u->shut_rd)
        r |= POLLIN;
    if (u->peer_gone || (u->peer && u->peer->shut_rd) || u->shut_wr)
        r |= POLLOUT;                            /* the write fails at once */
    else if (u->peer && UNIX_BUF - (u->peer->wn - u->peer->rn) > 0)
        r |= POLLOUT;
    if (u->peer_gone || (u->shut_wr && u->peer_shut_wr))
        r |= POLLHUP;
    return r;
}

/* ------------------------------------------------------------------ */
/* Options and system calls                                            */
/* ------------------------------------------------------------------ */

static long do_getsockopt(struct usock *u, long level, long name, void *val, unsigned int *ulen)
{
    if (level != SIEOS_SOL_SOCKET)
        return -ENOPROTOOPT_K;
    if (!user_ok(ulen, sizeof(*ulen), true))
        return -EFAULT;
    int iv;
    switch (name) {
    case SIEOS_SO_TYPE:       iv = u->type; break;
    case SIEOS_SO_DOMAIN:     iv = SIEOS_AF_UNIX; break;
    case SIEOS_SO_PROTOCOL:   iv = 0; break;
    case SIEOS_SO_ACCEPTCONN: iv = u->state == US_LISTEN; break;
    case SIEOS_SO_SNDBUF:
    case SIEOS_SO_RCVBUF:     iv = u->type == SIEOS_SOCK_STREAM ? UNIX_BUF : UNIX_QUEUE; break;
    case SIEOS_SO_ERROR:
    case SIEOS_SO_DEBUG:
    case SIEOS_SO_REUSEADDR:
    case SIEOS_SO_KEEPALIVE:
    case SIEOS_SO_DONTROUTE:
    case SIEOS_SO_BROADCAST:
    case SIEOS_SO_OOBINLINE:  iv = 0; break;
    default:
        return -ENOPROTOOPT_K;
    }
    if (*ulen < sizeof(int) || !user_ok(val, sizeof(int), true))
        return -EINVAL;
    memcpy(val, &iv, sizeof(int));
    *ulen = sizeof(int);
    return 0;
}

static long do_setsockopt(long level, long name, const void *val, long len)
{
    if (level != SIEOS_SOL_SOCKET)
        return -ENOPROTOOPT_K;
    switch (name) {
    case SIEOS_SO_SNDBUF:
    case SIEOS_SO_RCVBUF:
    case SIEOS_SO_REUSEADDR:
    case SIEOS_SO_KEEPALIVE:
    case SIEOS_SO_DEBUG:
    case SIEOS_SO_DONTROUTE:
    case SIEOS_SO_BROADCAST:
    case SIEOS_SO_OOBINLINE:
    case SIEOS_SO_LINGER:
    case SIEOS_SO_RCVTIMEO:
    case SIEOS_SO_SNDTIMEO:
        if (len < (long)sizeof(int) || !user_ok(val, sizeof(int), false))
            return -EINVAL;
        return 0;                                /* accepted; fixed behaviour */
    }
    return -ENOPROTOOPT_K;
}

static long do_msg(struct usock *u, struct file *f, struct sieos_msghdr *um, long flags, bool send)
{
    if (!user_ok(um, sizeof(*um), !send))
        return -EFAULT;
    struct sieos_msghdr m = *um;
    struct sieos_iovec one;
    if (m.msg_iovlen == 1) {                     /* a vector of one is copied, like read(2)'s */
        if (!user_ok(m.msg_iov, sizeof(one), false))
            return -EFAULT;
        one = m.msg_iov[0];
        m.msg_iov = &one;
    }
    if (send)
        return do_send(u, f, m.msg_iov, m.msg_iovlen, m.msg_name, m.msg_namelen, m.msg_control, m.msg_controllen,
                       flags);
    return do_recv(u, f, m.msg_iov, m.msg_iovlen, m.msg_name, m.msg_name ? &um->msg_namelen : NULL, um, flags);
}

static long syscall_locked(struct usock *u, struct file *f, uint64_t nr, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5, uint64_t a6);

/* A socket call on an AF_UNIX descriptor (a1 is the descriptor). */
long unix_syscall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct file *f;
    struct usock *u = ufd(a1, &f);
    if (!u)
        return -ENOTSOCK;
    mutex_enter(&unix_lock);
    long r = syscall_locked(u, f, nr, a2, a3, a4, a5, a6);
    unix_unlock();
    return r;
}

static long syscall_locked(struct usock *u, struct file *f, uint64_t nr, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5, uint64_t a6)
{
    switch (nr) {
    case SIEOS_SYS_bind:        return do_bind(u, (const void *)a2, a3);
    case SIEOS_SYS_listen:      return do_listen(u, a2);
    case SIEOS_SYS_accept:
        if (a4 & ~(uint64_t)(SIEOS_SOCK_CLOEXEC | SIEOS_SOCK_NONBLOCK | SIEOS_SOCK_NDELAY))
            return -EINVAL;
        return do_accept(u, f, (void *)a2, (unsigned int *)a3, a4);
    case SIEOS_SYS_connect:     return do_connect(u, f, (const void *)a2, a3);
    case SIEOS_SYS_shutdown:    return do_shutdown(u, a2);
    case SIEOS_SYS_sendto: {
        struct sieos_iovec v = { (void *)a2, a3 };
        return do_send(u, f, &v, 1, (const void *)a5, a6, NULL, 0, a4);
    }
    case SIEOS_SYS_recvfrom: {
        struct sieos_iovec v = { (void *)a2, a3 };
        return do_recv(u, f, &v, 1, (void *)a5, (unsigned int *)a6, NULL, a4);
    }
    case SIEOS_SYS_sendmsg:     return do_msg(u, f, (struct sieos_msghdr *)a2, a3, true);
    case SIEOS_SYS_recvmsg:     return do_msg(u, f, (struct sieos_msghdr *)a2, a3, false);
    case SIEOS_SYS_getsockname: return put_name((void *)a2, (unsigned int *)a3, &u->addr, u->addrlen);
    case SIEOS_SYS_getpeername: {
        struct usock *p = u->peer;
        if (u->type == SIEOS_SOCK_DGRAM && !p && u->has_dest)
            return put_name((void *)a2, (unsigned int *)a3, &u->dest, name_len(&u->dest));
        if (!p)
            return -ENOTCONN;
        return put_name((void *)a2, (unsigned int *)a3, &p->addr, p->addrlen);
    }
    case SIEOS_SYS_getsockopt:  return do_getsockopt(u, a2, a3, (void *)a4, (unsigned int *)a5);
    case SIEOS_SYS_setsockopt:  return do_setsockopt(a2, a3, (const void *)a4, a5);
    }
    return -EOPNOTSUPP;
}
