/*
 * namei.c - Path name resolution.
 *
 * Paths are resolved from the process's root (absolute) or from a start
 * directory (the cwd, or the directory of an *at() call).  Mount points are
 * crossed in both directions, ".." never climbs above the process root, and
 * symbolic links are followed (at most MAXSYMLINKS per lookup: ELOOP).
 */
#include "fs.h"
#include "proc.h"
#include "mm.h"

#define PBUF 1024

static struct inode *proc_root(void)
{
    return current && current->root ? current->root : root_fs->root;
}

static struct inode *namex(struct inode *start, const char *path, int flags, bool want_parent, char *name,
                           int *err)
{
    if (!*path) {
        *err = -ENOENT;
        return NULL;
    }
    char *buf = kmalloc(PBUF), *tmp = kmalloc(PBUF);
    struct inode *ip = NULL, *root = proc_root();
    int r = 0, links = 0;
    if (!buf || !tmp) {
        r = -ENOMEM;
        goto out;
    }
    if (strlen(path) >= PBUF) {
        r = -ENAMETOOLONG;
        goto out;
    }
    strcpy(buf, path);
    if (buf[0] == '/')
        ip = idup(root);
    else if (start)
        ip = idup(start);
    else
        ip = idup(current && current->cwd ? current->cwd : root);

    const char *p = buf;
    for (;;) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *comp = p;
        while (*p && *p != '/')
            p++;
        size_t len = p - comp;
        const char *rest = p;
        while (*rest == '/')
            rest++;
        if (len > 255) {
            r = -ENAMETOOLONG;
            goto out;
        }
        if (!S_ISDIR(inode_mode(ip))) {
            r = -ENOTDIR;
            goto out;
        }
        if ((r = inode_permission(ip, X_OK)) < 0)
            goto out;
        if (want_parent && !*rest) {
            memcpy(name, comp, len);
            name[len] = 0;
            kfree(buf);
            kfree(tmp);
            return ip;
        }
        if (len == 1 && comp[0] == '.') {
            p = rest;
            continue;
        }
        if (len == 2 && comp[0] == '.' && comp[1] == '.') {
            if (same_inode(ip, root)) {
                p = rest;
                continue;
            }
            while (same_inode(ip, ip->fs->root) && ip->fs->covered) {
                struct inode *up = idup(ip->fs->covered);    /* climb out of a mount */
                iput(ip);
                ip = up;
            }
            if (same_inode(ip, root)) {
                p = rest;
                continue;
            }
        }
        struct inode *next, *m;
        if ((r = vfs_lookup(ip, comp, len, &next)) < 0)
            goto out;
        while ((m = vfs_covering(next))) {       /* descend into a mount */
            iput(next);
            next = m;
        }
        if (S_ISLNK(inode_mode(next)) && (*rest || !(flags & NAMEI_NOFOLLOW))) {
            if (++links > MAXSYMLINKS) {
                iput(next);
                r = -ELOOP;
                goto out;
            }
            long n = vfs_readlink(next, tmp, PBUF - 1);
            iput(next);
            if (n <= 0) {
                r = n < 0 ? n : -ENOENT;
                goto out;
            }
            size_t rl = strlen(rest);
            if ((size_t)n + 1 + rl >= PBUF) {
                r = -ENAMETOOLONG;
                goto out;
            }
            tmp[n] = 0;
            if (rl) {
                tmp[n] = '/';
                memcpy(tmp + n + 1, rest, rl + 1);
            }
            strcpy(buf, tmp);
            p = buf;
            if (buf[0] == '/') {
                iput(ip);
                ip = idup(root);
            }
            continue;                            /* relative targets resolve from ip */
        }
        iput(ip);
        ip = next;
        p = rest;
    }
    if (want_parent)
        r = -EINVAL;                             /* no final component, e.g. "/" */
out:
    kfree(buf);
    kfree(tmp);
    if (r < 0) {
        if (ip)
            iput(ip);
        *err = r;
        return NULL;
    }
    return ip;
}

struct inode *namei_at(struct inode *start, const char *path, int flags, int *err)
{
    return namex(start, path, flags, false, NULL, err);
}

/* Resolve all but the last component; the last one is copied to name[256]. */
struct inode *nameiparent_at(struct inode *start, const char *path, char *name, int *err)
{
    return namex(start, path, 0, true, name, err);
}

struct inode *namei(const char *path, int *err)
{
    return namex(NULL, path, 0, false, NULL, err);
}

struct inode *nameiparent(const char *path, char *name, int *err)
{
    return namex(NULL, path, 0, true, name, err);
}
