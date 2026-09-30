/*
 * fstest - file system stress test.
 * Writes two files with interleaved 4K blocks (forcing fragmented extent
 * trees), verifies their contents, then deletes one of them.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

#define BLK 4096

static void fill(char *buf, int file, int blk)
{
    for (int i = 0; i < BLK; i++)
        buf[i] = (char)(file * 31 + blk * 7 + i);
}

static int verify(const char *path, int file, int nblk)
{
    static char want[BLK], got[BLK];
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return 1;
    }
    for (int b = 0; b < nblk; b++) {
        fill(want, file, b);
        if (read(fd, got, BLK) != BLK || memcmp(want, got, BLK) != 0) {
            printf("fstest: %s: mismatch in block %d\n", path, b);
            close(fd);
            return 1;
        }
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    int nblk = argc > 1 ? atoi(argv[1]) : 64;
    static char buf[BLK];
    const char *a = "/tmp/fstest.a", *b = "/tmp/fstest.b";
    int fa = open(a, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int fb = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fa < 0 || fb < 0) {
        perror("fstest: open");
        return 1;
    }
    printf("fstest: writing 2 x %d interleaved blocks...\n", nblk);
    for (int i = 0; i < nblk; i++) {
        fill(buf, 0, i);
        if (write(fa, buf, BLK) != BLK) { perror("write a"); return 1; }
        fill(buf, 1, i);
        if (write(fb, buf, BLK) != BLK) { perror("write b"); return 1; }
    }
    close(fa);
    close(fb);
    struct stat st;
    stat(a, &st);
    printf("fstest: %s is %lu bytes, %lu sectors\n", a, st.st_size, st.st_blocks);
    int bad = verify(a, 0, nblk) + verify(b, 1, nblk);
    printf("fstest: verify %s\n", bad ? "FAILED" : "ok");
    if (unlink(a) < 0)
        perror("unlink");
    printf("fstest: removed %s, kept %s\n", a, b);
    return bad;
}
