/*
 * flock.c - POSIX record locks (fcntl F_GETLK / F_SETLK / F_SETLKW).
 *
 * Locks belong to a process and cover an inclusive byte range of an inode.
 * A process's own locks never conflict with each other: setting a lock
 * replaces (and splits) its existing locks in the range.  All of a
 * process's locks on a file go when it closes any descriptor of that file.
 * F_SETLKW sleeps; a wait that would close a cycle fails with EDEADLK.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "fs.h"
#include "proc.h"
#include "mm.h"
#include "abi2.h"

struct lk {
    struct lk *next;
    struct inode *ip;
    int pid;
    short type;
    uint64_t start, end;
};

static struct lk *locks;
static int lock_chan;
static int waits_for[NPROC];          /* proc_table index -> pid it waits for (0: none) */

static bool overlap(const struct lk *a, const struct kflock *b)
{
    return a->start <= b->end && b->start <= a->end;
}

static struct lk *conflict(struct inode *ip, const struct kflock *l)
{
    for (struct lk *k = locks; k; k = k->next)
        if (same_inode(k->ip, ip) && k->pid != l->pid && overlap(k, l) &&
            (k->type == F_WRLCK_K || l->type == F_WRLCK_K))
            return k;
    return NULL;
}

int flock_get(struct inode *ip, struct kflock *l)
{
    struct lk *c = conflict(ip, l);
    if (!c) {
        l->type = F_UNLCK_K;
        return 0;
    }
    l->type = c->type;
    l->start = c->start;
    l->end = c->end;
    l->pid = c->pid;
    return 0;
}

static int pindex(int pid)
{
    struct proc *p = proc_find(pid);
    return p ? p - proc_table : -1;
}

/* Would 'me' waiting for 'owner' close a cycle of waiters? */
static bool deadlock(int me, int owner)
{
    for (int steps = 0; owner && steps < NPROC; steps++) {
        if (owner == me)
            return true;
        int i = pindex(owner);
        owner = i >= 0 ? waits_for[i] : 0;
    }
    return false;
}

static struct lk *lk_new(struct inode *ip, int pid, short type, uint64_t start, uint64_t end)
{
    struct lk *k = kmalloc(sizeof(*k));
    if (!k)
        return NULL;
    k->ip = ip;
    k->pid = pid;
    k->type = type;
    k->start = start;
    k->end = end;
    k->next = locks;
    locks = k;
    return k;
}

int flock_set(struct inode *ip, struct kflock *l, bool wait)
{
    if (l->start > l->end)
        return -EINVAL;
    if (l->type != F_UNLCK_K) {
        struct lk *c;
        int me = pindex(l->pid);
        while ((c = conflict(ip, l))) {
            if (!wait)
                return -EAGAIN;
            if (deadlock(l->pid, c->pid))
                return -EDEADLK;
            if (signal_pending(current))
                return -EINTR;
            if (me >= 0)
                waits_for[me] = c->pid;
            sleep_on(&lock_chan);
            if (me >= 0)
                waits_for[me] = 0;
        }
    }
    /* Carve the range out of the process's own locks. */
    for (struct lk **pp = &locks; *pp;) {
        struct lk *k = *pp;
        if (!same_inode(k->ip, ip) || k->pid != l->pid || !overlap(k, l)) {
            pp = &k->next;
            continue;
        }
        if (k->start < l->start && k->end > l->end) {            /* split in two */
            if (!lk_new(ip, k->pid, k->type, l->end + 1, k->end))
                return -ENOLCK;
            k->end = l->start - 1;
            pp = &k->next;
        } else if (k->start < l->start) {
            k->end = l->start - 1;
            pp = &k->next;
        } else if (k->end > l->end) {
            k->start = l->end + 1;
            pp = &k->next;
        } else {
            *pp = k->next;
            kfree(k);
        }
    }
    if (l->type != F_UNLCK_K && !lk_new(ip, l->pid, l->type, l->start, l->end))
        return -ENOLCK;
    wakeup(&lock_chan);
    return 0;
}

void flock_release(struct inode *ip, int pid)
{
    bool any = false;
    for (struct lk **pp = &locks; *pp;) {
        struct lk *k = *pp;
        if (same_inode(k->ip, ip) && k->pid == pid) {
            *pp = k->next;
            kfree(k);
            any = true;
        } else {
            pp = &k->next;
        }
    }
    if (any)
        wakeup(&lock_chan);
}
