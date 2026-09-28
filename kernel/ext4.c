/*
 * ext4.c - ext4 file system driver.
 *
 * Supported: 1K-4K block sizes, 32/64-bit group descriptors, flex_bg,
 * extent trees (any depth for reading, depth <= 1 for allocation),
 * legacy indirect block maps (read-only), linear and htree directories
 * (htree directories are read-only), metadata_csum / gdt_csum checksums.
 *
 * Writes bypass the journal and go straight to disk (write-through), so
 * the file system stays consistent as long as the machine is not reset
 * in the middle of an operation.
 */
#include "fs.h"
#include "bcache.h"
#include "ata.h"
#include "blkdev.h"
#include "mm.h"
#include "abi2.h"

#define NINODE 64

static struct ext4_super sb;
static uint32_t bs;                    /* block size */
static uint32_t ngroups, ipg, bpg, inode_sz, desc_size, fdb;
static uint64_t total_blocks;
static struct ext4_gd *gds;
static uint8_t *gd_dirty;
static bool sb_dirty;
static bool mounted, rw;
static bool f_csum, f_gdt_csum, f_filetype, f_extents, f_64bit;
static uint32_t csum_seed;
static uint32_t generation_seed;
static struct inode icache[NINODE];

/* ------------------------------------------------------------------ */
/* Checksums                                                           */
/* ------------------------------------------------------------------ */

static uint32_t crc32c_table[256];
static uint16_t crc16_table[256];

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0x82F63B78 : c >> 1;
        crc32c_table[i] = c;
        uint16_t d = i;
        for (int k = 0; k < 8; k++)
            d = (d & 1) ? (d >> 1) ^ 0xA001 : d >> 1;
        crc16_table[i] = d;
    }
}

/* Raw CRC32C without pre/post inversion, as used by ext4. */
static uint32_t crc32c(uint32_t crc, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len--)
        crc = crc32c_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

static uint16_t crc16(uint16_t crc, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len--)
        crc = (crc >> 8) ^ crc16_table[(crc ^ *p++) & 0xFF];
    return crc;
}

static uint32_t sb_checksum(void)
{
    return crc32c(~0U, &sb, __builtin_offsetof(struct ext4_super, s_checksum));
}

static uint16_t gd_checksum(uint32_t g)
{
    uint32_t le_group = g;
    uint8_t *d = (uint8_t *)&gds[g];
    size_t off = __builtin_offsetof(struct ext4_gd, bg_checksum);
    if (f_csum) {
        uint16_t zero = 0;
        uint32_t c = crc32c(csum_seed, &le_group, 4);
        c = crc32c(c, d, off);
        c = crc32c(c, &zero, 2);
        if (off + 2 < desc_size)
            c = crc32c(c, d + off + 2, desc_size - off - 2);
        return c & 0xFFFF;
    }
    if (f_gdt_csum) {
        uint16_t c = crc16(~0, sb.s_uuid, 16);
        c = crc16(c, &le_group, 4);
        c = crc16(c, d, off);
        if (f_64bit && off + 2 < desc_size)
            c = crc16(c, d + off + 2, desc_size - off - 2);
        return c;
    }
    return 0;
}

static uint32_t inode_seed(uint32_t ino, const uint8_t *raw)
{
    const struct ext4_inode *di = (const struct ext4_inode *)raw;
    uint32_t gen = di->i_generation;
    uint32_t c = crc32c(csum_seed, &ino, 4);
    return crc32c(c, &gen, 4);
}

static bool inode_has_csum_hi(const uint8_t *raw)
{
    const struct ext4_inode *di = (const struct ext4_inode *)raw;
    return inode_sz > 128 && 128 + di->i_extra_isize >= 0x84;
}

static uint32_t inode_checksum(uint32_t ino, const uint8_t *raw)
{
    uint16_t zero = 0;
    uint32_t c = inode_seed(ino, raw);
    c = crc32c(c, raw, 0x7C);
    c = crc32c(c, &zero, 2);
    c = crc32c(c, raw + 0x7E, 128 - 0x7E);
    if (inode_sz > 128) {
        size_t off = 0x82;
        c = crc32c(c, raw + 128, off - 128);
        if (inode_has_csum_hi(raw)) {
            c = crc32c(c, &zero, 2);
            off += 2;
        }
        c = crc32c(c, raw + off, inode_sz - off);
    }
    if (!inode_has_csum_hi(raw))
        c &= 0xFFFF;
    return c;
}

static void inode_csum_set(uint32_t ino, uint8_t *raw)
{
    if (!f_csum)
        return;
    struct ext4_inode *di = (struct ext4_inode *)raw;
    uint32_t c = inode_checksum(ino, raw);
    di->i_checksum_lo = c & 0xFFFF;
    if (inode_has_csum_hi(raw))
        di->i_checksum_hi = c >> 16;
}

static bool inode_csum_ok(uint32_t ino, const uint8_t *raw)
{
    if (!f_csum)
        return true;
    const struct ext4_inode *di = (const struct ext4_inode *)raw;
    uint32_t stored = di->i_checksum_lo;
    if (inode_has_csum_hi(raw))
        stored |= (uint32_t)di->i_checksum_hi << 16;
    return stored == inode_checksum(ino, raw);
}

/* ------------------------------------------------------------------ */
/* Superblock and group descriptors                                    */
/* ------------------------------------------------------------------ */

static uint64_t sb_free_blocks(void)
{
    return sb.s_free_blocks_count_lo | (f_64bit ? (uint64_t)sb.s_free_blocks_count_hi << 32 : 0);
}

static void sb_set_free_blocks(uint64_t v)
{
    sb.s_free_blocks_count_lo = v & 0xFFFFFFFF;
    if (f_64bit)
        sb.s_free_blocks_count_hi = v >> 32;
    sb_dirty = true;
}

static uint64_t gd_block_bitmap(uint32_t g)
{
    return gds[g].bg_block_bitmap_lo | (desc_size >= 64 ? (uint64_t)gds[g].bg_block_bitmap_hi << 32 : 0);
}

static uint64_t gd_inode_bitmap(uint32_t g)
{
    return gds[g].bg_inode_bitmap_lo | (desc_size >= 64 ? (uint64_t)gds[g].bg_inode_bitmap_hi << 32 : 0);
}

static uint64_t gd_inode_table(uint32_t g)
{
    return gds[g].bg_inode_table_lo | (desc_size >= 64 ? (uint64_t)gds[g].bg_inode_table_hi << 32 : 0);
}

#define GD_FIELD16(name)                                                      \
    static uint32_t gd_##name(uint32_t g)                                     \
    {                                                                         \
        return gds[g].bg_##name##_lo |                                        \
               (desc_size >= 64 ? (uint32_t)gds[g].bg_##name##_hi << 16 : 0); \
    }                                                                         \
    static void gd_set_##name(uint32_t g, uint32_t v)                         \
    {                                                                         \
        gds[g].bg_##name##_lo = v & 0xFFFF;                                   \
        if (desc_size >= 64)                                                  \
            gds[g].bg_##name##_hi = v >> 16;                                  \
        gd_dirty[g] = 1;                                                      \
    }

GD_FIELD16(free_blocks_count)
GD_FIELD16(free_inodes_count)
GD_FIELD16(used_dirs_count)
GD_FIELD16(itable_unused)

static int sb_write(void)
{
    uint64_t blk = 1024 / bs;
    uint32_t off = 1024 % bs;
    sb.s_wtime = kernel_time();
    if (f_csum)
        sb.s_checksum = sb_checksum();
    struct buf *b = bread(blk);
    if (!b)
        return -EIO;
    memcpy(b->data + off, &sb, sizeof(sb));
    int r = bwrite(b);
    brelse(b);
    sb_dirty = false;
    return r;
}

static int gd_write(uint32_t g)
{
    uint64_t byte = (uint64_t)g * desc_size;
    uint64_t blk = fdb + 1 + byte / bs;
    if (f_csum || f_gdt_csum)
        gds[g].bg_checksum = gd_checksum(g);
    struct buf *b = bread(blk);
    if (!b)
        return -EIO;
    memcpy(b->data + byte % bs, &gds[g], desc_size);
    int r = bwrite(b);
    brelse(b);
    gd_dirty[g] = 0;
    return r;
}

void ext4_sync(void)
{
    if (!mounted || !rw)
        return;
    for (uint32_t g = 0; g < ngroups; g++)
        if (gd_dirty[g])
            gd_write(g);
    if (sb_dirty)
        sb_write();
}

static void block_bitmap_csum(uint32_t g, const uint8_t *bitmap)
{
    if (!f_csum)
        return;
    uint32_t c = crc32c(csum_seed, bitmap, sb.s_clusters_per_group / 8);
    gds[g].bg_block_bitmap_csum_lo = c & 0xFFFF;
    if (desc_size >= 0x3A)
        gds[g].bg_block_bitmap_csum_hi = c >> 16;
    gd_dirty[g] = 1;
}

static void inode_bitmap_csum(uint32_t g, const uint8_t *bitmap)
{
    if (!f_csum)
        return;
    uint32_t c = crc32c(csum_seed, bitmap, ipg / 8);
    gds[g].bg_inode_bitmap_csum_lo = c & 0xFFFF;
    if (desc_size >= 0x3C)
        gds[g].bg_inode_bitmap_csum_hi = c >> 16;
    gd_dirty[g] = 1;
}

/* ------------------------------------------------------------------ */
/* Block and inode allocation                                          */
/* ------------------------------------------------------------------ */

static uint32_t blocks_in_group(uint32_t g)
{
    uint64_t start = fdb + (uint64_t)g * bpg;
    uint64_t n = total_blocks - start;
    return n < bpg ? n : bpg;
}

static uint64_t balloc(uint64_t goal)
{
    if (goal < fdb || goal >= total_blocks)
        goal = fdb;
    uint32_t g0 = (goal - fdb) / bpg;
    for (uint32_t k = 0; k < ngroups; k++) {
        uint32_t g = (g0 + k) % ngroups;
        if (gd_free_blocks_count(g) == 0 || (gds[g].bg_flags & BG_BLOCK_UNINIT))
            continue;
        struct buf *b = bread(gd_block_bitmap(g));
        if (!b)
            return 0;
        uint32_t nbits = blocks_in_group(g);
        uint32_t start = (k == 0) ? (goal - fdb) % bpg : 0;
        for (uint32_t scan = 0; scan < nbits; scan++) {
            uint32_t bit = (start + scan) % nbits;
            if (b->data[bit / 8] & (1 << (bit % 8)))
                continue;
            b->data[bit / 8] |= 1 << (bit % 8);
            block_bitmap_csum(g, b->data);
            bwrite(b);
            brelse(b);
            gd_set_free_blocks_count(g, gd_free_blocks_count(g) - 1);
            sb_set_free_blocks(sb_free_blocks() - 1);
            return fdb + (uint64_t)g * bpg + bit;
        }
        brelse(b);
    }
    return 0;
}

static void bfree(uint64_t blk)
{
    if (blk < fdb || blk >= total_blocks) {
        kprintf("ext4: freeing bad block %lu\n", blk);
        return;
    }
    uint32_t g = (blk - fdb) / bpg, bit = (blk - fdb) % bpg;
    struct buf *b = bread(gd_block_bitmap(g));
    if (!b)
        return;
    if (!(b->data[bit / 8] & (1 << (bit % 8)))) {
        kprintf("ext4: block %lu already free\n", blk);
        brelse(b);
        return;
    }
    b->data[bit / 8] &= ~(1 << (bit % 8));
    block_bitmap_csum(g, b->data);
    bwrite(b);
    brelse(b);
    gd_set_free_blocks_count(g, gd_free_blocks_count(g) + 1);
    sb_set_free_blocks(sb_free_blocks() + 1);
}

static uint32_t ialloc(bool is_dir)
{
    for (uint32_t g = 0; g < ngroups; g++) {
        if (gd_free_inodes_count(g) == 0 || (gds[g].bg_flags & BG_INODE_UNINIT))
            continue;
        struct buf *b = bread(gd_inode_bitmap(g));
        if (!b)
            return 0;
        for (uint32_t bit = 0; bit < ipg; bit++) {
            uint32_t ino = g * ipg + bit + 1;
            if ((b->data[bit / 8] & (1 << (bit % 8))) || ino < sb.s_first_ino)
                continue;
            b->data[bit / 8] |= 1 << (bit % 8);
            inode_bitmap_csum(g, b->data);
            bwrite(b);
            brelse(b);
            gd_set_free_inodes_count(g, gd_free_inodes_count(g) - 1);
            if (is_dir)
                gd_set_used_dirs_count(g, gd_used_dirs_count(g) + 1);
            if (f_csum || f_gdt_csum) {
                uint32_t unused = gd_itable_unused(g);
                if (bit >= ipg - unused)
                    gd_set_itable_unused(g, ipg - bit - 1);
            }
            sb.s_free_inodes_count--;
            sb_dirty = true;
            return ino;
        }
        brelse(b);
    }
    return 0;
}

static void ifree(uint32_t ino, bool is_dir)
{
    uint32_t g = (ino - 1) / ipg, bit = (ino - 1) % ipg;
    struct buf *b = bread(gd_inode_bitmap(g));
    if (!b)
        return;
    b->data[bit / 8] &= ~(1 << (bit % 8));
    inode_bitmap_csum(g, b->data);
    bwrite(b);
    brelse(b);
    gd_set_free_inodes_count(g, gd_free_inodes_count(g) + 1);
    if (is_dir)
        gd_set_used_dirs_count(g, gd_used_dirs_count(g) - 1);
    sb.s_free_inodes_count++;
    sb_dirty = true;
}

/* ------------------------------------------------------------------ */
/* Inodes                                                              */
/* ------------------------------------------------------------------ */

static int inode_loc(uint32_t ino, uint64_t *blk, uint32_t *off)
{
    if (ino == 0 || ino > sb.s_inodes_count)
        return -EINVAL;
    uint32_t g = (ino - 1) / ipg, idx = (ino - 1) % ipg;
    uint64_t byte = (uint64_t)idx * inode_sz;
    *blk = gd_inode_table(g) + byte / bs;
    *off = byte % bs;
    return 0;
}

static int read_inode(uint32_t ino, uint8_t *raw)
{
    uint64_t blk;
    uint32_t off;
    if (inode_loc(ino, &blk, &off) < 0)
        return -EINVAL;
    struct buf *b = bread(blk);
    if (!b)
        return -EIO;
    memcpy(raw, b->data + off, inode_sz);
    brelse(b);
    if (!inode_csum_ok(ino, raw))
        kprintf("ext4: warning: inode %u checksum mismatch\n", ino);
    return 0;
}

static int write_inode(uint32_t ino, uint8_t *raw)
{
    uint64_t blk;
    uint32_t off;
    if (!rw)
        return -EROFS;
    if (inode_loc(ino, &blk, &off) < 0)
        return -EINVAL;
    inode_csum_set(ino, raw);
    struct buf *b = bread(blk);
    if (!b)
        return -EIO;
    memcpy(b->data + off, raw, inode_sz);
    int r = bwrite(b);
    brelse(b);
    return r;
}

static const struct fs_ops ext4_ops;
static struct fs ext4_fs;
static int ext4_mount_sb(void);

static int ext4_update(struct inode *ip)
{
    return write_inode(ip->ino, ip->raw);
}

static struct inode *ext4_iget(uint32_t ino)
{
    struct inode *slot = NULL;
    for (int i = 0; i < NINODE; i++) {
        struct inode *ip = &icache[i];
        if (ip->valid && ip->ino == ino) {
            ip->ref++;
            return ip;
        }
        if (ip->ref == 0 && (!slot || (slot->valid && !ip->valid)))
            slot = ip;
    }
    if (!slot) {
        kprintf("ext4: inode cache full\n");
        return NULL;
    }
    if (read_inode(ino, slot->raw) < 0)
        return NULL;
    slot->ino = ino;
    slot->ref = 1;
    slot->valid = true;
    slot->fs = &ext4_fs;
    return slot;
}

static void icache_forget(uint32_t ino)
{
    for (int i = 0; i < NINODE; i++)
        if (icache[i].valid && icache[i].ino == ino && icache[i].ref == 0)
            icache[i].valid = false;
}

static int ext4_truncate(struct inode *ip, uint64_t len);

/* The last reference is gone: free the inode if it has no links left. */
static void ext4_release(struct inode *ip)
{
    struct ext4_inode *di = DI(ip);
    if (rw && di->i_links_count == 0 && di->i_mode != 0) {
        bool is_dir = S_ISDIR(di->i_mode);
        ext4_truncate(ip, 0);
        di->i_dtime = kernel_time();
        ext4_update(ip);
        ifree(ip->ino, is_dir);
        ip->valid = false;
        ext4_sync();
    }
}

static uint64_t inode_blocks512(struct inode *ip)
{
    struct ext4_inode *di = DI(ip);
    uint64_t n = di->i_blocks_lo | ((uint64_t)di->i_blocks_high << 32);
    if (di->i_flags & EXT4_HUGE_FILE_FL)
        n *= bs / 512;
    return n;
}

static void inode_add_blocks(struct inode *ip, int64_t fs_blocks)
{
    struct ext4_inode *di = DI(ip);
    uint64_t n = di->i_blocks_lo | ((uint64_t)di->i_blocks_high << 32);
    int64_t delta = fs_blocks;
    if (!(di->i_flags & EXT4_HUGE_FILE_FL))
        delta *= bs / 512;
    n += delta;
    di->i_blocks_lo = n & 0xFFFFFFFF;
    di->i_blocks_high = (n >> 32) & 0xFFFF;
}

/* ------------------------------------------------------------------ */
/* Block mapping                                                       */
/* ------------------------------------------------------------------ */

static inline struct ext4_extent_header *eh_root(struct inode *ip)
{
    return (struct ext4_extent_header *)DI(ip)->i_block;
}

static inline uint64_t ext_start(const struct ext4_extent *e)
{
    return e->ee_start_lo | ((uint64_t)e->ee_start_hi << 32);
}

static inline uint32_t ext_len(const struct ext4_extent *e)
{
    return e->ee_len > 32768 ? e->ee_len - 32768 : e->ee_len;
}

static inline bool ext_uninit(const struct ext4_extent *e)
{
    return e->ee_len > 32768;
}

static inline uint64_t idx_leaf(const struct ext4_extent_idx *ix)
{
    return ix->ei_leaf_lo | ((uint64_t)ix->ei_leaf_hi << 32);
}

static void extent_block_csum(struct inode *ip, struct ext4_extent_header *eh)
{
    if (!f_csum)
        return;
    size_t off = sizeof(*eh) + eh->eh_max * sizeof(struct ext4_extent);
    uint32_t c = crc32c(inode_seed(ip->ino, ip->raw), eh, off);
    memcpy((uint8_t *)eh + off, &c, 4);
}

/* Map a logical block to a physical block (0 = hole). */
static int bmap(struct inode *ip, uint64_t lblk, uint64_t *out)
{
    struct ext4_inode *di = DI(ip);
    *out = 0;
    if (di->i_flags & EXT4_INLINE_DATA_FL)
        return -EIO;
    if (di->i_flags & EXT4_EXTENTS_FL) {
        struct ext4_extent_header *eh = eh_root(ip);
        struct buf *b = NULL;
        for (int level = 0; level < 8; level++) {
            if (eh->eh_magic != EXT4_EXT_MAGIC) {
                if (b)
                    brelse(b);
                kprintf("ext4: bad extent header in inode %u\n", ip->ino);
                return -EIO;
            }
            if (eh->eh_depth == 0) {
                struct ext4_extent *ex = (struct ext4_extent *)(eh + 1);
                for (int i = 0; i < eh->eh_entries; i++) {
                    if (lblk >= ex[i].ee_block && lblk < (uint64_t)ex[i].ee_block + ext_len(&ex[i])) {
                        if (!ext_uninit(&ex[i]))
                            *out = ext_start(&ex[i]) + (lblk - ex[i].ee_block);
                        break;
                    }
                }
                if (b)
                    brelse(b);
                return 0;
            }
            struct ext4_extent_idx *ix = (struct ext4_extent_idx *)(eh + 1);
            int found = -1;
            for (int i = 0; i < eh->eh_entries && ix[i].ei_block <= lblk; i++)
                found = i;
            if (found < 0) {
                if (b)
                    brelse(b);
                return 0;
            }
            struct buf *nb = bread(idx_leaf(&ix[found]));
            if (b)
                brelse(b);
            if (!nb)
                return -EIO;
            b = nb;
            eh = (struct ext4_extent_header *)b->data;
        }
        if (b)
            brelse(b);
        return -EIO;
    }

    /* Classic ext2/3 indirect block map */
    uint64_t apb = bs / 4;
    uint64_t blk;
    int levels;
    if (lblk < 12) {
        *out = di->i_block[lblk];
        return 0;
    }
    lblk -= 12;
    if (lblk < apb) {
        blk = di->i_block[12];
        levels = 1;
    } else if ((lblk -= apb) < apb * apb) {
        blk = di->i_block[13];
        levels = 2;
    } else {
        lblk -= apb * apb;
        blk = di->i_block[14];
        levels = 3;
    }
    while (levels-- > 0) {
        if (!blk)
            return 0;
        uint64_t div = levels == 2 ? apb * apb : levels == 1 ? apb : 1;
        struct buf *b = bread(blk);
        if (!b)
            return -EIO;
        blk = ((uint32_t *)b->data)[(lblk / div) % apb];
        brelse(b);
    }
    *out = blk;
    return 0;
}

/* Physical block to use as allocation goal for lblk within a leaf. */
static uint64_t leaf_goal(struct inode *ip, struct ext4_extent_header *eh, uint64_t lblk)
{
    struct ext4_extent *ex = (struct ext4_extent *)(eh + 1);
    for (int i = eh->eh_entries - 1; i >= 0; i--)
        if (ex[i].ee_block <= lblk)
            return ext_start(&ex[i]) + (lblk - ex[i].ee_block);
    if (eh->eh_entries > 0)
        return ext_start(&ex[0]);
    uint32_t g = (ip->ino - 1) / ipg;
    return fdb + (uint64_t)g * bpg;
}

/* Insert (lblk -> pblk) into a leaf.  Returns false if the leaf is full. */
static bool leaf_insert(struct ext4_extent_header *eh, uint64_t lblk, uint64_t pblk)
{
    struct ext4_extent *ex = (struct ext4_extent *)(eh + 1);
    int n = eh->eh_entries, pos = 0;
    while (pos < n && ex[pos].ee_block < lblk)
        pos++;
    if (pos > 0) {
        struct ext4_extent *p = &ex[pos - 1];
        if (!ext_uninit(p) && p->ee_len < 32768 &&
            (uint64_t)p->ee_block + p->ee_len == lblk && ext_start(p) + p->ee_len == pblk) {
            p->ee_len++;
            return true;
        }
    }
    if (n >= eh->eh_max)
        return false;
    memmove(&ex[pos + 1], &ex[pos], (n - pos) * sizeof(*ex));
    ex[pos].ee_block = lblk;
    ex[pos].ee_len = 1;
    ex[pos].ee_start_hi = pblk >> 32;
    ex[pos].ee_start_lo = pblk & 0xFFFFFFFF;
    eh->eh_entries++;
    return true;
}

static void init_leaf_block(struct ext4_extent_header *eh)
{
    eh->eh_magic = EXT4_EXT_MAGIC;
    eh->eh_entries = 0;
    eh->eh_max = (bs - sizeof(*eh) - (f_csum ? 4 : 0)) / sizeof(struct ext4_extent);
    eh->eh_depth = 0;
    eh->eh_generation = 0;
}

/*
 * Map lblk, allocating a zeroed block if it is a hole.  The inode's
 * in-memory copy is updated; the caller must ext4_update() it.
 */
/* A new zeroed block near goal, counted in the inode; 0 if the disk is full. */
static uint64_t balloc_zero(struct inode *ip, uint64_t goal)
{
    uint64_t nb = balloc(goal);
    if (!nb)
        return 0;
    inode_add_blocks(ip, 1);
    struct buf *zb = bzero_get(nb);
    bwrite(zb);
    brelse(zb);
    return nb;
}

/*
 * Block-mapped files (no EXTENTS_FL, as written by ext2/ext3 tools): map
 * lblk, allocating the data block and any missing single, double or triple
 * indirect block on the way.  New blocks read back as zeros.
 */
static int bmap_alloc_ind(struct inode *ip, uint64_t lblk, uint64_t *out)
{
    struct ext4_inode *di = DI(ip);
    uint64_t per = bs / 4, goal = di->i_block[0] ? di->i_block[0] : 0;
    if (lblk < 12) {
        if (!di->i_block[lblk]) {
            uint64_t nb = balloc_zero(ip, lblk ? di->i_block[lblk - 1] + 1 : goal);
            if (!nb)
                return -ENOSPC;
            di->i_block[lblk] = nb;
        }
        *out = di->i_block[lblk];
        return 0;
    }
    uint64_t l = lblk - 12, idx[3];
    int level, slot;
    if (l < per) {
        level = 1, slot = 12, idx[0] = l;
    } else if ((l -= per) < per * per) {
        level = 2, slot = 13, idx[0] = l / per, idx[1] = l % per;
    } else if ((l -= per * per) < per * per * per) {
        level = 3, slot = 14, idx[0] = l / (per * per), idx[1] = (l / per) % per, idx[2] = l % per;
    } else {
        return -EFBIG;
    }
    if (!di->i_block[slot]) {                    /* the top indirect block */
        uint64_t nb = balloc_zero(ip, goal);
        if (!nb)
            return -ENOSPC;
        di->i_block[slot] = nb;
    }
    uint64_t blk = di->i_block[slot];
    for (int k = 0; k < level; k++) {
        struct buf *b = bread(blk);
        if (!b)
            return -EIO;
        uint32_t *p = (uint32_t *)b->data;
        if (!p[idx[k]]) {                        /* an indirect block below, or the data block */
            uint64_t nb = balloc_zero(ip, blk + 1);
            if (!nb) {
                brelse(b);
                return -ENOSPC;
            }
            p[idx[k]] = nb;
            bwrite(b);
        }
        blk = p[idx[k]];
        brelse(b);
    }
    *out = blk;
    return 0;
}

static int bmap_alloc(struct inode *ip, uint64_t lblk, uint64_t *out)
{
    int r = bmap(ip, lblk, out);
    if (r < 0 || *out)
        return r;
    if (lblk >= 0xFFFFFFFFUL)
        return -EFBIG;
    if (!(DI(ip)->i_flags & EXT4_EXTENTS_FL))
        return bmap_alloc_ind(ip, lblk, out);

    struct ext4_extent_header *root = eh_root(ip);
    uint64_t newb;

    if (root->eh_depth == 0) {
        newb = balloc(leaf_goal(ip, root, lblk));
        if (!newb)
            return -ENOSPC;
        if (!leaf_insert(root, lblk, newb)) {
            /* Root is full: move the extents into a new leaf block (depth 1). */
            uint64_t leafb = balloc(newb + 1);
            if (!leafb) {
                bfree(newb);
                return -ENOSPC;
            }
            struct buf *lb = bzero_get(leafb);
            struct ext4_extent_header *leh = (struct ext4_extent_header *)lb->data;
            init_leaf_block(leh);
            leh->eh_entries = root->eh_entries;
            memcpy(leh + 1, root + 1, root->eh_entries * sizeof(struct ext4_extent));
            leaf_insert(leh, lblk, newb);
            extent_block_csum(ip, leh);
            bwrite(lb);
            uint32_t first = ((struct ext4_extent *)(leh + 1))[0].ee_block;
            brelse(lb);

            root->eh_depth = 1;
            root->eh_entries = 1;
            struct ext4_extent_idx *ix = (struct ext4_extent_idx *)(root + 1);
            memset(ix, 0, sizeof(struct ext4_extent_idx) * root->eh_max);
            ix[0].ei_block = first;
            ix[0].ei_leaf_lo = leafb & 0xFFFFFFFF;
            ix[0].ei_leaf_hi = leafb >> 32;
            inode_add_blocks(ip, 1);
        }
    } else if (root->eh_depth == 1) {
        struct ext4_extent_idx *ix = (struct ext4_extent_idx *)(root + 1);
        int i = 0;
        while (i + 1 < root->eh_entries && ix[i + 1].ei_block <= lblk)
            i++;
        struct buf *lb = bread(idx_leaf(&ix[i]));
        if (!lb)
            return -EIO;
        struct ext4_extent_header *leh = (struct ext4_extent_header *)lb->data;
        newb = balloc(leaf_goal(ip, leh, lblk));
        if (!newb) {
            brelse(lb);
            return -ENOSPC;
        }
        if (leaf_insert(leh, lblk, newb)) {
            extent_block_csum(ip, leh);
            bwrite(lb);
            if (lblk < ix[i].ei_block)
                ix[i].ei_block = lblk;
            brelse(lb);
        } else {
            struct ext4_extent *last = (struct ext4_extent *)(leh + 1) + leh->eh_entries - 1;
            bool append = i == root->eh_entries - 1 && lblk >= (uint64_t)last->ee_block + ext_len(last);
            brelse(lb);
            if (!append || root->eh_entries >= root->eh_max) {
                bfree(newb);
                return -EFBIG;
            }
            uint64_t leafb = balloc(newb + 1);
            if (!leafb) {
                bfree(newb);
                return -ENOSPC;
            }
            struct buf *nb = bzero_get(leafb);
            struct ext4_extent_header *neh = (struct ext4_extent_header *)nb->data;
            init_leaf_block(neh);
            leaf_insert(neh, lblk, newb);
            extent_block_csum(ip, neh);
            bwrite(nb);
            brelse(nb);
            int n = root->eh_entries++;
            ix[n].ei_block = lblk;
            ix[n].ei_leaf_lo = leafb & 0xFFFFFFFF;
            ix[n].ei_leaf_hi = leafb >> 32;
            ix[n].ei_unused = 0;
            inode_add_blocks(ip, 1);
        }
    } else {
        return -EFBIG;
    }

    inode_add_blocks(ip, 1);
    struct buf *zb = bzero_get(newb);    /* fresh block reads back as zeros */
    brelse(zb);
    *out = newb;
    return 0;
}

static void free_extent_tree(struct ext4_extent_header *eh, int guard)
{
    if (eh->eh_magic != EXT4_EXT_MAGIC || guard > 8)
        return;
    if (eh->eh_depth == 0) {
        struct ext4_extent *ex = (struct ext4_extent *)(eh + 1);
        for (int i = 0; i < eh->eh_entries; i++)
            for (uint32_t k = 0; k < ext_len(&ex[i]); k++)
                bfree(ext_start(&ex[i]) + k);
        return;
    }
    struct ext4_extent_idx *ix = (struct ext4_extent_idx *)(eh + 1);
    for (int i = 0; i < eh->eh_entries; i++) {
        uint64_t leaf = idx_leaf(&ix[i]);
        struct buf *b = bread(leaf);
        if (b) {
            free_extent_tree((struct ext4_extent_header *)b->data, guard + 1);
            brelse(b);
        }
        bfree(leaf);
    }
}

/* Free an indirect block of the given level and what it maps; the blocks freed. */
static long free_indirect(uint64_t blk, int level)
{
    if (!blk)
        return 0;
    long n = 1;
    if (level > 0) {
        struct buf *b = bread(blk);
        if (b) {
            uint32_t *p = (uint32_t *)b->data;
            for (uint32_t i = 0; i < bs / 4; i++)
                if (p[i])
                    n += free_indirect(p[i], level - 1);
            brelse(b);
        }
    }
    bfree(blk);
    return n;
}

/*
 * Block-mapped files: free what the indirect block in *slot (of the given
 * level, mapping logical blocks from 'base') maps at or beyond logical block
 * 'keep', and the indirect blocks left empty.  Returns the blocks freed.
 */
static long trim_indirect(uint32_t *slot, int level, uint64_t base, uint64_t keep)
{
    if (!*slot)
        return 0;
    uint64_t per = bs / 4, span = 1;
    for (int i = 1; i < level; i++)
        span *= per;                             /* logical blocks behind one entry */
    if (base >= keep) {
        long n = free_indirect(*slot, level);
        *slot = 0;
        return n;
    }
    if (base + span * per <= keep)
        return 0;                                /* entirely kept */
    struct buf *b = bread(*slot);
    if (!b)
        return -EIO;
    uint32_t *p = (uint32_t *)b->data;
    long freed = 0;
    bool any = false;
    for (uint64_t j = 0; j < per; j++) {
        uint64_t cb = base + j * span;
        if (level == 1) {
            if (cb >= keep && p[j]) {
                bfree(p[j]);
                p[j] = 0;
                freed++;
            }
        } else {
            long r = trim_indirect(&p[j], level - 1, cb, keep);
            if (r < 0) {
                brelse(b);
                return r;
            }
            freed += r;
        }
        any |= p[j] != 0;
    }
    bwrite(b);
    brelse(b);
    if (!any) {
        bfree(*slot);
        *slot = 0;
        freed++;
    }
    return freed;
}

/*
 * Drop every mapping at or beyond logical block 'keep' from an extent
 * (sub)tree.  Returns the number of file system blocks freed.
 */
static long trim_extents(struct inode *ip, struct ext4_extent_header *eh, uint64_t keep, int guard)
{
    if (eh->eh_magic != EXT4_EXT_MAGIC || guard > 8)
        return -EIO;
    long freed = 0;
    int n = 0;
    if (eh->eh_depth == 0) {
        struct ext4_extent *ex = (struct ext4_extent *)(eh + 1);
        for (int i = 0; i < eh->eh_entries; i++) {
            uint64_t s = ex[i].ee_block;
            uint32_t len = ext_len(&ex[i]);
            uint32_t nl = s >= keep ? 0 : s + len > keep ? keep - s : len;
            for (uint32_t k = nl; k < len; k++)
                bfree(ext_start(&ex[i]) + k);
            freed += len - nl;
            if (!nl)
                continue;
            ex[i].ee_len = nl + (ext_uninit(&ex[i]) ? 32768 : 0);
            ex[n++] = ex[i];
        }
        eh->eh_entries = n;
        return freed;
    }
    struct ext4_extent_idx *ix = (struct ext4_extent_idx *)(eh + 1);
    for (int i = 0; i < eh->eh_entries; i++) {
        uint64_t leaf = idx_leaf(&ix[i]);
        struct buf *b = bread(leaf);
        if (!b)
            return -EIO;
        struct ext4_extent_header *ceh = (struct ext4_extent_header *)b->data;
        long r = trim_extents(ip, ceh, keep, guard + 1);
        if (r < 0) {
            brelse(b);
            return r;
        }
        freed += r;
        if (ceh->eh_entries == 0) {
            brelse(b);
            bfree(leaf);
            freed++;
            continue;
        }
        if (r) {
            extent_block_csum(ip, ceh);
            bwrite(b);
        }
        brelse(b);
        ix[n++] = ix[i];
    }
    eh->eh_entries = n;
    return freed;
}

/* Set the length of a file, releasing the blocks past the new end. */
static int ext4_truncate(struct inode *ip, uint64_t len)
{
    struct ext4_inode *di = DI(ip);
    if (!rw)
        return -EROFS;
    if (di->i_flags & EXT4_INLINE_DATA_FL)
        return -EIO;
    uint64_t size = inode_size(ip);
    bool fast_link = S_ISLNK(di->i_mode) && size < 60 && inode_blocks512(ip) == 0;
    if (len && len >= size) {
        inode_set_size(ip, len);                 /* grow: the new range is a hole */
        goto done;
    }
    if (len && !fast_link) {
        uint64_t keep = (len + bs - 1) / bs;
        long freed = 0;
        if (di->i_flags & EXT4_EXTENTS_FL) {
            struct ext4_extent_header *root = eh_root(ip);
            freed = trim_extents(ip, root, keep, 0);
            if (freed < 0)
                return freed;
            if (root->eh_depth && root->eh_entries == 0) {
                root->eh_depth = 0;
                root->eh_max = 4;
            }
        } else {                                 /* block-mapped: 12 direct, then 1-3 levels of indirection */
            uint64_t per = bs / 4;
            for (uint64_t i = keep; i < 12; i++)
                if (di->i_block[i]) {
                    bfree(di->i_block[i]);
                    di->i_block[i] = 0;
                    freed++;
                }
            uint64_t base = 12, span = per;
            for (int lvl = 1; lvl <= 3; lvl++, base += span, span *= per) {
                uint32_t slot = di->i_block[11 + lvl];   /* the inode is packed: work on a copy */
                long r = trim_indirect(&slot, lvl, base, keep);
                di->i_block[11 + lvl] = slot;
                if (r < 0)
                    return r;
                freed += r;
            }
        }
        inode_add_blocks(ip, -freed);
        if (len % bs) {                          /* bytes past the end must read back as zeros */
            uint64_t pblk;
            if (bmap(ip, len / bs, &pblk) == 0 && pblk) {
                struct buf *b = bread(pblk);
                if (b) {
                    memset(b->data + len % bs, 0, bs - len % bs);
                    bwrite(b);
                    brelse(b);
                }
            }
        }
        inode_set_size(ip, len);
        goto done;
    }
    if (fast_link)
        goto zero;                               /* fast symlink: data lives in i_block */
    if (di->i_flags & EXT4_EXTENTS_FL) {
        free_extent_tree(eh_root(ip), 0);
    } else {
        for (int i = 0; i < 12; i++)
            if (di->i_block[i])
                bfree(di->i_block[i]);
        free_indirect(di->i_block[12], 1);
        free_indirect(di->i_block[13], 2);
        free_indirect(di->i_block[14], 3);
    }
zero:
    memset(di->i_block, 0, sizeof(di->i_block));
    if (di->i_flags & EXT4_EXTENTS_FL) {
        struct ext4_extent_header *eh = eh_root(ip);
        eh->eh_magic = EXT4_EXT_MAGIC;
        eh->eh_max = 4;
    }
    di->i_blocks_lo = 0;
    di->i_blocks_high = 0;
    inode_set_size(ip, 0);
done:
    inode_touch(ip, true, true);
    int r = ext4_update(ip);
    ext4_sync();
    return r;
}

/* ------------------------------------------------------------------ */
/* File data                                                           */
/* ------------------------------------------------------------------ */

static long ext4_read(struct inode *ip, void *dst, uint64_t off, size_t n)
{
    uint64_t size = inode_size(ip);
    if (S_ISLNK(DI(ip)->i_mode) && size < 60 && inode_blocks512(ip) == 0) {
        if (off >= size)
            return 0;
        n = MIN(n, size - off);
        memcpy(dst, (uint8_t *)DI(ip)->i_block + off, n);
        return n;
    }
    if (off >= size)
        return 0;
    if (n > size - off)
        n = size - off;
    size_t done = 0;
    while (done < n) {
        uint64_t lblk = (off + done) / bs;
        uint32_t boff = (off + done) % bs;
        size_t chunk = MIN(n - done, (size_t)(bs - boff));
        uint64_t pblk;
        int r = bmap(ip, lblk, &pblk);
        if (r < 0)
            return done ? (long)done : r;
        if (pblk == 0) {
            memset((uint8_t *)dst + done, 0, chunk);
        } else {
            if (boff == 0 || lblk % 8 == 0) {    /* read ahead the file's contiguous blocks */
                uint64_t last = (size - 1) / bs, want = MIN(last - lblk + 1, 32UL), k = 1, next;
                while (k < want && bmap(ip, lblk + k, &next) == 0 && next == pblk + k)
                    k++;
                if (k > 1)
                    bprefetch(pblk, k);
            }
            struct buf *b = bread(pblk);
            if (!b)
                return done ? (long)done : -EIO;
            memcpy((uint8_t *)dst + done, b->data + boff, chunk);
            brelse(b);
        }
        done += chunk;
    }
    return done;
}

static long ext4_write(struct inode *ip, const void *src, uint64_t off, size_t n)
{
    if (!rw)
        return -EROFS;
    if (off + n < off || (off + n) / bs >= 0xFFFFFFFFUL)
        return -EFBIG;
    size_t done = 0;
    long err = 0;
    while (done < n) {
        uint64_t lblk = (off + done) / bs;
        uint32_t boff = (off + done) % bs;
        size_t chunk = MIN(n - done, (size_t)(bs - boff));
        uint64_t pblk;
        int r = bmap_alloc(ip, lblk, &pblk);
        if (r < 0) {
            err = r;
            break;
        }
        struct buf *b = bread(pblk);
        if (!b) {
            err = -EIO;
            break;
        }
        memcpy(b->data + boff, (const uint8_t *)src + done, chunk);
        r = bwrite(b);
        brelse(b);
        if (r < 0) {
            err = r;
            break;
        }
        done += chunk;
    }
    if (off + done > inode_size(ip))
        inode_set_size(ip, off + done);
    if (done)
        inode_touch(ip, true, true);
    ext4_update(ip);
    ext4_sync();
    return done ? (long)done : err;
}

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */

static inline uint32_t rec_len_for(uint32_t name_len)
{
    return (8 + name_len + 3) & ~3U;
}

static uint32_t dir_block_limit(void)
{
    return f_csum ? bs - sizeof(struct ext4_dirent_tail) : bs;
}

static void dirblock_init_tail(uint8_t *data)
{
    if (!f_csum)
        return;
    struct ext4_dirent_tail *t = (struct ext4_dirent_tail *)(data + bs - sizeof(*t));
    memset(t, 0, sizeof(*t));
    t->det_rec_len = sizeof(*t);
    t->det_reserved_ft = EXT4_FT_DIR_CSUM;
}

static void dirblock_csum(struct inode *dir, uint8_t *data)
{
    if (!f_csum)
        return;
    struct ext4_dirent_tail *t = (struct ext4_dirent_tail *)(data + bs - sizeof(*t));
    if (t->det_rec_len != sizeof(*t) || t->det_reserved_ft != EXT4_FT_DIR_CSUM)
        return;       /* no tail (e.g. htree node) */
    t->det_checksum = crc32c(inode_seed(dir->ino, dir->raw), data, bs - sizeof(*t));
}

static uint8_t mode_to_ftype(uint16_t mode)
{
    if (!f_filetype)
        return 0;
    switch (mode & S_IFMT) {
    case S_IFREG:  return EXT4_FT_REG;
    case S_IFDIR:  return EXT4_FT_DIR;
    case S_IFCHR:  return EXT4_FT_CHRDEV;
    case S_IFBLK:  return EXT4_FT_BLKDEV;
    case S_IFIFO:  return EXT4_FT_FIFO;
    case S_IFSOCK: return EXT4_FT_SOCK;
    case S_IFLNK:  return EXT4_FT_SYMLINK;
    }
    return EXT4_FT_UNKNOWN;
}

/*
 * Walk every entry of a directory.  The callback returns non-zero to stop;
 * it may modify the block, in which case it must set *dirty.
 */
typedef int (*dirent_cb)(struct ext4_dirent *de, struct ext4_dirent *prev, void *arg, bool *dirty);

static int dir_iterate(struct inode *dir, dirent_cb cb, void *arg)
{
    uint64_t nblocks = (inode_size(dir) + bs - 1) / bs;
    for (uint64_t lblk = 0; lblk < nblocks; lblk++) {
        uint64_t pblk;
        int r = bmap(dir, lblk, &pblk);
        if (r < 0)
            return r;
        if (!pblk)
            continue;
        struct buf *b = bread(pblk);
        if (!b)
            return -EIO;
        struct ext4_dirent *prev = NULL;
        bool dirty = false;
        int stop = 0;
        for (uint32_t off = 0; off + 8 <= bs;) {
            struct ext4_dirent *de = (struct ext4_dirent *)(b->data + off);
            if (de->rec_len < 8 || off + de->rec_len > bs)
                break;
            stop = cb(de, prev, arg, &dirty);
            if (stop)
                break;
            prev = de;
            off += de->rec_len;
        }
        if (dirty) {
            dirblock_csum(dir, b->data);
            bwrite(b);
        }
        brelse(b);
        if (stop)
            return stop;
    }
    return 0;
}

struct lookup_arg {
    const char *name;
    size_t len;
    uint32_t ino;
    bool remove;
};

static int lookup_cb(struct ext4_dirent *de, struct ext4_dirent *prev, void *arg, bool *dirty)
{
    struct lookup_arg *a = arg;
    if (de->inode == 0 || de->name_len != a->len || memcmp(de->name, a->name, a->len) != 0)
        return 0;
    a->ino = de->inode;
    if (a->remove) {
        if (prev)
            prev->rec_len += de->rec_len;
        else
            de->inode = 0;
        *dirty = true;
    }
    return 1;
}

static int ext4_dir_lookup(struct inode *dir, const char *name, size_t len, uint32_t *ino)
{
    if (!S_ISDIR(inode_mode(dir)))
        return -ENOTDIR;
    struct lookup_arg a = { name, len, 0, false };
    int r = dir_iterate(dir, lookup_cb, &a);
    if (r < 0)
        return r;
    if (!a.ino)
        return -ENOENT;
    *ino = a.ino;
    return 0;
}

static int empty_cb(struct ext4_dirent *de, struct ext4_dirent *prev, void *arg, bool *dirty)
{
    UNUSED(prev);
    UNUSED(arg);
    UNUSED(dirty);
    if (de->inode == 0)
        return 0;
    if ((de->name_len == 1 && de->name[0] == '.') ||
        (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.'))
        return 0;
    return 1;
}

static int dir_is_empty(struct inode *dir)
{
    int r = dir_iterate(dir, empty_cb, NULL);
    return r < 0 ? r : r == 0;
}

struct add_arg {
    const char *name;
    size_t len;
    uint32_t ino;
    uint8_t ftype;
    uint32_t limit;
    uint8_t *block_start;
};

static void fill_dirent(struct ext4_dirent *de, const struct add_arg *a)
{
    de->inode = a->ino;
    de->name_len = a->len;
    de->file_type = a->ftype;
    memcpy(de->name, a->name, a->len);
}

static int add_cb(struct ext4_dirent *de, struct ext4_dirent *prev, void *arg, bool *dirty)
{
    UNUSED(prev);
    struct add_arg *a = arg;
    if (de->inode == 0 && de->rec_len == sizeof(struct ext4_dirent_tail) &&
        de->file_type == EXT4_FT_DIR_CSUM)
        return 0;                                   /* checksum tail */
    uint32_t need = rec_len_for(a->len);
    uint32_t used = de->inode ? rec_len_for(de->name_len) : 0;
    if (de->rec_len < used + need)
        return 0;
    if (de->inode) {
        struct ext4_dirent *n = (struct ext4_dirent *)((uint8_t *)de + used);
        n->rec_len = de->rec_len - used;
        de->rec_len = used;
        de = n;
    }
    fill_dirent(de, a);
    *dirty = true;
    return 1;
}

static int dir_add(struct inode *dir, const char *name, size_t len, uint32_t ino, uint8_t ftype)
{
    if (DI(dir)->i_flags & EXT4_INDEX_FL) {
        kprintf("ext4: adding entries to hashed (htree) directories is not supported\n");
        return -EPERM;
    }
    struct add_arg a = { name, len, ino, ftype, dir_block_limit(), NULL };
    int r = dir_iterate(dir, add_cb, &a);
    if (r < 0)
        return r;
    if (r == 0) {
        /* No room: append a new directory block. */
        uint64_t lblk = (inode_size(dir) + bs - 1) / bs, pblk;
        r = bmap_alloc(dir, lblk, &pblk);
        if (r < 0)
            return r;
        struct buf *b = bread(pblk);
        if (!b)
            return -EIO;
        struct ext4_dirent *de = (struct ext4_dirent *)b->data;
        de->rec_len = dir_block_limit();
        fill_dirent(de, &a);
        dirblock_init_tail(b->data);
        dirblock_csum(dir, b->data);
        bwrite(b);
        brelse(b);
        inode_set_size(dir, (lblk + 1) * bs);
    }
    inode_touch(dir, true, true);
    return ext4_update(dir);
}

/*
 * Directory offsets are byte positions of entries; an offset that is not
 * an entry boundary resumes at the next entry.
 */
static int ext4_readdir(struct inode *dir, uint64_t *offp, filldir_t fill, void *arg)
{
    uint64_t dsize = inode_size(dir);
    while (*offp < dsize) {
        uint64_t lblk = *offp / bs;
        uint32_t boff = *offp % bs;
        uint64_t pblk;
        int r = bmap(dir, lblk, &pblk);
        if (r < 0)
            return r;
        if (pblk) {
            struct buf *b = bread(pblk);
            if (!b)
                return -EIO;
            for (uint32_t o = 0; o + 8 <= bs;) {
                struct ext4_dirent *de = (struct ext4_dirent *)(b->data + o);
                if (de->rec_len < 8 || o + de->rec_len > bs)
                    break;
                uint32_t next = o + de->rec_len;
                if (o >= boff && de->inode && de->name_len) {
                    uint64_t cookie = next + 8 > bs ? (lblk + 1) * bs : lblk * bs + next;
                    int dt = f_filetype && de->file_type <= 7 ? de->file_type : DT_UNKNOWN;
                    if (fill(arg, de->name, de->name_len, de->inode, dt, cookie)) {
                        brelse(b);
                        return 0;
                    }
                    *offp = cookie;
                }
                o = next;
            }
            brelse(b);
        }
        *offp = (lblk + 1) * bs;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Namespace operations                                                */
/* ------------------------------------------------------------------ */

static uint32_t new_inode(uint16_t mode, uint16_t links, int uid, int gid)
{
    uint32_t ino = ialloc(S_ISDIR(mode));
    if (!ino)
        return 0;
    icache_forget(ino);
    uint8_t raw[INODE_RAW_MAX];
    memset(raw, 0, inode_sz);
    struct ext4_inode *di = (struct ext4_inode *)raw;
    uint32_t now = kernel_time();
    di->i_mode = mode;
    di->i_links_count = links;
    di->i_uid = uid & 0xFFFF;
    di->i_uid_high = uid >> 16;
    di->i_gid = gid & 0xFFFF;
    di->i_gid_high = gid >> 16;
    di->i_atime = di->i_ctime = di->i_mtime = now;
    generation_seed = generation_seed * 1103515245 + 12345 + ticks;
    di->i_generation = generation_seed;
    if (f_extents) {
        di->i_flags = EXT4_EXTENTS_FL;
        struct ext4_extent_header *eh = (struct ext4_extent_header *)di->i_block;
        eh->eh_magic = EXT4_EXT_MAGIC;
        eh->eh_max = 4;
    }
    if (inode_sz > 128) {
        uint16_t extra = sb.s_want_extra_isize ? sb.s_want_extra_isize : 32;
        if (extra > inode_sz - 128)
            extra = inode_sz - 128;
        di->i_extra_isize = extra;
        if (extra >= 0x94 - 128)
            di->i_crtime = now;
    }
    if (write_inode(ino, raw) < 0) {
        ifree(ino, S_ISDIR(mode));
        return 0;
    }
    return ino;
}

static int check_name(struct inode *dir, const char *name, size_t *len)
{
    if (!rw)
        return -EROFS;
    if (!S_ISDIR(inode_mode(dir)))
        return -ENOTDIR;
    *len = strlen(name);
    if (*len == 0)
        return -ENOENT;
    if (*len > 255)
        return -ENAMETOOLONG;
    uint32_t ino;
    if (ext4_dir_lookup(dir, name, *len, &ino) == 0)
        return -EEXIST;
    return 0;
}

static int ext4_create(struct inode *dir, const char *name, uint16_t mode, uint32_t rdev, int uid, int gid,
                       struct inode **out)
{
    size_t len;
    int r = check_name(dir, name, &len);
    if (r < 0)
        return r;
    uint32_t ino = new_inode(mode, 1, uid, gid);
    if (!ino) {
        ext4_sync();
        return -ENOSPC;
    }
    struct inode *ip = ext4_iget(ino);
    if (!ip)
        return -EIO;
    if (!S_ISREG(mode)) {                        /* special files keep no block map */
        DI(ip)->i_flags &= ~EXT4_EXTENTS_FL;
        memset(DI(ip)->i_block, 0, sizeof(DI(ip)->i_block));
        if (S_ISCHR(mode) || S_ISBLK(mode))
            inode_set_rdev(ip, rdev);
        ext4_update(ip);
    }
    r = dir_add(dir, name, len, ino, mode_to_ftype(mode));
    if (r < 0) {
        DI(ip)->i_links_count = 0;
        iput(ip);
        return r;
    }
    ext4_sync();
    if (out)
        *out = ip;
    else
        iput(ip);
    return 0;
}

static int ext4_symlink(struct inode *dir, const char *name, const char *target, int uid, int gid)
{
    size_t len, tlen = strlen(target);
    int r = check_name(dir, name, &len);
    if (r < 0)
        return r;
    if (tlen >= bs)
        return -ENAMETOOLONG;
    uint32_t ino = new_inode(S_IFLNK | 0777, 1, uid, gid);
    if (!ino) {
        ext4_sync();
        return -ENOSPC;
    }
    struct inode *ip = ext4_iget(ino);
    if (!ip)
        return -EIO;
    if (tlen < 60) {                             /* fast symlink: the target lives in i_block */
        DI(ip)->i_flags &= ~EXT4_EXTENTS_FL;
        memset(DI(ip)->i_block, 0, sizeof(DI(ip)->i_block));
        memcpy(DI(ip)->i_block, target, tlen);
        inode_set_size(ip, tlen);
        r = ext4_update(ip);
    } else {
        long n = ext4_write(ip, target, 0, tlen);
        r = n == (long)tlen ? 0 : n < 0 ? n : -ENOSPC;
    }
    if (r == 0)
        r = dir_add(dir, name, len, ino, mode_to_ftype(S_IFLNK));
    if (r < 0)
        DI(ip)->i_links_count = 0;
    iput(ip);
    ext4_sync();
    return r;
}

static int ext4_link(struct inode *dir, const char *name, struct inode *ip)
{
    size_t len;
    int r = check_name(dir, name, &len);
    if (r < 0)
        return r;
    if (S_ISDIR(inode_mode(ip)))
        return -EPERM;
    if (DI(ip)->i_links_count >= 65000)
        return -EMLINK;
    r = dir_add(dir, name, len, ip->ino, mode_to_ftype(inode_mode(ip)));
    if (r == 0) {
        DI(ip)->i_links_count++;
        inode_touch(ip, false, true);
        r = ext4_update(ip);
    }
    ext4_sync();
    return r;
}

static int ext4_lookup(struct inode *dir, const char *name, size_t len, struct inode **out)
{
    uint32_t ino;
    int r = ext4_dir_lookup(dir, name, len, &ino);
    if (r < 0)
        return r;
    *out = ext4_iget(ino);
    return *out ? 0 : -EIO;
}

static int ext4_mkdir(struct inode *dir, const char *name, uint16_t mode, int uid, int gid)
{
    size_t len;
    int r = check_name(dir, name, &len);
    if (r < 0)
        return r;
    uint32_t ino = new_inode(S_IFDIR | (mode & 07777), 2, uid, gid);
    if (!ino) {
        ext4_sync();
        return -ENOSPC;
    }
    struct inode *ip = ext4_iget(ino);
    if (!ip)
        return -EIO;

    uint64_t pblk;
    r = bmap_alloc(ip, 0, &pblk);
    if (r < 0)
        goto fail;
    struct buf *b = bread(pblk);
    if (!b) {
        r = -EIO;
        goto fail;
    }
    struct ext4_dirent *dot = (struct ext4_dirent *)b->data;
    dot->inode = ino;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = mode_to_ftype(S_IFDIR);
    dot->name[0] = '.';
    struct ext4_dirent *dotdot = (struct ext4_dirent *)(b->data + 12);
    dotdot->inode = dir->ino;
    dotdot->rec_len = dir_block_limit() - 12;
    dotdot->name_len = 2;
    dotdot->file_type = mode_to_ftype(S_IFDIR);
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dirblock_init_tail(b->data);
    dirblock_csum(ip, b->data);
    bwrite(b);
    brelse(b);
    inode_set_size(ip, bs);
    ext4_update(ip);

    r = dir_add(dir, name, len, ino, mode_to_ftype(S_IFDIR));
    if (r < 0)
        goto fail;
    if (DI(dir)->i_links_count < 65000)
        DI(dir)->i_links_count++;
    ext4_update(dir);
    iput(ip);
    ext4_sync();
    return 0;

fail:
    DI(ip)->i_links_count = 0;
    iput(ip);
    return r;
}

static int ext4_unlink(struct inode *dir, const char *name, bool is_dir)
{
    if (!rw)
        return -EROFS;
    size_t len = strlen(name);
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return is_dir ? -EINVAL : -EISDIR;
    if (DI(dir)->i_flags & EXT4_INDEX_FL)
        return -EPERM;
    uint32_t ino;
    int r = ext4_dir_lookup(dir, name, len, &ino);
    if (r < 0)
        return r;
    struct inode *ip = ext4_iget(ino);
    if (!ip)
        return -EIO;
    uint16_t mode = inode_mode(ip);
    if (is_dir && !S_ISDIR(mode)) {
        r = -ENOTDIR;
        goto out;
    }
    if (!is_dir && S_ISDIR(mode)) {
        r = -EISDIR;
        goto out;
    }
    if (is_dir) {
        r = dir_is_empty(ip);
        if (r <= 0) {
            r = r < 0 ? r : -ENOTEMPTY;
            goto out;
        }
    }
    struct lookup_arg a = { name, len, 0, true };
    r = dir_iterate(dir, lookup_cb, &a);
    if (r < 0)
        goto out;
    r = 0;
    uint32_t now = kernel_time();
    if (is_dir) {
        DI(ip)->i_links_count = 0;
        if (DI(dir)->i_links_count > 2)
            DI(dir)->i_links_count--;
    } else if (DI(ip)->i_links_count > 0) {
        DI(ip)->i_links_count--;
    }
    UNUSED(now);
    inode_touch(ip, false, true);
    inode_touch(dir, true, true);
    ext4_update(dir);
    ext4_update(ip);
out:
    iput(ip);
    ext4_sync();
    return r;
}

/* Point an existing entry 'name' in dir at a different inode. */
struct retarget_arg {
    const char *name;
    size_t len;
    uint32_t ino;
    uint8_t ftype;
    bool done;
};

static int retarget_cb(struct ext4_dirent *de, struct ext4_dirent *prev, void *arg, bool *dirty)
{
    UNUSED(prev);
    struct retarget_arg *a = arg;
    if (de->inode == 0 || de->name_len != a->len || memcmp(de->name, a->name, a->len) != 0)
        return 0;
    de->inode = a->ino;
    de->file_type = a->ftype;
    a->done = true;
    *dirty = true;
    return 1;
}

static int dir_retarget(struct inode *dir, const char *name, uint32_t ino, uint8_t ftype)
{
    struct retarget_arg a = { name, strlen(name), ino, ftype, false };
    int r = dir_iterate(dir, retarget_cb, &a);
    return r < 0 ? r : a.done ? 0 : -ENOENT;
}

/* Is directory inode 'ancestor' equal to 'dir' or one of its parents? */
static int is_ancestor(uint32_t ancestor, struct inode *dir)
{
    uint32_t cur = dir->ino;
    for (int depth = 0; depth < 256; depth++) {
        if (cur == ancestor)
            return 1;
        if (cur == EXT4_ROOT_INO)
            return 0;
        struct inode *ip = ext4_iget(cur);
        if (!ip)
            return -EIO;
        uint32_t parent;
        int r = ext4_dir_lookup(ip, "..", 2, &parent);
        iput(ip);
        if (r < 0)
            return r;
        cur = parent;
    }
    return -ELOOP;
}

/*
 * rename(olddir/oldname -> newdir/newname).  An existing target is
 * replaced (a file by a file, a directory by an empty directory).
 * Moving a directory to a new parent rewrites its ".." entry.
 */
static int ext4_rename(struct inode *olddir, const char *oldname, struct inode *newdir, const char *newname)
{
    if (!rw)
        return -EROFS;
    size_t olen = strlen(oldname), nlen = strlen(newname);
    if (!olen || !nlen)
        return -ENOENT;
    if (nlen > 255)
        return -ENAMETOOLONG;
    if ((olen == 1 && oldname[0] == '.') || (olen == 2 && !memcmp(oldname, "..", 2)) ||
        (nlen == 1 && newname[0] == '.') || (nlen == 2 && !memcmp(newname, "..", 2)))
        return -EINVAL;
    if ((DI(olddir)->i_flags | DI(newdir)->i_flags) & EXT4_INDEX_FL)
        return -EPERM;

    uint32_t ino, tino = 0;
    int r = ext4_dir_lookup(olddir, oldname, olen, &ino);
    if (r < 0)
        return r;
    struct inode *ip = ext4_iget(ino), *tip = NULL;
    if (!ip)
        return -EIO;
    bool is_dir = S_ISDIR(inode_mode(ip));

    if (ext4_dir_lookup(newdir, newname, nlen, &tino) == 0) {
        if (tino == ino) {                       /* same file: nothing to do */
            iput(ip);
            return 0;
        }
        if (!(tip = ext4_iget(tino))) {
            r = -EIO;
            goto out;
        }
        bool tdir = S_ISDIR(inode_mode(tip));
        if (is_dir && !tdir) {
            r = -ENOTDIR;
            goto out;
        }
        if (!is_dir && tdir) {
            r = -EISDIR;
            goto out;
        }
        if (tdir && (r = dir_is_empty(tip)) <= 0) {
            r = r < 0 ? r : -ENOTEMPTY;
            goto out;
        }
    }
    if (is_dir && olddir->ino != newdir->ino) {
        r = is_ancestor(ino, newdir);            /* can't move a dir inside itself */
        if (r != 0) {
            r = r < 0 ? r : -EINVAL;
            goto out;
        }
    }

    /* 1. Create or retarget the new name. */
    uint8_t ftype = mode_to_ftype(inode_mode(ip));
    r = tip ? dir_retarget(newdir, newname, ino, ftype) : dir_add(newdir, newname, nlen, ino, ftype);
    if (r < 0)
        goto out;

    /* 2. Remove the old name. */
    struct lookup_arg a = { oldname, olen, 0, true };
    r = dir_iterate(olddir, lookup_cb, &a);
    if (r < 0)
        goto out;
    r = 0;
    uint32_t now = kernel_time();

    /* 3. A moved directory gets a new ".." and parent link counts change. */
    if (is_dir && olddir->ino != newdir->ino) {
        dir_retarget(ip, "..", newdir->ino, mode_to_ftype(S_IFDIR));
        if (DI(olddir)->i_links_count > 2)
            DI(olddir)->i_links_count--;
        if (!tip && DI(newdir)->i_links_count < 65000)
            DI(newdir)->i_links_count++;
    }

    /* 4. Drop the replaced target. */
    if (tip) {
        if (S_ISDIR(inode_mode(tip))) {
            DI(tip)->i_links_count = 0;
            if (olddir->ino == newdir->ino && DI(newdir)->i_links_count > 2)
                DI(newdir)->i_links_count--;
        } else if (DI(tip)->i_links_count > 0) {
            DI(tip)->i_links_count--;
        }
        inode_touch(tip, false, true);
        ext4_update(tip);
    }
    UNUSED(now);
    inode_touch(ip, false, true);
    inode_touch(olddir, true, true);
    inode_touch(newdir, true, true);
    ext4_update(ip);
    ext4_update(olddir);
    if (newdir->ino != olddir->ino)
        ext4_update(newdir);
out:
    if (tip)
        iput(tip);
    iput(ip);
    ext4_sync();
    return r;
}

/* ------------------------------------------------------------------ */
/* Mount                                                               */
/* ------------------------------------------------------------------ */

bool ext4_writable(void)
{
    return rw;
}

static void ext4_statvfs(struct fs *fs, struct kstatvfs *sv)
{
    UNUSED(fs);
    sv->bsize = bs;
    sv->blocks = total_blocks;
    sv->bfree = sb_free_blocks();
    sv->files = sb.s_inodes_count;
    sv->ffree = sb.s_free_inodes_count;
    sv->namemax = 255;
    sv->rdonly = !rw;
}

static void ext4_fs_sync(struct fs *fs)
{
    UNUSED(fs);
    ext4_sync();
}

static const struct fs_ops ext4_ops = {
    .name = "ext4",
    .read = ext4_read,
    .write = ext4_write,
    .truncate = ext4_truncate,
    .lookup = ext4_lookup,
    .readdir = ext4_readdir,
    .create = ext4_create,
    .mkdir = ext4_mkdir,
    .unlink = ext4_unlink,
    .rename = ext4_rename,
    .link = ext4_link,
    .symlink = ext4_symlink,
    .update = ext4_update,
    .release = ext4_release,
    .statvfs = ext4_statvfs,
    .sync = ext4_fs_sync,
};


struct fs *ext4_mount(void)
{
    ext4_fs.ops = &ext4_ops;
    int err = ext4_mount_sb();
    if (err < 0)
        return NULL;
    ext4_fs.dev_major = 8;
    ext4_fs.dev_minor = 0;
    ext4_fs.bsize = bs;
    ext4_fs.rdonly = !rw;
    ext4_fs.root = ext4_iget(EXT4_ROOT_INO);
    strcpy(ext4_fs.mntpoint, "/");
    return ext4_fs.root ? &ext4_fs : NULL;
}

static int ext4_mount_sb(void)
{
    crc_init();
    if (blk_read(2, 2, &sb) < 0)
        return -EIO;
    if (sb.s_magic != EXT4_SUPER_MAGIC) {
        kprintf("ext4: bad superblock magic %x\n", sb.s_magic);
        return -EINVAL;
    }
    bs = 1024U << sb.s_log_block_size;
    if (bs > 4096) {
        kprintf("ext4: block size %u not supported\n", bs);
        return -EINVAL;
    }
    uint32_t incompat_ok = INCOMPAT_FILETYPE | INCOMPAT_RECOVER | INCOMPAT_EXTENTS | INCOMPAT_64BIT |
                           INCOMPAT_FLEX_BG | INCOMPAT_CSUM_SEED | INCOMPAT_LARGEDIR;
    if (sb.s_feature_incompat & ~incompat_ok) {
        kprintf("ext4: unsupported incompatible features %x\n", sb.s_feature_incompat & ~incompat_ok);
        return -EINVAL;
    }
    uint32_t ro_ok = RO_COMPAT_SPARSE_SUPER | RO_COMPAT_LARGE_FILE | RO_COMPAT_HUGE_FILE |
                     RO_COMPAT_GDT_CSUM | RO_COMPAT_DIR_NLINK | RO_COMPAT_EXTRA_ISIZE |
                     RO_COMPAT_METADATA_CSUM | RO_COMPAT_BTREE_DIR;
    rw = true;
    if (sb.s_feature_ro_compat & ~ro_ok) {
        kprintf("ext4: ro_compat features %x not supported for writing, mounting read-only\n",
                sb.s_feature_ro_compat & ~ro_ok);
        rw = false;
    }
    if (sb.s_feature_incompat & INCOMPAT_RECOVER) {
        kprintf("ext4: journal needs recovery, mounting read-only\n");
        rw = false;
    }

    f_csum = sb.s_feature_ro_compat & RO_COMPAT_METADATA_CSUM;
    f_gdt_csum = !f_csum && (sb.s_feature_ro_compat & RO_COMPAT_GDT_CSUM);
    f_filetype = sb.s_feature_incompat & INCOMPAT_FILETYPE;
    f_extents = sb.s_feature_incompat & INCOMPAT_EXTENTS;
    f_64bit = sb.s_feature_incompat & INCOMPAT_64BIT;
    csum_seed = (sb.s_feature_incompat & INCOMPAT_CSUM_SEED) ? sb.s_checksum_seed
                                                             : crc32c(~0U, sb.s_uuid, 16);
    if (f_csum && sb.s_checksum != sb_checksum())
        kprintf("ext4: warning: superblock checksum mismatch\n");

    ipg = sb.s_inodes_per_group;
    bpg = sb.s_blocks_per_group;
    fdb = sb.s_first_data_block;
    inode_sz = sb.s_rev_level == 0 ? 128 : sb.s_inode_size;
    desc_size = f_64bit ? sb.s_desc_size : 32;
    if (desc_size < 32)
        desc_size = 32;
    if (inode_sz > INODE_RAW_MAX || desc_size > 64) {
        kprintf("ext4: unsupported inode/descriptor size\n");
        return -EINVAL;
    }
    total_blocks = sb.s_blocks_count_lo | (f_64bit ? (uint64_t)sb.s_blocks_count_hi << 32 : 0);
    ngroups = (total_blocks - fdb + bpg - 1) / bpg;

    if (bcache_init(bs) < 0)
        return -ENOMEM;

    gds = kzalloc(ngroups * sizeof(struct ext4_gd));
    gd_dirty = kzalloc(ngroups);
    if (!gds || !gd_dirty)
        return -ENOMEM;
    int bad_gd = 0;
    for (uint32_t g = 0; g < ngroups; g++) {
        uint64_t byte = (uint64_t)g * desc_size;
        struct buf *b = bread(fdb + 1 + byte / bs);
        if (!b)
            return -EIO;
        memcpy(&gds[g], b->data + byte % bs, desc_size);
        brelse(b);
        if ((f_csum || f_gdt_csum) && gds[g].bg_checksum != gd_checksum(g))
            bad_gd++;
    }
    if (bad_gd)
        kprintf("ext4: warning: %d group descriptor checksum mismatches\n", bad_gd);

    generation_seed = kernel_time();
    mounted = true;
    struct inode *root = ext4_iget(EXT4_ROOT_INO);
    if (!root || !S_ISDIR(inode_mode(root))) {
        kprintf("ext4: root inode is not a directory\n");
        mounted = false;
        return -EINVAL;
    }
    iput(root);

    char label[17];
    memcpy(label, sb.s_volume_name, 16);
    label[16] = 0;
    kprintf("ext4: '%s' %lu MiB, %uK blocks, %s%s%s%s\n",
            label, total_blocks * bs / (1024 * 1024), bs / 1024,
            f_extents ? "extents " : "", f_64bit ? "64bit " : "",
            f_csum ? "metadata_csum " : "", rw ? "rw" : "ro");
    return 0;
}
