/*
 * fsck.siefs - Check a SieFS image: every checksum, the trees, the free
 * space, the link counts, the attribute index.
 *
 *   fsck.siefs [-d] IMAGE      -d: also read and check every data block
 *
 * Exit status: 0 clean, 1 problems found, 2 cannot mount.
 * (Repairing is not needed after a crash: a commit is all or nothing. A
 * damaged image falls back to the previous commit when mounted.)
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <string.h>
#include "host.h"

static void report(void *ctx, const char *msg) { (void)ctx; printf("  %s\n", msg); }

int main(int argc, char **argv)
{
    int data = argc > 2 && !strcmp(argv[1], "-d");
    const char *img = argv[argc - 1];
    image_t im;
    siefs_t *fs;
    siefs_statfs_t st;
    if (argc < 2 || argc > 3 || (argc == 3 && !data)) { fprintf(stderr, "usage: fsck.siefs [-d] IMAGE\n"); return 2; }
    if (!(fs = host_mount(img, &im))) return 2;
    siefs_statfs(fs, &st);
    printf("%s: \"%s\", commit %llu, %llu objects, %llu of %llu blocks free\n", img, st.label,
           (unsigned long long)st.commits, (unsigned long long)st.inodes,
           (unsigned long long)st.free, (unsigned long long)st.blocks);
    long n = siefs_check(fs, data, report, 0);
    printf("%s: %ld problem%s\n", img, n, n == 1 ? "" : "s");
    siefs_unmount(fs);
    image_close(&im);
    return n ? 1 : 0;
}
