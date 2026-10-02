/*
 * poll.c - poll(2): wait until one of several files is ready.
 *
 * Every event source calls poll_wakeup(); pollers re-check their file
 * descriptors each time.  Simple and adequate for a handful of pollers.
 * A generation count, read before the check and compared under poll_mx
 * before sleeping, keeps a wake-up between the two from being lost.
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

static volatile uint64_t poll_gen;
static kmutex_t poll_mx = MUTEX_SPIN_INITIALIZER;

void poll_wakeup(void)
{
    mutex_enter(&poll_mx);
    poll_gen++;
    mutex_exit(&poll_mx);
    sleepq_wakeup((const void *)&poll_gen, -1);
}

uint64_t poll_generation(void)
{
    return __atomic_load_n(&poll_gen, __ATOMIC_ACQUIRE);
}

/* Sleep until a poll_wakeup after generation gen (none since: at once), a signal, or the tick deadline (0: none). */
void poll_sleep(uint64_t gen, uint64_t deadline)
{
    mutex_enter(&poll_mx);
    if (poll_gen != gen) {
        mutex_exit(&poll_mx);
        return;
    }
    curlwp->wake_tick = deadline;
    sleepq_block((const void *)&poll_gen, &poll_mx, true);
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
        uint64_t gen = poll_generation();
        for (int i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0)
                continue;
            struct file *f = getf(fds[i].fd);
            fds[i].revents = f ? file_poll(f, fds[i].events) : POLLNVAL;
            if (f)
                releasef(f);
            if (fds[i].revents)
                n++;
        }
        if (n || timeout_ms == 0)
            return n;
        if (deadline && ticks >= deadline)
            return 0;
        if (signal_pending(current))
            return -EINTR;
        poll_sleep(gen, deadline);
    }
}
