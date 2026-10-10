/*
 * host.c - A SieFS disk image in an ordinary file, for the host tools.
 * Block n lives at byte n * 4096 of the file. ISO C only.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "host.h"

static int seek(FILE *f, uint64_t blk) { return fseek(f, (long)(blk * SIEFS_BS), SEEK_SET); }

static int rd(void *ctx, uint64_t blk, uint32_t n, void *buf)
{
    image_t *im = ctx;
    return seek(im->f, blk) || fread(buf, SIEFS_BS, n, im->f) != n;
}
static int wr(void *ctx, uint64_t blk, uint32_t n, const void *buf)
{
    image_t *im = ctx;
    return seek(im->f, blk) || fwrite(buf, SIEFS_BS, n, im->f) != n;
}
static int fl(void *ctx) { return fflush(((image_t *)ctx)->f) != 0; }

static int64_t now(void)
{
    static int64_t last;                    /* seconds from time(); keep it moving forward */
    int64_t t = (int64_t)time(0) * 1000000000LL;
    last = t > last ? t : last + 1;
    return last;
}

int image_open(image_t *im, const char *path, uint64_t create_bytes, siefs_env_t *env)
{
    im->f = fopen(path, create_bytes ? "wb+" : "rb+");
    if (!im->f) return -1;
    if (create_bytes) {                     /* write the last byte: the file gets its size */
        if (fseek(im->f, (long)(create_bytes - 1), SEEK_SET) || fputc(0, im->f) == EOF) { fclose(im->f); return -1; }
    }
    if (fseek(im->f, 0, SEEK_END)) { fclose(im->f); return -1; }
    im->nblocks = (uint64_t)ftell(im->f) / SIEFS_BS;
    *env = (siefs_env_t){ .ctx = im, .nblocks = im->nblocks, .read = rd, .write = wr, .flush = fl,
                          .alloc = malloc, .free = free, .now = now };
    return 0;
}

void image_close(image_t *im) { if (im->f) fclose(im->f); im->f = 0; }

const char *host_err(long e)
{
    switch (-e) {
    case SIEFS_ENOENT: return "no such file or directory";
    case SIEFS_EIO: return "input/output error, or damaged data (checksum)";
    case SIEFS_E2BIG: return "too big";
    case SIEFS_ENOMEM: return "out of memory";
    case SIEFS_EACCES: return "permission denied";
    case SIEFS_EEXIST: return "already exists";
    case SIEFS_ENOTDIR: return "not a directory";
    case SIEFS_EISDIR: return "is a directory";
    case SIEFS_EINVAL: return "invalid argument, or not a SieFS image";
    case SIEFS_EFBIG: return "too large";
    case SIEFS_ENOSPC: return "no space left";
    case SIEFS_EMLINK: return "too many links";
    case SIEFS_ERANGE: return "buffer too small";
    case SIEFS_ENAMETOOLONG: return "name too long";
    case SIEFS_ENOTEMPTY: return "directory not empty";
    case SIEFS_ENODATA: return "no such attribute";
    case SIEFS_EPERM: return "operation not permitted";
    }
    return "error";
}

siefs_t *host_mount(const char *path, image_t *im)
{
    siefs_env_t env;
    siefs_t *fs;
    int e;
    if (image_open(im, path, 0, &env)) { fprintf(stderr, "%s: cannot open\n", path); return 0; }
    if ((e = siefs_mount(&env, &fs))) {
        fprintf(stderr, "%s: cannot mount: %s\n", path, host_err(e));
        image_close(im);
        return 0;
    }
    return fs;
}

uint64_t parse_size(const char *s)
{
    char *end;
    uint64_t v = strtoull(s, &end, 10);
    switch (*end) {
    case 'k': case 'K': return v << 10;
    case 'm': case 'M': return v << 20;
    case 'g': case 'G': return v << 30;
    case 't': case 'T': return v << 40;
    }
    return v;
}
