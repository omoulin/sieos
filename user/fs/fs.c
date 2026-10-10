/*
 * fs - The file server: SieFS (siefs/) on the disk served by vblk, offered
 * to every program on the port "fs" (mk/proto.h).
 *
 *   program --FS_OPEN "/etc/motd"--> fs --DISK_READ--> vblk --virtio--> disk
 *
 * The SieFS library does the file system; this server adds what a shared
 * service needs:
 *   - paths: it walks them component by component, following symbolic
 *     links, and checks at each step that the caller may go there;
 *   - permissions: from the uid/gid the kernel stamps on every message, so
 *     no program can lie about who it is (root, uid 0, may do anything);
 *   - open files: a table of handles, each valid only for its process;
 *   - commits: changes reach the disk 1 s after the first one (or at
 *     FS_SYNC), written by SieFS all together, so a crash loses at most
 *     that last second and never leaves a half-done change.
 *
 * If this server crashes, init starts it again: it mounts the disk's last
 * commit, and programs' file handles are reopened by their library. If the
 * disk driver crashes, the requests to it are simply sent again.
 *
 * One request at a time (the library is single-threaded); a helper thread
 * only times the commits, and sleeps for good while nothing has changed.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "siefs.h"

#define COMMIT_NS 1000000000L               /* commit 1 s after the first change: at most 1 s of
                                               changes is lost if the server (or the machine) dies */
#define PATH_MAX  1024

static siefs_t *fs;
static long disk, port;
static int self;                            /* our pid: FS_TICK comes from our own thread */
static char buf[FS_MAX + PATH_MAX + 8];     /* every message in, and every reply out */
static uint64_t orphans;                    /* "/.orphans": open files whose last name went */

/* ---- The block device, for the library: calls to the disk driver. */
static int dio(int op, uint64_t blk, uint32_t n, void *p)
{
    while (n) {
        uint32_t c = n < DISK_MAX / DISK_BS ? n : DISK_MAX / DISK_BS;
        msg_t m = { .w = { op, blk, c } };
        if (op == DISK_READ) { m.rbuf = p; m.rlen = c * DISK_BS; }
        else { m.sbuf = p; m.slen = c * DISK_BS; }
        long e = call_named(&disk, "disk", &m, 1);    /* disk requests are safe to repeat */
        if (e < 0 || (long)m.w[0] < 0) return -EIO;
        blk += c; n -= c; p = (char *)p + (uint64_t)c * DISK_BS;
    }
    return 0;
}
static int d_read(void *ctx, uint64_t blk, uint32_t n, void *p)        { (void)ctx; return dio(DISK_READ, blk, n, p); }
static int d_write(void *ctx, uint64_t blk, uint32_t n, const void *p) { (void)ctx; return dio(DISK_WRITE, blk, n, (void *)p); }
static int d_flush(void *ctx)
{
    (void)ctx;
    msg_t m = { .w = { DISK_FLUSH } };
    return call_named(&disk, "disk", &m, 1) < 0 || (long)m.w[0] < 0 ? -EIO : 0;
}
/* Wall-clock time: there is no clock chip driver yet, so the build's date
 * plus the time since boot. */
static int64_t now(void) { return (int64_t)EPOCH * 1000000000 + sys_clock(); }

/* ---- Commit timing. The commit thread asks FS_TICK; the answer is how
 * long to sleep. With nothing changed, the answer waits until a change
 * comes: no wake-ups at all while the disk is idle. */
static int dirty;
static int64_t dirty_since;
static long tick_from;                      /* the commit thread, waiting for a change */

static void changed(void)
{
    if (dirty) return;
    dirty = 1;
    dirty_since = sys_clock();
    if (tick_from) { reply_val(tick_from, COMMIT_NS); tick_from = 0; }
}

static void ticker(void *arg)
{
    (void)arg;
    for (;;) {
        msg_t m = { .w = { FS_TICK } };
        if (ipc_call(port, &m) < 0) return;
        sys_sleep(m.w[0]);
    }
}

/* ---- Open files. A handle is an index in this table; pid 0 = free. */
typedef struct { int pid, flags, orphan; uint64_t ino; } hnd_t;
static hnd_t *hnd;
static int nhnd;

/* Programs that ended without closing their files: drop their handles.
 * Done only when the table is full, so it costs nothing normally. */
static void collect(void)
{
    static mk_task_t t[64];
    for (int i = 0; i < nhnd; i++) hnd[i].flags |= 1 << 30;        /* "maybe dead" */
    long n;
    int after = 0;
    do {
        n = sys_tasks(t, 64, after);
        for (long k = 0; k < n; k++)
            for (int i = 0; i < nhnd; i++) if (hnd[i].pid == t[k].pid) hnd[i].flags &= ~(1 << 30);
        if (n > 0) after = t[n - 1].pid;
    } while (n == 64);
    for (int i = 0; i < nhnd; i++) if (hnd[i].flags >> 30 & 1) hnd[i].pid = 0;
}

static long hnd_new(int pid, int flags, uint64_t ino)
{
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nhnd; i++)
            if (!hnd[i].pid) { hnd[i] = (hnd_t){ pid, flags, 0, ino }; return i; }
        if (!pass) collect();
    }
    if (nhnd >= 4096) return -EMFILE;
    int n = nhnd ? nhnd * 2 : 16;
    hnd_t *h = realloc(hnd, n * sizeof *h);
    if (!h) return -ENOMEM;
    memset(h + nhnd, 0, (n - nhnd) * sizeof *h);
    hnd = h;
    nhnd = n;
    return hnd_new(pid, flags, ino);
}

static hnd_t *hnd_get(uint64_t i, int pid) { return i < (uint64_t)nhnd && hnd[i].pid == pid ? &hnd[i] : 0; }
static int is_open(uint64_t ino) { for (int i = 0; i < nhnd; i++) if (hnd[i].pid && hnd[i].ino == ino) return 1; return 0; }

/* The name of an orphan in /.orphans: its object number in hex. */
static void orphan_name(uint64_t ino, char *s) { int i = 16; s[i] = 0; while (i--) { s[i] = "0123456789abcdef"[ino & 15]; ino >>= 4; } }

static void hnd_close(hnd_t *h)
{
    h->pid = 0;
    if (h->orphan && !is_open(h->ino)) {          /* the last user of a deleted file: delete it now */
        char nm[17];
        orphan_name(h->ino, nm);
        siefs_unlink(fs, orphans, nm);
        changed();
    }
}

/* ---- Paths. Walk `path` as the caller: search permission (x) on every
 * directory on the way, symbolic links followed (at most 8), the last one
 * only if `follow`. Gives the object, and also the directory and name of
 * the last component (even when that name does not exist yet: -ENOENT with
 * *dir set is how "create" finds where to create). */
typedef struct { uint32_t uid, gid; int pid; } cred_t;

/* The other groups of a process: the kernel stamps only uid and gid on a
 * message, so they are asked (SYS_IDENT) the first time they could change
 * the answer, and kept: a process's identity never changes. */
static struct { int pid; mk_groups_t g; } gcache[16];
static int in_group(cred_t c, uint32_t gid)
{
    int i = 0;
    while (i < 16 && gcache[i].pid != c.pid) i++;
    if (i == 16) {
        static int next;
        mk_ident_t id;
        i = next++ % 16;
        gcache[i].pid = c.pid;
        gcache[i].g.n = sys_ident(c.pid, &id) ? 0 : id.groups.n;
        if (gcache[i].g.n) gcache[i].g = id.groups;
    }
    for (int k = 0; k < gcache[i].g.n; k++) if ((uint32_t)gcache[i].g.g[k] == gid) return 1;
    return 0;
}
/* May the caller do `want` (4 read, 2 write, 1 search/run) on st? */
static int may(cred_t c, const siefs_stat_t *st, int want)
{
    uint32_t g = c.gid;
    if (c.uid && c.uid != st->uid && g != st->gid && in_group(c, st->gid)) g = st->gid;
    return siefs_access(st, c.uid, g, want);
}

static int walk(const char *path, int follow, cred_t c, uint64_t *ino, uint64_t *dir, char *name)
{
    static char a[PATH_MAX], b[PATH_MAX];
    char nm[SIEFS_NAME_MAX + 1];
    uint64_t cur = SIEFS_ROOT, next;
    siefs_stat_t st;
    int links = 0, r;
    if (strlcpy(a, path, sizeof a) >= sizeof a) return -ENAMETOOLONG;
    char *p = a;
    if (dir) *dir = 0;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) { *ino = cur; return 0; }
        char *e = p;
        while (*e && *e != '/') e++;
        size_t len = (size_t)(e - p);
        if (len > SIEFS_NAME_MAX) return -ENAMETOOLONG;
        memcpy(nm, p, len);
        nm[len] = 0;
        while (*e == '/') e++;
        int last = !*e;
        if ((r = siefs_stat(fs, cur, &st))) return r;
        if ((st.mode & SIEFS_IFMT) != SIEFS_IFDIR) return -ENOTDIR;
        if ((r = may(c, &st, 1))) return r;
        if (last && dir) { *dir = cur; memcpy(name, nm, len + 1); }
        if ((r = siefs_lookup(fs, cur, nm, &next))) return r;   /* handles "." and ".." too */
        if ((r = siefs_stat(fs, next, &st))) return r;
        if ((st.mode & SIEFS_IFMT) == SIEFS_IFLNK && (!last || follow)) {
            if (++links > 8) return -ELOOP;
            long n = siefs_readlink(fs, next, b, sizeof b - 1);
            if (n < 0) return (int)n;
            size_t rest = strlen(e);
            if ((size_t)n + 1 + rest >= sizeof b) return -ENAMETOOLONG;
            b[n] = '/';
            memcpy(b + n + 1, e, rest + 1);
            memcpy(a, b, (size_t)n + rest + 2);
            p = a;
            if (*p == '/') cur = SIEFS_ROOT;     /* else: relative to the link's directory */
            if (dir) *dir = 0;
            continue;
        }
        cur = next;
        p = e;
        if (last) { *ino = cur; return 0; }
    }
}

/* May the caller change object `ino` in directory `dir` (delete, rename)?
 * Write and search on the directory; in a "sticky" directory (/tmp), only
 * the file's or the directory's owner. */
static int may_remove(cred_t c, uint64_t dir, uint64_t ino)
{
    siefs_stat_t d, f;
    int r;
    if ((r = siefs_stat(fs, dir, &d)) || (r = siefs_stat(fs, ino, &f))) return r;
    if ((r = may(c, &d, 3))) return r;
    if ((d.mode & 01000) && c.uid && c.uid != d.uid && c.uid != f.uid) return -EPERM;
    return 0;
}
static int may_create(cred_t c, uint64_t dir)
{
    siefs_stat_t d;
    int r = siefs_stat(fs, dir, &d);
    return r ? r : may(c, &d, 3);
}
static int is_owner(cred_t c, uint64_t ino, siefs_stat_t *st)
{
    int r = siefs_stat(fs, ino, st);
    return r ? r : c.uid && c.uid != st->uid ? -EPERM : 0;
}

/* Look for object `ino` below `dir`, depth first; path[0..len) is dir's path. */
static int search(uint64_t dir, uint64_t ino, char *path, size_t len, int depth)
{
    siefs_dirent_t de;
    uint64_t cur = 2;                               /* after "." and ".." */
    if (depth > 16) return 0;
    while (siefs_readdir(fs, dir, &cur, &de) == 1) {
        size_t n = strlen(de.name);
        if (len + n + 2 > PATH_MAX) continue;
        path[len] = '/';
        memcpy(path + len + 1, de.name, n + 1);
        if (de.ino == ino) return 1;
        if (de.type == 4 && de.ino != orphans && search(de.ino, ino, path, len + n + 1, depth + 1)) return 1;
    }
    path[len] = 0;
    return 0;
}

/* The path of object `ino` (for FS_FIND): from its parent upwards, looking
 * for its name in each directory (SieFS keeps every object's parent). If a
 * hard link was removed, the recorded parent may no longer hold it: then
 * search the whole tree, which is slow but rare. */
static int path_of(uint64_t ino, char *out, size_t size)
{
    char tmp[PATH_MAX];
    size_t len = 0;
    uint64_t want = ino;
    siefs_dirent_t de;
    tmp[0] = 0;
    for (int depth = 0; ino != SIEFS_ROOT; depth++) {
        siefs_stat_t st;
        uint64_t cur = 2, parent;
        if (depth > 64 || siefs_stat(fs, ino, &st)) return -ENOENT;
        parent = st.parent;
        int found = 0;
        while (!found && siefs_readdir(fs, parent, &cur, &de) == 1) found = de.ino == ino;
        if (!found) return size >= PATH_MAX && search(SIEFS_ROOT, want, out, 0, 0) ? 0 : -ENOENT;
        size_t n = strlen(de.name);
        if (len + n + 1 >= sizeof tmp) return -ENAMETOOLONG;
        memmove(tmp + n + 1, tmp, len + 1);
        tmp[0] = '/';
        memcpy(tmp + 1, de.name, n);
        len += n + 1;
        ino = parent;
    }
    if (!len) { tmp[0] = '/'; tmp[1] = 0; len = 1; }
    if (len + 1 > size) return -ENAMETOOLONG;
    memcpy(out, tmp, len + 1);
    return 0;
}

/* ---- One request. Returns the result; out and outlen: data to send back,
 * w1: a second value (cursors). */
static long serve(msg_t *m, long from, const void **out, uint64_t *outlen, uint64_t *w1)
{
    cred_t c = { (uint32_t)m->uid, (uint32_t)m->gid, m->pid };
    char *s1 = buf, *s2 = buf + strlen(buf) + 1, name[SIEFS_NAME_MAX + 1], name2[SIEFS_NAME_MAX + 1];
    uint64_t ino, dir, ino2, dir2;
    siefs_stat_t st;
    hnd_t *h;
    long r;
    int pid = m->pid;
    if (s2 > buf + m->rlen) s2 = buf + m->rlen;   /* (a message with a single string) */

    switch (m->w[0]) {
    case FS_OPEN: {
        int fl = (int)m->w[1], acc = fl & 3;
        r = walk(s1, 1, c, &ino, &dir, name);
        if (r == -ENOENT && (fl & FS_CREAT) && dir) {
            if ((r = may_create(c, dir))) return r;
            if ((r = siefs_create(fs, dir, name, SIEFS_IFREG | (m->w[2] & 07777), c.uid, c.gid, &ino))) return r;
            changed();
        } else if (r) return r;
        else if ((fl & FS_CREAT) && (fl & FS_EXCL)) return -EEXIST;
        if ((r = siefs_stat(fs, ino, &st))) return r;
        int isdir = (st.mode & SIEFS_IFMT) == SIEFS_IFDIR;
        if (isdir && acc != FS_RDONLY) return -EISDIR;
        if (!isdir && (fl & FS_DIRECTORY)) return -ENOTDIR;
        if ((r = may(c, &st, acc == FS_RDONLY ? 4 : acc == FS_WRONLY ? 2 : 6))) return r;
        if ((fl & FS_TRUNC) && acc != FS_RDONLY && st.size) { if ((r = siefs_truncate(fs, ino, 0))) return r; changed(); }
        return hnd_new(pid, fl, ino);
    }
    case FS_CLOSE:
        if (!(h = hnd_get(m->w[1], pid))) return -EBADF;
        hnd_close(h);
        return 0;
    case FS_READ: {
        if (!(h = hnd_get(m->w[1], pid)) || (h->flags & 3) == FS_WRONLY) return -EBADF;
        uint64_t n = m->w[3] < FS_MAX ? m->w[3] : FS_MAX;
        r = siefs_read(fs, h->ino, m->w[2], buf, n);
        if (r > 0) { *out = buf; *outlen = r; }
        return r;
    }
    case FS_WRITE: {
        if (!(h = hnd_get(m->w[1], pid)) || (h->flags & 3) == FS_RDONLY) return -EBADF;
        uint64_t off = m->w[2];
        if (h->flags & FS_APPEND) { if ((r = siefs_stat(fs, h->ino, &st))) return r; off = st.size; }
        r = siefs_write(fs, h->ino, off, buf, m->rlen);
        if (r > 0) changed();
        return r;
    }
    case FS_STAT: case FS_FSTAT:
        if (m->w[0] == FS_FSTAT) { if (!(h = hnd_get(m->w[1], pid))) return -EBADF; ino = h->ino; }
        else if ((r = walk(s1, !m->w[1], c, &ino, 0, 0))) return r;    /* w[1] = 1: the link itself */
        if ((r = siefs_stat(fs, ino, (siefs_stat_t *)buf))) return r;
        *out = buf; *outlen = sizeof(siefs_stat_t);
        return 0;
    case FS_READDIR: {
        if (!(h = hnd_get(m->w[1], pid))) return -EBADF;
        uint64_t cur = m->w[2], max = m->w[3] < FS_MAX ? m->w[3] : FS_MAX, len = 0;
        static siefs_dirent_t de;
        for (;;) {
            uint64_t before = cur;
            if ((r = siefs_readdir(fs, h->ino, &cur, &de)) <= 0) { if (r < 0) return r; break; }
            size_t n = strlen(de.name);
            if (len + 10 + n > max) { cur = before; break; }    /* next time */
            memcpy(buf + len, &de.ino, 8);
            buf[len + 8] = (char)de.type;
            buf[len + 9] = (char)n;
            memcpy(buf + len + 10, de.name, n);
            len += 10 + n;
        }
        *out = buf; *outlen = len; *w1 = cur;
        return (long)len;
    }
    case FS_MKDIR:
        r = walk(s1, 0, c, &ino, &dir, name);
        if (!r) return -EEXIST;
        if (r != -ENOENT || !dir) return r;
        if ((r = may_create(c, dir)) || (r = siefs_create(fs, dir, name, SIEFS_IFDIR | (m->w[1] & 07777), c.uid, c.gid, &ino))) return r;
        changed();
        return 0;
    case FS_UNLINK: case FS_RMDIR:
        if ((r = walk(s1, 0, c, &ino, &dir, name))) return r;
        if (!dir) return -EBUSY;                      /* "/" */
        if ((r = may_remove(c, dir, ino)) || (r = siefs_stat(fs, ino, &st))) return r;
        if (m->w[0] == FS_RMDIR) r = siefs_rmdir(fs, dir, name);
        else if ((st.mode & SIEFS_IFMT) == SIEFS_IFDIR) return -EISDIR;
        else if (st.nlink == 1 && is_open(ino)) {     /* still open: keep it until the last close */
            char nm[17];
            orphan_name(ino, nm);
            if (!(r = siefs_rename(fs, dir, name, orphans, nm)))
                for (int i = 0; i < nhnd; i++) if (hnd[i].pid && hnd[i].ino == ino) hnd[i].orphan = 1;
        } else r = siefs_unlink(fs, dir, name);
        if (!r) changed();
        return r;
    case FS_RENAME: case FS_LINK:
        if ((r = walk(s1, 0, c, &ino, &dir, name))) return r;
        r = walk(s2, 0, c, &ino2, &dir2, name2);
        if (r && (r != -ENOENT || !dir2)) return r;
        if (!dir || !dir2) return -EBUSY;
        if (m->w[0] == FS_RENAME) {
            if ((r = may_remove(c, dir, ino))) return r;
            if (!walk(s2, 0, c, &ino2, 0, 0) && (r = may_remove(c, dir2, ino2))) return r;
            if ((r = may_create(c, dir2)) || (r = siefs_rename(fs, dir, name, dir2, name2))) return r;
        } else if ((r = may_create(c, dir2)) || (r = siefs_link(fs, ino, dir2, name2))) return r;
        changed();
        return 0;
    case FS_SYMLINK:                                  /* s1: the target, s2: the new link */
        r = walk(s2, 0, c, &ino, &dir, name);
        if (!r) return -EEXIST;
        if (r != -ENOENT || !dir) return r;
        if ((r = may_create(c, dir)) || (r = siefs_symlink(fs, dir, name, s1, c.uid, c.gid, &ino))) return r;
        changed();
        return 0;
    case FS_READLINK:
        if ((r = walk(s1, 0, c, &ino, 0, 0))) return r;
        if ((r = siefs_readlink(fs, ino, buf, PATH_MAX)) >= 0) { *out = buf; *outlen = r; }
        return r;
    case FS_TRUNCATE:
        if (!(h = hnd_get(m->w[1], pid)) || (h->flags & 3) == FS_RDONLY) return -EBADF;
        if ((r = siefs_truncate(fs, h->ino, m->w[2]))) return r;
        changed();
        return 0;
    case FS_CHMOD: case FS_CHOWN: case FS_UTIMES: {
        unsigned mask;
        if ((r = walk(s1, 1, c, &ino, 0, 0)) || (r = is_owner(c, ino, &st))) return r;
        if (m->w[0] == FS_CHMOD) { st.mode = (st.mode & SIEFS_IFMT) | (m->w[1] & 07777); mask = SIEFS_SET_MODE; }
        else if (m->w[0] == FS_CHOWN) { if (c.uid) return -EPERM; st.uid = m->w[1]; st.gid = m->w[2]; mask = SIEFS_SET_UID | SIEFS_SET_GID; }
        else { st.atime = m->w[1]; st.mtime = m->w[2]; mask = SIEFS_SET_ATIME | SIEFS_SET_MTIME; }
        if ((r = siefs_setattr(fs, ino, &st, mask))) return r;
        changed();
        return 0;
    }
    case FS_GETXATTR: case FS_LISTXATTR:
        if ((r = walk(s1, 1, c, &ino, 0, 0)) || (r = siefs_stat(fs, ino, &st)) || (r = may(c, &st, 4))) return r;
        r = m->w[0] == FS_GETXATTR ? siefs_getxattr(fs, ino, s2, buf, FS_MAX) : siefs_listxattr(fs, ino, buf, FS_MAX);
        if (r >= 0) { *out = buf; *outlen = r; }
        return r;
    case FS_SETXATTR: case FS_RMXATTR: {
        if ((r = walk(s1, 1, c, &ino, 0, 0)) || (r = siefs_stat(fs, ino, &st)) || (r = may(c, &st, 2))) return r;
        const char *val = s2 + strlen(s2) + 1;
        if (m->w[0] == FS_SETXATTR && val + m->w[1] > buf + m->rlen) return -EINVAL;
        r = m->w[0] == FS_SETXATTR ? siefs_setxattr(fs, ino, s2, val, m->w[1]) : siefs_removexattr(fs, ino, s2);
        if (!r) changed();
        return r;
    }
    case FS_FIND: {                                   /* s1 = name, s2 = value */
        uint64_t cur = m->w[1], len = 0, max = m->w[2] < FS_MAX ? m->w[2] : FS_MAX;
        static char p[PATH_MAX];
        size_t vlen = strlen(s2);
        char nm[SIEFS_NAME_MAX + 1], val[256];
        if (strlcpy(nm, s1, sizeof nm) >= sizeof nm || strlcpy(val, s2, sizeof val) >= sizeof val) return -EINVAL;
        for (;;) {
            uint64_t before = cur;
            if ((r = siefs_find(fs, nm, val, vlen, &cur, &ino)) <= 0) { if (r < 0) return r; cur = 0; break; }
            /* only what the caller could reach by its path */
            if (path_of(ino, p, sizeof p) || walk(p, 0, c, &ino2, 0, 0)) continue;
            size_t n = strlen(p) + 1;
            if (len + n > max) { cur = before; break; }
            memcpy(buf + len, p, n);
            len += n;
        }
        *out = buf; *outlen = len; *w1 = cur;
        return (long)len;
    }
    case FS_STATFS:
        if ((r = siefs_statfs(fs, (siefs_statfs_t *)buf))) return r;
        *out = buf; *outlen = sizeof(siefs_statfs_t);
        return 0;
    case FS_SYNC:
        r = dirty ? siefs_sync(fs) : 0;
        if (!r) dirty = 0;
        return r;
    case FS_TICK: {
        if (pid != self) return -EPERM;
        int64_t left = dirty_since + COMMIT_NS - sys_clock();
        if (dirty && left > 0) return left;
        if (dirty && !siefs_sync(fs)) dirty = 0;
        tick_from = from;                             /* answered at the next change */
        return -EBUSY;                                /* (means: no reply now) */
    }
    }
    return -ENOSYS;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    mk_info_t in;
    sys_info(&in);
    self = in.self_pid;
    msg_t m = { .w = { DISK_INFO } };
    static siefs_env_t env = { .read = d_read, .write = d_write, .flush = d_flush,
                               .alloc = malloc, .free = free, .now = now, .commit_ns = 1L << 62 };
    long r = call_named(&disk, "disk", &m, 1) < 0 || (long)m.w[0] <= 0 ? -EIO : 0;
    env.nblocks = m.w[0];
    if (!r) r = siefs_mount(&env, &fs);
    if (r) {                     /* no file system: still answer (-EIO), so programs do not wait forever */
        printf("fs: cannot mount the disk (error %ld)\n", -r);
        port = port_create("fs");
        for (long from = ipc_recv(port, &m); ; from = from > 0 ? ipc_reply_recv(from, port, &m) : ipc_recv(port, &m))
            m = (msg_t){ .w = { -EIO } };
    }

    /* Files deleted while open, left over from the last run: delete them. */
    char nm[SIEFS_NAME_MAX + 1];
    if (siefs_lookup(fs, SIEFS_ROOT, ".orphans", &orphans) &&
        siefs_create(fs, SIEFS_ROOT, ".orphans", SIEFS_IFDIR | 0700, 0, 0, &orphans) == 0) changed();
    for (siefs_dirent_t de; ; ) {
        uint64_t cur = 2;
        if (siefs_readdir(fs, orphans, &cur, &de) != 1) break;
        strlcpy(nm, de.name, sizeof nm);
        if (siefs_unlink(fs, orphans, nm)) break;
        changed();
    }

    port = port_create("fs");
    thread_start(ticker, 0, 8192);
    m = (msg_t){ .rbuf = buf, .rlen = FS_MAX + PATH_MAX };
    long from = ipc_recv(port, &m);
    for (;;) {
        const void *out = 0;
        uint64_t outlen = 0, w1 = 0;
        r = -EINVAL;
        if (from > 0) {
            buf[m.rlen < sizeof buf ? m.rlen : sizeof buf - 1] = 0;
            r = serve(&m, from, &out, &outlen, &w1);
        }
        int noreply = from <= 0 || (m.w[0] == FS_TICK && r == -EBUSY && tick_from == from);
        m = (msg_t){ .w = { r, w1 }, .sbuf = out, .slen = outlen, .rbuf = buf, .rlen = FS_MAX + PATH_MAX };
        from = noreply ? ipc_recv(port, &m) : ipc_reply_recv(from, port, &m);
    }
}
