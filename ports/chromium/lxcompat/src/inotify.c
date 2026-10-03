/*
 * inotify.c - Linux's inotify on SIEOS's event ports (File Event
 * Notification, port_associate(3C) with PORT_SOURCE_FILE).
 *
 * An inotify descriptor is one end of a socket pair; a thread of the
 * instance waits on its event port and writes struct inotify_event records
 * into the other end, so that the descriptor reads, polls and FIONREADs as
 * Linux's does.  The application closing it hangs the pair up: the thread
 * sees it on the port (PORT_SOURCE_FD) and ends.
 *
 * A watched file reports IN_MODIFY (IN_CLOSE_WRITE when only that is
 * asked: closes are not seen), IN_ATTRIB, IN_ACCESS, IN_DELETE_SELF and
 * IN_MOVE_SELF (then IN_IGNORED: the watch goes, as the path no longer
 * names the file), IN_UNMOUNT.  A watched directory is kept as a list of
 * its names: when FEN reports it modified, the list is compared with the
 * directory's, giving IN_CREATE, IN_DELETE and, for a name gone and one
 * come with the same inode, IN_MOVED_FROM/IN_MOVED_TO with one cookie.
 * Its first 256 regular files are watched too, for IN_MODIFY and
 * IN_ATTRIB with their names.  File events are reported once: the layer
 * associates again after each, with the times it then sees.
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
#include <poll.h>
#include <port.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>

#define MAX_CHILDREN 256

struct ent {
    char *name;
    ino_t ino;
    bool dir;
};

/* An association: a watch's path, or a file in a watched directory. */
struct assoc {
    uint64_t id;                     /* the port events' user value */
    struct watch *w;
    char *name;                      /* a child's name; NULL: the watch's own path */
    char *path;
    struct file_obj fo;
    struct assoc *next;
};

struct watch {
    int wd;
    uint32_t mask;
    char *path;
    dev_t dev;
    ino_t ino;
    bool dir;
    struct ent *ents;                /* a directory's names, sorted */
    int nents;
    struct watch *next;
};

struct inst {
    int app, mine, port;
    pthread_mutex_t lock;
    struct watch *watches;
    struct assoc *assocs;
    int next_wd;
    uint64_t next_id;
    uint32_t next_cookie;
    bool dead;
    struct buf q;                    /* events not written yet (lock held) */
    bool overflow;
    struct inst *next;
};

static struct inst *insts;
static pthread_mutex_t insts_lock = PTHREAD_MUTEX_INITIALIZER;

static struct inst *inst_of(int fd)
{
    pthread_mutex_lock(&insts_lock);
    struct inst *in = insts;
    while (in && (in->app != fd || in->dead))
        in = in->next;
    pthread_mutex_unlock(&insts_lock);
    return in;
}

/* ---------------- events ---------------- */

#define MAX_QUEUED (16384 * sizeof(struct inotify_event))   /* (Linux's max_queued_events, about) */

/* An event, queued (inst->lock held): the thread writes the queue out. */
static void emit(struct inst *in, int wd, uint32_t mask, uint32_t cookie, const char *name)
{
    size_t nl = name ? strlen(name) + 1 : 0, len = nl ? (nl + 15) & ~(size_t)15 : 0;
    size_t total = sizeof(struct inotify_event) + len;
    char rec[sizeof(struct inotify_event) + NAME_MAX + 16];
    if (total > sizeof rec)
        return;
    if (in->q.n + total > MAX_QUEUED) {
        if (!in->overflow) {
            struct inotify_event ov = { -1, IN_Q_OVERFLOW, 0, 0 };
            bmem(&in->q, &ov, sizeof ov);
        }
        in->overflow = true;
        return;
    }
    in->overflow = false;
    struct inotify_event *ev = (struct inotify_event *)rec;
    memset(rec, 0, total);
    ev->wd = wd;
    ev->mask = mask;
    ev->cookie = cookie;
    ev->len = (uint32_t)len;
    if (nl)
        memcpy(ev->name, name, nl);
    bmem(&in->q, rec, total);
}

/* The queued events, to the application's end (without the lock: it may wait for the reader). */
static void flush(struct inst *in)
{
    pthread_mutex_lock(&in->lock);
    struct buf q = in->q;
    in->q = (struct buf){ 0 };
    pthread_mutex_unlock(&in->lock);
    for (size_t off = 0; off < q.n;) {
        ssize_t n = send(in->mine, q.s + off, q.n - off, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;                           /* (the application closed it: the thread ends soon) */
        off += n;
    }
    free(q.s);
}

/* ---------------- associations (inst->lock held) ---------------- */

static void fill_times(struct file_obj *fo, const struct stat *st)
{
    fo->fo_atime = st->st_atim;
    fo->fo_mtime = st->st_mtim;
    fo->fo_ctime = st->st_ctim;
}

static int file_events(const struct watch *w, bool child)
{
    int ev = 0;
    if (w->mask & (IN_MODIFY | IN_CLOSE_WRITE))
        ev |= FILE_MODIFIED;
    if (w->mask & IN_ATTRIB)
        ev |= FILE_ATTRIB;
    if (!child && (w->mask & IN_ACCESS))
        ev |= FILE_ACCESS;
    if (!child && w->dir && (w->mask & (IN_CREATE | IN_DELETE | IN_MOVE)))
        ev |= FILE_MODIFIED;
    if (w->mask & IN_DONT_FOLLOW)
        ev |= FILE_NOFOLLOW;
    return ev;
}

/* Watch path (for w, or its child name) from now: the times it has now. */
static int associate(struct inst *in, struct watch *w, const char *name, struct assoc *reuse)
{
    struct assoc *a = reuse;
    if (!a) {
        a = calloc(1, sizeof *a);
        if (!a)
            return -1;
        a->w = w;
        if (name) {
            a->name = strdup(name);
            size_t n = strlen(w->path) + strlen(name) + 2;
            a->path = malloc(n);
            if (a->name && a->path)
                snprintf(a->path, n, "%s/%s", w->path, name);
        } else {
            a->path = strdup(w->path);
        }
        if (!a->path || (name && !a->name)) {
            free(a->name);
            free(a->path);
            free(a);
            return -1;
        }
        a->next = in->assocs;
        in->assocs = a;
    }
    a->id = ++in->next_id;
    struct stat st;
    int (*st_fn)(const char *, struct stat *) = (w->mask & IN_DONT_FOLLOW) ? lstat : stat;
    memset(&a->fo, 0, sizeof a->fo);
    a->fo.fo_name = a->path;
    if (st_fn(a->path, &st) == 0)
        fill_times(&a->fo, &st);
    return port_associate(in->port, PORT_SOURCE_FILE, (uintptr_t)&a->fo, file_events(w, name != NULL),
                          (void *)(uintptr_t)a->id);
}

static void drop_assoc(struct inst *in, struct assoc *a)
{
    port_dissociate(in->port, PORT_SOURCE_FILE, (uintptr_t)&a->fo);
    for (struct assoc **pp = &in->assocs; *pp; pp = &(*pp)->next)
        if (*pp == a) {
            *pp = a->next;
            break;
        }
    free(a->name);
    free(a->path);
    free(a);
}

static int ent_cmp(const void *x, const void *y)
{
    return strcmp(((const struct ent *)x)->name, ((const struct ent *)y)->name);
}

static void free_ents(struct ent *e, int n)
{
    for (int i = 0; i < n; i++)
        free(e[i].name);
    free(e);
}

static int list_dir(const char *path, struct ent **out)
{
    DIR *d = opendir(path);
    *out = NULL;
    if (!d)
        return 0;
    int n = 0, cap = 0;
    struct ent *e = NULL;
    for (struct dirent *de; (de = readdir(d));) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            struct ent *ne = realloc(e, cap * sizeof *e);
            if (!ne)
                break;
            e = ne;
        }
        bool dir = de->d_type == DT_DIR;
        if (de->d_type == DT_UNKNOWN) {
            struct stat st;
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/%s", path, de->d_name);
            dir = lstat(p, &st) == 0 && S_ISDIR(st.st_mode);
        }
        e[n].name = strdup(de->d_name);
        e[n].ino = de->d_ino;
        e[n].dir = dir;
        if (e[n].name)
            n++;
    }
    closedir(d);
    qsort(e, n, sizeof *e, ent_cmp);
    *out = e;
    return n;
}

/* The directory's children: watched while there are few enough. */
static void watch_children(struct inst *in, struct watch *w)
{
    if (!(w->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB)))
        return;
    int k = 0;
    for (int i = 0; i < w->nents && k < MAX_CHILDREN; i++) {
        if (w->ents[i].dir)
            continue;
        bool have = false;
        for (struct assoc *a = in->assocs; a && !have; a = a->next)
            have = a->w == w && a->name && !strcmp(a->name, w->ents[i].name);
        if (!have)
            associate(in, w, w->ents[i].name, NULL);
        k++;
    }
}

static void remove_watch(struct inst *in, struct watch *w, bool ignored)
{
    for (struct assoc *a = in->assocs, *next; a; a = next) {
        next = a->next;
        if (a->w == w)
            drop_assoc(in, a);
    }
    for (struct watch **pp = &in->watches; *pp; pp = &(*pp)->next)
        if (*pp == w) {
            *pp = w->next;
            break;
        }
    if (ignored)
        emit(in, w->wd, IN_IGNORED, 0, NULL);
    free_ents(w->ents, w->nents);
    free(w->path);
    free(w);
}

/* A directory changed: what came and went since its list. */
static void dir_diff(struct inst *in, struct watch *w)
{
    struct ent *now;
    int n = list_dir(w->path, &now), i = 0, j = 0;
    struct ent *gone[64], *come[64];
    int ng = 0, nc = 0;
    while (i < w->nents || j < n) {
        int c = i == w->nents ? 1 : j == n ? -1 : strcmp(w->ents[i].name, now[j].name);
        if (c == 0 && w->ents[i].ino != now[j].ino)
            c = 2;                               /* replaced */
        if (c < 0 || c == 2) {
            if (ng < 64)
                gone[ng++] = &w->ents[i];
            else
                emit(in, w->wd, IN_DELETE | (w->ents[i].dir ? IN_ISDIR : 0), 0, w->ents[i].name);
            i++;
        }
        if (c > 0) {
            if (nc < 64)
                come[nc++] = &now[j];
            else
                emit(in, w->wd, IN_CREATE | (now[j].dir ? IN_ISDIR : 0), 0, now[j].name);
            j++;
        }
        if (c == 0) {
            i++;
            j++;
        }
    }
    for (int g = 0; g < ng; g++) {
        struct ent *to = NULL;
        for (int c = 0; c < nc && !to; c++)
            if (come[c] && come[c]->ino == gone[g]->ino)
                to = come[c], come[c] = NULL;
        uint32_t isdir = gone[g]->dir ? IN_ISDIR : 0;
        if (to) {
            uint32_t cookie = ++in->next_cookie;
            if (w->mask & IN_MOVED_FROM)
                emit(in, w->wd, IN_MOVED_FROM | isdir, cookie, gone[g]->name);
            if (w->mask & IN_MOVED_TO)
                emit(in, w->wd, IN_MOVED_TO | isdir, cookie, to->name);
        } else if (w->mask & IN_DELETE) {
            emit(in, w->wd, IN_DELETE | isdir, 0, gone[g]->name);
        }
        /* a file that went: its association too */
        for (struct assoc *a = in->assocs, *next; a; a = next) {
            next = a->next;
            if (a->w == w && a->name && !strcmp(a->name, gone[g]->name))
                drop_assoc(in, a);
        }
    }
    for (int c = 0; c < nc; c++)
        if (come[c] && (w->mask & IN_CREATE))
            emit(in, w->wd, IN_CREATE | (come[c]->dir ? IN_ISDIR : 0), 0, come[c]->name);
    free_ents(w->ents, w->nents);
    w->ents = now;
    w->nents = n;
    watch_children(in, w);
}

static void file_event(struct inst *in, struct assoc *a, int ev)
{
    struct watch *w = a->w;
    if (a->name) {                               /* a file in a watched directory */
        if (ev & (FILE_DELETE | FILE_RENAME_FROM | FILE_RENAME_TO | UNMOUNTED | MOUNTEDOVER)) {
            drop_assoc(in, a);                   /* (the directory's own event tells the rest) */
            return;
        }
        if ((ev & FILE_MODIFIED) && (w->mask & (IN_MODIFY | IN_CLOSE_WRITE)))
            emit(in, w->wd, (w->mask & IN_MODIFY) ? IN_MODIFY : IN_CLOSE_WRITE, 0, a->name);
        if ((ev & FILE_ATTRIB) && (w->mask & IN_ATTRIB))
            emit(in, w->wd, IN_ATTRIB, 0, a->name);
        associate(in, w, a->name, a);
        return;
    }
    uint32_t isdir = w->dir ? IN_ISDIR : 0;
    if (ev & (UNMOUNTED | MOUNTEDOVER)) {
        emit(in, w->wd, IN_UNMOUNT, 0, NULL);
        remove_watch(in, w, true);
        return;
    }
    if (ev & FILE_DELETE) {
        if (w->mask & IN_DELETE_SELF)
            emit(in, w->wd, IN_DELETE_SELF, 0, NULL);
        remove_watch(in, w, true);
        return;
    }
    if (ev & (FILE_RENAME_FROM | FILE_RENAME_TO)) {
        if (w->mask & IN_MOVE_SELF)
            emit(in, w->wd, IN_MOVE_SELF, 0, NULL);
        remove_watch(in, w, true);
        return;
    }
    if (w->dir && (ev & FILE_MODIFIED))
        dir_diff(in, w);
    else if ((ev & FILE_MODIFIED) && (w->mask & (IN_MODIFY | IN_CLOSE_WRITE)))
        emit(in, w->wd, ((w->mask & IN_MODIFY) ? IN_MODIFY : IN_CLOSE_WRITE) | isdir, 0, NULL);
    if ((ev & FILE_ATTRIB) && (w->mask & IN_ATTRIB))
        emit(in, w->wd, IN_ATTRIB | isdir, 0, NULL);
    if ((ev & FILE_ACCESS) && (w->mask & IN_ACCESS))
        emit(in, w->wd, IN_ACCESS | isdir, 0, NULL);
    if (w->mask & IN_ONESHOT) {
        remove_watch(in, w, true);
        return;
    }
    associate(in, w, NULL, a);
}

/* ---------------- the instance's thread ---------------- */

static void inst_free(struct inst *in)
{
    while (in->watches)
        remove_watch(in, in->watches, false);
    close(in->port);
    close(in->mine);
    pthread_mutex_destroy(&in->lock);
    free(in->q.s);
    free(in);
}

static void *inst_main(void *arg)
{
    struct inst *in = arg;
    for (;;) {
        port_event_t pe;
        if (port_get(in->port, &pe, NULL) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pe.portev_source == PORT_SOURCE_FD) {
            char c;
            ssize_t n = recv(in->mine, &c, 1, MSG_DONTWAIT);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
                break;                           /* the application closed it */
            port_associate(in->port, PORT_SOURCE_FD, in->mine, POLLIN, 0);
            continue;
        }
        if (pe.portev_source == PORT_SOURCE_FILE) {
            pthread_mutex_lock(&in->lock);
            uint64_t id = (uintptr_t)pe.portev_user;
            struct assoc *a = in->assocs;
            while (a && a->id != id)
                a = a->next;
            if (a)
                file_event(in, a, pe.portev_events);
            pthread_mutex_unlock(&in->lock);
        }
        flush(in);                               /* (PORT_SOURCE_USER: inotify_rm_watch's) */
    }
    pthread_mutex_lock(&insts_lock);
    in->dead = true;
    for (struct inst **pp = &insts; *pp; pp = &(*pp)->next)
        if (*pp == in) {
            *pp = in->next;
            break;
        }
    pthread_mutex_unlock(&insts_lock);
    pthread_mutex_lock(&in->lock);
    pthread_mutex_unlock(&in->lock);
    inst_free(in);
    return 0;
}

/* ---------------- the interface ---------------- */

int inotify_init1(int flags)
{
    if (flags & ~(IN_NONBLOCK | IN_CLOEXEC)) {
        errno = EINVAL;
        return -1;
    }
    struct inst *in = calloc(1, sizeof *in);
    if (!in)
        return -1;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        free(in);
        return -1;
    }
    in->app = sv[0];
    in->mine = sv[1];
    in->port = port_create();
    if (in->port < 0) {
        int e = errno;
        close(sv[0]);
        close(sv[1]);
        free(in);
        errno = e;
        return -1;
    }
    fcntl(in->port, F_SETFD, FD_CLOEXEC);
    if (!(flags & IN_CLOEXEC))
        fcntl(in->app, F_SETFD, 0);
    if (flags & IN_NONBLOCK)
        fcntl(in->app, F_SETFL, fcntl(in->app, F_GETFL) | O_NONBLOCK);
    pthread_mutex_init(&in->lock, NULL);
    port_associate(in->port, PORT_SOURCE_FD, in->mine, POLLIN, 0);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 128 * 1024);
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    pthread_t t;
    int r = pthread_create(&t, &attr, inst_main, in);
    pthread_sigmask(SIG_SETMASK, &old, 0);
    pthread_attr_destroy(&attr);
    if (r) {
        close(in->app);
        close(in->mine);
        close(in->port);
        free(in);
        errno = r;
        return -1;
    }
    pthread_mutex_lock(&insts_lock);
    in->next = insts;
    insts = in;
    pthread_mutex_unlock(&insts_lock);
    return in->app;
}

int inotify_init(void)
{
    return inotify_init1(0);
}

int inotify_add_watch(int fd, const char *path, uint32_t mask)
{
    struct inst *in = inst_of(fd);
    if (!in) {
        errno = fcntl(fd, F_GETFD) < 0 ? EBADF : EINVAL;
        return -1;
    }
    if (!(mask & IN_ALL_EVENTS)) {
        errno = EINVAL;
        return -1;
    }
    struct stat st;
    if (((mask & IN_DONT_FOLLOW) ? lstat(path, &st) : stat(path, &st)) < 0)
        return -1;
    if ((mask & IN_ONLYDIR) && !S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    char abs[PATH_MAX];
    if (path[0] == '/') {
        snprintf(abs, sizeof abs, "%s", path);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd))
            return -1;
        if (snprintf(abs, sizeof abs, "%s/%s", cwd, path) >= (int)sizeof abs) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }
    pthread_mutex_lock(&in->lock);
    struct watch *w = in->watches;
    while (w && !(w->dev == st.st_dev && w->ino == st.st_ino))
        w = w->next;
    int r;
    if (w) {                                     /* the same file: its watch, its mask changed */
        w->mask = (mask & IN_MASK_ADD) ? w->mask | mask : mask;
        for (struct assoc *a = in->assocs; a; a = a->next)
            if (a->w == w && !a->name)
                associate(in, w, NULL, a);
        if (w->dir)
            watch_children(in, w);
        r = w->wd;
    } else if (!(w = calloc(1, sizeof *w)) || !(w->path = strdup(abs))) {
        free(w);
        errno = ENOMEM;
        r = -1;
    } else {
        w->wd = ++in->next_wd;
        w->mask = mask;
        w->dev = st.st_dev;
        w->ino = st.st_ino;
        w->dir = S_ISDIR(st.st_mode);
        w->next = in->watches;
        in->watches = w;
        if (w->dir)
            w->nents = list_dir(w->path, &w->ents);
        if (associate(in, w, NULL, NULL) < 0) {
            int e = errno;
            remove_watch(in, w, false);
            errno = e;
            r = -1;
        } else {
            if (w->dir)
                watch_children(in, w);
            r = w->wd;
        }
    }
    pthread_mutex_unlock(&in->lock);
    return r;
}

int inotify_rm_watch(int fd, int wd)
{
    struct inst *in = inst_of(fd);
    if (!in) {
        errno = fcntl(fd, F_GETFD) < 0 ? EBADF : EINVAL;
        return -1;
    }
    pthread_mutex_lock(&in->lock);
    struct watch *w = in->watches;
    while (w && w->wd != wd)
        w = w->next;
    if (w)
        remove_watch(in, w, true);
    pthread_mutex_unlock(&in->lock);
    if (w)
        port_send(in->port, 0, 0);               /* the thread writes IN_IGNORED out */
    if (!w) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}
