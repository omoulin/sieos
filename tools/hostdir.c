/*
 * hostdir.c - Walking a directory of the development machine, for
 * mkfs.siefs -d. This is the ONLY part of the host tools that is not ISO C:
 * ISO C has no notion of directories, so this file uses the POSIX directory
 * and file-status calls. Porting the tools elsewhere (or to SIEOS itself)
 * means rewriting just these two functions.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "siefs/host.h"

int hostdir_list(const char *path, int (*fn)(void *ctx, const hostent_t *e), void *ctx)
{
    DIR *d = opendir(path);
    struct dirent *de;
    char full[4096];
    int r = 0;
    if (!d) return -1;
    while (!r && (de = readdir(d))) {
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(full, sizeof full, "%s/%s", path, de->d_name);
        if (lstat(full, &st)) continue;
        hostent_t e = { de->d_name, S_ISDIR(st.st_mode) ? 'd' : S_ISREG(st.st_mode) ? 'f' : S_ISLNK(st.st_mode) ? 'l' : '?',
                        (unsigned)(st.st_mode & 07777) };
        r = fn(ctx, &e);
    }
    closedir(d);
    return r;
}

long hostdir_readlink(const char *path, char *buf, size_t size)
{
    ssize_t n = readlink(path, buf, size);
    return n;
}
