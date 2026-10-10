/*
 * bench - Measure the disk path from a program's point of view: program ->
 * file server -> disk driver -> virtio disk, every step a message.
 *   bench [MiB]   write then read a file of MiB (default 16), then create,
 *                 stat and delete 500 small files.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

static char blk[FS_MAX];

/* x per second, with one decimal, from a count and nanoseconds */
static void rate(const char *what, uint64_t n, uint64_t ns, const char *unit)
{
    uint64_t r10 = ns ? n * 10000000000UL / ns : 0;
    printf("  %-28s %lu.%lu %s/s  (%lu.%03lu s)\n", what, r10 / 10, r10 % 10, unit, ns / 1000000000, ns / 1000000 % 1000);
}

int main(int argc, char **argv)
{
    uint64_t mib = 16, t;
    if (argc > 1) { mib = 0; for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) mib = mib * 10 + *p - '0'; }
    if (!mib) mib = 16;
    for (size_t i = 0; i < sizeof blk; i++) blk[i] = (char)(i * 7);
    const char *f = "/tmp/bench.dat";
    long h = fs_open(f, FS_RDWR | FS_CREAT | FS_TRUNC, 0644);
    if (h < 0) { printf("bench: cannot create %s\n", f); return 1; }
    printf("bench: %lu MiB file, 128 KiB per request\n", mib);

    t = sys_clock();
    for (uint64_t off = 0; off < mib << 20; off += sizeof blk)
        if (fs_write(h, off, blk, sizeof blk) != (long)sizeof blk) { printf("bench: write failed\n"); return 1; }
    fs_sync();                                       /* count the commit to the disk too */
    rate("write + commit", mib, sys_clock() - t, "MiB");

    t = sys_clock();
    for (uint64_t off = 0; off < mib << 20; off += sizeof blk)
        if (fs_read(h, off, blk, sizeof blk) != (long)sizeof blk || blk[1] != 7) { printf("bench: read failed\n"); return 1; }
    rate("read (from the disk)", mib, sys_clock() - t, "MiB");
    fs_close(h);
    fs_unlink(f);

    char name[32] = "/tmp/b.";
    const int n = 500;
    t = sys_clock();
    for (int i = 0; i < n; i++) {
        int k = 7, x = i;
        do name[k++] = (char)('0' + x % 10); while (x /= 10);
        name[k] = 0;
        long fh = fs_open(name, FS_WRONLY | FS_CREAT, 0644);
        if (fh < 0 || fs_write(fh, 0, "small file\n", 11) != 11) { printf("bench: create failed\n"); return 1; }
        fs_close(fh);
    }
    fs_sync();
    rate("create 500 small files", n, sys_clock() - t, "files");
    siefs_stat_t st;
    t = sys_clock();
    for (int r = 0; r < 4; r++) fs_stat("/tmp/b.123", &st, 0);
    rate("stat (path lookup)", 4, sys_clock() - t, "calls");
    t = sys_clock();
    for (int i = 0; i < n; i++) {
        int k = 7, x = i;
        do name[k++] = (char)('0' + x % 10); while (x /= 10);
        name[k] = 0;
        fs_unlink(name);
    }
    fs_sync();
    rate("delete 500 files", n, sys_clock() - t, "files");
    return 0;
}
