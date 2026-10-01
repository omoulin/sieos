/*
 * clipboard.c - The desktop's clipboard: text copied in one program, pasted
 * in another.
 *
 * Every program of a Facet session runs as its user with the same HOME, so
 * the clipboard is a file of theirs, $HOME/.facet/clipboard (mode 0600, in a
 * directory of mode 0700): set writes a new file and renames it over the old
 * one (a reader sees the old text or the new, never half); get reads it,
 * refusing a symbolic link or a file that is not the user's own.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "facet/facet.h"

#define CLIP_MAX (16 << 20)                      /* 16 MiB: enough for any text */

static bool clip_path(char *path, size_t n, bool make_dir)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        return false;
    snprintf(path, n, "%s/.facet", home);
    if (make_dir)
        mkdir(path, 0700);
    snprintf(path, n, "%s/.facet/clipboard", home);
    return true;
}

bool fct_clipboard_set(const char *text, size_t len)
{
    char path[512], tmp[560];
    if (!clip_path(path, sizeof(path), true) || len > CLIP_MAX)
        return false;
    snprintf(tmp, sizeof(tmp), "%s.%d", path, (int)getpid());
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, text + done, len - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        done += n;
    }
    close(fd);
    if (done != len || rename(tmp, path) < 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

char *fct_clipboard_get(size_t *len)
{
    char path[512];
    if (len)
        *len = 0;
    if (!clip_path(path, sizeof(path), false))
        return NULL;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_size > CLIP_MAX) {
        close(fd);
        return NULL;
    }
    char *buf = malloc((size_t)st.st_size + 1);
    size_t got = 0;
    while (buf && got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, st.st_size - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += n;
    }
    close(fd);
    if (!buf)
        return NULL;
    buf[got] = 0;
    if (len)
        *len = got;
    return buf;
}
