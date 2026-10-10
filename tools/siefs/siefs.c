/*
 * siefs - Look inside and change a SieFS image from the host.
 *
 *   siefs IMAGE ls [PATH]          siefs IMAGE tree [PATH]
 *   siefs IMAGE cat PATH           siefs IMAGE stat PATH
 *   siefs IMAGE put HOSTFILE PATH  siefs IMAGE get PATH HOSTFILE
 *   siefs IMAGE mkdir PATH         siefs IMAGE rm PATH     siefs IMAGE rmdir PATH
 *   siefs IMAGE mv FROM TO         siefs IMAGE ln FROM TO  siefs IMAGE symlink TARGET PATH
 *   siefs IMAGE chmod MODE PATH    siefs IMAGE chown UID:GID PATH
 *   siefs IMAGE truncate SIZE PATH
 *   siefs IMAGE attr PATH [NAME [VALUE]]   list, show, or set attributes
 *   siefs IMAGE rmattr PATH NAME   siefs IMAGE find NAME VALUE
 *   siefs IMAGE df
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdlib.h>
#include <string.h>
#include "host.h"

static siefs_t *fs;
static char buf[128 << 10];

static int fail(const char *what, long e) { fprintf(stderr, "%s: %s\n", what, host_err(e)); return 1; }

/* Split "/a/b/c" into the directory /a/b (as an object) and the name "c". */
static int parent(const char *path, uint64_t *dir, const char **name)
{
    const char *s = strrchr(path, '/');
    char d[4096];
    if (!s) { *dir = SIEFS_ROOT; *name = path; return 0; }
    size_t n = (size_t)(s - path);
    if (n >= sizeof d) return -SIEFS_ENAMETOOLONG;
    memcpy(d, path, n); d[n] = 0;
    *name = s + 1;
    return siefs_walk(fs, d, dir);
}

static void tree(uint64_t dir, int depth)
{
    siefs_dirent_t de;
    uint64_t cur = 2;                                    /* skip "." and ".." */
    while (siefs_readdir(fs, dir, &cur, &de) == 1) {
        printf("%*s%s%s\n", depth * 2, "", de.name, de.type == 4 ? "/" : de.type == 10 ? "@" : "");
        if (de.type == 4) tree(de.ino, depth + 1);
    }
}

static int run(int argc, char **argv)
{
    const char *cmd = argv[2], *a = argc > 3 ? argv[3] : "/", *b = argc > 4 ? argv[4] : 0, *name;
    uint64_t ino, dir;
    siefs_stat_t st;
    long e;
    if (!strcmp(cmd, "df")) {
        siefs_statfs_t s;
        siefs_statfs(fs, &s);
        printf("\"%s\": %llu blocks of 4 KiB, %llu free, %llu freed (usable after the next commits), %llu objects, commit %llu\n",
               s.label, (unsigned long long)s.blocks, (unsigned long long)s.free, (unsigned long long)s.pending,
               (unsigned long long)s.inodes, (unsigned long long)s.commits);
        return 0;
    }
    if (!strcmp(cmd, "find") && b) {
        uint64_t cur = 0;
        while ((e = siefs_find(fs, a, b, strlen(b), &cur, &ino)) == 1) printf("%llu\n", (unsigned long long)ino);
        return e < 0 ? fail("find", e) : 0;
    }
    if (!strcmp(cmd, "ls")) {
        siefs_dirent_t de;
        uint64_t cur = 0;
        if ((e = siefs_walk(fs, a, &ino))) return fail(a, e);
        while ((e = siefs_readdir(fs, ino, &cur, &de)) == 1) {
            siefs_stat(fs, de.ino, &st);
            printf("%06o %4u %4u %4u %10llu  %s\n", st.mode, st.nlink, st.uid, st.gid, (unsigned long long)st.size, de.name);
        }
        return e < 0 ? fail(a, e) : 0;
    }
    if (!strcmp(cmd, "tree")) { if ((e = siefs_walk(fs, a, &ino))) return fail(a, e); tree(ino, 0); return 0; }
    if (!strcmp(cmd, "cat") || (!strcmp(cmd, "get") && b)) {
        FILE *out = b ? fopen(b, "wb") : stdout;
        if (!out) { fprintf(stderr, "%s: cannot create\n", b); return 1; }
        if ((e = siefs_walk(fs, a, &ino))) return fail(a, e);
        for (uint64_t off = 0; (e = siefs_read(fs, ino, off, buf, sizeof buf)) > 0; off += (uint64_t)e) fwrite(buf, 1, (size_t)e, out);
        if (b) fclose(out);
        return e < 0 ? fail(a, e) : 0;
    }
    if (!strcmp(cmd, "put") && b) {
        FILE *in = fopen(a, "rb");
        size_t n;
        if (!in) { fprintf(stderr, "%s: cannot read\n", a); return 1; }
        if ((e = parent(b, &dir, &name))) return fail(b, e);
        e = siefs_create(fs, dir, name, SIEFS_IFREG | 0644, 0, 0, &ino);
        if (e == -SIEFS_EEXIST) e = siefs_walk(fs, b, &ino) || siefs_truncate(fs, ino, 0);
        for (uint64_t off = 0; !e && (n = fread(buf, 1, sizeof buf, in)) > 0; off += n)
            if ((e = siefs_write(fs, ino, off, buf, n)) > 0) e = 0;
        fclose(in);
        return e ? fail(b, e) : 0;
    }
    if (!strcmp(cmd, "stat")) {
        if ((e = siefs_walk(fs, a, &ino)) || (e = siefs_stat(fs, ino, &st))) return fail(a, e);
        printf("object %llu: mode %o, %u links, owner %u:%u, %llu bytes, parent %llu\n", (unsigned long long)ino,
               st.mode, st.nlink, st.uid, st.gid, (unsigned long long)st.size, (unsigned long long)st.parent);
        return 0;
    }
    if (!strcmp(cmd, "attr")) {
        if ((e = siefs_walk(fs, a, &ino))) return fail(a, e);
        if (b && argc > 5) return (e = siefs_setxattr(fs, ino, b, argv[5], strlen(argv[5]))) ? fail(b, e) : 0;
        if (b) {
            if ((e = siefs_getxattr(fs, ino, b, buf, sizeof buf)) < 0) return fail(b, e);
            printf("%.*s\n", (int)e, buf);
            return 0;
        }
        if ((e = siefs_listxattr(fs, ino, buf, sizeof buf)) < 0) return fail(a, e);
        for (char *p = buf; p < buf + e; p += strlen(p) + 1) {
            char v[SIEFS_XVAL_MAX];
            long l = siefs_getxattr(fs, ino, p, v, sizeof v);
            printf("%s = %.*s\n", p, (int)(l > 0 ? l : 0), v);
        }
        return 0;
    }
    if (!b && strcmp(cmd, "mkdir") && strcmp(cmd, "rm") && strcmp(cmd, "rmdir")) {
        fprintf(stderr, "siefs: unknown command or missing argument (see the top of tools/siefs/siefs.c)\n");
        return 2;
    }
    if (!strcmp(cmd, "mkdir")) e = parent(a, &dir, &name) ? -SIEFS_ENOENT : siefs_create(fs, dir, name, SIEFS_IFDIR | 0755, 0, 0, &ino);
    else if (!strcmp(cmd, "rm")) e = parent(a, &dir, &name) ? -SIEFS_ENOENT : siefs_unlink(fs, dir, name);
    else if (!strcmp(cmd, "rmdir")) e = parent(a, &dir, &name) ? -SIEFS_ENOENT : siefs_rmdir(fs, dir, name);
    else if (!strcmp(cmd, "symlink")) e = parent(b, &dir, &name) ? -SIEFS_ENOENT : siefs_symlink(fs, dir, name, a, 0, 0, &ino);
    else if (!strcmp(cmd, "mv") || !strcmp(cmd, "ln")) {
        uint64_t sdir;
        const char *sname;
        if ((e = parent(a, &sdir, &sname)) || (e = parent(b, &dir, &name))) return fail(a, e);
        if (cmd[0] == 'm') e = siefs_rename(fs, sdir, sname, dir, name);
        else e = siefs_walk(fs, a, &ino) ? -SIEFS_ENOENT : siefs_link(fs, ino, dir, name);
    } else if (!strcmp(cmd, "chmod") || !strcmp(cmd, "chown") || !strcmp(cmd, "truncate") || !strcmp(cmd, "rmattr")) {
        const char *path = cmd[1] == 'm' && cmd[0] == 'r' ? a : b;
        if ((e = siefs_walk(fs, path, &ino))) return fail(path, e);
        if (cmd[0] == 'r') e = siefs_removexattr(fs, ino, b);
        else if (cmd[0] == 't') e = siefs_truncate(fs, ino, parse_size(a));
        else {
            st.mode = (uint32_t)strtoul(a, 0, 8);
            st.uid = (uint32_t)strtoul(a, 0, 10);
            st.gid = strchr(a, ':') ? (uint32_t)strtoul(strchr(a, ':') + 1, 0, 10) : st.uid;
            e = siefs_setattr(fs, ino, &st, cmd[3] == 'o' ? SIEFS_SET_MODE : SIEFS_SET_UID | SIEFS_SET_GID);
        }
    } else { fprintf(stderr, "siefs: unknown command %s\n", cmd); return 2; }
    return e ? fail(cmd, e) : 0;
}

int main(int argc, char **argv)
{
    image_t im;
    if (argc < 3) { fprintf(stderr, "usage: siefs IMAGE COMMAND [ARGUMENTS] (see tools/siefs/siefs.c)\n"); return 2; }
    if (!(fs = host_mount(argv[1], &im))) return 1;
    int r = run(argc, argv);
    long e = siefs_unmount(fs);
    image_close(&im);
    return e ? fail("commit", e) : r;
}
