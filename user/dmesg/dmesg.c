/*
 * dmesg - The log since boot: the kernel's messages and every server's
 * (SYS_LOG; the last 32 KiB).
 *
 *   dmesg           print it
 *   dmesg -s FILE   save it to FILE
 *   dmesg -w        (init, at boot) save it to /etc/boot.log 20 s, 1, 2 and
 *                   5 min after starting, then end: on a new machine, the
 *                   key read on another computer tells what was found
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
        static const int at[] = { 20, 60, 120, 300 };
        for (int i = 0, t = 0; i < 4; t = at[i++]) {
            sys_sleep((uint64_t)(at[i] - t) * 1000000000);
            save("/etc/boot.log");
        }
        return 0;
    }
    if (argc > 1) { printf("usage: dmesg [-s FILE | -w]\n"); return 2; }
    con_write(buf, grab());
    return 0;
}
