/*
 * siefs.h - SieFS, the SIEOS file system: the library's interface.
 *
 * The library is plain C with no operating system underneath: whoever uses
 * it (the SIEOS file server, or the host tools in tools/siefs) hands it a
 * siefs_env_t saying how to read and write blocks, get memory and read the
 * clock. Files and directories are named by their object number ("ino");
 * siefs_walk turns a path into one. Errors are negative POSIX numbers.
 *
 * Every operation is atomic on disk: changes collect in memory and reach the
 * disk together at the next commit (siefs_sync, or automatically between
 * operations). A crash shows either the state of the last commit or the one
 * before it, never a mix. The library does no permission checks: the
 * server does them with the caller's uid/gid (siefs_access helps).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define SIEFS_BS        4096        /* block size */
#define SIEFS_ROOT      2           /* the root directory's object number */
#define SIEFS_NAME_MAX  255
#define SIEFS_XVAL_MAX  1024        /* largest extended attribute value */

/* File types and permission bits, as in POSIX (st_mode). */
#define SIEFS_IFMT  0170000
#define SIEFS_IFDIR 0040000
#define SIEFS_IFREG 0100000
#define SIEFS_IFLNK 0120000

/* Errors (negated). */
enum { SIEFS_EPERM = 1, SIEFS_ENOENT = 2, SIEFS_EIO = 5, SIEFS_E2BIG = 7, SIEFS_EACCES = 13,
       SIEFS_ENOMEM = 12, SIEFS_EEXIST = 17, SIEFS_EXDEV = 18, SIEFS_ENOTDIR = 20,
       SIEFS_EISDIR = 21, SIEFS_EINVAL = 22, SIEFS_EFBIG = 27, SIEFS_ENOSPC = 28,
       SIEFS_EMLINK = 31, SIEFS_ERANGE = 34, SIEFS_ENAMETOOLONG = 36, SIEFS_ENOTEMPTY = 39,
       SIEFS_ENODATA = 61 };

typedef struct siefs siefs_t;

/* What the library needs from its user. */
typedef struct {
    void *ctx;                                          /* passed back to the three below */
    uint64_t nblocks;                                   /* size of the device, in blocks */
    int (*read)(void *ctx, uint64_t blk, uint32_t n, void *buf);
    int (*write)(void *ctx, uint64_t blk, uint32_t n, const void *buf);
    int (*flush)(void *ctx);                            /* make all writes so far durable */
    void *(*alloc)(size_t);                             /* memory for the library */
    void (*free)(void *);
    int64_t (*now)(void);                               /* nanoseconds since 1970 */
    uint32_t cache_nodes;   /* tree nodes kept in RAM besides dirty ones (0 = 128: 512 KiB) */
    uint32_t dirty_limit;   /* commit by itself past this many changed nodes (0 = 256: 1 MiB) */
    int64_t commit_ns;      /* ... or this long after the first change (0 = 5 s) */
} siefs_env_t;

typedef struct {
    uint64_t ino, size, blocks, parent;     /* parent: of a directory (".."), else the directory it was
                                               created or last moved into (with hard links: one of them) */
    uint32_t mode, uid, gid, nlink;
    int64_t atime, mtime, ctime, btime;     /* ns; btime = creation */
} siefs_stat_t;

/* siefs_setattr: which fields to change. */
enum { SIEFS_SET_MODE = 1, SIEFS_SET_UID = 2, SIEFS_SET_GID = 4, SIEFS_SET_ATIME = 8,
       SIEFS_SET_MTIME = 16, SIEFS_SET_SIZE = 32 };

typedef struct {
    uint64_t ino;
    uint32_t type;                          /* mode >> 12: 4 directory, 8 file, 10 symlink */
    char name[SIEFS_NAME_MAX + 1];
} siefs_dirent_t;

typedef struct {
    uint64_t blocks, free, pending;         /* pending: freed, usable after the next commits */
    uint64_t inodes, commits;
    char label[32];
} siefs_statfs_t;

/* Format, mount, commit. */
int  siefs_format(const siefs_env_t *env, const char *label);
int  siefs_mount(const siefs_env_t *env, siefs_t **fs);
int  siefs_unmount(siefs_t *fs);            /* commits, then frees everything */
int  siefs_sync(siefs_t *fs);               /* commit now */
int  siefs_tick(siefs_t *fs);               /* commit if commit_ns has passed: call it now and then */

/* Names. */
int  siefs_lookup(siefs_t *fs, uint64_t dir, const char *name, uint64_t *ino);
int  siefs_walk(siefs_t *fs, const char *path, uint64_t *ino);   /* "/a/b", no symlinks followed */
int  siefs_create(siefs_t *fs, uint64_t dir, const char *name, uint32_t mode,
                  uint32_t uid, uint32_t gid, uint64_t *ino);    /* a file, or a directory (IFDIR) */
int  siefs_symlink(siefs_t *fs, uint64_t dir, const char *name, const char *target,
                   uint32_t uid, uint32_t gid, uint64_t *ino);
long siefs_readlink(siefs_t *fs, uint64_t ino, char *buf, size_t size);
int  siefs_link(siefs_t *fs, uint64_t ino, uint64_t dir, const char *name);
int  siefs_unlink(siefs_t *fs, uint64_t dir, const char *name);  /* not directories */
int  siefs_rmdir(siefs_t *fs, uint64_t dir, const char *name);
int  siefs_rename(siefs_t *fs, uint64_t sdir, const char *sname, uint64_t ddir, const char *dname);
int  siefs_readdir(siefs_t *fs, uint64_t dir, uint64_t *cursor, siefs_dirent_t *de); /* 1, or 0 at the end; start with *cursor = 0 */

/* Contents and attributes. */
long siefs_read(siefs_t *fs, uint64_t ino, uint64_t off, void *buf, size_t n);
long siefs_write(siefs_t *fs, uint64_t ino, uint64_t off, const void *buf, size_t n);
int  siefs_truncate(siefs_t *fs, uint64_t ino, uint64_t size);
int  siefs_stat(siefs_t *fs, uint64_t ino, siefs_stat_t *st);
int  siefs_setattr(siefs_t *fs, uint64_t ino, const siefs_stat_t *st, unsigned mask);
int  siefs_access(const siefs_stat_t *st, uint32_t uid, uint32_t gid, int want); /* want: 4 r, 2 w, 1 x -> 0 or -EACCES */

/* Extended attributes ("project" = "SIEOS"). Short values (<= 255 bytes)
 * are indexed: siefs_find lists every object with a given name = value. */
long siefs_getxattr(siefs_t *fs, uint64_t ino, const char *name, void *buf, size_t size);
int  siefs_setxattr(siefs_t *fs, uint64_t ino, const char *name, const void *val, size_t len);
int  siefs_removexattr(siefs_t *fs, uint64_t ino, const char *name);
long siefs_listxattr(siefs_t *fs, uint64_t ino, char *buf, size_t size);  /* "a\0b\0"; size 0: just the length */
int  siefs_find(siefs_t *fs, const char *name, const void *val, size_t len, uint64_t *cursor, uint64_t *ino); /* 1, or 0 at the end */

int  siefs_statfs(siefs_t *fs, siefs_statfs_t *st);

/* Check everything: checksums, tree order, free space, links, attribute
 * index. data != 0: also read and check every data block. Returns the
 * number of problems found (each reported through report). */
long siefs_check(siefs_t *fs, int data, void (*report)(void *ctx, const char *msg), void *ctx);

uint32_t siefs_crc32c(uint32_t crc, const void *buf, size_t n);
