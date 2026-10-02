/*
 * pty.c - Pseudo-terminals.
 *
 * openpty() returns a master and a slave file.  The slave is a full tty
 * (line discipline, job control); bytes written to the master are fed to
 * the slave as keyboard input, and output written to the slave is queued
 * for the master to read.  Closing the master hangs up the slave's
 * session; closing every slave makes master reads return EOF.
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

#define NPTY    16
#define PTY_OUT 16384

struct pty {
    struct tty tty;                /* slave side */
    char obuf[PTY_OUT];            /* slave output, read by the master */
    uint64_t onr, onw;
    int master_open, slave_open;
    bool used;
    bool locked;                   /* /dev/ptmx: slave opens refused until unlockpt */
    bool slave_seen;               /* a slave has been opened (EOF only after that) */
    int uid, gid;                  /* owner of /dev/pts/N (grantpt) */
};

static struct pty ptys[NPTY];

static size_t pty_output(struct tty *t, const char *s, size_t n)
{
    struct pty *p = t->priv;
    size_t done = 0;
    while (done < n && p->onw - p->onr < PTY_OUT)
        p->obuf[p->onw++ % PTY_OUT] = s[done++];
    if (done) {
        wakeup(p);
        poll_wakeup();
    }
    return done;                   /* echo may drop bytes if the master is slow */
}

/* tcflush(TCOFLUSH) on the slave: what the master has not read goes. */
static void pty_oflush(struct tty *t)
{
    struct pty *p = t->priv;
    p->onr = p->onw;
    wakeup(p);
}

bool tty_is_pty(struct tty *t)
{
    return t->output == pty_output;
}


static void pty_maybe_free(struct pty *p)
{
    if (!p->master_open && !p->slave_open) {
        tty_unregister(&p->tty);
        p->used = false;
    }
}

/* Open /dev/ptmx: a master whose slave is /dev/pts/N, locked until unlockpt. */
int pty_open_master(struct file *f)
{
    struct pty *p = NULL;
    for (int i = 0; i < NPTY; i++)
        if (!ptys[i].used) {
            p = &ptys[i];
            break;
        }
    if (!p)
        return -EAGAIN;
    memset(p, 0, sizeof(*p));
    p->used = true;
    p->locked = true;
    p->uid = current->euid;
    p->gid = current->egid;
    tty_setup(&p->tty, pty_output, p);
    p->tty.oflush = pty_oflush;
    tty_register(&p->tty);
    p->master_open = 1;
    f->type = FD_PTM;
    f->pty = p;
    return 0;
}

/* Open /dev/pts/N. */
int pty_open_slave(int n, struct file *f)
{
    if (n < 0 || n >= NPTY || !ptys[n].used || !ptys[n].master_open)
        return -ENXIO;
    struct pty *p = &ptys[n];
    if (p->locked)
        return -EIO;
    p->slave_open++;
    p->slave_seen = true;
    f->type = FD_TTY;
    f->tty = &p->tty;
    return 0;
}

int pty_index(struct tty *t)
{
    if (!tty_is_pty(t))
        return -1;
    return (struct pty *)t->priv - ptys;
}

int pty_master_index(struct pty *p)
{
    return p - ptys;
}

/* For devpts: is /dev/pts/n present, and who owns it? */
bool pty_slot(int n, int *uid, int *gid)
{
    if (n < 0 || n >= NPTY || !ptys[n].used || !ptys[n].master_open)
        return false;
    *uid = ptys[n].uid;
    *gid = ptys[n].gid;
    return true;
}

int pty_count(void)
{
    return NPTY;
}

/* ISPTM, UNLKPT, PTSNAME on the master (ABI v2). */
long pty_master_ioctl(struct pty *p, int op, char *buf)
{
    switch (op) {
    case 1:                                          /* ISPTM */
        return 0;
    case 2:                                          /* UNLKPT */
        p->locked = false;
        return 0;
    case 3:                                          /* PTSNAME */
        if (!user_range_ok(current->pml4, (uint64_t)buf, 32, true))
            return -EFAULT;
        snprintf(buf, 32, "/dev/pts/%d", pty_master_index(p));
        return 0;
    }
    return -ENOTTY;
}

bool pty_master_readable(struct pty *p)
{
    return p->onr != p->onw || (p->slave_seen && !p->slave_open);
}

long pty_master_read(struct pty *p, char *buf, size_t n)
{
    while (p->onr == p->onw) {
        if (p->slave_seen && !p->slave_open)
            return 0;
        if (signal_pending(current))
            return -ERESTART;
        sleep_on(p);
    }
    size_t got = 0;
    while (got < n && p->onr < p->onw)
        buf[got++] = p->obuf[p->onr++ % PTY_OUT];
    wakeup(p);                     /* writers waiting for space */
    return got;
}

long pty_master_write(struct pty *p, const char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        tty_input(&p->tty, buf[i]);
    return n;
}

/* What the slave writes, with the output processing of c_oflag: ONLCR (with
 * OPOST) sends a newline as CR LF. */
long pty_slave_write(struct tty *t, const char *buf, size_t n)
{
    struct pty *p = t->priv;
    bool onlcr = (t->t.c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR);
    size_t done = 0;
    while (done < n) {
        if (!p->master_open)
            return done ? (long)done : -EIO;
        size_t k;
        if (onlcr && buf[done] == '\n') {
            k = 0;
            if (PTY_OUT - (p->onw - p->onr) >= 2) {
                pty_output(t, "\r\n", 2);
                k = 1;
            }
        } else {
            size_t run = 0;                      /* up to the next newline */
            while (done + run < n && !(onlcr && buf[done + run] == '\n'))
                run++;
            k = pty_output(t, buf + done, run);
        }
        done += k;
        if (k == 0) {                            /* the master's buffer is full */
            if (signal_pending(current))
                return done ? (long)done : -ERESTART;
            sleep_on(p);
        }
    }
    return done;
}

void pty_master_close(struct pty *p)
{
    p->master_open = 0;
    tty_hangup(&p->tty);
    wakeup(p);
    pty_maybe_free(p);
}

void pty_slave_close(struct tty *t)
{
    if (!tty_is_pty(t))
        return;
    struct pty *p = t->priv;
    if (p->slave_open > 0)
        p->slave_open--;
    wakeup(p);
    poll_wakeup();
    pty_maybe_free(p);
}

/* Window size get/set from the master side (terminal emulators resize). */
long pty_winsize(struct pty *p, unsigned long cmd, uint64_t arg)
{
    size_t sz = sizeof(struct winsize);
    if (!user_range_ok(current->pml4, arg, sz, cmd == TIOCGWINSZ))
        return -EFAULT;
    if (cmd == TIOCGWINSZ) {
        memcpy((void *)arg, &p->tty.ws, sz);
    } else {
        memcpy(&p->tty.ws, (void *)arg, sz);
        if (p->tty.pgrp)
            signal_pgrp(p->tty.pgrp, SIGWINCH);
    }
    return 0;
}

void pty_slave_ref(struct tty *t)
{
    if (tty_is_pty(t))
        ((struct pty *)t->priv)->slave_open++;
}
