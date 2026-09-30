/*
 * dmesg - the kernel's messages since boot (the last 64 KiB: /proc/msgbuf).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

int main(void)
{
    int fd = open("/proc/msgbuf", O_RDONLY);
    if (fd < 0) {
        perror("dmesg: /proc/msgbuf");
        return 1;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        if (write(STDOUT_FILENO, buf, n) != n)
            return 1;
    close(fd);
    return n < 0;
}
