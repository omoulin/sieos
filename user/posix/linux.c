/*
 * linux.c - The Linux personality (docs/webkit-plan.md): each Linux system
 * call musl makes (syscall_arch.h sends them all here) done with SIEOS's
 * own services, so that software ported from Linux builds unchanged on its
 * Linux paths. What this file keeps for the program:
 *
 *   the fd table      fd -> what it is (the terminal, a file of the file
 *                     server with its offset, /dev/null..., a pipe or socket
 *                     end of unixd's, a socket of netd's, a memfd, an
 *                     eventfd: sock.c), its flags
 *   the current folder (paths are made absolute here: the file server
 *                     knows none)
 *   the main stack    (for pthread_getattr_np's probe, through mremap)
 *
 * A call it does not know answers -ENOSYS, as an old Linux would, and is
 * named once on the terminal ("posix: system call 123 not supported"), so
 * a port shows what it still lacks.
 *
 * SIEOS's headers come first: their names (EPERM, PROT_READ...) are the
 * same numbers as Linux's, which musl's headers then define again as macros.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#define _GNU_SOURCE
#include "mk/abi.h"
#include "mk/proto.h"
#include "siefs.h"
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <time.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>

#define FS_PATH 512                       /* longest path (as user/lib/mk.h) */

/* ---- SIEOS's system calls (as user/lib/mk.h makes them) */
#if defined(__x86_64__)
static inline long sc(long n, long a, long b, long c, long d, long e, long f)
{
    register long r10 __asm__("r10") = d, r8 __asm__("r8") = e, r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "+a"(n) : "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return n;
}
#else
static inline long sc(long n, long a, long b, long c, long d, long e, long f)
{
    register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c,
                  x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}
#endif
#define S0(n) sc(n, 0, 0, 0, 0, 0, 0)
#define S1(n, a) sc(n, (long)(a), 0, 0, 0, 0, 0)
#define S2(n, a, b) sc(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define S3(n, a, b, c) sc(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define S4(n, a, b, c, d) sc(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)

/* A call to a named server; if it ended (-EPIPE) or our port number is
 * stale (-ENOENT), find it again and send once more. */
static long call(long *port, const char *name, msg_t *m)
{
    msg_t copy = *m;
    for (int tries = 0; ; tries++) {
        if (*port <= 0 && (*port = S2(SYS_PORT_LOOKUP, name, 0)) < 0) return *port;
        long e = S2(SYS_CALL, *port, m);
        if ((e == -EPIPE || e == -ENOENT) && tries < 2) { *port = 0; *m = copy; continue; }
        return e < 0 ? e : (long)m->w[0];
    }
}

/* A lock for this file's tables (threads may call at once): short holds. */
static volatile char big;
static void lock(void)   { while (__atomic_test_and_set(&big, __ATOMIC_ACQUIRE)) S0(SYS_YIELD); }
static void unlock(void) { __atomic_clear(&big, __ATOMIC_RELEASE); }

/* ---- The terminal: this process's (inherited), as user/lib/lib.c finds it. */
static long con_port;
static char con_name[16];
static long con_chan;
static long con(msg_t *m)
{
    if (!con_name[0]) {
        mk_ident_t id;
        const char *c = S2(SYS_IDENT, 0, &id) || !id.console[0] ? "console" : id.console;
        size_t i = 0;
        for (; c[i] && c[i] != '#' && i < sizeof con_name - 1; i++) con_name[i] = c[i];
        con_name[i] = 0;
        if (c[i] == '#') for (const char *p = c + i + 1; *p >= '0' && *p <= '9'; p++) con_chan = con_chan * 10 + *p - '0';
    }
    m->w[3] = con_chan;
    return call(&con_port, con_name, m);
}

/* ---- The file server */
static long fs_port;
static long fs(msg_t *m) { return call(&fs_port, "fs", m); }

/* ---- Paths: absolute, "." and ".." resolved as text (as user/lib/fs.c). */
static char cwd[FS_PATH] = "/";
static int absolute(const char *path, char *out)
{
    char tmp[FS_PATH * 2];
    size_t n = 0;
    if (!path) return -EFAULT;
    if (path[0] != '/') { n = strlen(cwd); memcpy(tmp, cwd, n); tmp[n++] = '/'; }
    size_t l = strlen(path);
    if (n + l >= FS_PATH) return -ENAMETOOLONG;
    memcpy(tmp + n, path, l + 1);
    size_t o = 0;
    for (char *p = tmp; *p; ) {
        while (*p == '/') p++;
        char *e = p;
        while (*e && *e != '/') e++;
        size_t len = (size_t)(e - p);
        if (len == 1 && p[0] == '.') ;
        else if (len == 2 && p[0] == '.' && p[1] == '.') { while (o && out[--o] != '/') ; }
        else if (len) { out[o++] = '/'; memcpy(out + o, p, len); o += len; }
        p = e;
    }
    if (!o) out[o++] = '/';
    out[o] = 0;
    return 0;
}
void __sieos_startdir(const char *dir) { char p[FS_PATH]; if (!absolute(dir, p)) memcpy(cwd, p, strlen(p) + 1); }

/* ---- The fd table */
enum { F_FREE, F_CON, F_FILE, F_DIR, F_NULL, F_ZERO, F_RANDOM, F_UNIX, F_SHM, F_EVENT, F_SOCK };
typedef struct {
    int kind, flags, cloexec;
    long sh;                                      /* F_FILE, F_DIR: the file server's handle */
    uint64_t off;                                 /* the offset (F_DIR: the cursor; F_SHM: the size) */
    char *path;
    uint64_t id;                                  /* F_UNIX: the end; F_SOCK: netd's handle; F_SHM: the object */
    int type;                                     /* F_UNIX: UNIX_*; F_SOCK: SOCK_*; F_SHM: made */
    void *obj;                                    /* F_EVENT: the counter; F_SOCK: a connect going on */
    uint32_t ip;                                  /* F_SOCK: the peer, our port */
    uint16_t port, lport;
} fd_t;
#define NFD 1024
static fd_t *fds;                                 /* NFD of them, mapped at start (pages given when touched):
                                                     0, 1, 2: the terminal (__sieos_main_stack) */
static fd_t *fd_get(long fd) { return fd >= 0 && fd < NFD && fds[fd].kind != F_FREE ? &fds[fd] : 0; }
static long fd_new(int from, fd_t f)              /* the lowest free fd >= from */
{
    lock();
    for (int i = from; i < NFD; i++)
        if (fds[i].kind == F_FREE) { fds[i] = f; unlock(); return i; }
    unlock();
    return -EMFILE;
}

void *malloc(size_t);
void free(void *);
static char *strdup_(const char *s) { size_t n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); return p; }

/* A path relative to a directory fd (or the current folder). */
static int at_path(long dirfd, const char *path, char *out)
{
    if (!path) return -EFAULT;
    if (path[0] == '/' || dirfd == AT_FDCWD) return absolute(path, out);
    fd_t *d = fd_get(dirfd);
    if (!d || d->kind != F_DIR) return -EBADF;
    char tmp[FS_PATH * 2];
    size_t n = strlen(d->path), l = strlen(path);
    if (n + 1 + l >= sizeof tmp) return -ENAMETOOLONG;
    memcpy(tmp, d->path, n); tmp[n] = '/'; memcpy(tmp + n + 1, path, l + 1);
    return absolute(tmp, out);
}

static long fs_path(int op, const char *p, long w1, long w2, void *rbuf, size_t rlen)
{
    msg_t m = { .w = { op, w1, w2 }, .sbuf = p, .slen = strlen(p) + 1, .rbuf = rbuf, .rlen = rlen };
    return fs(&m);
}

static void release(fd_t *f);
static long do_close(long fd)
{
    lock();
    fd_t *f = fd_get(fd), c;
    if (f) { c = *f; f->kind = F_FREE; }
    unlock();
    if (!f) return -EBADF;
    if (c.kind == F_FILE || c.kind == F_DIR) {
        msg_t m = { .w = { FS_CLOSE, c.sh } };
        fs(&m);
        free(c.path);
    } else release(&c);
    return 0;
}

static long do_open(long dirfd, const char *path, long flags, long mode)
{
    char p[FS_PATH];
    long e = at_path(dirfd, path, p);
    if (e) return e;
    fd_t f = { F_FILE, (int)(flags & (O_ACCMODE | O_APPEND | O_NONBLOCK)), !!(flags & O_CLOEXEC) };
    if (!strcmp(p, "/dev/null")) f.kind = F_NULL;
    else if (!strcmp(p, "/dev/zero")) f.kind = F_ZERO;
    else if (!strcmp(p, "/dev/urandom") || !strcmp(p, "/dev/random")) f.kind = F_RANDOM;
    else if (!strcmp(p, "/dev/tty") || !strcmp(p, "/dev/console")) f.kind = F_CON;
    else {
        siefs_stat_t st;
        long r = fs_path(FS_STAT, p, 0, 0, &st, sizeof st);
        int dir = r == 0 && (st.mode >> 12) == 4;
        if ((flags & O_DIRECTORY) && r == 0 && !dir) return -ENOTDIR;
        int fl = (flags & O_ACCMODE) | (flags & O_CREAT ? FS_CREAT : 0) | (flags & O_EXCL ? FS_EXCL : 0)
               | (flags & O_TRUNC ? FS_TRUNC : 0) | (flags & O_APPEND ? FS_APPEND : 0) | (dir ? FS_DIRECTORY : 0);
        if ((f.sh = fs_path(FS_OPEN, p, fl, mode & 07777, 0, 0)) < 0) return f.sh;
        if (dir) f.kind = F_DIR;
        if (!(f.path = strdup_(p))) { msg_t m = { .w = { FS_CLOSE, f.sh } }; fs(&m); return -ENOMEM; }
    }
    long fd = fd_new(0, f);
    if (fd < 0 && f.path) { msg_t m = { .w = { FS_CLOSE, f.sh } }; fs(&m); free(f.path); }
    return fd;
}

static long file_io(fd_t *f, int write, void *buf, size_t n, uint64_t off)
{
    size_t done = 0;
    while (done < n) {
        size_t c = n - done < FS_MAX ? n - done : FS_MAX;
        msg_t m = { .w = { write ? FS_WRITE : FS_READ, f->sh, off + done, c } };
        if (write) { m.sbuf = (char *)buf + done; m.slen = c; } else { m.rbuf = (char *)buf + done; m.rlen = c; }
        long r = fs(&m);
        if (r < 0) return done ? (long)done : r;
        done += r;
        if (!write && (size_t)r < c) break;
    }
    return done;
}

/* (sock.c, included below) */
static long unix_send(fd_t *f, const void *data, size_t n, const unix_desc_t *d, int nd);
static long unix_recv(fd_t *f, void *data, size_t n, int flags, int *fdv, int maxfd, int *nd, int cloexec);
static long sock_send(fd_t *f, const void *buf, size_t n, int flags, uint32_t ip, uint16_t port);
static long sock_recv(fd_t *f, void *buf, size_t n, int flags, uint32_t *ip, uint16_t *port);
static long event_rw(fd_t *f, int write, void *buf, size_t n);

static long do_rw(long fd, int write, void *buf, size_t n, int64_t pos)   /* pos < 0: at the fd's offset */
{
    fd_t *f = fd_get(fd);
    if (!f) return -EBADF;
    switch (f->kind) {
    case F_CON: {
        if (write) { msg_t m = { .w = { CON_WRITE }, .sbuf = buf, .slen = n }; return con(&m); }
        msg_t m = { .w = { CON_READ, n }, .rbuf = buf, .rlen = n };
        return con(&m);
    }
    case F_NULL: return write ? (long)n : 0;
    case F_ZERO: if (!write) memset(buf, 0, n); return n;
    case F_RANDOM:
        if (write) return n;
        for (size_t d = 0; d < n; ) { long r = S2(SYS_RANDOM, (char *)buf + d, n - d > 4096 ? 4096 : n - d); if (r < 0) return r; d += r; }
        return n;
    case F_DIR: return -EISDIR;
    case F_UNIX:
        if (write) return unix_send(f, buf, n, 0, 0);
        { int nd; return unix_recv(f, buf, n, 0, 0, 0, &nd, 0); }
    case F_SOCK: return write ? sock_send(f, buf, n, 0, 0, 0) : sock_recv(f, buf, n, 0, 0, 0);
    case F_EVENT: return event_rw(f, write, buf, n);
    case F_SHM: return -EINVAL;
    }
    if (write ? (f->flags & O_ACCMODE) == O_RDONLY : (f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    long r = file_io(f, write, buf, n, pos >= 0 ? (uint64_t)pos : f->off);
    if (r > 0 && pos < 0) {
        if (write && (f->flags & O_APPEND)) {            /* (the server wrote at the end: where is it now?) */
            siefs_stat_t st;
            msg_t m = { .w = { FS_FSTAT, f->sh }, .rbuf = &st, .rlen = sizeof st };
            f->off = fs(&m) == 0 ? st.size : f->off + r;
        } else f->off += r;
    }
    return r;
}

static long do_rwv(long fd, int write, const struct iovec *iov, long n, int64_t pos)
{
    long total = 0;
    for (long i = 0; i < n; i++) {
        if (!iov[i].iov_len) continue;
        long r = do_rw(fd, write, iov[i].iov_base, iov[i].iov_len, pos < 0 ? -1 : pos + total);
        if (r < 0) return total ? total : r;
        total += r;
        if ((size_t)r < iov[i].iov_len) break;
    }
    return total;
}

/* struct stat as each processor's Linux returns it (musl's kstat.h) */
#if defined(__x86_64__)
struct kstat { dev_t dev; ino_t ino; nlink_t nlink; mode_t mode; uid_t uid; gid_t gid; unsigned pad0; dev_t rdev;
               off_t size; blksize_t blksize; blkcnt_t blocks; long at, atn, mt, mtn, ct, ctn, unused[3]; };
#else
struct kstat { dev_t dev; ino_t ino; mode_t mode; nlink_t nlink; uid_t uid; gid_t gid; dev_t rdev; unsigned long pad;
               off_t size; blksize_t blksize; int pad2; blkcnt_t blocks; long at, atn, mt, mtn, ct, ctn; unsigned unused[2]; };
#endif
static void to_kstat(const siefs_stat_t *s, struct kstat *k)
{
    memset(k, 0, sizeof *k);
    k->dev = 1; k->ino = s->ino; k->nlink = s->nlink; k->mode = s->mode; k->uid = s->uid; k->gid = s->gid;
    k->size = s->size; k->blksize = 4096; k->blocks = (s->size + 511) / 512;
    k->at = s->atime / 1000000000; k->atn = s->atime % 1000000000;
    k->mt = s->mtime / 1000000000; k->mtn = s->mtime % 1000000000;
    k->ct = s->ctime / 1000000000; k->ctn = s->ctime % 1000000000;
}
static long do_fstat(long fd, struct kstat *k)
{
    fd_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->kind != F_FILE && f->kind != F_DIR) {
        memset(k, 0, sizeof *k);
        k->mode = 020666; k->nlink = 1; k->blksize = 4096; k->rdev = f->kind;   /* a character device */
        if (f->kind == F_SHM) { k->mode = 0100600; k->size = f->off; }
        else if (f->kind == F_SOCK || (f->kind == F_UNIX && f->type != UNIX_PIPE)) k->mode = 0140777;
        else if (f->kind == F_UNIX) k->mode = 010600;
        k->ino = f->id;
        return 0;
    }
    siefs_stat_t st;
    msg_t m = { .w = { FS_FSTAT, f->sh }, .rbuf = &st, .rlen = sizeof st };
    long r = fs(&m);
    if (!r) to_kstat(&st, k);
    return r;
}
static long do_stat(long dirfd, const char *path, struct kstat *k, long flags)
{
    if ((flags & AT_EMPTY_PATH) && path && !*path) return do_fstat(dirfd, k);
    char p[FS_PATH];
    long e = at_path(dirfd, path, p);
    if (e) return e;
    if (!strncmp(p, "/dev/", 5)) {
        memset(k, 0, sizeof *k);
        k->mode = 020666; k->nlink = 1;
        return !strcmp(p, "/dev/null") || !strcmp(p, "/dev/zero") || !strcmp(p, "/dev/urandom") ||
               !strcmp(p, "/dev/random") || !strcmp(p, "/dev/tty") ? 0 : -ENOENT;
    }
    siefs_stat_t st;
    long r = fs_path(FS_STAT, p, !!(flags & AT_SYMLINK_NOFOLLOW), 0, &st, sizeof st);
    if (!r) to_kstat(&st, k);
    return r;
}

/* getdents64: the file server's records turned into Linux's. The fd's
 * offset holds the server's cursor. */
struct dent64 { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; };
static long do_getdents(long fd, char *buf, size_t size)
{
    fd_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->kind != F_DIR) return -ENOTDIR;
    char tmp[4096];
    size_t ask = size / 2 < sizeof tmp ? size / 2 : sizeof tmp;   /* (Linux records are longer) */
    msg_t m = { .w = { FS_READDIR, f->sh, f->off, ask }, .rbuf = tmp, .rlen = ask };
    long r = fs(&m);
    if (r <= 0) return r;
    size_t o = 0;
    for (const char *p = tmp, *end = tmp + r; p + 10 <= end; ) {
        int n = (unsigned char)p[9];
        size_t rl = (offsetof(struct dent64, name) + n + 1 + 7) & ~7UL;
        if (o + rl > size) return -EINVAL;
        struct dent64 *d = (struct dent64 *)(buf + o);
        memcpy(&d->ino, p, 8);
        d->type = (unsigned char)p[8];
        d->reclen = (unsigned short)rl;
        memcpy(d->name, p + 10, n);
        d->name[n] = 0;
        o += rl;
        p += 10 + n;
        d->off = (int64_t)o;
    }
    f->off = m.w[1];
    return o;
}

static long do_lseek(long fd, int64_t off, int whence)
{
    fd_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->kind == F_CON) return -ESPIPE;
    if (f->kind != F_FILE) return 0;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->off : -1;
    if (whence == SEEK_END) {
        struct kstat k;
        long r = do_fstat(fd, &k);
        if (r) return r;
        base = k.size;
    }
    if (base < 0 || base + off < 0) return -EINVAL;
    return f->off = base + off;
}

static long do_fcntl(long fd, int cmd, long arg)
{
    fd_t *f = fd_get(fd);
    if (!f) return -EBADF;
    switch (cmd) {
    case F_GETFD: return f->cloexec ? FD_CLOEXEC : 0;
    case F_SETFD: f->cloexec = !!(arg & FD_CLOEXEC); return 0;
    case F_GETFL: return f->flags;
    case F_SETFL: f->flags = (f->flags & O_ACCMODE) | (arg & (O_APPEND | O_NONBLOCK)); return 0;
    case F_DUPFD: case F_DUPFD_CLOEXEC: {
        fd_t c = *f;
        c.cloexec = cmd == F_DUPFD_CLOEXEC;
        if (c.path) {                                  /* the copy has its own handle */
            long sh = fs_path(FS_OPEN, c.path, (c.flags & O_ACCMODE) | (c.kind == F_DIR ? FS_DIRECTORY : 0), 0, 0, 0);
            if (sh < 0) return sh;
            c.sh = sh;
            c.path = strdup_(c.path);
        }
        return fd_new(arg, c);
    }
    case F_GETLK: case F_SETLK: case F_SETLKW: case F_OFD_GETLK: case F_OFD_SETLK: case F_OFD_SETLKW:
        return 0;                                      /* (one machine, no locks between programs yet) */
    }
    return -EINVAL;
}

static long do_dup3(long old, long new, long flags)
{
    if (old == new) return fd_get(old) ? -EINVAL : -EBADF;
    if (!fd_get(old) || new < 0 || new >= NFD) return -EBADF;
    if (fds[new].kind != F_FREE) do_close(new);
    long r = do_fcntl(old, flags & O_CLOEXEC ? F_DUPFD_CLOEXEC : F_DUPFD, new);
    return r == new || r < 0 ? r : (do_close(r), -EBUSY);
}

static volatile uint32_t note_word;                /* SYS_NOTE adds to it (poll sleeps on it: sock.c) */

/* ---- Memory */
static uintptr_t stack_lo, stack_hi;                  /* the main thread's stack */
void *__sieos_main_stack(size_t size)
{
    long a = S3(SYS_MMAP, 0, size, PROT_READ | PROT_WRITE);
    if (a < 0) S1(SYS_EXIT, a);
    stack_lo = a; stack_hi = a + size;
    fds = (fd_t *)S3(SYS_MMAP, 0, NFD * sizeof(fd_t), PROT_READ | PROT_WRITE);
    if ((long)fds < 0) S1(SYS_EXIT, (long)fds);
    fds[0] = (fd_t){ F_CON }; fds[1] = fds[2] = (fd_t){ F_CON, O_WRONLY };
    S1(SYS_NOTE_WORD, &note_word);                     /* (poll sleeps on it: sock.c) */
    return (void *)a;
}
#define L_MAP_SHARED 1
#define L_MAP_FIXED 0x10
#define L_MAP_ANON 0x20
static long do_mmap(long addr, size_t len, long prot, long flags, long fd, long off)
{
    if (!len) return -EINVAL;
    long rw = prot & 7;
    fd_t *sf = fd >= 0 && !(flags & L_MAP_ANON) ? fd_get(fd) : 0;
    if (sf && sf->kind == F_SHM) {                    /* a memfd: its object, shared */
        if (!sf->type || off % 4096 || (uint64_t)off + len > ((sf->off + 4095) & ~4095UL)) return -EINVAL;
        long a = S3(SYS_SHM_MAP, sf->id, (flags & L_MAP_FIXED) ? addr - off : 0, rw | (flags & L_MAP_FIXED ? MAP_FIXED : 0));
        if (a < 0) return a;
        uint64_t whole = (sf->off + 4095) & ~4095UL, l = (len + 4095) & ~4095UL;
        if (off) S3(SYS_MUNMAP, a, off, 0);           /* (only the part asked for stays) */
        if (off + l < whole) S3(SYS_MUNMAP, a + off + l, whole - off - l, 0);
        return a + off;
    }
    if (fd >= 0 && !(flags & L_MAP_ANON)) {           /* a file: a private copy of it */
        fd_t *f = fd_get(fd);
        if (!f || f->kind != F_FILE) return f ? -ENODEV : -EBADF;
        if ((flags & L_MAP_SHARED) && (prot & PROT_WRITE)) return -ENODEV;   /* (shared memory: a later phase) */
        long a = S3(SYS_MMAP, addr, len, PROT_READ | PROT_WRITE | (flags & L_MAP_FIXED ? MAP_FIXED : 0));
        if (a < 0) return a;
        long r = file_io(f, 0, (void *)a, len, off);
        if (r < 0) { S3(SYS_MUNMAP, a, len, 0); return r; }
        if (rw != (PROT_READ | PROT_WRITE)) S3(SYS_MPROTECT, a, len, rw);
        return a;
    }
    return S3(SYS_MMAP, addr, len, rw | (flags & L_MAP_FIXED ? MAP_FIXED : 0));
}
static long do_mremap(uintptr_t old, size_t size, size_t nsize, long flags)
{
    (void)nsize; (void)flags;
    if (old >= stack_lo && old + size <= stack_hi) return -ENOMEM;   /* (musl's main-stack probe: mapped) */
    return old >= stack_hi || old + size <= stack_lo ? -EFAULT : -ENOMEM;
}

/* ---- Threads */
struct clone_start { int (*fn)(void *); void *arg, *tls; int *ptid, *ctid; int flags; };
static void clone_entry(struct clone_start *s)    /* the new thread, before its TLS exists */
{
    if (s->flags & CLONE_SETTLS) S1(SYS_SET_FS, s->tls);
    long tid = S1(SYS_THREAD_SELF, s->flags & CLONE_CHILD_CLEARTID ? s->ctid : 0);
    if (s->flags & CLONE_PARENT_SETTID) *s->ptid = (int)tid;
    s->fn(s->arg);                                     /* (musl's thread function ends the thread itself) */
    S0(SYS_THREAD_EXIT);
}
long __sieos_clone(int (*fn)(void *), void *stack, int flags, void *arg, int *ptid, void *tls, int *ctid)
{
    if ((flags & (CLONE_VM | CLONE_THREAD)) != (CLONE_VM | CLONE_THREAD)) return -ENOSYS;   /* no fork */
    struct clone_start *s = (struct clone_start *)(((uintptr_t)stack - sizeof *s) & ~15UL);
    *s = (struct clone_start){ fn, arg, tls, ptid, ctid, flags };
    long tid = S3(SYS_THREAD_CREATE, clone_entry, s, s);
    if (tid > 0 && (flags & CLONE_PARENT_SETTID)) *ptid = (int)tid;
    return tid;
}
long __sieos_set_tp(void *p) { return S1(SYS_SET_FS, p); }

static uint64_t ts_ns(const struct timespec *t) { return (uint64_t)t->tv_sec * 1000000000 + t->tv_nsec; }
#ifndef EPOCH
#define EPOCH 0
#endif
static uint64_t now_ns(int clk)                    /* CLOCK_REALTIME: the kernel's wall clock (SYS_TIME) */
{
    return clk == CLOCK_REALTIME || clk == CLOCK_REALTIME_COARSE ? (uint64_t)S1(SYS_TIME, 0) : (uint64_t)S0(SYS_CLOCK);
}

static long do_futex(volatile int *addr, int op, int val, const struct timespec *t, int val3)
{
    int cmd = op & 127;                                /* (FUTEX_PRIVATE_FLAG: all are) */
    if (cmd == 1 || cmd == 10) return S4(SYS_FUTEX, FUTEX_WAKE, addr, val, 0);   /* WAKE, WAKE_BITSET */
    if (cmd == 3 || cmd == 4) {                        /* REQUEUE, CMP_REQUEUE: the waiters to move */
        if (cmd == 4 && *addr != val3) return -EAGAIN; /* are woken instead (a wake-up they may treat */
        return S4(SYS_FUTEX, FUTEX_WAKE, addr, (long)val + (long)(uintptr_t)t, 0);   /* as spurious; musl's condition variables do) */
    }
    if (cmd != 0 && cmd != 9) return -ENOSYS;          /* PI futexes */
    uint64_t ns = 0;
    if (t) {
        uint64_t w = ts_ns(t);
        if (cmd == 9) {                                /* WAIT_BITSET: an absolute time */
            uint64_t n = now_ns(op & 256 ? CLOCK_REALTIME : CLOCK_MONOTONIC);
            w = w > n ? w - n : 0;
        }
        if (!w) return -ETIMEDOUT;
        ns = w;
    }
    return S4(SYS_FUTEX, FUTEX_WAIT, addr, (uint32_t)val, ns);
}

static long do_sleep(int clk, int flags, const struct timespec *t, struct timespec *rem)
{
    uint64_t ns = ts_ns(t), start = now_ns(clk);
    if (flags & TIMER_ABSTIME) ns = ns > start ? ns - start : 0;
    if (!ns) return 0;
    for (uint64_t end = start + ns, n = start; n < end; n = now_ns(clk)) {
        long r = S1(SYS_SLEEP, end - n);
        if (r == -EINTR) {
            n = now_ns(clk);
            if (rem && !(flags & TIMER_ABSTIME)) { uint64_t l = end > n ? end - n : 0; rem->tv_sec = l / 1000000000; rem->tv_nsec = l % 1000000000; }
            return -EINTR;
        }
    }
    return 0;
}

/* ---- Identity */
static long ident(int which)
{
    mk_ident_t id;
    long r = S2(SYS_IDENT, 0, &id);
    return r ? r : which == 0 ? id.pid : which == 1 ? id.uid : id.gid;
}
static long self_pid(void) { static long me; if (!me) me = ident(0); return me; }
static long ppid(void)
{
    mk_task_t t;
    long pid = ident(0);
    return S3(SYS_TASKS, &t, 1, pid - 1) == 1 && t.pid == pid ? t.ppid : 1;
}

void __sieos_random(void *buf, size_t n)
{
    for (size_t d = 0; d < n; ) {
        long r = S2(SYS_RANDOM, (char *)buf + d, n - d > 4096 ? 4096 : n - d);
        if (r == -EAGAIN) { S1(SYS_SLEEP, 1000000); continue; }
        if (r < 0) S1(SYS_EXIT, r);
        d += r;
    }
}
unsigned long __sieos_hwcap(void) { mk_info_t in; return S1(SYS_INFO, &in) ? 0 : in.hwcap; }

#include "sock.c"
#include "spawn.c"

_Noreturn void __sieos_exit(int code) { S1(SYS_EXIT, code); for (;;) ; }
unsigned __sieos_dns(void)                       /* (musl's resolver, resolvconf.c) */
{
    net_info_t in;
    msg_t m = { .w = { NET_INFO }, .rbuf = &in, .rlen = sizeof in };
    return netd(&m) < 0 ? 0 : in.dns;
}

static void unsupported(long n)
{
    static uint8_t said[64];
    if (n < 0 || n >= 512 || (said[n / 8] & 1 << n % 8)) return;
    said[n / 8] |= 1 << n % 8;
    char m[64] = "posix: system call ", *p = m + 19, d[8];
    int k = 0;
    do d[k++] = '0' + n % 10; while (n /= 10);
    while (k) *p++ = d[--k];
    memcpy(p, " not supported\n", 16);
    msg_t c = { .w = { CON_WRITE }, .sbuf = m, .slen = strlen(m) };
    con(&c);
}

#define A(x) ((void *)(x))
long __sieos_syscall(long n, long a, long b, long c, long d, long e, long f)
{
    char p[FS_PATH], q[FS_PATH];
    long r;
    switch (n) {
    /* files */
    case SYS_read:       return do_rw(a, 0, A(b), c, -1);
    case SYS_write:      return do_rw(a, 1, A(b), c, -1);
    case SYS_pread64:    return do_rw(a, 0, A(b), c, d);
    case SYS_pwrite64:   return do_rw(a, 1, A(b), c, d);
    case SYS_readv:      return do_rwv(a, 0, A(b), c, -1);
    case SYS_writev:     return do_rwv(a, 1, A(b), c, -1);
    case SYS_preadv:     return do_rwv(a, 0, A(b), c, d);
    case SYS_pwritev:    return do_rwv(a, 1, A(b), c, d);
    case SYS_preadv2:    return d < -1 ? -EINVAL : do_rwv(a, 0, A(b), c, d);   /* (offset -1: the fd's own; flags ignored) */
    case SYS_pwritev2:   return d < -1 ? -EINVAL : do_rwv(a, 1, A(b), c, d);
#ifdef SYS_open
    case SYS_open:       return do_open(AT_FDCWD, A(a), b, c);
#endif
    case SYS_openat:     return do_open(a, A(b), c, d);
    case SYS_close:      return do_close(a);
    case SYS_lseek:      return do_lseek(a, b, c);
    case SYS_fstat:      return do_fstat(a, A(b));
#ifdef SYS_stat
    case SYS_stat:       return do_stat(AT_FDCWD, A(a), A(b), 0);
    case SYS_lstat:      return do_stat(AT_FDCWD, A(a), A(b), AT_SYMLINK_NOFOLLOW);
#endif
    case SYS_newfstatat: return do_stat(a, A(b), A(c), d);
    case SYS_getdents64: return do_getdents(a, A(b), c);
    case SYS_fcntl:      return do_fcntl(a, b, c);
    case SYS_dup:        return do_fcntl(a, F_DUPFD, 0);
#ifdef SYS_dup2
    case SYS_dup2:       return a == b ? (fd_get(a) ? b : -EBADF) : do_dup3(a, b, 0);
#endif
    case SYS_dup3:       return do_dup3(a, b, c);
    case SYS_ioctl: {
        fd_t *fl = fd_get(a);
        if (!fl) return -EBADF;
        if (fl->kind != F_CON) return -ENOTTY;
        if (b == 0x5413) { unsigned short *w = A(c); w[0] = 24; w[1] = 80; w[2] = w[3] = 0; return 0; }   /* TIOCGWINSZ */
        if (b == 0x5401) { memset(A(c), 0, 60); return 0; }   /* TCGETS: a terminal */
        if (b == 0x5402 || b == 0x5403 || b == 0x5404) return 0;   /* TCSETS* */
        return -ENOTTY;
    }
    case SYS_ftruncate: {
        fd_t *fl = fd_get(a);
        if (fl && fl->kind == F_SHM) return shm_size(fl, b);
        if (!fl || fl->kind != F_FILE) return fl ? -EINVAL : -EBADF;
        msg_t m = { .w = { FS_TRUNCATE, fl->sh, b } };
        return fs(&m);
    }
    case SYS_truncate:
        if ((r = absolute(A(a), p))) return r;
        if ((r = fs_path(FS_OPEN, p, FS_WRONLY, 0, 0, 0)) < 0) return r;
        { msg_t m = { .w = { FS_TRUNCATE, r, b } }; long t = fs(&m); msg_t cl = { .w = { FS_CLOSE, r } }; fs(&cl); return t; }
    case SYS_fsync: case SYS_fdatasync: case SYS_sync:
        { msg_t m = { .w = { FS_SYNC } }; return fs(&m) < 0 ? -EIO : 0; }
#ifdef SYS_access
    case SYS_access:     d = 0; c = b; b = a; a = AT_FDCWD; /* fall through */
#endif
    case SYS_faccessat: case SYS_faccessat2: {
        struct kstat k;
        return do_stat(a, A(b), &k, 0);
    }
#ifdef SYS_mkdir
    case SYS_mkdir:      c = b; b = a; a = AT_FDCWD; /* fall through */
#endif
    case SYS_mkdirat:    return (r = at_path(a, A(b), p)) ? r : fs_path(FS_MKDIR, p, c & 07777, 0, 0, 0);
#ifdef SYS_unlink
    case SYS_unlink:     return (r = absolute(A(a), p)) ? r : fs_path(FS_UNLINK, p, 0, 0, 0, 0);
    case SYS_rmdir:      return (r = absolute(A(a), p)) ? r : fs_path(FS_RMDIR, p, 0, 0, 0, 0);
#endif
    case SYS_unlinkat:   return (r = at_path(a, A(b), p)) ? r : fs_path(c & AT_REMOVEDIR ? FS_RMDIR : FS_UNLINK, p, 0, 0, 0, 0);
#ifdef SYS_rename
    case SYS_rename:     d = b; c = AT_FDCWD; b = a; a = AT_FDCWD; /* fall through */
#endif
    case SYS_renameat: case SYS_renameat2: {
        if ((r = at_path(a, A(b), p)) || (r = at_path(c, A(d), q))) return r;
        size_t l = strlen(p) + 1;
        char two[FS_PATH * 2];
        memcpy(two, p, l); memcpy(two + l, q, strlen(q) + 1);
        msg_t m = { .w = { FS_RENAME }, .sbuf = two, .slen = l + strlen(q) + 1 };
        return fs(&m);
    }
#ifdef SYS_symlink
    case SYS_symlink:    c = b; b = AT_FDCWD; /* fall through */
#endif
    case SYS_symlinkat: {
        if ((r = at_path(b, A(c), q))) return r;
        size_t l = strlen(A(a)) + 1;
        char two[FS_PATH * 2];
        if (l > FS_PATH) return -ENAMETOOLONG;
        memcpy(two, A(a), l); memcpy(two + l, q, strlen(q) + 1);
        msg_t m = { .w = { FS_SYMLINK }, .sbuf = two, .slen = l + strlen(q) + 1 };
        return fs(&m);
    }
#ifdef SYS_readlink
    case SYS_readlink:   d = c; c = b; b = a; a = AT_FDCWD; /* fall through */
#endif
    case SYS_readlinkat:
        if ((r = at_path(a, A(b), p))) return r;
        if (!strcmp(p, "/proc/self/exe")) return -ENOENT;
        return fs_path(FS_READLINK, p, 0, 0, A(c), d);
#ifdef SYS_chmod
    case SYS_chmod:      c = b; b = a; a = AT_FDCWD; /* fall through */
#endif
    case SYS_fchmodat:   return (r = at_path(a, A(b), p)) ? r : fs_path(FS_CHMOD, p, c & 07777, 0, 0, 0);
    case SYS_fchmod: {
        fd_t *fl = fd_get(a);
        return !fl || !fl->path ? -EBADF : fs_path(FS_CHMOD, fl->path, b & 07777, 0, 0, 0);
    }
    case SYS_getcwd: {
        size_t l = strlen(cwd) + 1;
        if (l > (size_t)b) return -ERANGE;
        memcpy(A(a), cwd, l);
        return l;
    }
    case SYS_chdir: {
        struct kstat k;
        if ((r = absolute(A(a), p)) || (r = do_stat(AT_FDCWD, p, &k, 0))) return r;
        if ((k.mode & 0170000) != 0040000) return -ENOTDIR;
        memcpy(cwd, p, strlen(p) + 1);
        return 0;
    }
    case SYS_fchdir: {
        fd_t *fl = fd_get(a);
        if (!fl || fl->kind != F_DIR) return fl ? -ENOTDIR : -EBADF;
        memcpy(cwd, fl->path, strlen(fl->path) + 1);
        return 0;
    }
    case SYS_umask:      return 022;
    case SYS_utimensat:  return 0;
    /* memory */
    case SYS_brk:        r = S1(SYS_BRK, a); return r < 0 ? S1(SYS_BRK, 0) : r;
    case SYS_mmap:       return do_mmap(a, b, c, d, e, f);
    case SYS_munmap:     return S3(SYS_MUNMAP, a, b, 0);
    case SYS_mprotect:   return S3(SYS_MPROTECT, a, b, c & 7);
    case SYS_madvise:    return c == 4 || c == 8 ? S3(SYS_MUNMAP, a & ~4095L, b, UNMAP_KEEP) : 0;   /* DONTNEED, FREE */
    case SYS_mremap:     return do_mremap(a, b, c, d);
    case SYS_mlock: case SYS_munlock: case SYS_msync: return 0;
    case SYS_membarrier: return -ENOSYS;
    /* threads and processes */
    case SYS_gettid:     return S1(SYS_THREAD_SELF, 0);
    case SYS_set_tid_address: return S1(SYS_THREAD_SELF, a);
    case SYS_futex:      return do_futex(A(a), b, c, A(d), f);
    case SYS_exit:       S0(SYS_THREAD_EXIT); return 0;
    case SYS_exit_group:
        if (used_unixd) { msg_t m = { .w = { UNIX_BYE } }; unixd(&m); }   /* (our pipes and sockets: closed) */
        S1(SYS_EXIT, a);
        return 0;
    case SYS_sched_yield: return S0(SYS_YIELD);
    case SYS_sched_getaffinity: {
        mk_info_t in;
        S1(SYS_INFO, &in);
        size_t bytes = ((size_t)in.ncpus + 63) / 64 * 8;
        if ((size_t)b < bytes) return -EINVAL;
        memset(A(c), 0, bytes);
        for (int i = 0; i < in.ncpus; i++) ((unsigned char *)A(c))[i / 8] |= 1 << i % 8;
        return bytes;
    }
    case SYS_sched_setaffinity: return 0;
#ifdef SYS_arch_prctl
    case SYS_arch_prctl: return a == 0x1002 ? S1(SYS_SET_FS, b) : -EINVAL;   /* ARCH_SET_FS */
#endif
#ifdef SYS_set_thread_area
    case SYS_set_thread_area: return S1(SYS_SET_FS, a);
#endif
    case SYS_getpid:     return ident(0);
    case SYS_getppid:    return ppid();
    case SYS_getuid: case SYS_geteuid: return ident(1);
    case SYS_getgid: case SYS_getegid: return ident(2);
    case SYS_setuid: case SYS_setgid: return -EPERM;
    case SYS_getpgid:    return ident(0);
    case SYS_setpgid: case SYS_setsid: return 0;
    case SYS_kill:       return S2(SYS_KILL, a, b);
    case SYS_tkill:      return S2(SYS_KILL, ident(0), b);
    case SYS_tgkill:     return S2(SYS_KILL, a, c);
    case SYS_rt_sigprocmask: if (d >= 8 && c) memset(A(c), 0, 8); return 0;   /* (no masks on SIEOS) */
    case SYS_rt_sigaction: return do_sigaction((int)a, A(b), A(c));
    case SYS_wait4:      if (d) memset(A(d), 0, 144); return do_wait4((int)a, A(b), (int)c);
    /* pipes, sockets, waiting (sock.c) */
#ifdef SYS_pipe
    case SYS_pipe:       return new_pair(UNIX_PIPE, 0, A(a));
#endif
    case SYS_pipe2:      return new_pair(UNIX_PIPE, (int)b, A(a));
    case SYS_socket: case SYS_socketpair: case SYS_connect: case SYS_bind: case SYS_listen: case SYS_accept:
    case SYS_accept4: case SYS_sendto: case SYS_recvfrom: case SYS_sendmsg: case SYS_recvmsg: case SYS_shutdown:
    case SYS_getsockname: case SYS_getpeername: case SYS_setsockopt: case SYS_getsockopt:
        return do_socket_call(n, a, b, c, d, e, f);
#ifdef SYS_poll
    case SYS_poll:       return do_poll(A(a), b, c < 0 ? -1 : c * 1000000LL);
    case SYS_select: {
        long *tv = A(e);
        return do_select((int)a, A(b), A(c), A(d), tv ? tv[0] * 1000000000LL + tv[1] * 1000 : -1);
    }
#endif
    case SYS_ppoll:      return do_poll(A(a), b, ts_or_inf(A(c)));
    case SYS_pselect6:   return do_select((int)a, A(b), A(c), A(d), ts_or_inf(A(e)));
    case SYS_eventfd2: {
        event_t *ev = malloc(sizeof *ev);
        if (!ev) return -ENOMEM;
        *ev = (event_t){ (uint64_t)a, !!(b & 1) };          /* (EFD_SEMAPHORE) */
        fd_t fe = { .kind = F_EVENT, .flags = O_RDWR | (b & O_NONBLOCK ? O_NONBLOCK : 0), .cloexec = !!(b & O_CLOEXEC), .obj = ev };
        r = fd_new(0, fe);
        if (r < 0) free(ev);
        return r;
    }
    case SYS_memfd_create: return memfd_new((int)b);
    case SYS_sigaltstack: return 0;
    case SYS_prlimit64:
        if (d) { unsigned long *l = A(d); l[0] = l[1] = b == 3 ? 8UL << 20 : ~0UL; }   /* RLIMIT_STACK: 8 MiB */
        return 0;
    case SYS_getrusage:  memset(A(b), 0, 144); return 0;
    /* time */
    case SYS_clock_gettime: {
        uint64_t t = now_ns(a);
        struct timespec *ts = A(b);
        ts->tv_sec = t / 1000000000; ts->tv_nsec = t % 1000000000;
        return 0;
    }
    case SYS_clock_getres: if (b) { ((struct timespec *)A(b))->tv_sec = 0; ((struct timespec *)A(b))->tv_nsec = 1; } return 0;
    case SYS_gettimeofday: {
        uint64_t t = now_ns(CLOCK_REALTIME);
        long *tv = A(a);
        if (tv) { tv[0] = t / 1000000000; tv[1] = t % 1000000000 / 1000; }
        return 0;
    }
    case SYS_nanosleep:  return do_sleep(CLOCK_MONOTONIC, 0, A(a), A(b));
    case SYS_clock_nanosleep: return do_sleep(a, b, A(c), A(d));
    /* the system */
    case SYS_uname: {
        struct utsname *u = A(a);
        memset(u, 0, sizeof *u);
        strcpy(u->sysname, "SIEOS"); strcpy(u->nodename, "sieos"); strcpy(u->release, "1.0");
        strcpy(u->version, "SIEOS"); strcpy(u->machine,
#if defined(__x86_64__)
            "x86_64");
#else
            "aarch64");
#endif
        return 0;
    }
    case SYS_getrandom:
        for (long done = 0; done < b; ) { r = S2(SYS_RANDOM, (char *)A(a) + done, b - done > 4096 ? 4096 : b - done); if (r < 0) return done ? done : r; done += r; }
        return b;
    case SYS_sysinfo: {
        mk_info_t in;
        S1(SYS_INFO, &in);
        unsigned long *s = A(a);
        memset(s, 0, 112);
        s[0] = in.uptime_ns / 1000000000; s[4] = in.pages_total; s[5] = in.pages_free;
        ((unsigned *)A(a))[26] = 4096;                 /* mem_unit */
        return 0;
    }
    case SYS_statx:      return -ENOSYS;               /* (musl then uses fstatat) */
    case SYS_rseq:       return -ENOSYS;
    }
    unsupported(n);
    return -ENOSYS;
}
