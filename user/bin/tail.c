/*
 * tail - print the last lines of a file:  tail [-n N] [file]
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

int main(int argc, char **argv)
{
    int lines = 10, i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-n")) {
        lines = atoi(argv[i + 1]);
        i += 2;
    }
    int fd = i < argc ? open(argv[i], O_RDONLY) : STDIN_FILENO;
    if (fd < 0) {
        dprintf(STDERR_FILENO, "tail: %s: %s\n", argv[i], strerror(errno));
        return 1;
    }
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    long n;
    while (buf && (n = read(fd, buf + len, cap - len)) > 0) {
        len += n;
        if (len == cap) {
            /* keep only the last half: enough for reasonable line counts */
            memmove(buf, buf + cap / 2, cap / 2);
            len = cap / 2;
        }
    }
    if (!buf)
        return 1;
    size_t start = len;
    int seen = 0;
    if (start > 0 && buf[start - 1] == '\n')
        start--;
    while (start > 0) {
        if (buf[start - 1] == '\n' && ++seen == lines)
            break;
        start--;
    }
    write(STDOUT_FILENO, buf + start, len - start);
    return 0;
}
