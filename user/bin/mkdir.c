/*
 * mkdir - create directories (-p: create parents, no error if existing)
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

/* A directory that cannot be made but is there already is fine (-p). */
static int mkdir_there(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return 0;
    int e = errno;
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return 0;
    errno = e;
    return -1;
}

static int mkdir_p(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        int r = mkdir_there(path);
        *p = '/';
        if (r < 0)
            return -1;
    }
    return mkdir_there(path);
}

int main(int argc, char **argv)
{
    int rc = 0, i = 1;
    bool parents = argc > 1 && strcmp(argv[1], "-p") == 0;
    if (parents)
        i++;
    if (i >= argc) {
        dprintf(STDERR_FILENO, "usage: mkdir [-p] dir...\n");
        return 2;
    }
    for (; i < argc; i++) {
        int r = parents ? mkdir_p(argv[i]) : mkdir(argv[i], 0755);
        if (r < 0) {
            dprintf(STDERR_FILENO, "mkdir: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
