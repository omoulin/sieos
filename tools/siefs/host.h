/*
 * host.h - What the SieFS host tools share: a disk image kept in a file,
 * handed to the library as a siefs_env_t. Only ISO C (stdio, stdlib,
 * string, time), so the same sources can later be built as SIEOS
 * programs. Walking a host directory (mkfs.siefs -d) is the one thing ISO C
 * cannot do: it lives apart, in tools/hostdir.c.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdio.h>
#include "siefs.h"

typedef struct { FILE *f; uint64_t nblocks; } image_t;

/* Open an image (create != 0: create it, or cut it to, create_bytes). */
int  image_open(image_t *im, const char *path, uint64_t create_bytes, siefs_env_t *env);
void image_close(image_t *im);
/* Open and mount; prints the error and returns 0 on failure. */
siefs_t *host_mount(const char *path, image_t *im);
const char *host_err(long e);
uint64_t parse_size(const char *s);              /* "64M", "2G", "4096" */

/* tools/hostdir.c: list a host directory, read a host symlink. */
typedef struct { const char *name; int type; unsigned mode; } hostent_t;   /* type: 'f', 'd', 'l', '?' */
int  hostdir_list(const char *path, int (*fn)(void *ctx, const hostent_t *e), void *ctx);
long hostdir_readlink(const char *path, char *buf, size_t size);
