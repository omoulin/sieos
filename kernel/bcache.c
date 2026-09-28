/*
 * bcache.c - Write-through block cache for the block devices.
 *
 * An eighth of memory (at most 64 MiB) of 4 KiB buffers, found by (device,
 * block) through a hash table and recycled least recently used first.  Each
 * device's block size (up to 4 KiB) is set by the file system on it.
 * Programs and shared libraries are read again at every exec and mmap, so
 * the cache has to hold the working set.
 */
#include "bcache.h"
#include "ata.h"
#include "blkdev.h"
#include "mm.h"

#define NHASH   4096
#define BUFSIZE 4096

static struct buf *bufs;
static size_t nbuf;
static struct buf *hash[NHASH];
static struct buf lru;                      /* list head: lru.next is the most recent */
static uint32_t dev_bsize[NBLKDEV];

static unsigned hkey(int dev, uint64_t blk)
{
    return (unsigned)((blk * 31 + (uint64_t)dev * 0x9E3779B1u) % NHASH);
}

static void lru_unlink(struct buf *b)
{
    b->prev->next = b->next;
    b->next->prev = b->prev;
}

static void lru_front(struct buf *b)
{
    b->next = lru.next;
    b->prev = &lru;
    lru.next->prev = b;
    lru.next = b;
}

static void hash_remove(struct buf *b)
{
    for (struct buf **pp = &hash[hkey(b->dev, b->blockno)]; *pp; pp = &(*pp)->hnext)
        if (*pp == b) {
            *pp = b->hnext;
            return;
        }
}

int bcache_init(void)
{
    if (bufs)
        return 0;
    size_t want = pmm_total_pages() / 8, max = (64UL << 20) / BUFSIZE;
    nbuf = want < 128 ? 128 : want > max ? max : want;
    bufs = kzalloc(nbuf * sizeof(*bufs));
    if (!bufs)
        return -ENOMEM;
    lru.next = lru.prev = &lru;
    for (size_t i = 0; i < nbuf; i++) {
        uint64_t pa = pmm_alloc_contig(BUFSIZE / PAGE_SIZE);
        if (!pa)
            return -ENOMEM;
        bufs[i].data = P2V(pa);
        lru_front(&bufs[i]);
    }
    return 0;
}

int bcache_set_bsize(int dev, uint32_t bs)
{
    if (dev < 0 || dev >= NBLKDEV || bs < SECTOR_SIZE || bs > BUFSIZE || (bs & (bs - 1)))
        return -EINVAL;
    dev_bsize[dev] = bs;
    return 0;
}

static struct buf *lookup(int dev, uint64_t blk)
{
    for (struct buf *b = hash[hkey(dev, blk)]; b; b = b->hnext)
        if (b->valid && b->dev == dev && b->blockno == blk)
            return b;
    return NULL;
}

static struct buf *bget(int dev, uint64_t blk)
{
    struct buf *b = lookup(dev, blk);
    if (b) {
        b->ref++;
        lru_unlink(b);
        lru_front(b);
        return b;
    }
    struct buf *victim = lru.prev;
    while (victim != &lru && victim->ref)
        victim = victim->prev;
    if (victim == &lru)
        panic("bcache: all buffers in use");
    if (victim->valid || victim->hashed)
        hash_remove(victim);
    victim->dev = dev;
    victim->blockno = blk;
    victim->valid = false;
    victim->hashed = true;
    victim->ref = 1;
    victim->hnext = hash[hkey(dev, blk)];
    hash[hkey(dev, blk)] = victim;
    lru_unlink(victim);
    lru_front(victim);
    return victim;
}

static uint32_t spb(int dev)                /* sectors per block */
{
    return dev_bsize[dev] / SECTOR_SIZE;
}

struct buf *bread(int dev, uint64_t blk)
{
    if (dev < 0 || dev >= NBLKDEV || !dev_bsize[dev])
        return NULL;
    struct buf *b = bget(dev, blk);
    if (!b->valid) {
        if (blk_read(dev, blk * spb(dev), spb(dev), b->data) < 0) {
            kprintf("bcache: read error on device %d block %lu\n", dev, blk);
            hash_remove(b);
            b->hashed = false;
            b->ref--;
            return NULL;
        }
        b->valid = true;
    }
    return b;
}

struct buf *bzero_get(int dev, uint64_t blk)
{
    struct buf *b = bget(dev, blk);
    memset(b->data, 0, dev_bsize[dev]);
    b->valid = true;
    return b;
}

int bwrite(struct buf *b)
{
    int r = blk_write(b->dev, b->blockno * spb(b->dev), spb(b->dev), b->data);
    if (r < 0)
        kprintf("bcache: write error on device %d block %lu\n", b->dev, b->blockno);
    return r;
}

void brelse(struct buf *b)
{
    if (b->ref <= 0)
        panic("brelse: refcount underflow");
    b->ref--;
}

/* A file system on dev went away: forget its blocks. */
void bcache_forget(int dev)
{
    for (size_t i = 0; i < nbuf; i++) {
        struct buf *b = &bufs[i];
        if (b->hashed && b->dev == dev && b->ref == 0) {
            hash_remove(b);
            b->hashed = false;
            b->valid = false;
        }
    }
}

/*
 * Read ahead: the blocks [blk, blk + n) that are not cached, up to the first
 * that is, with one device read (a run of a file's blocks, see ext4_read).
 */
#define PREFETCH_MAX 32
static uint8_t *prefetch_buf;

void bprefetch(int dev, uint64_t blk, int n)
{
    if (n > PREFETCH_MAX)
        n = PREFETCH_MAX;
    while (n > 0 && lookup(dev, blk)) {
        blk++;
        n--;
    }
    int run = 0;
    while (run < n && !lookup(dev, blk + run))
        run++;
    if (run < 2)
        return;                                  /* bread does single blocks */
    if (!prefetch_buf) {
        uint64_t pa = pmm_alloc_contig(PREFETCH_MAX * BUFSIZE / PAGE_SIZE);
        if (!pa)
            return;
        prefetch_buf = P2V(pa);
    }
    uint32_t bs = dev_bsize[dev];
    if (blk_read(dev, blk * spb(dev), run * spb(dev), prefetch_buf) < 0)
        return;
    for (int i = 0; i < run; i++) {
        struct buf *b = bget(dev, blk + i);
        if (!b->valid) {
            memcpy(b->data, prefetch_buf + (size_t)i * bs, bs);
            b->valid = true;
        }
        brelse(b);
    }
}

size_t bcache_blocks(void)
{
    return nbuf;
}
