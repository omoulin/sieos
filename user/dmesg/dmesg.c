/*
 * dmesg - The log since boot: the kernel's messages and every server's
 * (SYS_LOG; the last 32 KiB).
 *
 *   dmesg           print it
 *   dmesg -s FILE   save it to FILE
 *   dmesg -w        (init, at boot) copy it to /etc/boot.log as it comes,
 *                   for 10 min (up to 256 KiB: the kernel keeps only the
 *                   last 32 KiB, which a looping driver soon fills), then
 *                   end: on a new machine, the key read on another computer
 *                   tells what was found. Only up to the console's first
 *                   login prompt: what a session on the console prints after
 *                   it is the user's (their files, in clear on the disk
 *                   otherwise); root only (0600)
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

static char buf[LOG_KEEP];

static size_t grab(void)
{
    uint64_t pos = 0;
    size_t n = 0;
    long r;
    while (n < sizeof buf && (r = sys_log(buf + n, sizeof buf - n, &pos)) > 0) n += (size_t)r;
    return n;
}

static int save(const char *path)
{
    size_t n = grab();
    long f = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC, 0644);
    if (f < 0) return 1;
    fs_write(f, 0, buf, n);
    fs_close(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "-s")) return save(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "-w")) {
        static char all[256 << 10];
        uint64_t pos = 0;
        size_t n = 0, saved = 0;
        int stop = 0;
        for (int s = 0; s < 600 && !stop; s++) {                /* 10 min, a look each second */
            long r;
            while (n < sizeof all && (r = sys_log(all + n, sizeof all - n, &pos)) > 0) {
                size_t from = n > 7 ? n - 7 : 0;
                n += (size_t)r;
                for (size_t i = from; i + 8 <= n; i++)
                    if (!memcmp(all + i, " login: ", 8)) { n = i + 8; stop = 1; break; }
                if (stop) break;
            }
            if (n != saved || stop) {
                long f = fs_open("/etc/boot.log", FS_WRONLY | FS_CREAT | FS_TRUNC, 0600);
                if (f >= 0) { fs_write(f, 0, all, n); fs_close(f); saved = n; }
            }
            if (n >= sizeof all) break;
            sys_sleep(1000000000);
        }
        return 0;
    }
    if (argc > 1) { printf("usage: dmesg [-s FILE | -w]\n"); return 2; }
    con_write(buf, grab());
    return 0;
}
