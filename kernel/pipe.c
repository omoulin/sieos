/*
 * pipe.c - Anonymous pipes and named FIFOs.  A FIFO's pipe is found through
 * its inode (file system and inode number) while it is open, and goes away
 * with the last open descriptor, as on Solaris.
 *
 * Locking: a pipe's state is under its mutex, and readers and writers wait
 * on its condition variable; the list of open FIFOs has its own lock, taken
 * before a pipe's.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "fs.h"
#include "proc.h"
#include "mm.h"
#include "poll.h"

#define PIPE_SIZE 65536             /* as Linux's */
#define PIPE_BUF  4096              /* writes up to this size are not split */

struct pipe {
    kmutex_t lock;
    kcondvar_t cv;
    char buf[PIPE_SIZE];
    uint64_t nread, nwrite;        /* total bytes read / written */
    int readers, writers;
    uint64_t ropens, wopens;       /* opens so far: a FIFO open waits for the other end */
    struct fs *fs;                 /* named FIFO: its inode, else NULL */
    uint32_t ino;
    struct pipe *next;
};

static struct pipe *fifos;
static kmutex_t fifo_lock;

int pipe_create(struct file **rf, struct file **wf)
{
    struct pipe *p = kzalloc(sizeof(*p));
    if (!p)
        return -ENOMEM;
    *rf = file_alloc();
    *wf = *rf ? file_alloc() : NULL;
    if (!*rf || !*wf) {
        if (*rf)
            file_close(*rf);
        kfree(p);
        return -ENFILE;
    }
    p->readers = p->writers = 1;
    (*rf)->type = FD_PIPE;
    (*rf)->flags = O_RDONLY;
    (*rf)->pipe = p;
    (*wf)->type = FD_PIPE;
    (*wf)->flags = O_WRONLY;
    (*wf)->pipe = p;
    return 0;
}

bool pipe_readable(struct pipe *p)
{
    return p->nread != p->nwrite || p->writers == 0;
}

/* As Linux: the read end hung up when no writer is left (a FIFO's: once it had
 * one), the write end in error when no reader is (asyncio waits for these). */
short pipe_hangup(struct pipe *p, int acc)
{
    short r = 0;
    if (acc != O_WRONLY && p->writers == 0 && (!p->fs || p->wopens))
        r |= POLLHUP;
    if (acc != O_RDONLY && p->readers == 0)
        r |= POLLERR;
    return r;
}

bool pipe_writable(struct pipe *p)
{
    return PIPE_SIZE - (p->nwrite - p->nread) >= PIPE_BUF || p->readers == 0;
}

void pipe_close(struct pipe *p, int acc)
{
    bool named = p->fs != NULL;
    if (named)
        mutex_enter(&fifo_lock);
    mutex_enter(&p->lock);
    if (acc != O_WRONLY)
        p->readers--;
    if (acc != O_RDONLY)
        p->writers--;
    cv_broadcast(&p->cv);
    bool gone = p->readers == 0 && p->writers == 0;
    if (gone && named)
        for (struct pipe **pp = &fifos; *pp; pp = &(*pp)->next)
            if (*pp == p) {
                *pp = p->next;
                break;
            }
    mutex_exit(&p->lock);
    if (named)
        mutex_exit(&fifo_lock);
    poll_wakeup();
    if (gone)
        kfree(p);
}

/* A FIFO's pipe made by an open that failed at once: gone, unless another open took it meanwhile. */
static void pipe_close_unused(struct pipe *p)
{
    mutex_enter(&fifo_lock);
    mutex_enter(&p->lock);
    bool gone = p->readers == 0 && p->writers == 0;
    if (gone)
        for (struct pipe **pp = &fifos; *pp; pp = &(*pp)->next)
            if (*pp == p) {
                *pp = p->next;
                break;
            }
    mutex_exit(&p->lock);
    mutex_exit(&fifo_lock);
    if (gone)
        kfree(p);
}

/*
 * Open a named FIFO.  A reader waits for a writer and a writer for a reader
 * unless O_NONBLOCK is set, in which case a writer without a reader fails
 * with ENXIO.  O_RDWR opens both ends and never waits.
 */
int fifo_open(struct file *f, struct inode *ip, int flags)
{
    int acc = flags & O_ACCMODE;
    bool nonblock = flags & O_NONBLOCK_K;
    struct pipe *p;
    mutex_enter(&fifo_lock);
    for (p = fifos; p; p = p->next)
        if (p->fs == ip->fs && p->ino == ip->ino)
            break;
    if (!p) {
        if ((acc == O_WRONLY && nonblock) || !(p = kzalloc(sizeof(*p)))) {
            mutex_exit(&fifo_lock);
            return acc == O_WRONLY && nonblock ? -ENXIO : -ENOMEM;
        }
        p->fs = ip->fs;
        p->ino = ip->ino;
        p->next = fifos;
        fifos = p;
    }
    mutex_enter(&p->lock);
    mutex_exit(&fifo_lock);
    if (acc == O_WRONLY && nonblock && p->readers == 0) {
        bool gone = p->writers == 0;             /* (made for this open, unused: drop it) */
        mutex_exit(&p->lock);
        if (gone)
            pipe_close_unused(p);
        return -ENXIO;
    }
    if (acc != O_WRONLY) {
        p->readers++;
        p->ropens++;
    }
    if (acc != O_RDONLY) {
        p->writers++;
        p->wopens++;
    }
    f->type = FD_PIPE;
    f->pipe = p;
    cv_broadcast(&p->cv);
    if (acc == O_RDWR || nonblock) {
        mutex_exit(&p->lock);
        poll_wakeup();
        return 0;
    }
    poll_wakeup();
    uint64_t *other = acc == O_RDONLY ? &p->wopens : &p->ropens, seen = *other;
    int *count = acc == O_RDONLY ? &p->writers : &p->readers;
    while (*count == 0 && *other == seen) {
        if (!cv_wait_sig(&p->cv, &p->lock)) {
            mutex_exit(&p->lock);
            f->type = FD_NONE;
            f->pipe = NULL;
            pipe_close(p, acc);
            return -EINTR;
        }
    }
    mutex_exit(&p->lock);
    return 0;
}

long pipe_read(struct pipe *p, char *buf, size_t n)
{
    mutex_enter(&p->lock);
    while (p->nread == p->nwrite) {
        if (p->writers == 0) {
            mutex_exit(&p->lock);
            return 0;                          /* EOF */
        }
        if (!cv_wait_sig(&p->cv, &p->lock)) {
            mutex_exit(&p->lock);
            return -ERESTART;
        }
    }
    size_t got = 0;
    while (got < n && p->nread < p->nwrite) {
        size_t at = p->nread % PIPE_SIZE;
        size_t c = MIN(n - got, MIN(p->nwrite - p->nread, PIPE_SIZE - at));
        memcpy(buf + got, p->buf + at, c);
        got += c;
        p->nread += c;
    }
    cv_broadcast(&p->cv);
    mutex_exit(&p->lock);
    poll_wakeup();
    return got;
}

/*
 * A write of up to PIPE_BUF bytes goes in at once, when there is room for all
 * of it; a larger one as room comes.  Non-blocking (O_NONBLOCK), what fits
 * now, or EAGAIN.
 */
long pipe_write(struct pipe *p, const char *buf, size_t n, bool nonblock)
{
    size_t done = 0;
    mutex_enter(&p->lock);
    while (done < n) {
        if (p->readers == 0) {
            mutex_exit(&p->lock);
            signal_send(current, SIGPIPE);
            return done ? (long)done : -EPIPE;
        }
        size_t room = PIPE_SIZE - (p->nwrite - p->nread);
        if (room == 0 || (n <= PIPE_BUF && room < n)) {
            if (nonblock) {
                mutex_exit(&p->lock);
                return done ? (long)done : -EAGAIN;
            }
            cv_broadcast(&p->cv);
            if (!cv_wait_sig(&p->cv, &p->lock)) {
                mutex_exit(&p->lock);
                return done ? (long)done : -ERESTART;
            }
            continue;
        }
        while (done < n && room) {
            size_t at = p->nwrite % PIPE_SIZE;
            size_t c = MIN(n - done, MIN(room, PIPE_SIZE - at));
            memcpy(p->buf + at, buf + done, c);
            done += c;
            room -= c;
            p->nwrite += c;
        }
        cv_broadcast(&p->cv);
        mutex_exit(&p->lock);
        poll_wakeup();
        mutex_enter(&p->lock);
    }
    mutex_exit(&p->lock);
    return done;
}
