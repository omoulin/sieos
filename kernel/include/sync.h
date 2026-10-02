/*
 * sync.h - Kernel synchronization, as Solaris has it (mutex(9F),
 * condvar(9F), rwlock(9F)).
 *
 * The kernel runs with interrupts disabled: kernel code is never
 * interrupted (interrupts are taken in user mode and in the idle loop), and
 * an LWP leaves its processor only when it blocks or on its way back to
 * user mode.  What one LWP does between two blocking points is therefore
 * atomic on its processor, not across processors: shared data is protected
 * by these locks.
 *
 *   kmutex_t   mutex_enter / mutex_exit / mutex_tryenter / mutex_owned.
 *              Adaptive (the default): a caller that finds it held spins
 *              while the owner runs on another processor, and blocks when
 *              the owner is blocked itself.  It may be held across blocking
 *              (disk I/O, cv_wait on another condition).  Not recursive.
 *              MUTEX_SPIN: spins only; for data that interrupt handlers
 *              (the clock, the trap path) touch; never held across
 *              blocking.  A zeroed kmutex_t is an unlocked adaptive mutex.
 *   kcondvar_t cv_wait / cv_wait_sig / cv_timedwait / cv_timedwait_sig /
 *              cv_signal / cv_broadcast: the mutex is released while the
 *              LWP sleeps and held again when it returns; a wake-up may be
 *              spurious (callers loop on their condition).  cv_wait and
 *              cv_timedwait are not interrupted by signals; the _sig forms
 *              return 0 when a signal (or the LWP's end) is pending.
 *   krwlock_t  rw_enter(RW_READER | RW_WRITER) / rw_exit / rw_tryenter /
 *              rw_downgrade / rw_tryupgrade / rw_lock_held: many readers or
 *              one writer; waiting writers hold new readers back.
 *   krmutex_t  rmutex_enter / rmutex_exit / rmutex_owned: an adaptive
 *              mutex its owner may enter again (a file system's lock: ext4
 *              re-enters itself through iput, and a page fault in a copy to
 *              user memory may enter it again).  SIEOS's own; Solaris has
 *              none.
 *
 * Blocking LWPs wait on sleep queues hashed by the address they wait on
 * (Solaris' sleepq and turnstile tables, without priority inheritance).
 * Lock order and what each lock protects: docs/locking.md.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_SYNC_H
#define SIEOS_SYNC_H

#include "kernel.h"

struct lwp;

/* ---- mutexes */

typedef struct kmutex {
    volatile uintptr_t m_owner;     /* the owning LWP; bit 0: LWPs wait (adaptive) */
    uint8_t m_spin;                 /* MUTEX_SPIN */
} kmutex_t;

enum { MUTEX_DEFAULT = 0, MUTEX_ADAPTIVE = 1, MUTEX_SPIN = 2, MUTEX_DRIVER = 4 };
#define MUTEX_SPIN_INITIALIZER { 0, 1 }

void mutex_init(kmutex_t *m, const char *name, int type, void *arg);
void mutex_destroy(kmutex_t *m);
void mutex_enter(kmutex_t *m);
int  mutex_tryenter(kmutex_t *m);
void mutex_exit(kmutex_t *m);
int  mutex_owned(const kmutex_t *m);

/* ---- condition variables */

typedef struct kcondvar {
    volatile uint32_t cv_waiters;   /* LWPs on its sleep queue (a hint for cv_signal) */
} kcondvar_t;

void cv_init(kcondvar_t *cv, const char *name, int type, void *arg);
void cv_destroy(kcondvar_t *cv);
void cv_wait(kcondvar_t *cv, kmutex_t *m);
int  cv_wait_sig(kcondvar_t *cv, kmutex_t *m);                  /* 0: a signal is pending */
/* Until the tick `deadline` (absolute, ticks): -1 if it passed. */
long cv_timedwait(kcondvar_t *cv, kmutex_t *m, uint64_t deadline);
/* As cv_timedwait, interruptible: 0 on a signal, -1 on the deadline, > 0 when woken. */
long cv_timedwait_sig(kcondvar_t *cv, kmutex_t *m, uint64_t deadline);
/* As cv_timedwait_sig with an hrtime deadline in nanoseconds (0: none). */
long cv_timedwait_sig_hires(kcondvar_t *cv, kmutex_t *m, uint64_t deadline_ns);
void cv_signal(kcondvar_t *cv);
void cv_broadcast(kcondvar_t *cv);

/* ---- reader/writer locks */

typedef struct krwlock {
    kmutex_t rw_mx;                 /* (a spin mutex: the lock word's) */
    int rw_readers;                 /* > 0 readers, -1 a writer */
    int rw_wwait;                   /* writers waiting */
    struct lwp *rw_owner;           /* the writer */
    kcondvar_t rw_cv;
} krwlock_t;

typedef enum { RW_WRITER, RW_READER } krw_t;
#define RW_READ_HELD  1
#define RW_WRITE_HELD 2
#define RW_LOCK_HELD  3

void rw_init(krwlock_t *rw, const char *name, int type, void *arg);
void rw_destroy(krwlock_t *rw);
void rw_enter(krwlock_t *rw, krw_t how);
int  rw_tryenter(krwlock_t *rw, krw_t how);
void rw_exit(krwlock_t *rw);
void rw_downgrade(krwlock_t *rw);
int  rw_tryupgrade(krwlock_t *rw);
int  rw_lock_held(krwlock_t *rw, int which);

/* ---- recursive mutexes (file systems) */

typedef struct krmutex {
    kmutex_t rm_mx;
    struct lwp *volatile rm_owner;
    int rm_depth;
} krmutex_t;

void rmutex_enter(krmutex_t *m);
int  rmutex_tryenter(krmutex_t *m);
void rmutex_exit(krmutex_t *m);
int  rmutex_owned(const krmutex_t *m);
int  rmutex_depth(const krmutex_t *m);              /* the owner's depth, 0 if not ours */
/* Release it whatever the depth (before blocking on something it must not be held across); restore with rmutex_reenter. */
int  rmutex_release_all(krmutex_t *m);
void rmutex_reenter(krmutex_t *m, int depth);

/* ---- sleep queues (the primitives above, the dispatcher, the legacy channels) */

/*
 * Block the calling LWP on chan.  interlock (if any) is released once the
 * LWP is on chan's queue, and not entered again.  sig: a pending signal
 * (or the end of the LWP) wakes it, and keeps it from sleeping at all.
 * Wakes on sleepq_wakeup(chan, ...), on its deadlines (curlwp->wake_tick,
 * curlwp->wake_ns, set by the caller), and on signals if sig.
 */
void sleepq_block(const void *chan, kmutex_t *interlock, bool sig);
/* Wake up to n LWPs blocked on chan (n < 0: all); how many were woken. */
int  sleepq_wakeup(const void *chan, int n);
/* Wake l from its queue, asleep or about to be (sig: only an interruptible sleep; due: checked under the queue's lock). */
bool sleepq_unsleep(struct lwp *l, bool sig, bool (*due)(struct lwp *));

#endif
