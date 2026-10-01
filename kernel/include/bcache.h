/*
 * bcache.h
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_BCACHE_H
#define SIEOS_BCACHE_H

#include "kernel.h"
#include "abi.h"

struct buf {
    int dev;
    uint64_t blockno;
    int ref;
    bool valid;
    bool hashed;                  /* on a hash chain (valid, or being read) */
    bool reading;                 /* a read is in progress (others wait: sleep_on(buf)) */
    struct buf *hnext;
    struct buf *prev, *next;      /* LRU list */
    uint8_t *data;
};

int  bcache_init(void);
int  bcache_set_bsize(int dev, uint32_t block_size);   /* before the first bread of dev */
struct buf *bread(int dev, uint64_t blk);              /* NULL on I/O error */
struct buf *bzero_get(int dev, uint64_t blk);          /* zero-filled buffer, not read from disk */
int  bwrite(struct buf *b);
void brelse(struct buf *b);
void bcache_forget(int dev);                           /* drop dev's unreferenced blocks */
struct buf *bcache_peek(int dev, uint64_t blk);         /* the cached buffer, or NULL (no reference) */
void bcache_wrote(int dev, uint64_t blk, size_t n, const uint8_t *data);   /* written past the cache */
void bprefetch(int dev, uint64_t blk, int n);          /* read ahead [blk, blk + n) in one device read */
size_t bcache_blocks(void);

#endif
