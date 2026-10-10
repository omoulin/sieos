/*
 * mkfs.siefs - Make a SieFS file system in an image file, optionally
 * filled with a copy of a directory.
 *
 *   mkfs.siefs [-s SIZE] [-L LABEL] [-d DIR] [-o UID:GID] IMAGE
 *     -s SIZE     create IMAGE with this size (64M, 1G...); without -s the
 *                 existing IMAGE keeps its size
 *     -d DIR      copy DIR's contents into the new file system
 *     -o UID:GID  owner of the copied files (default 0:0, root)
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdlib.h>
#include <string.h>
#include "host.h"

typedef struct { siefs_t *fs; uint64_t dir; const char *path; uint32_t uid, gid; long files, err; } copy_t;
static char buf[128 << 10];

static int copy_one(void *ctx, const hostent_t *e)
{
    copy_t *c = ctx;
    char full[4096];
    uint64_t ino;
    int r = 0;
    snprintf(full, sizeof full, "%s/%s", c->path, e->name);
    if (e->type == 'd') {
        if (!(r = siefs_create(c->fs, c->dir, e->name, SIEFS_IFDIR | e->mode, c->uid, c->gid, &ino))) {
            copy_t sub = *c;
            sub.dir = ino; sub.path = full;
            if (hostdir_list(full, copy_one, &sub)) r = -1;
            c->files += sub.files;
        }
    } else if (e->type == 'f') {
        FILE *f = fopen(full, "rb");
        if (!f) { fprintf(stderr, "%s: cannot read\n", full); return 1; }
        if (!(r = siefs_create(c->fs, c->dir, e->name, SIEFS_IFREG | e->mode, c->uid, c->gid, &ino))) {
            size_t n;
            uint64_t off = 0;
            while (!r && (n = fread(buf, 1, sizeof buf, f)) > 0) {
                long w = siefs_write(c->fs, ino, off, buf, n);
                if (w < 0) r = (int)w; else off += n;
            }
        }
        fclose(f);
    } else if (e->type == 'l') {
        long n = hostdir_readlink(full, buf, sizeof buf - 1);
        if (n < 0) return 0;
        buf[n] = 0;
        r = siefs_symlink(c->fs, c->dir, e->name, buf, c->uid, c->gid, &ino);
    } else return 0;                                    /* devices, sockets...: skipped */
    if (r) { fprintf(stderr, "%s: %s\n", full, r < 0 ? host_err(r) : "failed"); c->err++; return 1; }
    c->files++;
    return 0;
}

int main(int argc, char **argv)
{
    uint64_t size = 0;
    const char *label = "SIEOS", *dir = 0, *img = 0;
    uint32_t uid = 0, gid = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) size = parse_size(argv[++i]);
        else if (!strcmp(argv[i], "-L") && i + 1 < argc) label = argv[++i];
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) { uid = (uint32_t)strtoul(argv[++i], 0, 10); char *p = strchr(argv[i], ':'); gid = p ? (uint32_t)strtoul(p + 1, 0, 10) : uid; }
        else if (argv[i][0] != '-' && !img) img = argv[i];
        else { img = 0; break; }
    }
    if (!img) {
        fprintf(stderr, "usage: mkfs.siefs [-s SIZE] [-L LABEL] [-d DIR] [-o UID:GID] IMAGE\n");
        return 2;
    }
    image_t im;
    siefs_env_t env;
    siefs_t *fs;
    int e;
    if (image_open(&im, img, size, &env)) { fprintf(stderr, "%s: cannot open or create\n", img); return 1; }
    if ((e = siefs_format(&env, label)) || (e = siefs_mount(&env, &fs))) {
        fprintf(stderr, "%s: %s\n", img, host_err(e));
        return 1;
    }
    copy_t c = { fs, SIEFS_ROOT, dir, uid, gid, 0, 0 };
    if (dir && hostdir_list(dir, copy_one, &c) < 0) { fprintf(stderr, "%s: cannot list\n", dir); c.err++; }
    siefs_statfs_t st;
    siefs_statfs(fs, &st);
    if ((e = siefs_unmount(fs))) { fprintf(stderr, "%s: %s\n", img, host_err(e)); return 1; }
    image_close(&im);
    printf("%s: SieFS \"%s\", %llu blocks of 4 KiB (%llu MiB), %llu objects, %llu blocks free\n", img, label,
           (unsigned long long)st.blocks, (unsigned long long)(st.blocks >> 8), (unsigned long long)st.inodes,
           (unsigned long long)(st.free + st.pending));
    return c.err ? 1 : 0;
}
