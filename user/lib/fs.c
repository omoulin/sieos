/*
 * fs.c - The file client: each function is one message (or a few, for
 * large reads and writes) to the file server, port "fs" (mk/proto.h).
 * Relative paths are made absolute here, with the current directory.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

static long fsport;
static int fsgen = 1;                       /* +1 each time the file server is found again */
static char cwd[FS_PATH] = "/";

/* Open files, as this program sees them: the handle it gets is an index
 * here. The server's own handle is kept with the path and the flags, so
 * that if the file server restarts (init does that when it crashes), the
 * file is opened again, without creating or truncating it a second time. */
typedef struct { long sh; int gen, flags; char *path; } file_t;
static file_t *files;
static int nfiles;

/* One message to the file server. If the server died during the call
 * (-EPIPE) or before it (-ENOENT: our port number is stale), wait for the
 * new one and send it again, once. What a restart means for the request:
 * the server starts again from the disk's last commit (at most 1 s old),
 * so a change the old server had not committed is lost and done again;
 * a request it had committed would be done twice, and then fails
 * harmlessly (EEXIST, ENOENT). Handles (f) are reopened first. */
static long raw_open(const char *path, int flags, int mode);
static long fs_ipc(msg_t *m, file_t *f)
{
    msg_t copy = *m;
    for (int tries = 0; ; tries++) {
        if (f) {
            if (f->gen != fsgen) {           /* opened before a restart: open it again */
                long sh = raw_open(f->path, f->flags & ~(FS_CREAT | FS_EXCL | FS_TRUNC), 0);
                if (sh < 0) return sh;
                f->sh = sh;
                f->gen = fsgen;
            }
            m->w[1] = f->sh;
        }
        if (!fsport) fsport = port_lookup("fs");
        long e = ipc_call(fsport, m);
        if ((e == -EPIPE || e == -ENOENT) && !tries) {
            fsport = port_lookup("fs");
            fsgen++;
            *m = copy;
            continue;
        }
        return e < 0 ? e : (long)m->w[0];
    }
}
static long fs_call(msg_t *m) { return fs_ipc(m, 0); }
static file_t *file_of(long h) { return h >= 0 && h < nfiles && files[h].path ? &files[h] : 0; }
static long hcall(long h, msg_t *m) { file_t *f = file_of(h); return f ? fs_ipc(m, f) : -EBADF; }

/* path, made absolute and simplified ("." and ".." removed, as text). */
static int absolute(const char *path, char *out)
{
    char tmp[FS_PATH * 2];
    size_t n = 0;
    if (path[0] != '/') { n = strlcpy(tmp, cwd, FS_PATH); tmp[n++] = '/'; }
    if (n + strlcpy(tmp + n, path, sizeof tmp - n) >= FS_PATH) return -ENAMETOOLONG;
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

/* A request carrying one or two paths, or a path and a name ("a\0b"). */
static long pcall(msg_t *m, const char *a, const char *b, int b_is_path)
{
    char s[FS_PATH * 2 + 2];
    long e = absolute(a, s);
    if (e) return e;
    size_t n = strlen(s) + 1;
    if (b) {
        if (b_is_path) { if ((e = absolute(b, s + n))) return e; }
        else if (strlcpy(s + n, b, FS_PATH) >= FS_PATH) return -ENAMETOOLONG;
        n += strlen(s + n) + 1;
    }
    m->sbuf = s;
    m->slen = n;
    return fs_call(m);
}

static long raw_open(const char *path, int flags, int mode)
{
    msg_t m = { .w = { FS_OPEN, flags, mode }, .sbuf = path, .slen = strlen(path) + 1 };
    if (!fsport) fsport = port_lookup("fs");
    long e = ipc_call(fsport, &m);
    return e < 0 ? e : (long)m.w[0];
}
long fs_open(const char *path, int flags, int mode)
{
    char p[FS_PATH];
    long e = absolute(path, p);
    if (e) return e;
    int i = 0;
    while (i < nfiles && files[i].path) i++;
    if (i == nfiles) {
        file_t *f = realloc(files, (nfiles + 8) * sizeof *f);
        if (!f) return -ENOMEM;
        memset(f + nfiles, 0, 8 * sizeof *f);
        files = f;
        nfiles += 8;
    }
    char *keep = malloc(strlen(p) + 1);
    if (!keep) return -ENOMEM;
    strlcpy(keep, p, strlen(p) + 1);
    msg_t m = { .w = { FS_OPEN, flags, mode }, .sbuf = p, .slen = strlen(p) + 1 };
    long sh = fs_call(&m);
    if (sh < 0) { free(keep); return sh; }
    files[i] = (file_t){ sh, fsgen, flags, keep };
    return i;
}
long fs_close(long h)
{
    file_t *f = file_of(h);
    if (!f) return -EBADF;
    long r = 0;
    if (f->gen == fsgen) {               /* (else the old server's handle died with it) */
        msg_t m = { .w = { FS_CLOSE, f->sh } };   /* never repeated: after a restart, f->sh may be another file's */
        long e = ipc_call(fsport, &m);
        r = e == -EPIPE || e == -ENOENT ? 0 : e < 0 ? e : (long)m.w[0];
    }
    free(f->path);
    f->path = 0;
    return r;
}
long fs_mkdir(const char *path, int mode)            { msg_t m = { .w = { FS_MKDIR, mode } }; return pcall(&m, path, 0, 0); }
long fs_unlink(const char *path)                     { msg_t m = { .w = { FS_UNLINK } }; return pcall(&m, path, 0, 0); }
long fs_rmdir(const char *path)                      { msg_t m = { .w = { FS_RMDIR } }; return pcall(&m, path, 0, 0); }
long fs_rename(const char *from, const char *to)     { msg_t m = { .w = { FS_RENAME } }; return pcall(&m, from, to, 1); }
long fs_link(const char *from, const char *to)       { msg_t m = { .w = { FS_LINK } }; return pcall(&m, from, to, 1); }
long fs_chmod(const char *path, int mode)            { msg_t m = { .w = { FS_CHMOD, mode } }; return pcall(&m, path, 0, 0); }
long fs_chown(const char *path, int uid, int gid)    { msg_t m = { .w = { FS_CHOWN, uid, gid } }; return pcall(&m, path, 0, 0); }
long fs_truncate(long h, uint64_t size)              { msg_t m = { .w = { FS_TRUNCATE, h, size } }; return hcall(h, &m); }
/* Commit everything now. If the file server restarted since this program's
 * last sync, changes made before the restart may be lost (they were not
 * committed): then this says so (-EIO), once, so the program can redo them. */
static int synced_gen = 1;
long fs_sync(void)
{
    msg_t m = { .w = { FS_SYNC } };
    long r = fs_call(&m);
    if (r >= 0 && synced_gen != fsgen) r = -EIO;
    synced_gen = fsgen;
    return r;
}

long fs_symlink(const char *target, const char *path)   /* the target is kept as written */
{
    char s[FS_PATH * 2 + 2];
    size_t n = strlcpy(s, target, FS_PATH) + 1;
    long e = n > FS_PATH ? -ENAMETOOLONG : absolute(path, s + n);
    if (e) return e;
    msg_t m = { .w = { FS_SYMLINK }, .sbuf = s, .slen = n + strlen(s + n) + 1 };
    return fs_call(&m);
}

long fs_stat(const char *path, siefs_stat_t *st, int nofollow)
{
    msg_t m = { .w = { FS_STAT, nofollow }, .rbuf = st, .rlen = sizeof *st };
    return pcall(&m, path, 0, 0);
}
long fs_fstat(long h, siefs_stat_t *st)
{
    msg_t m = { .w = { FS_FSTAT, h }, .rbuf = st, .rlen = sizeof *st };
    return hcall(h, &m);
}
long fs_readlink(const char *path, char *buf, size_t size)
{
    msg_t m = { .w = { FS_READLINK }, .rbuf = buf, .rlen = size };
    return pcall(&m, path, 0, 0);
}

/* Reads and writes: FS_MAX bytes per message at most. */
long fs_read(long h, uint64_t off, void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        size_t c = n - done < FS_MAX ? n - done : FS_MAX;
        msg_t m = { .w = { FS_READ, h, off + done, c }, .rbuf = (char *)buf + done, .rlen = c };
        long r = hcall(h, &m);
        if (r < 0) return done ? (long)done : r;
        done += r;
        if ((size_t)r < c) break;                    /* end of the file */
    }
    return done;
}
long fs_write(long h, uint64_t off, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        size_t c = n - done < FS_MAX ? n - done : FS_MAX;
        msg_t m = { .w = { FS_WRITE, h, off + done }, .sbuf = (const char *)buf + done, .slen = c };
        long r = hcall(h, &m);
        if (r < 0) return done ? (long)done : r;
        done += r;
    }
    return done;
}

/* Directory entries, several per message: call with *cursor = 0, then
 * fs_dirent() walks the records in buf. Returns the bytes filled, 0 at the end. */
long fs_readdir(long h, uint64_t *cursor, void *buf, size_t size)
{
    msg_t m = { .w = { FS_READDIR, h, *cursor, size }, .rbuf = buf, .rlen = size };
    long r = hcall(h, &m);
    if (r >= 0) *cursor = m.w[1];
    return r;
}
const char *fs_dirent(const char **p, const char *end, uint64_t *ino, int *type, char *name)
{
    if (*p + 10 > end) return 0;
    const char *e = *p;
    memcpy(ino, e, 8);
    *type = (uint8_t)e[8];
    int n = (uint8_t)e[9];
    memcpy(name, e + 10, n);
    name[n] = 0;
    *p = e + 10 + n;
    return name;
}

long fs_getxattr(const char *path, const char *name, void *buf, size_t size)
{
    msg_t m = { .w = { FS_GETXATTR }, .rbuf = buf, .rlen = size };
    return pcall(&m, path, name, 0);
}
long fs_listxattr(const char *path, char *buf, size_t size)
{
    msg_t m = { .w = { FS_LISTXATTR }, .rbuf = buf, .rlen = size };
    return pcall(&m, path, 0, 0);
}
long fs_rmxattr(const char *path, const char *name)  { msg_t m = { .w = { FS_RMXATTR } }; return pcall(&m, path, name, 0); }
long fs_setxattr(const char *path, const char *name, const void *val, size_t len)
{
    char s[FS_PATH + 256 + 1100];
    long e = absolute(path, s);
    if (e) return e;
    size_t n = strlen(s) + 1, k = strlen(name) + 1;
    if (k > 256 || len > 1024) return -EINVAL;
    memcpy(s + n, name, k);
    memcpy(s + n + k, val, len);
    msg_t m = { .w = { FS_SETXATTR, len }, .sbuf = s, .slen = n + k + len };
    return fs_call(&m);
}
/* Paths of the objects whose attribute `name` is `value`, "p\0p\0" in buf;
 * call again while *cursor != 0. */
long fs_find(const char *name, const char *value, uint64_t *cursor, char *buf, size_t size)
{
    char s[512];
    size_t a = strlen(name) + 1, b = strlen(value) + 1;
    if (a + b > sizeof s) return -EINVAL;
    memcpy(s, name, a);
    memcpy(s + a, value, b);
    msg_t m = { .w = { FS_FIND, *cursor, size }, .sbuf = s, .slen = a + b, .rbuf = buf, .rlen = size };
    long r = fs_call(&m);
    if (r >= 0) *cursor = m.w[1];
    return r;
}
long fs_statfs(siefs_statfs_t *st)
{
    msg_t m = { .w = { FS_STATFS }, .rbuf = st, .rlen = sizeof *st };
    return fs_call(&m);
}

/* The current directory: kept by each program. */
long fs_chdir(const char *path)
{
    char p[FS_PATH];
    siefs_stat_t st;
    long e = absolute(path, p);
    if (!e) e = fs_stat(p, &st, 0);
    if (!e && (st.mode & SIEFS_IFMT) != SIEFS_IFDIR) e = -ENOTDIR;
    if (!e) strlcpy(cwd, p, sizeof cwd);
    return e;
}
const char *fs_getcwd(void) { return cwd; }

/* Run a program from the disk: read its file, start it, wait for its end.
 * Returns its exit status, or -error if it could not start. */
/* Start the program in file `path` (root may give it another identity;
 * -1, -1, 0: the caller's) -> its pid. */
long spawn_file(const char *path, const char *args, int uid, int gid, const mk_spawn_t *o)
{
    siefs_stat_t st;
    long h = fs_open(path, FS_RDONLY, 0), r;
    if (h < 0) return h;
    if ((r = fs_fstat(h, &st)) || !(st.mode & 0111)) { fs_close(h); return r ? r : -EACCES; }
    char *elf = malloc(st.size ? st.size : 1);
    if (!elf) { fs_close(h); return -ENOMEM; }
    r = fs_read(h, 0, elf, st.size);
    fs_close(h);
    if (r == (long)st.size) r = sys_spawn(elf, st.size, args, uid, gid, o);
    else if (r >= 0) r = -EIO;
    free(elf);                                       /* the kernel has its own copy */
    return r;
}

/* Run a program and wait for it -> its exit status. */
long run(const char *path, const char *args)
{
    long pid = spawn_file(path, args, -1, -1, 0);
    if (pid < 0) return pid;
    int status = 0;
    sys_wait((int)pid, &status, 0);
    return status;
}
