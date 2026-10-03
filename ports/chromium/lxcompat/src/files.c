/*
 * files.c - The file calls that reach the layer's /proc and /sys files
 * (procfiles.c): open, openat, fopen, readlink, readlinkat, access,
 * faccessat, stat, lstat, fstatat, opendir.  A path the layer does not
 * make goes to the C library unchanged.
 *
 * A path is made absolute (relative to the directory descriptor or the
 * working directory) and its "." and ".." parts and repeated slashes
 * dropped before it is compared; only paths under /proc and /sys are.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "lx.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* The path, normalised in out, if it may be one of the layer's. */
static bool candidate(int dirfd, const char *path, char *out)
{
    if (!path || !path[0])
        return false;
    char tmp[PATH_MAX];
    if (path[0] != '/') {
        if (strncmp(path, "proc", 4) && strncmp(path, "sys", 3) && strncmp(path, "..", 2) &&
            strncmp(path, "./", 2) && strncmp(path, "self", 4) && strncmp(path, "devices", 7) &&
            (path[0] < '0' || path[0] > '9') && strncmp(path, "cpu", 3) && strncmp(path, "task", 4))
            return false;                        /* (cheap: most relative paths are not) */
        char base[PATH_MAX];
        if (dirfd == AT_FDCWD) {
            if (!getcwd(base, sizeof base))
                return false;
        } else {
            char link[64];
            snprintf(link, sizeof link, "/proc/self/fd/%d", dirfd);
            ssize_t n = REAL(readlink)(link, base, sizeof base - 1);
            if (n <= 0)
                return false;
            base[n] = 0;
        }
        if (snprintf(tmp, sizeof tmp, "%s/%s", base, path) >= (int)sizeof tmp)
            return false;
        path = tmp;
    } else if (strncmp(path, "/proc", 5) && strncmp(path, "/sys", 4) && strncmp(path, "//", 2) &&
               strncmp(path, "/.", 2)) {
        return false;
    }
    /* normalise into out */
    size_t n = 0;
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == 1 && p[0] == '.') {
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            while (n > 0 && out[n - 1] != '/')
                n--;
            if (n > 0)
                n--;
        } else {
            if (n + 1 + len >= PATH_MAX)
                return false;
            out[n++] = '/';
            memcpy(out + n, p, len);
            n += len;
        }
        p += len;
    }
    out[n] = 0;
    if (!n) {
        out[0] = '/';
        out[1] = 0;
    }
    return !strncmp(out, "/proc/", 6) || !strncmp(out, "/sys/", 5);
}

/* 1: the layer's file, opened (*fd); 0: not the layer's; -1: errno. */
static int lx_openat(int dirfd, const char *path, int flags, int *fd)
{
    char abs[PATH_MAX];
    if (!candidate(dirfd, path, abs))
        return 0;
    int save = errno;
    struct buf b = { 0 };
    int r = lx_text(abs, &b);
    if (r > 0) {
        *fd = lx_open_text(&b, flags);
        r = *fd < 0 ? -1 : 1;
    }
    free(b.s);
    if (r == 0)
        errno = save;
    return r;
}

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    int fd, r = lx_openat(AT_FDCWD, path, flags, &fd);
    if (r)
        return r < 0 ? -1 : fd;
    return REAL(open)(path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    int fd, r = lx_openat(dirfd, path, flags, &fd);
    if (r)
        return r < 0 ? -1 : fd;
    return REAL(openat)(dirfd, path, flags, mode);
}

FILE *fopen(const char *restrict path, const char *restrict mode)
{
    int fd, r = 0;
    if (mode[0] == 'r' && !strchr(mode, '+'))
        r = lx_openat(AT_FDCWD, path, O_RDONLY | (strchr(mode, 'e') ? O_CLOEXEC : 0), &fd);
    if (r < 0)
        return NULL;
    if (r > 0) {
        FILE *f = fdopen(fd, "r");
        if (!f)
            close(fd);
        return f;
    }
    return REAL(fopen)(path, mode);
}

static ssize_t link_at(int dirfd, const char *path, char *buf, size_t size, bool *mine)
{
    char abs[PATH_MAX], target[PATH_MAX];
    *mine = false;
    if (!candidate(dirfd, path, abs))
        return 0;
    int r = lx_link(abs, target, sizeof target);
    if (!r)
        return 0;
    *mine = true;
    if (r < 0)
        return -1;
    size_t n = strlen(target);
    if (n > size)
        n = size;
    memcpy(buf, target, n);
    return n;
}

ssize_t readlink(const char *restrict path, char *restrict buf, size_t size)
{
    bool mine;
    ssize_t n = link_at(AT_FDCWD, path, buf, size, &mine);
    return mine ? n : REAL(readlink)(path, buf, size);
}

ssize_t readlinkat(int dirfd, const char *restrict path, char *restrict buf, size_t size)
{
    bool mine;
    ssize_t n = link_at(dirfd, path, buf, size, &mine);
    return mine ? n : REAL(readlinkat)(dirfd, path, buf, size);
}

/* Is it one of the layer's files (1), directories (2) or links (3)?  0: not; -1: errno. */
static int kind_at(int dirfd, const char *path, size_t *size)
{
    char abs[PATH_MAX];
    if (!candidate(dirfd, path, abs))
        return 0;
    int save = errno;
    struct buf b = { 0 };
    int r = lx_text(abs, &b), k = 1;
    if (r == 0) {
        free(b.s);
        b = (struct buf){ 0 };
        r = lx_dir(abs, &b);
        k = 2;
    }
    if (r == 0) {
        char t[PATH_MAX];
        r = lx_link(abs, t, sizeof t);
        k = 3;
        b.n = r > 0 ? strlen(t) : 0;
    }
    if (size)
        *size = b.n;
    free(b.s);
    if (r == 0) {
        errno = save;
        return 0;
    }
    return r < 0 ? -1 : k;
}

int access(const char *path, int amode)
{
    int k = kind_at(AT_FDCWD, path, 0);
    if (k < 0)
        return -1;
    if (k > 0) {
        if (amode & W_OK) {
            errno = EACCES;
            return -1;
        }
        return 0;
    }
    return REAL(access)(path, amode);
}

int faccessat(int dirfd, const char *path, int amode, int flags)
{
    int k = kind_at(dirfd, path, 0);
    if (k < 0)
        return -1;
    if (k > 0) {
        if (amode & W_OK) {
            errno = EACCES;
            return -1;
        }
        return 0;
    }
    return REAL(faccessat)(dirfd, path, amode, flags);
}

static void fake_stat(struct stat *st, int k, size_t size)
{
    memset(st, 0, sizeof *st);
    st->st_mode = k == 2 ? S_IFDIR | 0555 : k == 3 ? S_IFLNK | 0777 : S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_size = k == 1 ? 0 : (off_t)size;     /* (as Linux's /proc files: 0 bytes) */
    st->st_blksize = 4096;
    st->st_dev = 0x10001;
}

static int stat_at(int dirfd, const char *path, struct stat *st, int flags, bool *mine)
{
    size_t size;
    *mine = false;
    int k = kind_at(dirfd, path, &size);
    if (k == 0)
        return 0;
    *mine = true;
    if (k < 0)
        return -1;
    if (k == 3 && !(flags & AT_SYMLINK_NOFOLLOW)) {
        char abs[PATH_MAX], t[PATH_MAX];
        candidate(dirfd, path, abs);
        lx_link(abs, t, sizeof t);
        return REAL(fstatat)(AT_FDCWD, t, st, 0);
    }
    fake_stat(st, k, size);
    return 0;
}

int stat(const char *restrict path, struct stat *restrict st)
{
    bool mine;
    int r = stat_at(AT_FDCWD, path, st, 0, &mine);
    return mine ? r : REAL(stat)(path, st);
}

int lstat(const char *restrict path, struct stat *restrict st)
{
    bool mine;
    int r = stat_at(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW, &mine);
    return mine ? r : REAL(lstat)(path, st);
}

int fstatat(int dirfd, const char *restrict path, struct stat *restrict st, int flags)
{
    bool mine;
    int r = stat_at(dirfd, path, st, flags, &mine);
    return mine ? r : REAL(fstatat)(dirfd, path, st, flags);
}

/* A directory the layer makes: its names are kept here, and readdir on
 * the DIR handed out (an open "/", a placeholder) comes back here.  The
 * list is read without a lock: entries are never freed, a closed one is
 * taken again by the next opendir. */
struct lxdir {
    struct buf names;
    size_t pos;
    struct dirent ent;
    DIR *real;                                   /* the placeholder handed out; NULL: free */
    struct lxdir *next;
};

static struct lxdir *lxdirs;

DIR *opendir(const char *path)
{
    char abs[PATH_MAX];
    if (!candidate(AT_FDCWD, path, abs))
        return REAL(opendir)(path);
    int save = errno;
    struct buf names = { 0 };
    int r = lx_dir(abs, &names);
    if (r <= 0) {
        free(names.s);
        if (r == 0)
            errno = save;
        return r == 0 ? REAL(opendir)(path) : NULL;
    }
    DIR *ph = REAL(opendir)("/");
    if (!ph) {
        free(names.s);
        return NULL;
    }
    struct lxdir *d;
    for (d = __atomic_load_n(&lxdirs, __ATOMIC_ACQUIRE); d; d = d->next) {
        DIR *none = NULL;
        if (!__atomic_load_n(&d->real, __ATOMIC_RELAXED) &&
            __atomic_compare_exchange_n(&d->real, &none, ph, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            break;
    }
    if (!d) {
        d = calloc(1, sizeof *d);
        if (!d) {
            REAL(closedir)(ph);
            free(names.s);
            return NULL;
        }
        d->real = ph;
        d->next = __atomic_load_n(&lxdirs, __ATOMIC_ACQUIRE);
        while (!__atomic_compare_exchange_n(&lxdirs, &d->next, d, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            ;
    }
    d->names = names;
    d->pos = 0;
    return ph;
}

static struct lxdir *lxdir_of(DIR *dir)
{
    for (struct lxdir *d = __atomic_load_n(&lxdirs, __ATOMIC_ACQUIRE); d; d = d->next)
        if (__atomic_load_n(&d->real, __ATOMIC_ACQUIRE) == dir)
            return d;
    return NULL;
}

struct dirent *readdir(DIR *dir)
{
    struct lxdir *d = lxdir_of(dir);
    if (!d)
        return REAL(readdir)(dir);
    if (d->pos >= d->names.n)
        return NULL;
    const char *name = d->names.s + d->pos;
    d->pos += strlen(name) + 1;
    memset(&d->ent, 0, sizeof d->ent);
    snprintf(d->ent.d_name, sizeof d->ent.d_name, "%s", name);
    d->ent.d_ino = 1 + d->pos;
    d->ent.d_type = DT_UNKNOWN;
    d->ent.d_reclen = sizeof d->ent;
    return &d->ent;
}

void rewinddir(DIR *dir)
{
    struct lxdir *d = lxdir_of(dir);
    if (d)
        d->pos = 0;
    else
        REAL(rewinddir)(dir);
}

int closedir(DIR *dir)
{
    struct lxdir *d = lxdir_of(dir);
    if (d) {
        free(d->names.s);
        d->names = (struct buf){ 0 };
        __atomic_store_n(&d->real, NULL, __ATOMIC_RELEASE);
    }
    return REAL(closedir)(dir);
}
