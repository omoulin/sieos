/*
 * fdext.c - Linux's descriptor kinds: eventfd, timerfd, pidfd, epoll, and
 * memfd (a tmpfs file no directory names).  System calls 213-222
 * (sieos/syscall.h, sieos/fdext.h).
 *
 * The first four are FD_OPS files (fs.h: their read, write, poll and close
 * here).  Whatever makes one of them ready wakes poll's sleepers
 * (poll_wakeup): poll, select and epoll_wait see it.
 *
 *   eventfd   a 64-bit counter: write adds, read takes it (or 1, EFD_SEMAPHORE);
 *   timerfd   a timer on CLOCK_REALTIME or CLOCK_MONOTONIC: read gives the
 *             expirations since the last read; the clock tick wakes the
 *             waiters when it expires (timerfd_tick);
 *   pidfd     a process: readable once it has ended (proc_teardown wakes);
 *             pidfd_send_signal signals it;
 *   epoll     a set of descriptors and the events wanted: epoll_wait polls
 *             them and sleeps as poll does.  Level-triggered, or EPOLLET
 *             (reported when an event appears that was not there at the last
 *             look), EPOLLONESHOT.  An entry goes when its descriptor is
 *             closed (it names the file, not only the number).
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
#include "sieos/syscall.h"
#include "sieos/fcntl.h"
#include "sieos/time.h"
#include "sieos/fdext.h"

struct inode *tmpfs_unnamed(struct fs *fs, int uid, int gid);   /* tmpfs.c */
void poll_sleep(uint64_t deadline);                             /* poll.c */

/* A new FD_OPS descriptor (SIEOS_O_CLOEXEC, SIEOS_O_NONBLOCK from oflags). */
static long ops_install(const struct file_ops *ops, void *priv, long oflags)
{
    struct file *f = file_alloc();
    if (!f)
        return -ENFILE;
    f->type = FD_OPS;
    f->ops = ops;
    f->priv = priv;
    f->flags = O_RDWR | ((oflags & SIEOS_O_NONBLOCK) ? O_NONBLOCK_K : 0);
    int fd = fsys_fdalloc(f, 0);
    if (fd < 0) {
        file_close(f);
        return fd;
    }
    if (oflags & SIEOS_O_CLOEXEC)
        current->fdflags[fd] |= FD_CLOEXEC;
    return fd;
}

static void *ops_priv(long fd, const struct file_ops *ops)
{
    struct file *f = fsys_file(fd);
    return f && f->type == FD_OPS && f->ops == ops ? f->priv : NULL;
}

static bool nonblock(struct file *f)
{
    return f->flags & O_NONBLOCK_K;
}

static uint64_t ns_to_ticks(uint64_t ns)
{
    return ns / (1000000000UL / TIMER_HZ) + 1;
}

/* ---------------- eventfd ---------------- */

struct efd {
    uint64_t count;
    bool semaphore;
};

static long efd_read(struct file *f, void *buf, size_t n)
{
    struct efd *e = f->priv;
    if (n < 8)
        return -EINVAL;
    while (e->count == 0) {
        if (nonblock(f))
            return -EAGAIN;
        if (signal_pending(current))
            return -ERESTART;
        sleep_on(e);
    }
    uint64_t v = e->semaphore ? 1 : e->count;
    e->count -= v;
    memcpy(buf, &v, 8);
    wakeup(e);
    poll_wakeup();
    return 8;
}

static long efd_write(struct file *f, const void *buf, size_t n)
{
    struct efd *e = f->priv;
    uint64_t v;
    if (n < 8)
        return -EINVAL;
    memcpy(&v, buf, 8);
    if (v == UINT64_MAX)
        return -EINVAL;
    while (e->count > UINT64_MAX - 1 - v) {      /* (the counter stops at 2^64 - 2) */
        if (nonblock(f))
            return -EAGAIN;
        if (signal_pending(current))
            return -ERESTART;
        sleep_on(e);
    }
    e->count += v;
    wakeup(e);
    poll_wakeup();
    return 8;
}

static short efd_poll(struct file *f)
{
    struct efd *e = f->priv;
    return (e->count ? POLLIN : 0) | (e->count < UINT64_MAX - 1 ? POLLOUT : 0);
}

static void efd_close(struct file *f)
{
    kfree(f->priv);
}

static const struct file_ops efd_ops = { "eventfd", efd_read, efd_write, efd_poll, efd_close };

static long do_eventfd(unsigned int initval, long flags)
{
    if (flags & ~(long)(SIEOS_O_CLOEXEC | SIEOS_O_NONBLOCK | SIEOS_EFD_SEMAPHORE))
        return -EINVAL;
    struct efd *e = kzalloc(sizeof(*e));
    if (!e)
        return -ENOMEM;
    e->count = initval;
    e->semaphore = flags & SIEOS_EFD_SEMAPHORE;
    long fd = ops_install(&efd_ops, e, flags);
    if (fd < 0)
        kfree(e);
    return fd;
}

/* ---------------- timerfd ---------------- */

struct tfd {
    struct tfd *next;                            /* the armed ones (timerfd_tick) */
    int clock;
    bool armed, signaled;
    uint64_t when, interval;                     /* ns on the clock */
    uint64_t pending;                            /* expirations not read yet */
};

static struct tfd *armed;

static uint64_t clock_ns(int clock)
{
    return clock == SIEOS_CLOCK_REALTIME ? (uint64_t)realtime_ns() : hrtime();
}

static void tfd_unlist(struct tfd *t)
{
    for (struct tfd **pp = &armed; *pp; pp = &(*pp)->next)
        if (*pp == t) {
            *pp = t->next;
            break;
        }
    t->next = NULL;
}

/* Count the expirations passed into t->pending, moving the timer on. */
static void tfd_update(struct tfd *t)
{
    if (!t->armed)
        return;
    uint64_t now = clock_ns(t->clock);
    if (now < t->when)
        return;
    uint64_t k = 1;
    if (t->interval) {
        k += (now - t->when) / t->interval;
        t->when += k * t->interval;
    } else {
        t->armed = false;
        tfd_unlist(t);
    }
    t->pending += k;
}

/* The clock tick: wake the waiters of a timer that has expired. */
void timerfd_tick(void)
{
    for (struct tfd *t = armed; t; t = t->next)
        if (!t->signaled && clock_ns(t->clock) >= t->when) {
            t->signaled = true;
            wakeup(t);
            poll_wakeup();
        }
}

static long tfd_read(struct file *f, void *buf, size_t n)
{
    struct tfd *t = f->priv;
    if (n < 8)
        return -EINVAL;
    for (;;) {
        tfd_update(t);
        if (t->pending)
            break;
        if (!t->armed || nonblock(f))
            return -EAGAIN;
        if (signal_pending(current))
            return -ERESTART;
        uint64_t now = clock_ns(t->clock);
        curlwp->wake_tick = ticks + ns_to_ticks(t->when > now ? t->when - now : 0);
        sleep_on(t);
        curlwp->wake_tick = 0;
    }
    memcpy(buf, &t->pending, 8);
    t->pending = 0;
    t->signaled = false;
    return 8;
}

static short tfd_poll(struct file *f)
{
    struct tfd *t = f->priv;
    return t->pending || (t->armed && clock_ns(t->clock) >= t->when) ? POLLIN : 0;
}

static void tfd_close(struct file *f)
{
    struct tfd *t = f->priv;
    tfd_unlist(t);
    kfree(t);
}

static const struct file_ops tfd_ops = { "timerfd", tfd_read, NULL, tfd_poll, tfd_close };

static long do_timerfd_create(int clock, long flags)
{
    if (clock != SIEOS_CLOCK_REALTIME && clock != SIEOS_CLOCK_MONOTONIC)
        return -EINVAL;
    if (flags & ~(long)(SIEOS_O_CLOEXEC | SIEOS_O_NONBLOCK))
        return -EINVAL;
    struct tfd *t = kzalloc(sizeof(*t));
    if (!t)
        return -ENOMEM;
    t->clock = clock;
    long fd = ops_install(&tfd_ops, t, flags);
    if (fd < 0)
        kfree(t);
    return fd;
}

static uint64_t ts_ns(const struct sieos_timespec *ts)
{
    return (uint64_t)ts->tv_sec * 1000000000UL + ts->tv_nsec;
}

static void ns_ts(uint64_t ns, struct sieos_timespec *ts)
{
    ts->tv_sec = ns / 1000000000UL;
    ts->tv_nsec = ns % 1000000000UL;
}

static void tfd_get(struct tfd *t, struct sieos_itimerspec *cur)
{
    tfd_update(t);
    uint64_t now = clock_ns(t->clock);
    ns_ts(t->interval, &cur->it_interval);
    ns_ts(t->armed && t->when > now ? t->when - now : 0, &cur->it_value);
}

static long do_timerfd_settime(long fd, long flags, const struct sieos_itimerspec *unew,
                               struct sieos_itimerspec *uold)
{
    struct tfd *t = ops_priv(fd, &tfd_ops);
    if (!t)
        return fsys_file(fd) ? -EINVAL : -EBADF;
    if (flags & ~(long)(SIEOS_TFD_TIMER_ABSTIME | 2))     /* (2: CANCEL_ON_SET, accepted) */
        return -EINVAL;
    if (!user_ok(unew, sizeof(*unew), false) || (uold && !user_ok(uold, sizeof(*uold), true)))
        return -EFAULT;
    struct sieos_itimerspec nv = *unew;
    if (nv.it_value.tv_nsec < 0 || nv.it_value.tv_nsec >= 1000000000L || nv.it_value.tv_sec < 0 ||
        nv.it_interval.tv_nsec < 0 || nv.it_interval.tv_nsec >= 1000000000L || nv.it_interval.tv_sec < 0)
        return -EINVAL;
    if (uold) {
        struct sieos_itimerspec ov;
        tfd_get(t, &ov);
        *uold = ov;
    }
    tfd_unlist(t);
    t->pending = 0;
    t->signaled = false;
    t->interval = ts_ns(&nv.it_interval);
    t->armed = nv.it_value.tv_sec || nv.it_value.tv_nsec;
    if (t->armed) {
        uint64_t v = ts_ns(&nv.it_value);
        t->when = (flags & SIEOS_TFD_TIMER_ABSTIME) ? v : clock_ns(t->clock) + v;
        t->next = armed;
        armed = t;
    }
    return 0;
}

static long do_timerfd_gettime(long fd, struct sieos_itimerspec *ucur)
{
    struct tfd *t = ops_priv(fd, &tfd_ops);
    if (!t)
        return fsys_file(fd) ? -EINVAL : -EBADF;
    if (!user_ok(ucur, sizeof(*ucur), true))
        return -EFAULT;
    struct sieos_itimerspec cur;
    tfd_get(t, &cur);
    *ucur = cur;
    return 0;
}

/* ---------------- pidfd ---------------- */

struct pidfd {
    int pid;
    uint64_t start;                              /* the process's start_tick: the same one */
};

static struct proc *pidfd_proc(struct pidfd *d)
{
    struct proc *p = proc_find(d->pid);
    return p && p->start_tick == d->start && p->state != PSTATE_UNUSED ? p : NULL;
}

static short pidfd_poll(struct file *f)
{
    struct proc *p = pidfd_proc(f->priv);
    return !p || p->state == PSTATE_ZOMBIE ? POLLIN : 0;
}

static void pidfd_close(struct file *f)
{
    kfree(f->priv);
}

static const struct file_ops pidfd_ops = { "pidfd", NULL, NULL, pidfd_poll, pidfd_close };

static long do_pidfd_open(int pid, long flags)
{
    if (flags & ~(long)SIEOS_O_NONBLOCK)
        return -EINVAL;
    if (pid <= 0)
        return -EINVAL;
    struct proc *p = proc_find(pid);
    if (!p || p->state == PSTATE_UNUSED || p->state == PSTATE_EMBRYO)
        return -ESRCH;
    struct pidfd *d = kzalloc(sizeof(*d));
    if (!d)
        return -ENOMEM;
    d->pid = pid;
    d->start = p->start_tick;
    long fd = ops_install(&pidfd_ops, d, flags | SIEOS_O_CLOEXEC);   /* (always close-on-exec, as Linux) */
    if (fd < 0)
        kfree(d);
    return fd;
}

static long do_pidfd_send_signal(long fd, int sig, const void *info, long flags)
{
    struct pidfd *d = ops_priv(fd, &pidfd_ops);
    struct file *f = fsys_file(fd);
    int dirpid = !d && f && f->type == FD_INODE ? procfs_dir_pid(f->ip) : 0;   /* an open /proc/PID */
    if (!d && !dirpid)
        return -EBADF;
    if (info || flags)
        return -EINVAL;
    if (sig < 0 || sig >= KNSIG)
        return -EINVAL;
    struct proc *p = d ? pidfd_proc(d) : proc_find(dirpid);
    if (!p || p->state != PSTATE_RUNNING)
        return -ESRCH;
    if (current->euid != 0 && current->euid != p->uid && current->uid != p->uid)
        return -EPERM;
    if (sig)
        signal_send(p, sig);
    return 0;
}

/* ---------------- memfd ---------------- */

static long do_memfd_create(const char *uname, long flags)
{
    if (flags & ~(long)(SIEOS_MFD_CLOEXEC | SIEOS_MFD_ALLOW_SEALING))
        return -EINVAL;
    char name[250];
    int r = user_fetch_str(uname, name, sizeof(name));
    if (r < 0)
        return r == -ENAMETOOLONG ? -EINVAL : r;
    int err;
    struct inode *shm = namei("/dev/shm", &err);
    if (!shm)
        return err;
    struct inode *ip = tmpfs_unnamed(shm->fs, current->euid, current->egid);
    iput(shm);
    if (!ip)
        return -ENOMEM;
    struct file *f = file_alloc();
    if (!f) {
        iput(ip);
        return -ENFILE;
    }
    f->type = FD_INODE;
    f->ip = ip;
    f->flags = O_RDWR;
    size_t n = strlen(name) + 8;
    f->pname = kmalloc(n);                      /* for /proc's fd link: "memfd:NAME" */
    if (f->pname) {
        strlcpy(f->pname, "memfd:", n);
        strlcat(f->pname, name, n);
    }
    int fd = fsys_fdalloc(f, 0);
    if (fd < 0) {
        file_close(f);
        return fd;
    }
    if (flags & SIEOS_MFD_CLOEXEC)
        current->fdflags[fd] |= FD_CLOEXEC;
    return fd;
}

/* ---------------- epoll ---------------- */

struct epitem {
    int fd;
    struct file *f;
    uint64_t gen;                                /* f->gen when added: the same file */
    uint32_t events;
    uint64_t data;
    short last;                                  /* EPOLLET: the events at the last look */
    bool off;                                    /* EPOLLONESHOT, reported */
};

struct epoll {
    int n, cap;
    struct epitem *items;
};

#define EP_POLLBITS (POLLIN | POLLPRI | POLLOUT | POLLERR | POLLHUP)

/* The item's file, if its descriptor still names it; NULL: the item goes. */
static struct file *ep_file(struct epitem *it)
{
    struct file *f = it->fd >= 0 && it->fd < NOFILE ? current->ofile[it->fd] : NULL;
    return f == it->f && f->gen == it->gen ? f : NULL;
}

static void ep_remove(struct epoll *ep, int i)
{
    ep->items[i] = ep->items[--ep->n];
}

/* The ready events: up to max into out (if out), each reported item updated. */
static int ep_scan(struct epoll *ep, struct sieos_epoll_event *out, int max, bool report)
{
    int n = 0;
    for (int i = 0; i < ep->n && n < max; i++) {
        struct epitem *it = &ep->items[i];
        struct file *f = ep_file(it);
        if (!f) {
            ep_remove(ep, i--);
            continue;
        }
        if (it->off)
            continue;
        short r = file_poll(f, (short)(it->events & EP_POLLBITS)), now = r;   /* (EPOLLRDHUP: as POLLHUP) */
        if (it->events & SIEOS_EPOLLET) {
            if (!(r & ~it->last))                    /* an edge: something appeared since the last look */
                r = 0;                               /* (then all that is ready, as Linux) */
            if (report)
                it->last = now;
        }
        if (!r)
            continue;
        if (out) {
            struct sieos_epoll_event e = { (uint32_t)(uint16_t)r, it->data };
            memcpy(&out[n], &e, sizeof(e));
        }
        n++;
        if (report && (it->events & SIEOS_EPOLLONESHOT))
            it->off = true;
    }
    return n;
}

static short ep_poll(struct file *f)
{
    struct epoll *ep = f->priv;
    return ep_scan(ep, NULL, 1, false) ? POLLIN : 0;
}

static void ep_close(struct file *f)
{
    struct epoll *ep = f->priv;
    kfree(ep->items);
    kfree(ep);
}

static const struct file_ops ep_ops = { "eventpoll", NULL, NULL, ep_poll, ep_close };

static long do_epoll_create(long flags)
{
    if (flags & ~(long)SIEOS_O_CLOEXEC)
        return -EINVAL;
    struct epoll *ep = kzalloc(sizeof(*ep));
    if (!ep)
        return -ENOMEM;
    long fd = ops_install(&ep_ops, ep, flags);
    if (fd < 0)
        kfree(ep);
    return fd;
}

static long do_epoll_ctl(long epfd, int op, int fd, const struct sieos_epoll_event *uev)
{
    struct file *ef = fsys_file(epfd), *f = fsys_file(fd);
    if (!ef || !f)
        return -EBADF;
    if (ef->type != FD_OPS || ef->ops != &ep_ops || f == ef)
        return -EINVAL;
    if (f->type == FD_INODE && f->ip && S_ISREG(inode_mode(f->ip)))
        return -EPERM;                               /* (regular files are always ready: as Linux) */
    struct epoll *ep = ef->priv;
    struct sieos_epoll_event ev = { 0, 0 };
    if (op != SIEOS_EPOLL_CTL_DEL) {
        if (!user_ok(uev, sizeof(ev), false))
            return -EFAULT;
        memcpy(&ev, uev, sizeof(ev));
    }
    int i;
    for (i = 0; i < ep->n; i++)
        if (ep->items[i].fd == fd && ep_file(&ep->items[i]))
            break;
    switch (op) {
    case SIEOS_EPOLL_CTL_ADD:
        if (i < ep->n)
            return -EEXIST;
        if (ep->n == ep->cap) {
            int cap = ep->cap ? ep->cap * 2 : 16;
            struct epitem *n = kmalloc(cap * sizeof(*n));
            if (!n)
                return -ENOMEM;
            memcpy(n, ep->items, ep->n * sizeof(*n));
            kfree(ep->items);
            ep->items = n;
            ep->cap = cap;
        }
        ep->items[ep->n++] = (struct epitem){ fd, f, f->gen, ev.events, ev.data, 0, false };
        poll_wakeup();                               /* (a waiter on the set looks again) */
        return 0;
    case SIEOS_EPOLL_CTL_MOD:
        if (i == ep->n)
            return -ENOENT;
        ep->items[i].events = ev.events;
        ep->items[i].data = ev.data;
        ep->items[i].last = 0;
        ep->items[i].off = false;
        poll_wakeup();
        return 0;
    case SIEOS_EPOLL_CTL_DEL:
        if (i == ep->n)
            return -ENOENT;
        ep_remove(ep, i);
        return 0;
    }
    return -EINVAL;
}

static long do_epoll_wait(long epfd, struct sieos_epoll_event *uev, int max, int timeout_ms)
{
    struct file *ef = fsys_file(epfd);
    if (!ef)
        return -EBADF;
    if (ef->type != FD_OPS || ef->ops != &ep_ops || max <= 0 || max > 65536)
        return -EINVAL;
    if (!user_ok(uev, (size_t)max * sizeof(*uev), true))
        return -EFAULT;
    uint64_t deadline = 0;
    if (timeout_ms > 0)
        deadline = ticks + (uint64_t)timeout_ms * TIMER_HZ / 1000 + 1;
    file_dup(ef);
    long n;
    for (;;) {
        n = ep_scan(ef->priv, uev, max, true);
        if (n || timeout_ms == 0)
            break;
        if (deadline && ticks >= deadline)
            break;
        if (signal_pending(current)) {
            n = -EINTR;
            break;
        }
        poll_sleep(deadline);
    }
    file_close(ef);
    return n;
}

/* ---------------- system calls 213-222 ---------------- */

long syscall_fdext_v2(struct trapframe *tf, bool *handled)
{
    uint64_t a1 = tf->rdi, a2 = tf->rsi, a3 = tf->rdx, a4 = tf->r10;
    *handled = true;
    switch (tf->rax) {
    case SIEOS_SYS_epoll_create1:     return do_epoll_create(a1);
    case SIEOS_SYS_epoll_ctl:         return do_epoll_ctl(a1, (int)a2, (int)a3, (const void *)a4);
    case SIEOS_SYS_epoll_wait:        return do_epoll_wait(a1, (void *)a2, (int)a3, (int)a4);
    case SIEOS_SYS_eventfd2:          return do_eventfd((unsigned int)a1, a2);
    case SIEOS_SYS_timerfd_create:    return do_timerfd_create((int)a1, a2);
    case SIEOS_SYS_timerfd_settime:   return do_timerfd_settime(a1, a2, (const void *)a3, (void *)a4);
    case SIEOS_SYS_timerfd_gettime:   return do_timerfd_gettime(a1, (void *)a2);
    case SIEOS_SYS_memfd_create:      return do_memfd_create((const char *)a1, a2);
    case SIEOS_SYS_pidfd_open:        return do_pidfd_open((int)a1, a2);
    case SIEOS_SYS_pidfd_send_signal: return do_pidfd_send_signal(a1, (int)a2, (const void *)a3, a4);
    }
    *handled = false;
    return 0;
}
