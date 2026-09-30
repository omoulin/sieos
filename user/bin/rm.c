/* rm - remove files (-r: recursive, -f: ignore missing).  Symbolic links
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 * are removed themselves, never followed. */
#include "sieos.h"

static bool opt_r, opt_f;

static int rm(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (opt_f && errno == ENOENT)
            return 0;
        dprintf(STDERR_FILENO, "rm: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!opt_r) {
            dprintf(STDERR_FILENO, "rm: %s: is a directory (use -r)\n", path);
            return 1;
        }
        int rc = 0;
        for (;;) {
            /* Re-read the directory after each batch since we modify it. */
            int fd = open(path, O_RDONLY | O_DIRECTORY);
            if (fd < 0)
                break;
            char buf[2048];
            long n = getdents(fd, (struct dirent *)buf, sizeof(buf));
            close(fd);
            int removed = 0;
            for (long off = 0; off < n;) {
                struct dirent *d = (struct dirent *)(buf + off);
                off += d->d_reclen;
                if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
                    continue;
                char child[512];
                snprintf(child, sizeof(child), "%s/%s", path, d->d_name);
                if (rm(child) == 0)
                    removed++;
                else
                    rc = 1;
            }
            if (removed == 0)
                break;
        }
        if (rmdir(path) < 0) {
            dprintf(STDERR_FILENO, "rm: %s: %s\n", path, strerror(errno));
            return 1;
        }
        return rc;
    }
    if (unlink(path) < 0) {
        dprintf(STDERR_FILENO, "rm: %s: %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (char *o = argv[i] + 1; *o; o++) {
            if (*o == 'r' || *o == 'R') opt_r = true;
            else if (*o == 'f') opt_f = true;
        }
    if (i >= argc) {
        dprintf(STDERR_FILENO, "usage: rm [-rf] file...\n");
        return 2;
    }
    for (; i < argc; i++)
        rc |= rm(argv[i]);
    return rc;
}
