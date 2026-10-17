/*
 * spawn.c - The Linux personality's processes and signals (included by
 * linux.c, whose fd table it shares):
 *
 *   posix_spawn   the program's file read once into a shared memory object
 *                 (kept for the next start: processes started from it share
 *                 its code, SPAWN_SHARE); the child's descriptors, current
 *                 folder, arguments and environment stashed with unixd
 *                 (UNIX_STASH), and the child started with SYS_SPAWN and the
 *                 stash's cookie in its argument string ("name \002cookie");
 *                 the child claims them at start (UNIX_CLAIM: start.c)
 *   wait4         SYS_WAIT, its status in Linux's form
 *   rt_sigaction  SYS_SIGNAL, the handler called from a trampoline that
 *                 ends with SYS_SIGRETURN
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "sieos_spawn.h"

/* ---- Program images in shared memory, by path (and size and time: a
 * program rewritten is read again). */
#define NIMG 8
static struct { char path[FS_PATH]; uint64_t size; int64_t mtime; void *at; uint64_t id; } img[NIMG];
static int img_next;
static long image(const char *path, void **at, uint64_t *size)
{
    siefs_stat_t st;
    long r = fs_path(FS_STAT, path, 0, 0, &st, sizeof st);
    if (r) return r;
    if ((st.mode >> 12) != 8) return -EACCES;
    if (!(st.mode & 0111)) return -EACCES;
    lock();
    for (int i = 0; i < NIMG; i++)
        if (img[i].at && !strcmp(img[i].path, path) && img[i].size == st.size && img[i].mtime == st.mtime) {
            *at = img[i].at; *size = st.size;
            unlock();
            return 0;
        }
    unlock();
    uint64_t id;
    __sieos_random(&id, sizeof id);
    id = id >> 1 | 1;
    if ((r = S2(SYS_SHM_CREATE, id, st.size))) return r;
    long a = S3(SYS_SHM_MAP, id, 0, PROT_READ | PROT_WRITE);
    S1(SYS_SHM_CLOSE, id);                             /* (the mapping keeps it) */
    if (a < 0) return a;
    fd_t f = { .kind = F_FILE };
    if ((f.sh = fs_path(FS_OPEN, path, FS_RDONLY, 0, 0, 0)) < 0) { S3(SYS_MUNMAP, a, st.size, 0); return f.sh; }
    r = file_io(&f, 0, (void *)a, st.size, 0);
    msg_t c = { .w = { FS_CLOSE, f.sh } };
    fs(&c);
    if (r != (long)st.size) { S3(SYS_MUNMAP, a, st.size, 0); return r < 0 ? r : -EIO; }
    lock();
    int k = img_next++ % NIMG;
    if (img[k].at) S3(SYS_MUNMAP, img[k].at, img[k].size, 0);   /* (an older image goes) */
    memcpy(img[k].path, path, strlen(path) + 1);
    img[k].size = st.size; img[k].mtime = st.mtime; img[k].at = (void *)a; img[k].id = id;
    unlock();
    *at = (void *)a; *size = st.size;
    return 0;
}

/* ---- What a child gets: its descriptor table, folder, arguments and
 * environment, as one block (the stash), with unixd's descriptors before it. */
typedef struct { int32_t fd, kind, flags, desc; uint64_t off, size; uint32_t type, pathlen; } kid_fd_t;
#define BLOB_MAGIC 0x53494531u
typedef struct { uint8_t *p; size_t n, cap; } out_t;
static int put(out_t *o, const void *d, size_t n)
{
    if (o->n + n > o->cap) return -1;
    memcpy(o->p + o->n, d, n);
    o->n += n;
    return 0;
}
static size_t count(char *const v[]) { size_t n = 0; while (v && v[n]) n++; return n; }

struct spawn_tmp { fd_t kid[64]; char opened[64][FS_PATH]; uint8_t blob[UNIX_MAX]; };
static long spawn(struct spawn_tmp *t, int *pid, const char *path, int search, const sieos_fa_t *fa, int nfa, int pgroup,
                  char *const argv[], char *const envp[]);
long __sieos_spawn(int *pid, const char *path, int search, const sieos_fa_t *fa, int nfa, int pgroup,
                   char *const argv[], char *const envp[])
{
    struct spawn_tmp *t = malloc(sizeof *t);
    if (!t) return -ENOMEM;
    long r = spawn(t, pid, path, search, fa, nfa, pgroup, argv, envp);
    free(t);
    return r;
}
static long spawn(struct spawn_tmp *t, int *pid, const char *path, int search, const sieos_fa_t *fa, int nfa, int pgroup,
                  char *const argv[], char *const envp[])
{
    fd_t *kid = t->kid;
    uint8_t *blob = t->blob;
    char full[FS_PATH];
    long r;
    if (search && !strchr(path, '/')) {                 /* posix_spawnp: the PATH */
        const char *pv = 0;
        for (size_t i = 0; envp && envp[i]; i++) if (!strncmp(envp[i], "PATH=", 5)) pv = envp[i] + 5;
        if (!pv) pv = "/bin";
        r = -ENOENT;
        for (const char *d = pv; *d && r == -ENOENT; d = *d ? d + 1 : d) {
            const char *e = strchr(d, ':');
            size_t dl = e ? (size_t)(e - d) : strlen(d);
            char cand[FS_PATH];
            if (dl + 1 + strlen(path) + 1 > sizeof cand) { d += dl; continue; }
            memcpy(cand, d, dl); cand[dl] = '/'; memcpy(cand + dl + 1, path, strlen(path) + 1);
            siefs_stat_t st;
            char ab[FS_PATH];
            if (!absolute(cand, ab) && !fs_path(FS_STAT, ab, 0, 0, &st, sizeof st)) { memcpy(full, ab, strlen(ab) + 1); r = 0; }
            d += dl;
        }
        if (r) return r;
    } else if ((r = absolute(path, full))) return r;
    void *at;
    uint64_t size;
    if ((r = image(full, &at, &size))) return r;
    /* the child's table: ours (but close-on-exec ones), then the file actions */
    char kcwd[FS_PATH];
    memcpy(kcwd, cwd, sizeof kcwd);
    for (int i = 0; i < 64; i++) kid[i] = fds[i];     /* (the actions first, then close-on-exec, as exec would) */
    for (int i = 0; i < nfa; i++) {
        const sieos_fa_t *a = &fa[i];
        if (a->cmd <= SIEOS_FA_OPEN && (a->fd < 0 || a->fd >= 64)) return -EBADF;
        switch (a->cmd) {
        case SIEOS_FA_CLOSE: kid[a->fd].kind = F_FREE; break;
        case SIEOS_FA_DUP2:
            if (a->srcfd < 0 || a->srcfd >= 64 || kid[a->srcfd].kind == F_FREE) return -EBADF;
            kid[a->fd] = kid[a->srcfd];
            kid[a->fd].cloexec = 0;
            break;
        case SIEOS_FA_OPEN: {
            char p[FS_PATH];
            if ((r = absolute(a->path, p))) return r;
            kid[a->fd] = (fd_t){ .kind = F_FILE, .flags = a->oflag, .type = a->mode, .path = 0 };
            memcpy(t->opened[a->fd], p, strlen(p) + 1);
            kid[a->fd].path = t->opened[a->fd];
            kid[a->fd].id = 1;                          /* (opened by the child, flags as given) */
            break;
        }
        case SIEOS_FA_CHDIR: if ((r = absolute(a->path, kcwd))) return r; break;
        case SIEOS_FA_FCHDIR: {
            fd_t *d = fd_get(a->fd);
            if (!d || d->kind != F_DIR) return -EBADF;
            memcpy(kcwd, d->path, strlen(d->path) + 1);
            break;
        }
        }
    }
    for (int i = 0; i < 64; i++)
        if (kid[i].cloexec || kid[i].kind == F_SOCK || kid[i].kind == F_EVENT || kid[i].kind == F_PIDFD) kid[i].kind = F_FREE;
    unix_desc_t desc[64];
    int nd = 0;
    out_t o = { blob + 64 * sizeof(unix_desc_t), 0, UNIX_MAX - 64 * sizeof(unix_desc_t) };
    uint32_t hdr[4] = { BLOB_MAGIC, (uint32_t)count(argv), (uint32_t)count(envp), 0 };
    put(&o, hdr, sizeof hdr);
    for (int i = 0; i < 64; i++) {
        fd_t *f = &kid[i];
        if (f->kind == F_FREE) continue;
        kid_fd_t k = { i, f->kind, f->flags, -1, f->off, 0, (uint32_t)f->type, 0 };
        if (f->kind == F_UNIX) { desc[nd] = (unix_desc_t){ UD_END, (uint32_t)f->type, f->id, 0 }; k.desc = nd++; }
        else if (f->kind == F_SHM) {
            if (!f->type) continue;                     /* (a memfd never sized: nothing to pass) */
            desc[nd] = (unix_desc_t){ UD_SHM, 0, f->id, f->off };
            k.desc = nd++;
        }
        if ((f->kind == F_FILE || f->kind == F_DIR) && f->path) {
            k.pathlen = (uint32_t)strlen(f->path) + 1;
            if (f->id != 1) k.flags &= ~(O_CREAT | O_TRUNC | O_EXCL);   /* (inherited: already open) */
            else k.size = 1;                            /* (a file action: opened as asked) */
        } else if (f->kind == F_FILE || f->kind == F_DIR) continue;
        if (put(&o, &k, sizeof k) || (k.pathlen && put(&o, f->path, k.pathlen))) return -E2BIG;
        ((uint32_t *)(o.p))[3]++;
    }
    if (put(&o, kcwd, strlen(kcwd) + 1)) return -E2BIG;
    for (size_t i = 0; argv && argv[i]; i++) if (put(&o, argv[i], strlen(argv[i]) + 1)) return -E2BIG;
    for (size_t i = 0; envp && envp[i]; i++) if (put(&o, envp[i], strlen(envp[i]) + 1)) return -E2BIG;
    uint8_t *start = o.p - nd * sizeof(unix_desc_t);
    memcpy(start, desc, nd * sizeof(unix_desc_t));
    msg_t m = { .w = { UNIX_STASH, (uint64_t)nd }, .sbuf = start, .slen = nd * sizeof(unix_desc_t) + o.n };
    if ((r = unixd(&m)) < 0) return r;
    used_unixd = 1;
    uint64_t cookie = m.w[1];
    /* the child's argument string: its name (the file's), then the cookie */
    char args[64];
    const char *base = strrchr(full, '/') + 1;
    size_t bl = strlen(base) < 30 ? strlen(base) : 30, n = 0;
    memcpy(args, base, bl); n = bl;
    args[n++] = ' '; args[n++] = '\002';
    for (int s = 60; s >= 0; s -= 4) args[n++] = "0123456789abcdef"[cookie >> s & 15];
    args[n] = 0;
    mk_spawn_t opt = { .flags = SPAWN_SHARE | (pgroup ? SPAWN_PGROUP : 0) };
    long p = sc(SYS_SPAWN, (long)at, (long)size, (long)args, -1, -1, (long)&opt);
    if (p < 0) { msg_t u = { .w = { UNIX_UNSTASH, cookie } }; unixd(&u); return p; }
    *pid = (int)p;
    return 0;
}

/* The child's side (start.c): the stash claimed, its fds set; argv and envp
 * point into buf. -> argc, or -errno. Files are opened again later, once the
 * C library is ready (claim_files: they need malloc). */
static const uint8_t *claim_at, *claim_end;
static uint32_t claim_n;
static const unix_desc_t *claim_d;
static const uint8_t *claim_fds(int files)
{
    const uint8_t *b = claim_at;
    for (uint32_t i = 0; i < claim_n && b + sizeof(kid_fd_t) <= claim_end; i++) {
        kid_fd_t k;
        memcpy(&k, b, sizeof k);
        b += sizeof k;
        const char *path = (const char *)b;
        b += k.pathlen;
        if (k.fd < 0 || k.fd >= NFD || !k.pathlen != !files) continue;
        if (!files) {
            fd_t f = { .kind = k.kind, .flags = k.flags, .off = k.off, .type = (int)k.type };
            if (k.desc >= 0) { f.id = claim_d[k.desc].id; if (f.kind == F_SHM) { f.off = claim_d[k.desc].size; f.type = 1; } }
            fds[k.fd] = f;
            continue;
        }
        fds[k.fd].kind = F_FREE;
        long fd = do_open(AT_FDCWD, path, k.flags, (int)k.type);   /* (k.type: the mode, for a file action) */
        if (fd < 0) continue;
        if (fd != k.fd) { fds[k.fd] = fds[fd]; fds[fd].kind = F_FREE; }
        if (!k.size) fds[k.fd].off = k.off;
    }
    return b;
}
__attribute__((constructor)) static void claim_files(void) { if (claim_at) claim_fds(1); claim_at = 0; }

int __sieos_claim(uint64_t cookie, char *buf, size_t size, char ***argvp, char ***envpp)
{
    size_t inlen = UNIX_MAX + 64 * sizeof(unix_desc_t);
    if (size < 2 * inlen) return -E2BIG;
    uint8_t *in = (uint8_t *)buf + size - inlen;       /* (the stash at buf's end, argv and envp from its start) */
    size -= inlen;
    msg_t m = { .w = { UNIX_CLAIM, cookie }, .rbuf = in, .rlen = inlen };
    long r = unixd(&m);
    if (r < 0) return (int)r;
    used_unixd = 1;
    uint8_t *b = in + m.w[1] * sizeof(unix_desc_t), *end = b + r;
    uint32_t hdr[4];
    memcpy(hdr, b, sizeof hdr);
    if (hdr[0] != BLOB_MAGIC) return -EINVAL;
    claim_d = (unix_desc_t *)in;
    claim_at = b + sizeof hdr; claim_end = end; claim_n = hdr[3];
    b = (uint8_t *)claim_fds(0);
    if (b < end) { __sieos_startdir((const char *)b); b += strlen((const char *)b) + 1; }
    /* argv, envp: pointers in buf, then copies of the strings */
    size_t na = hdr[1], ne = hdr[2], need = (na + ne + 2) * sizeof(char *) + (size_t)(end - b);
    if (need > size) return -E2BIG;
    char **av = (char **)buf, **ev = av + na + 1, *s = (char *)(ev + ne + 1);
    memcpy(s, b, end - b);
    for (size_t i = 0; i < na; i++) { av[i] = s; s += strlen(s) + 1; }
    av[na] = 0;
    for (size_t i = 0; i < ne; i++) { ev[i] = s; s += strlen(s) + 1; }
    ev[ne] = 0;
    *argvp = av; *envpp = ev;
    return (int)na;
}

/* ---- wait4: the status as Linux gives it */
static long do_wait4(int pid, int *status, int options)
{
    int st = 0;
    long r = sc(SYS_WAIT, pid, (long)&st, options & 1 ? WAIT_NOHANG : 0, 0, 0, 0);   /* (WNOHANG) */
    if (r <= 0) return r == -ESRCH ? -ECHILD : r;
    if (status) {
        int sig = EXIT_SIGNO(st);
        *status = sig ? sig : st == -1 ? 11 : (st & 0xff) << 8;   /* (killed for a fault: SIGSEGV) */
    }
    return r;
}

/* ---- pidfds (GLib's child watches): poll says when the child has ended;
 * waitid(P_PIDFD) collects it */
static int ended(long pid)                             /* 1: ended (not collected yet), 0: runs, -1: no such child */
{
    mk_task_t t;
    if (S3(SYS_TASKS, &t, 1, pid - 1) != 1 || t.pid != pid) return -1;
    return t.state == TS_DEAD;
}
static long do_waitid(int type, long id, int *info, int options)
{
    if (type == 3) {                                   /* P_PIDFD */
        fd_t *f = fd_get(id);
        if (!f || f->kind != F_PIDFD) return -EBADF;
        id = (long)f->id;
    } else if (type == 0) id = -1;                     /* P_ALL */
    else if (type != 1) return -EINVAL;                /* (P_PGID: not here) */
    int st = 0;
    long r = sc(SYS_WAIT, id, (long)&st, options & 1 ? WAIT_NOHANG : 0, 0, 0, 0);   /* (WNOHANG) */
    if (r < 0) return r == -ESRCH ? -ECHILD : r;
    if (info) {
        memset(info, 0, 128);
        if (r) {
            int sig = EXIT_SIGNO(st) ? EXIT_SIGNO(st) : st == -1 ? 11 : 0;
            info[0] = 17;                              /* si_signo: SIGCHLD */
            info[2] = sig ? 2 : 1;                     /* si_code: CLD_KILLED, CLD_EXITED */
            info[4] = (int)r;                          /* si_pid */
            info[6] = sig ? sig : st & 0xff;           /* si_status */
        }
    }
    return 0;
}

/* ---- Signals: Linux's sigaction over SIEOS's one function per signal */
static struct { void (*h)(int, void *, void *); unsigned long flags; } act[65];
static void trampoline(int sig)
{
    void (*h)(int, void *, void *) = act[sig].h;
    unsigned long fl = act[sig].flags;
    if (fl & 0x80000000UL) { act[sig].h = 0; S2(SYS_SIGNAL, sig, 0); }   /* SA_RESETHAND */
    if (fl & 4) {                                       /* SA_SIGINFO */
        long info[16] = { sig };
        h(sig, info, 0);
    } else {
        union { void (*three)(int, void *, void *); void (*one)(int); } u = { h };
        u.one(sig);
    }
    S0(SYS_SIGRETURN);
}
static long do_sigaction(int sig, const unsigned long *ka, unsigned long *old)
{
    if (sig < 1 || sig > 64) return -EINVAL;
    if (old) { memset(old, 0, 4 * sizeof *old); old[0] = (unsigned long)act[sig].h; old[1] = act[sig].flags; }
    if (!ka) return 0;
    if (sig == SIGKILL || sig == 19) return -EINVAL;   /* (SIGSTOP) */
    act[sig].h = (void (*)(int, void *, void *))ka[0];
    act[sig].flags = ka[1];
    if (sig >= NSIG) return 0;                         /* (musl's own, 32-34: never sent here) */
    void (*entry)(int) = ka[0] == 0 ? SIG_DFL : ka[0] == 1 ? SIG_IGN : trampoline;
    long r = S2(SYS_SIGNAL, sig, entry);
    return r == -EINVAL ? 0 : r;
}
