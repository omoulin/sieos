/*
 * kmutex.h - A sleeping, recursive kernel mutex.
 *
 * For code that may sleep while holding it (disk I/O): under the big kernel
 * lock, a caller that finds it taken by another LWP sleeps until it is
 * released, so the rest of the kernel keeps running meanwhile.  The owner
 * may take it again (ext4 re-enters itself through iput, for example).
 * Outside process context (at boot) nobody can contend, and it is a no-op
 * besides the bookkeeping.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_KMUTEX_H
#define SIEOS_KMUTEX_H

#include "proc.h"

struct kmutex {
    struct lwp *owner;
    int depth;
    int waiters;
};

static inline void kmutex_lock(struct kmutex *m)
{
    struct lwp *me = curlwp;
    while (m->depth && m->owner != me) {
        m->waiters++;
        sleep_on(m);
        m->waiters--;
    }
    m->owner = me;
    m->depth++;
}

static inline void kmutex_unlock(struct kmutex *m)
{
    if (--m->depth == 0) {
        m->owner = NULL;
        if (m->waiters)
            wakeup(m);
    }
}

static inline bool kmutex_held(const struct kmutex *m)
{
    return m->depth && m->owner == curlwp;
}

#endif
