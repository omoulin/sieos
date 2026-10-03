/*
 * misc.c - Linux's calls the layer answers from SIEOS's interfaces:
 *
 *   prctl            PR_SET_NAME/PR_GET_NAME: the thread's name; PR_SET_PDEATHSIG:
 *                    a thread waits for the parent's exit (a pidfd) and sends
 *                    the signal; PR_SET/GET_DUMPABLE, PR_SET_NO_NEW_PRIVS,
 *                    PR_SET_TIMERSLACK, PR_SET_PTRACER, PR_SET_VMA: kept or
 *                    accepted; others EINVAL
 *   statfs, fstatfs  statvfs's f_basetype as Linux's f_type magic numbers
 *   getsockopt       SO_PEERCRED: getpeerucred(3C)
 *   setsockopt       SO_PASSCRED: accepted (no SCM_CREDENTIALS are sent)
 *   memfd_create     MFD_NOEXEC_SEAL, MFD_EXEC dropped (SIEOS has no exec seals)
 *   fcntl            F_ADD_SEALS/F_GET_SEALS: the seals recorded per file and
 *                    reported (nothing enforces them: Chromium only checks them)
 *   sendmmsg, recvmmsg  loops over sendmsg and recvmsg
 *   sched_getcpu     getcpuid(3C)
 *   syscall          SYS_futex's FUTEX_WAIT_BITSET/FUTEX_WAKE_BITSET with every
 *                    bit: FUTEX_WAIT (an absolute time made relative), FUTEX_WAKE
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "lx.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucred.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/processor.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include "sieos/stat.h"
#include "sieos/syscall.h"

#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#endif
#define LX_FUTEX_WAIT         0
#define LX_FUTEX_WAKE         1
#define LX_FUTEX_WAIT_BITSET  9
#define LX_FUTEX_WAKE_BITSET  10
#define LX_FUTEX_PRIVATE      128
#define LX_FUTEX_CLOCK_REALTIME 256
#define LX_MFD_NOEXEC_SEAL    0x0008U
#define LX_MFD_EXEC           0x0010U

/* ---------------- prctl ---------------- */

static int dumpable = 1, no_new_privs;
static int pdeathsig;
static pthread_t pdeath_thread;
static bool pdeath_running;
static pthread_mutex_t pdeath_lock = PTHREAD_MUTEX_INITIALIZER;

static void *pdeath_watch(void *arg)
{
    int fd = (int)(intptr_t)arg;
    struct pollfd p = { fd, POLLIN, 0 };
    while (poll(&p, 1, -1) < 0 && errno == EINTR)
        ;
    close(fd);
    int sig = __atomic_load_n(&pdeathsig, __ATOMIC_ACQUIRE);
    if (sig)
        kill(getpid(), sig);
    return 0;
}

static int set_pdeathsig(int sig)
{
    if (sig < 0 || sig >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    __atomic_store_n(&pdeathsig, sig, __ATOMIC_RELEASE);
    if (!sig)
        return 0;
    pthread_mutex_lock(&pdeath_lock);
    int r = 0;
    if (!pdeath_running) {
        pid_t pp = getppid();
        int fd = pp > 1 ? (int)syscall(SYS_pidfd_open, pp, 0) : -1;
        if (fd >= 0) {
            pthread_attr_t a;
            pthread_attr_init(&a);
            pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
            pthread_attr_setstacksize(&a, 64 * 1024);
            sigset_t all, old;
            sigfillset(&all);
            pthread_sigmask(SIG_BLOCK, &all, &old);
            r = pthread_create(&pdeath_thread, &a, pdeath_watch, (void *)(intptr_t)fd);
            pthread_sigmask(SIG_SETMASK, &old, 0);
            pthread_attr_destroy(&a);
            if (r) {
                close(fd);
                errno = r;
                r = -1;
            } else {
                pdeath_running = true;
            }
        } else if (pp == 1) {
            kill(getpid(), sig);                 /* the parent is gone already */
        }
    }
    pthread_mutex_unlock(&pdeath_lock);
    return r;
}

int prctl(int op, ...)
{
    va_list ap;
    va_start(ap, op);
    unsigned long a2 = va_arg(ap, unsigned long);
    va_end(ap);
    switch (op) {
    case PR_SET_NAME: {
        char name[16];
        strncpy(name, (const char *)a2, 15);
        name[15] = 0;
        int e = pthread_setname_np(pthread_self(), name);
        if (e) {
            errno = e;
            return -1;
        }
        return 0;
    }
    case PR_GET_NAME: {
        int e = pthread_getname_np(pthread_self(), (char *)a2, 16);
        if (e) {
            errno = e;
            return -1;
        }
        return 0;
    }
    case PR_SET_PDEATHSIG:
        return set_pdeathsig((int)a2);
    case PR_GET_PDEATHSIG:
        *(int *)a2 = __atomic_load_n(&pdeathsig, __ATOMIC_ACQUIRE);
        return 0;
    case PR_SET_DUMPABLE:
        if (a2 > 1) {
            errno = EINVAL;
            return -1;
        }
        dumpable = (int)a2;
        return 0;
    case PR_GET_DUMPABLE:
        return dumpable;
    case PR_SET_NO_NEW_PRIVS:
        if (a2 != 1) {
            errno = EINVAL;
            return -1;
        }
        no_new_privs = 1;
        return 0;
    case PR_GET_NO_NEW_PRIVS:
        return no_new_privs;
    case PR_SET_TIMERSLACK:
    case PR_SET_PTRACER:
    case PR_SET_VMA:
        return 0;
    }
    errno = EINVAL;
    return -1;
}

/* ---------------- statfs ---------------- */

static const struct { const char *name; unsigned long magic; } magics[] = {
    { "ext4", 0xEF53 }, { "ext3", 0xEF53 }, { "ext2", 0xEF53 }, { "tmpfs", 0x01021994 },
    { "proc", 0x9fa0 }, { "devpts", 0x1cd1 }, { "devfs", 0x1373 }, { "pcfs", 0x4d44 },
    { "hsfs", 0x9660 }, { "nfs", 0x6969 }, { "lofs", 0x794c7630 }, { "fifofs", 0x50495045 },
    { "sockfs", 0x534F434B },
};

static void to_linux(struct statfs *out, const struct sieos_statvfs *k)
{
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < sizeof magics / sizeof magics[0]; i++)
        if (!strncmp(k->f_basetype, magics[i].name, sizeof k->f_basetype))
            out->f_type = magics[i].magic;
    out->f_bsize = k->f_bsize;
    out->f_frsize = k->f_frsize ? k->f_frsize : k->f_bsize;
    out->f_blocks = k->f_blocks;
    out->f_bfree = k->f_bfree;
    out->f_bavail = k->f_bavail;
    out->f_files = k->f_files;
    out->f_ffree = k->f_ffree;
    memcpy(&out->f_fsid, &k->f_fsid, sizeof out->f_fsid < sizeof k->f_fsid ? sizeof out->f_fsid : sizeof k->f_fsid);
    out->f_namelen = k->f_namemax;
    out->f_flags = (k->f_flag & SIEOS_ST_RDONLY ? 1 : 0) | (k->f_flag & SIEOS_ST_NOSUID ? 2 : 0);
}

int statfs(const char *path, struct statfs *buf)
{
    struct sieos_statvfs k;
    memset(&k, 0, sizeof k);
    if (syscall(SIEOS_SYS_statvfs, path, &k) < 0)
        return -1;
    to_linux(buf, &k);
    return 0;
}

int fstatfs(int fd, struct statfs *buf)
{
    struct sieos_statvfs k;
    memset(&k, 0, sizeof k);
    if (syscall(SIEOS_SYS_fstatvfs, fd, &k) < 0)
        return -1;
    to_linux(buf, &k);
    return 0;
}

/* ---------------- sockets ---------------- */

int getsockopt(int fd, int level, int name, void *restrict val, socklen_t *restrict len)
{
    if (level == SOL_SOCKET && name == SO_PEERCRED) {
        if (!len || *len < sizeof(struct ucred)) {
            errno = EINVAL;
            return -1;
        }
        ucred_t *uc = NULL;
        if (getpeerucred(fd, &uc) < 0)
            return -1;
        struct ucred c = { ucred_getpid(uc), ucred_geteuid(uc), ucred_getegid(uc) };
        ucred_free(uc);
        memcpy(val, &c, sizeof c);
        *len = sizeof c;
        return 0;
    }
    return REAL(getsockopt)(fd, level, name, val, len);
}

int setsockopt(int fd, int level, int name, const void *val, socklen_t len)
{
    if (level == SOL_SOCKET && name == SO_PASSCRED) {
        int type;
        socklen_t tl = sizeof type;
        return REAL(getsockopt)(fd, SOL_SOCKET, SO_TYPE, &type, &tl);   /* (a socket: accepted) */
    }
    return REAL(setsockopt)(fd, level, name, val, len);
}

int sendmmsg(int fd, struct mmsghdr *msgs, unsigned int n, unsigned int flags)
{
    unsigned int i;
    for (i = 0; i < n; i++) {
        ssize_t r = sendmsg(fd, &msgs[i].msg_hdr, flags);
        if (r < 0)
            return i ? (int)i : -1;
        msgs[i].msg_len = (unsigned int)r;
    }
    return (int)i;
}

int recvmmsg(int fd, struct mmsghdr *msgs, unsigned int n, unsigned int flags, struct timespec *timeout)
{
    (void)timeout;                               /* (Linux checks it only between datagrams too) */
    bool waitforone = flags & MSG_WAITFORONE;
    flags &= ~MSG_WAITFORONE;
    unsigned int i;
    for (i = 0; i < n; i++) {
        ssize_t r = recvmsg(fd, &msgs[i].msg_hdr, flags);
        if (r < 0)
            return i ? (int)i : -1;
        msgs[i].msg_len = (unsigned int)r;
        if (waitforone)
            flags |= MSG_DONTWAIT;
    }
    return (int)i;
}

/* ---------------- shared memory seals ---------------- */

struct seal {
    dev_t dev;
    ino_t ino;
    int seals;
    struct seal *next;
};
static struct seal *seals;
static pthread_mutex_t seal_lock = PTHREAD_MUTEX_INITIALIZER;

static struct seal *seal_of(const struct stat *st, bool make);

int memfd_create(const char *name, unsigned flags)
{
    int fd = REAL(memfd_create)(name, flags & ~(LX_MFD_NOEXEC_SEAL | LX_MFD_EXEC));
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0) {
        pthread_mutex_lock(&seal_lock);          /* (a file of the same number that went: not its seals) */
        struct seal *s = seal_of(&st, false);
        if (s)
            s->seals = 0;
        pthread_mutex_unlock(&seal_lock);
    }
    return fd;
}

static struct seal *seal_of(const struct stat *st, bool make)
{
    struct seal *s;
    for (s = seals; s; s = s->next)
        if (s->dev == st->st_dev && s->ino == st->st_ino)
            return s;
    if (make && (s = calloc(1, sizeof *s))) {
        s->dev = st->st_dev;
        s->ino = st->st_ino;
        s->next = seals;
        seals = s;
    }
    return s;
}

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    unsigned long arg = va_arg(ap, unsigned long);
    va_end(ap);
    if (cmd == F_ADD_SEALS || cmd == F_GET_SEALS) {
        struct stat st;
        if (fstat(fd, &st) < 0)
            return -1;
        if (!S_ISREG(st.st_mode)) {
            errno = EINVAL;
            return -1;
        }
        pthread_mutex_lock(&seal_lock);
        struct seal *s = seal_of(&st, cmd == F_ADD_SEALS);
        int r = 0;
        if (cmd == F_GET_SEALS) {
            r = s ? s->seals : 0;
        } else if (!s) {
            errno = ENOMEM;
            r = -1;
        } else if (s->seals & F_SEAL_SEAL) {
            errno = EPERM;
            r = -1;
        } else {
            s->seals |= (int)arg;
        }
        pthread_mutex_unlock(&seal_lock);
        return r;
    }
    return REAL(fcntl)(fd, cmd, arg);
}

/* ---------------- processors ---------------- */

int sched_getcpu(void)
{
    return getcpuid();
}

/* ---------------- futexes ---------------- */

long syscall(long n, ...)
{
    va_list ap;
    va_start(ap, n);
    long a = va_arg(ap, long), b = va_arg(ap, long), c = va_arg(ap, long);
    long d = va_arg(ap, long), e = va_arg(ap, long), f = va_arg(ap, long);
    va_end(ap);
    if (n == SYS_futex) {
        int op = (int)b, cmd = op & ~(LX_FUTEX_PRIVATE | LX_FUTEX_CLOCK_REALTIME);
        if ((cmd == LX_FUTEX_WAIT_BITSET || cmd == LX_FUTEX_WAKE_BITSET) && (unsigned)f != 0xffffffffu) {
            errno = EINVAL;                      /* (only every bit: what Chromium's code uses) */
            return -1;
        }
        if (cmd == LX_FUTEX_WAKE_BITSET)
            return REAL(syscall)(n, a, LX_FUTEX_WAKE | (op & LX_FUTEX_PRIVATE), c, 0, 0, 0);
        if (cmd == LX_FUTEX_WAIT_BITSET) {
            const struct timespec *at = (const struct timespec *)d;
            struct timespec rel, *to = NULL;
            if (at) {
                struct timespec now;
                clock_gettime(op & LX_FUTEX_CLOCK_REALTIME ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
                long long ns = (at->tv_sec - now.tv_sec) * 1000000000LL + (at->tv_nsec - now.tv_nsec);
                if (ns <= 0) {
                    errno = ETIMEDOUT;
                    return -1;
                }
                rel.tv_sec = ns / 1000000000LL;
                rel.tv_nsec = ns % 1000000000LL;
                to = &rel;
            }
            return REAL(syscall)(n, a, LX_FUTEX_WAIT | (op & LX_FUTEX_PRIVATE), c, to, 0, 0);
        }
    }
    return REAL(syscall)(n, a, b, c, d, e, f);
}
