/*
 * cp - copy a file (cp src dst, or cp src... dir)
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

static int copy(const char *src, const char *dst)
{
    struct stat st;
    char target[512];
    if (stat(dst, &st) == 0 && S_ISDIR(st.st_mode)) {
        const char *base = strrchr(src, '/');
        snprintf(target, sizeof(target), "%s/%s", dst, base ? base + 1 : src);
        dst = target;
    }
    int in = open(src, O_RDONLY);
    if (in < 0) {
        dprintf(STDERR_FILENO, "cp: %s: %s\n", src, strerror(errno));
        return 1;
    }
    if (fstat(in, &st) == 0 && S_ISDIR(st.st_mode)) {
        dprintf(STDERR_FILENO, "cp: %s: is a directory\n", src);
        close(in);
        return 1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (out < 0) {
        dprintf(STDERR_FILENO, "cp: %s: %s\n", dst, strerror(errno));
        close(in);
        return 1;
    }
    char buf[8192];
    long n;
    int rc = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, n) != n) {
            dprintf(STDERR_FILENO, "cp: %s: %s\n", dst, strerror(errno));
            rc = 1;
            break;
        }
    }
    close(in);
    close(out);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        dprintf(STDERR_FILENO, "usage: cp src... dst\n");
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc - 1; i++)
        rc |= copy(argv[i], argv[argc - 1]);
    return rc;
}
