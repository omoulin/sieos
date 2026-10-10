/*
 * siefs_int.h - SieFS inside: the on-disk formats and the library's state.
 * docs/siefs.md explains them with pictures; this file is the reference.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "siefs.h"
#if __STDC_HOSTED__
#include <string.h>
#else
#include "mk/lib.h"            /* memcpy & co. in SIEOS (lib/string.c) */
#endif

#define BS        SIEFS_BS
#define E(x)      (-SIEFS_##x)
#define SB_MAGIC  0x0031765346454953ULL     /* "SIEFSv1\0" */
#define NODE_MAGIC 0x45444f4eU              /* "NODE" */
#define TMP       (1ULL << 62)  /* a "block number" with this bit: a changed node not yet on disk */
#define MAXH      10            /* tree height limit (72^9 leaves: more than any disk) */
#define MAXITEM   1900          /* largest item value: two always fit in a node */
#define INLINE_MAX 1536         /* files up to this size live inside the tree */
#define EXT_MAX   32            /* blocks per extent item (128 KiB) */
#define BPB       (BS * 8)      /* blocks covered by one bitmap block (128 MiB) */
#define SUMS_PER_IDX (BS / 4)   /* bitmap checksums per index block */
#define MAX_IDX   900           /* index blocks the superblock can check: 113 TiB disks */
#define IDX_BIT   (1ULL << 63)  /* object ids with this bit are attribute-index entries */

/* ---- Keys. Every item in a tree has a key (id, type, offset), sorted in
 * that order: all items of one object are next to each other, inode first.
 * The type sits in the top byte of the 64-bit offset field. */
typedef struct { uint64_t id, off; } skey_t;
enum { T_INODE = 1, T_INLINE = 2, T_EXTENT = 3, T_DIRENT = 4, T_XATTR = 5, T_XINDEX = 6,
       T_VOLUME = 16 };
#define KOFF(type, o) ((uint64_t)(type) << 56 | (o))
#define KTYPE(off)    ((int)((off) >> 56))
#define OFFMASK       ((1ULL << 56) - 1)
static inline skey_t K(uint64_t id, int type, uint64_t o) { return (skey_t){ id, KOFF(type, o) }; }
static inline int kcmp(skey_t a, skey_t b)
{
    return a.id != b.id ? (a.id < b.id ? -1 : 1) : a.off != b.off ? (a.off < b.off ? -1 : 1) : 0;
}

/* ---- A pointer to a block: where, which commit wrote it, and its
 * checksum. A parent node holds its children's checksums, so the whole tree
 * is checked from the superblock down (a "Merkle tree"). mac[] is reserved
 * for authenticated encryption (the 16-byte tag), flags for its key/mode. */
typedef struct { uint64_t blk, gen; uint32_t csum, flags; uint8_t mac[16]; } bptr_t;   /* 40 bytes */

/* ---- A tree node, one 4 KiB block. Leaves hold items: a table of
 * (key, where, length) growing from the front, and the values packed from
 * the back. Internal nodes hold (key, pointer) entries; an entry's key is
 * the first key of its child. */
typedef struct { uint32_t magic; uint16_t level, n, dstart, pad; uint32_t pad2; uint64_t gen, tree; } nhdr_t; /* 32 */
typedef struct { skey_t k; uint16_t off, len; uint32_t pad; } litem_t;     /* 24 */
typedef struct { skey_t k; bptr_t p; } ientry_t;                           /* 56 */
#define IMAX ((int)((BS - sizeof(nhdr_t)) / sizeof(ientry_t)))             /* 72 children */

/* ---- Item values. */
typedef struct {                        /* T_INODE, offset 0 */
    uint32_t mode, uid, gid, nlink;
    uint64_t size, parent, flags, rdev;
    int64_t atime, mtime, ctime, btime;
    uint64_t reserved[2];
} inode_t;                              /* 96 */
#define F_INLINE 1                      /* the contents are in a T_INLINE item */

typedef struct {                        /* T_EXTENT, offset = file offset of its first block */
    uint64_t blk;                       /* first disk block */
    uint32_t n, flags;                  /* how many blocks; flags reserved (encryption) */
    uint32_t csum[EXT_MAX];             /* one checksum per block (only n stored) */
} extent_t;
#define EXT_SIZE(n) (16 + 4 * (n))

/* T_DIRENT (offset = hash of the name) holds a list of entries, packed:
 *   u64 ino, u8 type (mode >> 12), u8 name length, the name.
 * T_XATTR (offset = hash of the name) likewise: u8 name length, u16 value
 * length, name, value. T_XINDEX: key (IDX_BIT | hash(name, value), type,
 * object id), no value: one per indexed attribute. */

typedef struct {                        /* in the root tree: (volume id, T_VOLUME, 0) */
    bptr_t root;                        /* the volume's file tree */
    uint64_t next_id, inodes, flags;
    uint8_t key_id[16];                 /* reserved: which key encrypts this volume */
    uint64_t reserved[4];
} vol_t;

/* ---- The superblock, twice: blocks 1 and 2. A commit with sequence
 * number s writes slot s % 2, so the previous one is never touched. */
typedef struct {
    uint64_t magic;
    uint32_t version, bs;
    uint64_t nblocks, seq;
    bptr_t root;                        /* the root tree */
    uint64_t bm[2], idx[2];             /* each slot's bitmap copy and its index */
    uint64_t nbm, data_start;           /* bitmap blocks per copy; first data block */
    uint32_t nidx, flags;
    uint64_t used;                      /* blocks in use (for information) */
    int64_t created;
    uint8_t uuid[16];
    char label[32];
    uint32_t idxsum[MAX_IDX];           /* checksum of each index block of this slot */
    uint8_t pad[BS - 4 - 192 - 4 * MAX_IDX];
    uint32_t csum;                      /* crc32c of the block with this field 0 */
} sb_t;
_Static_assert(sizeof(sb_t) == BS, "superblock size");

/* ---- In memory. */
typedef struct node {
    uint64_t blk;                       /* its block (clean), or TMP | slot (dirty) */
    struct node *hnext, *prev, *next;   /* clean ones: hash chain and LRU list */
    int dirty;
    uint8_t d[BS] __attribute__((aligned(16)));
} node_t;
#define NH(n)    ((nhdr_t *)(n)->d)
#define LI(n, i) ((litem_t *)((n)->d + sizeof(nhdr_t)) + (i))
#define IE(n, i) ((ientry_t *)((n)->d + sizeof(nhdr_t)) + (i))

typedef struct { bptr_t root; uint64_t id; } tree_t;
typedef struct { node_t *n[MAXH]; int i[MAXH]; int h; } path_t;   /* n[0] = the leaf */
typedef struct { uint64_t b, n; } run_t;
typedef struct { run_t *r; size_t n, cap; } runs_t;

struct siefs {
    siefs_env_t env;
    sb_t sb;                            /* the last commit's superblock */
    tree_t rt, ft;                      /* the root tree; volume 1's file tree */
    vol_t vol;
    uint64_t txn;                       /* the transaction being built: sb.seq + 1 */
    /* space: three bitmaps of nblocks bits */
    uint8_t *map;                       /* referenced by the trees: what the next commit writes */
    uint8_t *busy;                      /* not allocatable: map + blocks freed less than 2 commits ago */
    uint8_t *fresh;                     /* allocated in this transaction: free them at once */
    uint8_t *bmdirty[2];                /* bitmap blocks changed since slot 0/1 was written */
    uint32_t *bmsum[2];                 /* checksum of each bitmap block of each slot */
    uint32_t idxsum[2][MAX_IDX];        /* checksum of each index block of each slot */
    runs_t pend[2], fruns;              /* freed in this / the previous transaction; fresh runs */
    uint64_t hint, nfree, npend;
    uint64_t pre, pren;                 /* commit: blocks reserved for its nodes */
    /* nodes */
    node_t **dirty; uint32_t dcap, ndirty, dnext;
    node_t **hash; uint32_t hbits, nclean;
    node_t lru;                         /* list head: most recently used first */
    uint8_t *io;                        /* EXT_MAX blocks: file data, or the commit's node writes */
    uint64_t wstart, wn;                /* commit: the run of nodes gathered in io */
    int64_t first_change;
    int changed, broken, opmod;
};

/* crc.c */
uint32_t siefs_crc32c(uint32_t crc, const void *buf, size_t n);
/* tree.c */
int  t_get(siefs_t *fs, tree_t *t, skey_t k, void *buf, unsigned max);
int  t_insert(siefs_t *fs, tree_t *t, skey_t k, const void *v, unsigned len);
int  t_update(siefs_t *fs, tree_t *t, skey_t k, const void *v, unsigned len);
int  t_put(siefs_t *fs, tree_t *t, skey_t k, const void *v, unsigned len);
int  t_delete(siefs_t *fs, tree_t *t, skey_t k);
int  t_seek(siefs_t *fs, tree_t *t, skey_t k, path_t *pa);
int  t_floor(siefs_t *fs, tree_t *t, skey_t k, path_t *pa);
int  t_next(siefs_t *fs, path_t *pa);
int  t_get_node(siefs_t *fs, const bptr_t *p, node_t **out);
int  t_new_root(siefs_t *fs, tree_t *t);
int  t_flush(siefs_t *fs, bptr_t *p);
int  t_wflush(siefs_t *fs);
void cache_trim(siefs_t *fs);
void nodes_free(siefs_t *fs);
static inline skey_t pkey(path_t *pa) { return LI(pa->n[0], pa->i[0])->k; }
static inline void *pval(path_t *pa) { return pa->n[0]->d + LI(pa->n[0], pa->i[0])->off; }
static inline unsigned plen(path_t *pa) { return LI(pa->n[0], pa->i[0])->len; }
/* fs.c */
uint64_t name_hash(const char *s, size_t n);
uint64_t xindex_id(const char *name, size_t nl, const void *v, size_t vl);
/* space.c */
uint64_t blk_alloc(siefs_t *fs, uint64_t want, uint64_t *got);
void blk_free(siefs_t *fs, uint64_t b, uint64_t n);
uint64_t commit_block(siefs_t *fs);
int  space_reserve(siefs_t *fs, uint64_t need);
int  commit(siefs_t *fs, int force);
int  dev_read(siefs_t *fs, uint64_t b, uint32_t n, void *buf);
int  dev_write(siefs_t *fs, uint64_t b, uint32_t n, const void *buf);
static inline int bit(const uint8_t *m, uint64_t b) { return m[b >> 3] >> (b & 7) & 1; }
