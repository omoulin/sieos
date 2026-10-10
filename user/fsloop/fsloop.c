/*
 * fsloop - A test of crash recovery: it keeps using files while servers
 * are killed and restarted (tools/boottest.py does that).
 *   fsloop DIR ROUNDS
 * Each round writes a file in DIR, commits it (sync), reads it back and
 * checks it, then renames it and commits again. A round that sees an error
 * is done again: a sync says so when the file server restarted since the
 * last one (changes it had not committed may be lost). At the end every
 * file is checked once more. Prints
 * "fsloop: N rounds, R redone, ... OK" or what was wrong.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

static char data[8192], back[8192];

static void name(char *p, const char *dir, const char *what, int i)
{
    size_t n = strlcpy(p, dir, 128);
    n += strlcpy(p + n, what, 128 - n);
    char d[12];
    int k = 0;
    do d[k++] = (char)('0' + i % 10); while (i /= 10);
    while (k) p[n++] = d[--k];
    p[n] = 0;
}

static void fill(int i) { for (size_t k = 0; k < sizeof data; k++) data[k] = (char)(i * 31 + k * 7); }

static long round_(const char *dir, int i)
{
    char tmp[128], fin[128];
    name(tmp, dir, "/tmp-", i);
    name(fin, dir, "/file-", i);
    fill(i);
    fs_unlink(tmp);                                  /* a leftover of a round that was cut */
    long h = fs_open(tmp, FS_RDWR | FS_CREAT | FS_TRUNC, 0644), r;
    if (h < 0) return h;
    r = fs_write(h, 0, data, sizeof data);
    if (r >= 0) r = fs_sync();
    if (r >= 0) r = fs_read(h, 0, back, sizeof back);
    fs_close(h);
    if (r < 0) return r;
    if (r != sizeof back || memcmp(data, back, sizeof data)) return -EIO;
    if ((r = fs_rename(tmp, fin)) < 0 && r != -ENOENT) return r;
    return fs_sync();
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: fsloop DIR ROUNDS\n"); return 1; }
    const char *dir = argv[1];
    long rounds = strnum(argv[2]), redone = 0;
    uint64_t t0 = sys_clock();
    fs_mkdir(dir, 0755);
    for (int i = 0; i < rounds; ) {
        long r = round_(dir, i);
        if (r < 0) { if (++redone > rounds * 4) { printf("fsloop: too many errors (%ld)\n", r); return 1; } continue; }
        i++;
    }
    int bad = 0;
    for (int i = 0; i < rounds; i++) {               /* every round's file: still there, still right? */
        char fin[128];
        name(fin, dir, "/file-", i);
        fill(i);
        long h = fs_open(fin, FS_RDONLY, 0);
        long r = h < 0 ? h : fs_read(h, 0, back, sizeof back);
        if (h >= 0) fs_close(h);
        if (r != sizeof back || memcmp(data, back, sizeof data)) { printf("fsloop: %s is wrong (%ld)\n", fin, r); bad++; }
    }
    printf("fsloop: %ld rounds, %ld redone, %lu ms, %s\n", rounds, redone, (sys_clock() - t0) / 1000000, bad ? "FAILED" : "OK");
    return bad != 0;
}
