/*
 * poll.c - poll(2): wait until one of several files is ready.
 *
 * Every event source calls poll_wakeup(); pollers re-check their file
 * descriptors each time.  Simple and adequate for a handful of pollers.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "proc.h"
#include "fs.h"
#include "tty.h"
#include "poll.h"
#include "mm.h"
#include "net.h"

static int poll_chan;

void poll_wakeup(void)
{
    wakeup(&poll_chan);
}

/* Sleep until poll_wakeup, or the tick deadline (0: none). */
void poll_sleep(uint64_t deadline)
{
    curlwp->wake_tick = deadline;
    sleep_on(&poll_chan);
    curlwp->wake_tick = 0;
}

bool pipe_readable(struct pipe *p);
bool pipe_writable(struct pipe *p);
short pipe_hangup(struct pipe *p, int acc);

short file_poll(struct file *f, short events)
{
    short r = 0;
    switch (f->type) {
    case FD_TTY:
        if (tty_readable(f->tty))
            r |= POLLIN;
        r |= POLLOUT;
        if (f->tty->hungup)
            r |= POLLHUP;
        break;
    case FD_PTM:
        if (pty_master_readable(f->pty))
            r |= POLLIN;
        r |= POLLOUT;
        break;
    case FD_PIPE:
        if ((f->flags & O_ACCMODE) != O_WRONLY && pipe_readable(f->pipe))
            r |= POLLIN;
        if ((f->flags & O_ACCMODE) != O_RDONLY && pipe_writable(f->pipe))
            r |= POLLOUT;
        r |= pipe_hangup(f->pipe, f->flags & O_ACCMODE);
        break;
    case FD_EVENTS:
        if (input_readable())
            r |= POLLIN;
        break;
    case FD_SOCKET:
        if (socket_readable(f->sock))
            r |= POLLIN;
        if (socket_writable(f->sock))
            r |= POLLOUT;
        if (socket_urgent(f->sock))
            r |= POLLPRI;
        if (socket_failed(f->sock))
            r |= POLLERR;
        break;
    case FD_UNIX:
        r |= unix_poll(f->usock);
        break;
    case FD_OPS:
        r |= f->ops->poll ? f->ops->poll(f) : 0;
        break;
    default:
        r |= POLLIN | POLLOUT;
    }
    return r & (events | POLLHUP | POLLERR);
}

long sys_poll(struct pollfd *fds, int nfds, int timeout_ms)
{
    if (nfds < 0 || nfds > 1024)
        return -EINVAL;
    if (nfds && !user_range_ok(current->pml4, (uint64_t)fds, nfds * sizeof(*fds), true))
        return -EFAULT;
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        uint64_t t = (uint64_t)timeout_ms * TIMER_HZ / 1000;
        deadline = ticks + (t ? t : 1);
    }
    for (;;) {
        int n = 0;
        for (int i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0)
                continue;
            struct file *f = fds[i].fd < NOFILE ? current->ofile[fds[i].fd] : NULL;
            fds[i].revents = f ? file_poll(f, fds[i].events) : POLLNVAL;
            if (fds[i].revents)
                n++;
        }
        if (n || timeout_ms == 0)
            return n;
        if (deadline && ticks >= deadline)
            return 0;
        if (signal_pending(current))
            return -EINTR;
        curlwp->wake_tick = deadline;
        sleep_on(&poll_chan);
        curlwp->wake_tick = 0;
    }
}
