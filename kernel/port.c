/*
 * port.c - Event ports (Solaris's port_create(3C) family, sieos/port.h):
 * a queue of events from several sources, collected by port_get/port_getn.
 *
 * A port is an FD_OPS file.  Its queue holds the events posted to it (by
 * port_send, a POSIX timer, a watched file, port_alert); its descriptor
 * associations are polled when a getter looks (as poll does: poll_wakeup
 * wakes the getters), and each is reported once, then dissociated.  A
 * descriptor association names the open file, not only the number: closing
 * the descriptor ends it.
 *
 * File Event Notification (PORT_SOURCE_FILE, Solaris's FEM): a watch on a
 * file (its file system and inode number) sends one event, then goes; the
 * VFS calls fem_notify when a file is read, written, truncated, changed in
 * its attributes, deleted, renamed, unmounted or mounted over, and a
 * directory when a name in it is added or removed.
 *
 * Locking: each port's lock covers its queue and associations; fem_lock
 * the watches (then a port's lock); a port lives while its file or a
 * timer (ptimer.c) holds it.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "fs.h"
#include "proc.h"
#include "mm.h"
#include "poll.h"
#include "abi2.h"
#include "port.h"
#include "sieos/port.h"
#include "sieos/time.h"
#include "sieos/syscall.h"

#define PORT_MAXQ   65536                        /* queued events a port holds (port_send: EAGAIN after) */
#define PORT_MAXGET 1024                         /* events one port_getn returns at most */

struct pevent {
    struct pevent *next;
    sieos_port_event_t ev;
};

struct pfd {                                     /* a descriptor association */
    struct pfd *next;
    int fd;
    struct file *f;                              /* (no reference: compared with the descriptor's) */
    uint64_t gen;
    int events;
    void *user;
};

struct port {
    kmutex_t lock;
    int refs;                                    /* its file, the timers that post to it */
    bool closed;
    struct pevent *qh, *qt;
    int nq;
    struct pfd *fds;
    bool alert;
    sieos_port_event_t alertev;
};

/* File Event Notification: the watches. */
struct fwatch {
    struct fwatch *next;
    struct port *port;
    struct fs *fs;
    uint32_t ino;
    int events;
    unsigned long object;
    void *user;
};

static kmutex_t fem_lock;
static struct fwatch *watches;
static volatile int fem_active;                  /* watches exist (the VFS's hooks look only then) */

bool fem_any(void)
{
    return __atomic_load_n(&fem_active, __ATOMIC_ACQUIRE) != 0;
}

static const struct file_ops port_ops;

static void port_hold(struct port *pt)
{
    mutex_enter(&pt->lock);
    pt->refs++;
    mutex_exit(&pt->lock);
}

/* A reference goes; the last one frees the port (closed by then). */
void port_rele(struct port *pt)
{
    mutex_enter(&pt->lock);
    bool last = --pt->refs == 0;
    mutex_exit(&pt->lock);
    if (!last)
        return;
    while (pt->qh) {
        struct pevent *e = pt->qh;
        pt->qh = e->next;
        kfree(e);
    }
    while (pt->fds) {
        struct pfd *a = pt->fds;
        pt->fds = a->next;
        kfree(a);
    }
    kfree(pt);
}

/* An event for the port (a timer's, a watched file's, port_send's): queued; false if the port is
 * closed or full. */
static bool post(struct port *pt, int source, int events, unsigned long object, void *user)
{
    struct pevent *e = kmalloc(sizeof(*e));
    if (!e)
        return false;
    e->next = NULL;
    e->ev.portev_events = events;
    e->ev.portev_source = source;
    e->ev.portev_pad = 0;
    e->ev.portev_object = object;
    e->ev.portev_user = user;
    mutex_enter(&pt->lock);
    if (pt->closed || pt->nq >= PORT_MAXQ) {
        mutex_exit(&pt->lock);
        kfree(e);
        return false;
    }
    if (pt->qt)
        pt->qt->next = e;
    else
        pt->qh = e;
    pt->qt = e;
    pt->nq++;
    mutex_exit(&pt->lock);
    poll_wakeup();                               /* (the getters, and pollers of the port) */
    return true;
}

bool port_post_timer(struct port *pt, int expirations, int timerid, void *user)
{
    return post(pt, SIEOS_PORT_SOURCE_TIMER, expirations, (unsigned long)timerid, user);
}

/* The caller's port on fd, held (port_rele), or NULL. */
struct port *port_of_fd(int fd)
{
    struct file *f = getf(fd);
    struct port *pt = NULL;
    if (f && f->type == FD_OPS && f->ops == &port_ops) {
        pt = f->priv;
        port_hold(pt);
    }
    if (f)
        releasef(f);
    return pt;
}

/* ---------------------------------------------------------------- File Event Notification */

/* Something happened to ip (events: FILE_*, exceptions included): the watches on it fire. */
void fem_notify(struct inode *ip, int events)
{
    if (!__atomic_load_n(&fem_active, __ATOMIC_ACQUIRE) || !ip)
        return;
    mutex_enter(&fem_lock);
    for (struct fwatch **pp = &watches; *pp;) {
        struct fwatch *w = *pp;
        int ev = events & (w->events | SIEOS_FILE_EXCEPTION);
        if (w->fs != ip->fs || w->ino != ip->ino || !ev) {
            pp = &w->next;
            continue;
        }
        *pp = w->next;                           /* (once: then dissociated) */
        __atomic_sub_fetch(&fem_active, 1, __ATOMIC_RELEASE);
        post(w->port, SIEOS_PORT_SOURCE_FILE, ev, w->object, w->user);
        port_rele(w->port);
        kfree(w);
    }
    mutex_exit(&fem_lock);
}

/* A file system goes (UNMOUNTED for every watch on it). */
void fem_unmount(struct fs *fs)
{
    if (!__atomic_load_n(&fem_active, __ATOMIC_ACQUIRE))
        return;
    mutex_enter(&fem_lock);
    for (struct fwatch **pp = &watches; *pp;) {
        struct fwatch *w = *pp;
        if (w->fs != fs) {
            pp = &w->next;
            continue;
        }
        *pp = w->next;
        __atomic_sub_fetch(&fem_active, 1, __ATOMIC_RELEASE);
        post(w->port, SIEOS_PORT_SOURCE_FILE, SIEOS_UNMOUNTED, w->object, w->user);
        port_rele(w->port);
        kfree(w);
    }
    mutex_exit(&fem_lock);
}

/* The watches of a port (or the one of an object), dropped. */
static int fem_dissociate(struct port *pt, bool all, unsigned long object)
{
    int n = 0;
    mutex_enter(&fem_lock);
    for (struct fwatch **pp = &watches; *pp;) {
        struct fwatch *w = *pp;
        if (w->port != pt || (!all && w->object != object)) {
            pp = &w->next;
            continue;
        }
        *pp = w->next;
        __atomic_sub_fetch(&fem_active, 1, __ATOMIC_RELEASE);
        port_rele(w->port);
        kfree(w);
        n++;
    }
    mutex_exit(&fem_lock);
    return n;
}

static bool ts_differs(const struct sieos_timespec *a, int64_t s, long ns)
{
    return a->tv_sec != s || a->tv_nsec != ns;
}

/* port_associate(PORT_SOURCE_FILE): the file named, watched (or its change reported at once). */
static long file_associate(struct port *pt, unsigned long object, int events, void *user)
{
    const struct sieos_file_obj *uo = (const struct sieos_file_obj *)object;
    if (!user_ok(uo, sizeof(*uo), false))
        return -EFAULT;
    struct sieos_file_obj fo = *uo;
    char *path = path_get();
    if (!path)
        return -ENOMEM;
    int err = user_fetch_str(fo.fo_name, path, MAXPATH);
    struct inode *ip = NULL;
    if (err == 0)
        ip = namei_at(NULL, path, (events & SIEOS_FILE_NOFOLLOW) ? NAMEI_NOFOLLOW : 0, &err);
    path_put(path);
    if (!ip)
        return err;
    struct kstat st;
    inode_getstat(ip, &st);
    int changed = 0;                             /* what already changed since the times given */
    if ((events & SIEOS_FILE_ACCESS) && ts_differs(&fo.fo_atime, st.atime, st.atime_ns))
        changed |= SIEOS_FILE_ACCESS;
    if ((events & SIEOS_FILE_MODIFIED) && ts_differs(&fo.fo_mtime, st.mtime, st.mtime_ns))
        changed |= SIEOS_FILE_MODIFIED;
    if ((events & SIEOS_FILE_ATTRIB) && ts_differs(&fo.fo_ctime, st.ctime, st.ctime_ns))
        changed |= SIEOS_FILE_ATTRIB;
    struct fs *fs = ip->fs;
    uint32_t ino = ip->ino;
    iput(ip);
    fem_dissociate(pt, false, object);           /* (associating again replaces it) */
    if (changed) {
        post(pt, SIEOS_PORT_SOURCE_FILE, changed, object, user);
        return 0;
    }
    struct fwatch *w = kzalloc(sizeof(*w));
    if (!w)
        return -ENOMEM;
    w->port = pt;
    w->fs = fs;
    w->ino = ino;
    w->events = events & (SIEOS_FILE_ACCESS | SIEOS_FILE_MODIFIED | SIEOS_FILE_ATTRIB | SIEOS_FILE_TRUNC);
    w->object = object;
    w->user = user;
    port_hold(pt);
    mutex_enter(&fem_lock);
    w->next = watches;
    watches = w;
    __atomic_add_fetch(&fem_active, 1, __ATOMIC_RELEASE);
    mutex_exit(&fem_lock);
    return 0;
}

/* ---------------------------------------------------------------- the port's operations */

/* pt->lock held: the descriptor association's file if it still stands, referenced; NULL: it goes. */
static struct file *assoc_file(struct pfd *a)
{
    struct file *f = getf(a->fd);
    if (f && (f != a->f || f->gen != a->gen)) {
        releasef(f);
        f = NULL;
    }
    return f;
}

/* pt->lock held: up to max events into out (the queue, then the descriptors that are ready). */
static int collect(struct port *pt, sieos_port_event_t *out, int max)
{
    int n = 0;
    if (pt->alert && max > 0) {                  /* (port_alert: every getter sees it, alone) */
        out[0] = pt->alertev;
        return 1;
    }
    while (n < max && pt->qh) {
        struct pevent *e = pt->qh;
        pt->qh = e->next;
        if (!pt->qh)
            pt->qt = NULL;
        pt->nq--;
        out[n++] = e->ev;
        kfree(e);
    }
    for (struct pfd **pp = &pt->fds; *pp && n < max;) {
        struct pfd *a = *pp;
        struct file *f = assoc_file(a);
        short r = f ? file_poll(f, (short)a->events) : 0;
        if (f)
            releasef(f);
        if (f && !r) {
            pp = &a->next;
            continue;
        }
        *pp = a->next;                           /* reported (or its descriptor closed): dissociated */
        if (f) {
            out[n].portev_events = r;
            out[n].portev_source = SIEOS_PORT_SOURCE_FD;
            out[n].portev_pad = 0;
            out[n].portev_object = (unsigned long)a->fd;
            out[n].portev_user = a->user;
            n++;
        }
        kfree(a);
    }
    return n;
}

static short port_poll(struct file *f)
{
    struct port *pt = f->priv;
    mutex_enter(&pt->lock);
    bool ready = pt->qh || pt->alert;
    for (struct pfd *a = pt->fds; a && !ready; a = a->next) {
        struct file *g = assoc_file(a);
        if (g) {
            ready = file_poll(g, (short)a->events) != 0;
            releasef(g);
        }
    }
    mutex_exit(&pt->lock);
    return ready ? POLLIN : 0;
}

static void port_close(struct file *f)
{
    struct port *pt = f->priv;
    fem_dissociate(pt, true, 0);
    mutex_enter(&pt->lock);
    pt->closed = true;
    mutex_exit(&pt->lock);
    port_rele(pt);
}

static const struct file_ops port_ops = { "port", NULL, NULL, port_poll, port_close, NULL, NULL };

static long port_create(void)
{
    struct port *pt = kzalloc(sizeof(*pt));
    if (!pt)
        return -ENOMEM;
    pt->refs = 1;
    struct file *f = file_alloc();
    if (!f) {
        kfree(pt);
        return -ENFILE;
    }
    f->type = FD_OPS;
    f->ops = &port_ops;
    f->priv = pt;
    f->flags = O_RDWR;
    int fd = fd_alloc(current, f, 0, 0);
    if (fd < 0)
        file_close(f);
    return fd;
}

static long fd_associate(struct port *pt, int fd, int events, void *user)
{
    struct file *f = getf(fd);
    if (!f)
        return -EBADF;
    if (f->type == FD_OPS && f->priv == pt) {
        releasef(f);
        return -EINVAL;                          /* (a port in itself) */
    }
    struct pfd *n = kmalloc(sizeof(*n));
    if (!n) {
        releasef(f);
        return -ENOMEM;
    }
    n->fd = fd;
    n->f = f;
    n->gen = f->gen;
    n->events = events;
    n->user = user;
    releasef(f);
    mutex_enter(&pt->lock);
    for (struct pfd **pp = &pt->fds; *pp; pp = &(*pp)->next)
        if ((*pp)->fd == fd) {                   /* associated already: replaced */
            struct pfd *old = *pp;
            *pp = old->next;
            kfree(old);
            break;
        }
    n->next = pt->fds;
    pt->fds = n;
    mutex_exit(&pt->lock);
    poll_wakeup();                               /* (a getter looks again) */
    return 0;
}

static long fd_dissociate(struct port *pt, int fd)
{
    long r = -ENOENT;
    mutex_enter(&pt->lock);
    for (struct pfd **pp = &pt->fds; *pp; pp = &(*pp)->next)
        if ((*pp)->fd == fd) {
            struct pfd *old = *pp;
            *pp = old->next;
            kfree(old);
            r = 0;
            break;
        }
    mutex_exit(&pt->lock);
    return r;
}

/* port_get/port_getn: at least `want` events (or the timeout), at most max; how many, or -errno. */
static long port_wait(struct port *pt, sieos_port_event_t *uev, int max, int want, const struct sieos_timespec *uto)
{
    uint64_t deadline = 0;
    bool poll_only = false;
    if (uto) {
        if (!user_ok(uto, sizeof(*uto), false))
            return -EFAULT;
        struct sieos_timespec to = *uto;
        if (to.tv_sec < 0 || to.tv_nsec < 0 || to.tv_nsec >= 1000000000L)
            return -EINVAL;
        uint64_t ns = (uint64_t)to.tv_sec * 1000000000UL + to.tv_nsec;
        poll_only = ns == 0;
        deadline = ticks + (ns * TIMER_HZ + 999999999UL) / 1000000000UL;
    }
    if (!user_ok(uev, (size_t)max * sizeof(*uev), true))
        return -EFAULT;
    sieos_port_event_t *kev = kmalloc((size_t)max * sizeof(*kev));
    if (!kev)
        return -ENOMEM;
    int n = 0;
    long r;
    for (;;) {
        uint64_t gen = poll_generation();
        mutex_enter(&pt->lock);
        n += collect(pt, kev + n, max - n);
        bool alert = pt->alert;
        mutex_exit(&pt->lock);
        if (n >= want || alert) {
            r = n;
            break;
        }
        if (poll_only || (deadline && ticks >= deadline)) {
            r = n ? n : -ETIME;
            break;
        }
        if (signal_pending(current)) {
            r = n ? n : -EINTR;
            break;
        }
        poll_sleep(gen, deadline);
    }
    if (r > 0)
        memcpy(uev, kev, (size_t)r * sizeof(*kev));
    kfree(kev);
    return r;
}

static long do_portfs(long op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    if (op == SIEOS_PORT_CREATE)
        return port_create();
    if (op == SIEOS_PORT_SENDN) {                /* (ports, errors, nent, events, user) */
        int *ports = (int *)a1, *errors = (int *)a2;
        long nent = (long)a3;
        if (nent <= 0 || nent > 1024)
            return -EINVAL;
        if (!user_ok(ports, nent * sizeof(int), false) || !user_ok(errors, nent * sizeof(int), true))
            return -EFAULT;
        long sent = 0;
        for (long i = 0; i < nent; i++) {
            struct port *pt = port_of_fd(ports[i]);
            int e = 0;
            if (!pt)
                e = EBADF;
            else if (!post(pt, SIEOS_PORT_SOURCE_USER, (int)a4, 0, (void *)a5))
                e = EAGAIN;
            if (pt)
                port_rele(pt);
            errors[i] = e ? sieos_errno(e) : 0;
            if (!e)
                sent++;
        }
        return sent;
    }
    struct port *pt = port_of_fd((int)a1);
    if (!pt)
        return -EBADF;                           /* (not a port) */
    long r = -EINVAL;
    switch (op) {
    case SIEOS_PORT_ASSOCIATE:                   /* (port, source, object, events, user) */
        if (a2 == SIEOS_PORT_SOURCE_FD)
            r = (long)a3 > INT32_MAX ? -EBADF : fd_associate(pt, (int)a3, (int)a4, (void *)a5);
        else if (a2 == SIEOS_PORT_SOURCE_FILE)
            r = file_associate(pt, a3, (int)a4, (void *)a5);
        break;
    case SIEOS_PORT_DISSOCIATE:                  /* (port, source, object) */
        if (a2 == SIEOS_PORT_SOURCE_FD)
            r = fd_dissociate(pt, (int)a3);
        else if (a2 == SIEOS_PORT_SOURCE_FILE)
            r = fem_dissociate(pt, false, a3) ? 0 : -ENOENT;
        break;
    case SIEOS_PORT_SEND:                        /* (port, events, user) */
        r = post(pt, SIEOS_PORT_SOURCE_USER, (int)a2, 0, (void *)a3) ? 0 : -EAGAIN;
        break;
    case SIEOS_PORT_GET:                         /* (port, event *, timeout *) */
        r = port_wait(pt, (sieos_port_event_t *)a2, 1, 1, (const struct sieos_timespec *)a3);
        r = r > 0 ? 0 : r;
        break;
    case SIEOS_PORT_GETN: {                      /* (port, list *, max, unsigned *nget, timeout *) */
        unsigned *nget = (unsigned *)a4;
        long max = (long)a3;
        if (!user_ok(nget, sizeof(*nget), true))
            r = -EFAULT;
        else if (max == 0) {
            mutex_enter(&pt->lock);              /* (max 0: how many are queued) */
            *nget = pt->nq;
            mutex_exit(&pt->lock);
            r = 0;
        } else if (max < 0 || max > PORT_MAXGET || *nget > (unsigned)max) {
            r = -EINVAL;
        } else {
            int want = *nget ? (int)*nget : 1;
            r = port_wait(pt, (sieos_port_event_t *)a2, (int)max, want, (const struct sieos_timespec *)a5);
            if (r >= 0 || r == -ETIME)
                *nget = r > 0 ? (unsigned)r : 0;
            r = r > 0 ? 0 : r;
        }
        break;
    }
    case SIEOS_PORT_ALERT: {                     /* (port, flags, events, user) */
        long flags = (long)a2;
        mutex_enter(&pt->lock);
        if (flags == SIEOS_PORT_ALERT_SET || flags == SIEOS_PORT_ALERT_UPDATE) {
            if (flags == SIEOS_PORT_ALERT_SET && pt->alert && a3) {
                r = -EBUSY;
            } else {
                pt->alert = a3 != 0;             /* (events 0: the alert ends) */
                pt->alertev.portev_events = (int)a3;
                pt->alertev.portev_source = SIEOS_PORT_SOURCE_ALERT;
                pt->alertev.portev_pad = 0;
                pt->alertev.portev_object = 0;
                pt->alertev.portev_user = (void *)a4;
                r = 0;
            }
        }
        mutex_exit(&pt->lock);
        if (r == 0)
            poll_wakeup();
        break;
    }
    }
    port_rele(pt);
    return r;
}

long sys2_portfs(struct trapframe *tf)
{
    return do_portfs((long)tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
}
