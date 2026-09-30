/*
 * head - print the first lines of files
 *   head [-n N | -N] [-c N] [file...]    (several files: a "==> name <==" header each)
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

static bool number(const char *s, long *out)
{
    if (!s || !*s)
        return false;
    long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return false;
        v = v * 10 + (*s - '0');
    }
    *out = v;
    return true;
}

static int head(int fd, long count, bool bytes)
{
    char buf[4096];
    long n = 0;
    while (count > 0 && (n = read(fd, buf, sizeof(buf))) > 0) {
        long k = 0;
        if (bytes) {
            k = n < count ? n : count;
            count -= k;
        } else {
            while (k < n && count > 0)
                if (buf[k++] == '\n')
                    count--;
        }
        if (write(STDOUT_FILENO, buf, k) != k)
            return 1;
    }
    return n < 0;
}

int main(int argc, char **argv)
{
    long count = 10;
    bool bytes = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            i++;
            break;
        }
        const char *v = NULL;
        if (a[1] == 'n' || a[1] == 'c') {
            v = a[2] ? a + 2 : argv[++i];
            bytes = a[1] == 'c';
        } else {
            v = a + 1;                                   /* -N (obsolete form) */
            bytes = false;
        }
        if (!number(v, &count)) {
            dprintf(STDERR_FILENO, "usage: head [-n lines | -lines] [-c bytes] [file...]\n");
            return 2;
        }
    }
    int status = 0, nfiles = argc - i;
    if (nfiles == 0)
        return head(STDIN_FILENO, count, bytes);
    for (int k = i; k < argc; k++) {
        int fd = strcmp(argv[k], "-") ? open(argv[k], O_RDONLY) : STDIN_FILENO;
        if (fd < 0) {
            dprintf(STDERR_FILENO, "head: %s: %s\n", argv[k], strerror(errno));
            status = 1;
            continue;
        }
        if (nfiles > 1)
            printf("%s==> %s <==\n", k > i ? "\n" : "", argv[k]);
        fflush(stdout);
        status |= head(fd, count, bytes);
        if (fd != STDIN_FILENO)
            close(fd);
    }
    return status;
}
