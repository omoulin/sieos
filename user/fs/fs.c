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
 * Volumes and snapshots (docs/siefs.md): a user's home can be a volume of
 * its own (FS_MKVOL); FS_SNAP takes, deletes, restores and lists the
 * snapshots of a path's volume, and a helper thread takes one of every
 * volume each hour ("auto-YYYY-MM-DD-HH00"), kept a day. When the disk is
 * full, a write first lets the oldest hourly snapshot go and tries again.
 *
 * Encrypted volumes (a home, docs/siefs.md): their key is wrapped by a
 * key-encryption key that auth derives from the user's password (it never
 * comes here); auth asks FS_UNLOCK at a login, FS_REKEY when the password
 * changes. A home stays open until the machine stops: what encryption
 * protects is the disk at rest.
 *
 * If this server crashes, init starts it again: it mounts the disk's last
 * commit, and programs' file handles are reopened by their library. If the
 * disk driver crashes, the requests to it are simply sent again.
 *
 * One request at a time (the library is single-threaded); a helper thread
 * only times the commits, and sleeps for good while nothing has changed.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "siefs.h"
#include "mk/crypto.h"

#define COMMIT_NS 1000000000L               /* commit 1 s after the first change: at most 1 s of
                                               changes is lost if the server (or the machine) dies */
#define PATH_MAX  1024

static siefs_t *fs;
static long disk, port;
static int self;                            /* our pid: FS_TICK comes from our own thread */
static char buf[FS_MAX + PATH_MAX + 8];     /* every message in, and every reply out */

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
static int rnd(void *b, size_t n)
{
    for (int i = 0; i < 100; i++) {                     /* the kernel may still be gathering */
        long r = sys_random(b, n);
        if (r == (long)n) return 0;
        if (r != -EAGAIN) return -1;
        sys_sleep(50000000);
    }
    return -1;
}

/* Wall-clock time: there is no clock chip driver yet, so the build's date
 * plus the time since boot, moved ahead to the newest file time asked
 * about (a disk made after this build): a file written now is never older
 * than the files it was made from (make compares those times). */
static int64_t ahead;
static int64_t now(void) { return (int64_t)EPOCH * 1000000000 + sys_clock() + ahead; }
static void seen(int64_t t) { int64_t n = now(); if (t >= n) ahead += t - n + 1; }

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
    for (;;) {
        msg_t m = { .w = { (uint64_t)arg } };           /* FS_TICK, or FS_AUTOSNAP */
        if (ipc_call(port, &m) < 0) return;
        sys_sleep(m.w[0]);
    }
}

/* ---- Open files. A handle is an index in this table; pid 0 = free. */
typedef struct { int pid, flags, orphan; uint64_t ino, odir; } hnd_t;
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
            if (!hnd[i].pid) { hnd[i] = (hnd_t){ pid, flags, 0, ino, 0 }; return i; }
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

/* The name of an orphan in .orphans: its object number in hex. */
static void orphan_name(uint64_t ino, char *s) { int i = 16; s[i] = 0; while (i--) { s[i] = "0123456789abcdef"[ino & 15]; ino >>= 4; } }

/* Each volume's ".orphans" (in its root: a file cannot be renamed into
 * another volume): open files whose last name went. Emptied the first time
 * this server run needs it, since what is there then is left from before. */
static struct { uint64_t root, dir; } orph[64];
static int norph;
static long orphans_of(uint64_t ino, uint64_t *dir)
{
    uint64_t root, d, cur;
    siefs_dirent_t de;
    long r = siefs_volume_root(fs, ino, &root);
    if (r) return r;
    for (int i = 0; i < norph; i++) if (orph[i].root == root) { *dir = orph[i].dir; return 0; }
    if (siefs_lookup(fs, root, ".orphans", &d) && (r = siefs_create(fs, root, ".orphans", SIEFS_IFDIR | 0700, 0, 0, &d))) return r;
    for (char nm[SIEFS_NAME_MAX + 1]; cur = 2, siefs_readdir(fs, d, &cur, &de) == 1; ) {
        strlcpy(nm, de.name, sizeof nm);
        if (siefs_unlink(fs, d, nm)) break;
    }
    changed();
    if (norph < 64) orph[norph++] = (typeof(orph[0])){ root, d };
    *dir = d;
    return 0;
}

static void hnd_close(hnd_t *h)
{
    h->pid = 0;
    if (h->orphan && !is_open(h->ino)) {          /* the last user of a deleted file: delete it now */
        char nm[17];
        orphan_name(h->ino, nm);
        siefs_unlink(fs, h->odir, nm);
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
        if (de.type == 4 && strcmp(de.name, ".orphans") && search(de.ino, ino, path, len + n + 1, depth + 1)) return 1;
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

/* ---- Hourly snapshots: "auto-YYYY-MM-DD-HH00" of every volume, kept a
 * day; the oldest ones also go first when the disk is full. */
#define KEEP_NS (24LL * 3600 * 1000000000)
static void auto_name(int64_t ns, char *s)              /* "auto-2026-10-15-1400" */
{
    char d[20];
    date_str(ns, d);                                    /* "2026-10-15 14:07" */
    d[10] = '-'; memcpy(d + 13, "00", 3);
    memcpy(s, "auto-", 5); memcpy(s + 5, d, 16);
}
static int is_auto(const char *n) { return !memcmp(n, "auto-", 5); }

/* The oldest hourly snapshot of any volume goes: 1 if there was one. */
static int drop_oldest_auto(void)
{
    uint64_t vc = 0, root, best_root = 0;
    siefs_snap_t sn;
    char best[64] = "";
    int64_t when = 0;
    while (siefs_volume_next(fs, &vc, &root) == 1)
        for (uint64_t cur = 0; siefs_snapshot_list(fs, root, &cur, &sn) == 1; )
            if (is_auto(sn.name) && (!best[0] || sn.created < when)) { strlcpy(best, sn.name, sizeof best); when = sn.created; best_root = root; }
    if (!best[0] || siefs_snapshot_delete(fs, best_root, best)) return 0;
    printf("fs: disk full: snapshot %s let go\n", best);
    return 1;
}

static void autosnap(void)
{
    char nm[32], old[64][64];
    uint64_t vc = 0, root;
    siefs_snap_t sn;
    siefs_statfs_t sf;
    int64_t t = now();
    auto_name(t, nm);
    while (siefs_volume_next(fs, &vc, &root) == 1) {
        int n = 0;
        for (uint64_t cur = 0; siefs_snapshot_list(fs, root, &cur, &sn) == 1 && n < 64; )
            if (is_auto(sn.name) && t - sn.created > KEEP_NS) strlcpy(old[n++], sn.name, sizeof old[0]);
        for (int i = 0; i < n; i++) siefs_snapshot_delete(fs, root, old[i]);
        long r = siefs_snapshot(fs, root, nm);
        if (r && r != -EEXIST) printf("fs: hourly snapshot %s: error %ld\n", nm, -r);
    }
    while (!siefs_statfs(fs, &sf) && sf.free + sf.pending < sf.blocks / 10 && drop_oldest_auto()) ;   /* keep a tenth free */
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
        if (r == -ENOSPC && drop_oldest_auto()) r = siefs_write(fs, h->ino, off, buf, m->rlen);
        if (r > 0) changed();
        return r;
    }
    case FS_STAT: case FS_FSTAT:
        if (m->w[0] == FS_FSTAT) { if (!(h = hnd_get(m->w[1], pid))) return -EBADF; ino = h->ino; }
        else if ((r = walk(s1, !m->w[1], c, &ino, 0, 0))) return r;    /* w[1] = 1: the link itself */
        if ((r = siefs_stat(fs, ino, (siefs_stat_t *)buf))) return r;
        seen(((siefs_stat_t *)buf)->mtime);
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
            uint64_t od;
            orphan_name(ino, nm);
            if (!(r = orphans_of(ino, &od)) && !(r = siefs_rename(fs, dir, name, od, nm)))
                for (int i = 0; i < nhnd; i++) if (hnd[i].pid && hnd[i].ino == ino) { hnd[i].orphan = 1; hnd[i].odir = od; }
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
    case FS_SNAP: {
        uint64_t root, cur = m->w[2], len = 0, max = m->w[3] < FS_MAX ? m->w[3] : FS_MAX;
        siefs_snap_t sn;
        if ((r = walk(s1, 1, c, &ino, 0, 0)) || (r = siefs_volume_root(fs, ino, &root)) || (r = siefs_stat(fs, root, &st))) return r;
        if (m->w[1] == SNAP_LIST) {
            if ((r = may(c, &st, 4))) return r;
            for (;;) {
                uint64_t before = cur;
                if ((r = siefs_snapshot_list(fs, root, &cur, &sn)) <= 0) { if (r < 0) return r; cur = 0; break; }
                size_t n = strlen(sn.name);
                if (len + 9 + n > max) { cur = before; break; }
                memcpy(buf + len, &sn.created, 8);
                buf[len + 8] = (char)n;
                memcpy(buf + len + 9, sn.name, n);
                len += 9 + n;
            }
            *out = buf; *outlen = len; *w1 = cur;
            return (long)len;
        }
        if (c.uid && c.uid != st.uid) return -EPERM;  /* the volume's owner (a home's user), or root */
        r = m->w[1] == SNAP_TAKE ? siefs_snapshot(fs, root, s2) : m->w[1] == SNAP_DELETE ? siefs_snapshot_delete(fs, root, s2) :
            m->w[1] == SNAP_RESTORE ? siefs_snapshot_restore(fs, root, s2) : -EINVAL;
        if (!r) printf("fs: snapshot %s of %s: %s\n", s2, s1, m->w[1] == SNAP_TAKE ? "taken" : m->w[1] == SNAP_DELETE ? "deleted" : "restored");
        return r;
    }
    case FS_MKVOL:
        if (c.uid) return -EPERM;
        r = walk(s1, 0, c, &ino, &dir, name);
        if (!r) return -EEXIST;
        if (r != -ENOENT || !dir) return r;
        const fs_key_t *key = (const fs_key_t *)s2;   /* (after the path, if it came) */
        int enc = s2 + sizeof *key <= buf + m->rlen;
        r = enc ? siefs_mkvol_enc(fs, dir, name, (uint32_t)m->w[1], (uint32_t)m->w[2], (uint32_t)m->w[3],
                                  (const siefs_kdf_t *)key, key->kek, &ino)
                : siefs_mkvol(fs, dir, name, (uint32_t)m->w[1], (uint32_t)m->w[2], (uint32_t)m->w[3], &ino);
        wipe(buf, m->rlen);
        if (r) return r;
        printf("fs: volume %s made%s\n", s1, enc ? ", encrypted" : "");
        changed();
        return 0;
    case FS_UNLOCK: case FS_REKEY: case FS_KDF: {
        if (c.uid) return -EPERM;
        uint64_t root, od;
        char path[96];
        strlcpy(path, s1, sizeof path);
        const uint8_t *kek = (const uint8_t *)s2;     /* FS_UNLOCK: the key; FS_REKEY: an fs_key_t, then the old key */
        const fs_key_t *nk = (const fs_key_t *)s2;
        size_t have = (size_t)(buf + m->rlen - s2);
        if (!(r = walk(s1, 1, c, &ino, 0, 0)) && !(r = siefs_volume_root(fs, ino, &root))) {
            if (m->w[0] == FS_KDF) {
                if (!(r = siefs_kdf(fs, root, (siefs_kdf_t *)buf))) { *out = buf; *outlen = sizeof(siefs_kdf_t); }
                return r;
            }
            if (m->w[0] == FS_UNLOCK) r = have >= 32 ? siefs_unlock(fs, root, kek) : -EINVAL;
            else if (have < sizeof *nk) r = -EINVAL;
            else {
                if (have >= sizeof *nk + 32) r = siefs_unlock(fs, root, (const uint8_t *)(nk + 1));   /* (locked: the old key) */
                if (!r) r = siefs_rekey(fs, root, (const siefs_kdf_t *)nk, nk->kek);
            }
            if (!r && m->w[0] == FS_UNLOCK) orphans_of(root, &od);
        }
        wipe(buf, m->rlen);
        if (!r) printf("fs: %s %s\n", path, m->w[0] == FS_UNLOCK ? "unlocked" : "has a new key");
        else if (r == -EACCES) printf("fs: %s: not its key\n", path);
        return r;
    }
    case FS_RMVOL:
        if (c.uid) return -EPERM;
        if ((r = walk(s1, 0, c, &ino, &dir, name))) return r;
        if (!dir) return -EBUSY;
        siefs_rmdir(fs, ino, ".orphans");             /* (its own, if empty) */
        for (int i = 0; i < norph; i++) if (orph[i].root == ino) orph[i] = orph[--norph];
        if ((r = siefs_rmvol(fs, dir, name))) return r;
        changed();
        return 0;
    case FS_AUTOSNAP: {
        if (pid != self) return -EPERM;
        autosnap();
        int64_t t = now(), hour = 3600LL * 1000000000;
        return hour - t % hour + 1000000000;         /* the next hour, and a second */
    }
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
                               .alloc = malloc, .free = free, .now = now, .commit_ns = 1L << 62, .random = rnd };
    long r = call_named(&disk, "disk", &m, 1) < 0 || (long)m.w[0] <= 0 ? -EIO : 0;
    env.nblocks = m.w[0];
    if (!r) r = siefs_mount(&env, &fs);
    if (r) {                     /* no file system: still answer (-EIO), so programs do not wait forever */
        printf("fs: cannot mount the disk (error %ld)\n", -r);
        port = port_create("fs");
        for (long from = ipc_recv(port, &m); ; from = from > 0 ? ipc_reply_recv(from, port, &m) : ipc_recv(port, &m))
            m = (msg_t){ .w = { -EIO } };
    }
    /* The disk is bigger than the file system (a small image written to a
     * big key or card; vblk extended its partition): take the rest, once. */
    siefs_statfs_t sf;
    siefs_statfs(fs, &sf);
    if (env.nblocks > sf.blocks) {
        uint64_t was = sf.blocks;
        if ((r = siefs_grow(fs, env.nblocks))) printf("fs: cannot grow the root file system (error %ld)\n", -r);
        else if (!siefs_statfs(fs, &sf) && sf.blocks > was)
            printf("fs: grew the root partition from %lu MiB to %lu MiB\n", (unsigned long)(was >> 8), (unsigned long)(sf.blocks >> 8));
    }

    /* Files deleted while open, left over from the last run: delete them. */
    uint64_t vc = 0, root, od;
    while (siefs_volume_next(fs, &vc, &root) == 1) orphans_of(root, &od);

    port = port_create("fs");
    thread_start(ticker, (void *)FS_TICK, 8192);
    thread_start(ticker, (void *)FS_AUTOSNAP, 8192);
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
