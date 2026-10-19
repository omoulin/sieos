/*
 * hostdir.c - Walking a directory of the development machine, for
 * mkfs.siefs -d, and locking an image file. This is the ONLY part of the
 * host tools that is not ISO C: ISO C has no notion of directories or of
 * locks, so this file uses the POSIX directory, file-status and fcntl
 * calls. Porting the tools elsewhere (or to SIEOS itself) means rewriting
 * just these three functions.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#define _GNU_SOURCE                         /* (F_OFD_SETLK) */
#include <dirent.h>
#include <fcntl.h>
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

/* The image is ours alone while it is open: a lock on the whole file,
 * of the kind QEMU takes on its disk images (an "open file description"
 * lock). A running QEMU with the image as its disk holds locks on some of
 * its bytes, so this fails (-1) and the tool stops: two programs mounting
 * one SieFS each overwrite the other's free blocks. Likewise QEMU will not
 * start on an image a tool has open. The lock goes when the file closes.
 * cut: then empty the file (mkfs.siefs makes a new image in it). */
int hostfile_lock(FILE *f, int cut)
{
    struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
    if (fcntl(fileno(f), F_OFD_SETLK, &l)) return -1;
    return cut && ftruncate(fileno(f), 0) ? -2 : 0;
}
