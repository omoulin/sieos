/*
 * lx.h - liblxcompat's internals: the C library's own functions, found
 * behind the layer's (RTLD_NEXT), and the files the layer makes.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef LX_H
#define LX_H

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>

/* The C library's f, typed like the layer's own: REAL(open)(...). */
#define REAL(f) ((__typeof__(&f))lx_real(#f))
void *lx_real(const char *name);

/* A growing text buffer. */
struct buf {
    char *s;
    size_t n, cap;
};
void bput(struct buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void bmem(struct buf *b, const void *p, size_t n);

/* procfiles.c: Linux's /proc and /sys files SIEOS has not.
 * lx_text: the contents of path (absolute, no "." or ".." parts) in *b, 1
 * if the layer makes this file, 0 if not, -1 (errno) if it should but cannot.
 * lx_link: the same for symbolic links (readlink).
 * lx_dir: the names of a directory the layer makes, NUL-separated. */
int lx_text(const char *path, struct buf *b);
int lx_link(const char *path, char *out, size_t size);
int lx_dir(const char *path, struct buf *names);
/* The file as a descriptor: a memfd holding the text, at offset 0. */
int lx_open_text(const struct buf *b, int flags);

#endif
