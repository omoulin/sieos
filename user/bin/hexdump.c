/*
 * hexdump - canonical hex+ASCII dump of a file
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

int main(int argc, char **argv)
{
    int fd = argc > 1 ? open(argv[1], O_RDONLY) : STDIN_FILENO;
    if (fd < 0) {
        dprintf(STDERR_FILENO, "hexdump: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    unsigned char buf[16];
    unsigned long off = 0;
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        char line[96];
        int p = snprintf(line, sizeof(line), "%08lx  ", off);
        for (int i = 0; i < 16; i++) {
            if (i < n)
                p += snprintf(line + p, sizeof(line) - p, "%02x ", buf[i]);
            else
                p += snprintf(line + p, sizeof(line) - p, "   ");
            if (i == 7)
                line[p++] = ' ';
        }
        line[p++] = ' ';
        line[p++] = '|';
        for (int i = 0; i < n; i++)
            line[p++] = isprint(buf[i]) ? buf[i] : '.';
        line[p++] = '|';
        line[p] = 0;
        printf("%s\n", line);
        off += n;
    }
    printf("%08lx\n", off);
    return 0;
}
