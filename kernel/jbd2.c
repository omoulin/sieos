/*
 * jbd2.c - The ext4 journal (JBD2 on-disk format): recovery at mount, and
 * metadata written through the journal.
 *
 * ext4.c adds every metadata block it changes to the running transaction
 * (jbd_add); data blocks go straight to disk first, as in ext4's
 * data=ordered mode.  A commit (at the end of an operation when the
 * transaction is large or old, at sync and at unmount):
 *
 *   1. the journal superblock marks the log in use (s_start = s_first),
 *   2. descriptor blocks with tags and the blocks' contents, then a commit
 *      block, go to the journal,
 *   3. the blocks go to their places in the file system (checkpoint),
 *   4. the journal superblock marks the log empty again (s_start = 0).
 *
 * Each write is flushed (ata.c), so a crash leaves either no committed
 * transaction (the file system as before) or one that recovery (here, or
 * e2fsck) replays in full.  The journal holds at most one transaction at a
 * time, so no revoke records are needed; blocks freed by the running
 * transaction are not reused until it commits (jbd_is_freed), so a freed
 * block never receives file data that an uncommitted transaction still
 * owns.  Recovery reads any valid JBD2 log (as Linux or e2fsck leave it):
 * scan, revoke and replay passes, v2/v3 checksums, 32/64-bit tags.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "jbd2.h"
#include "blkdev.h"
#include "mm.h"

bool jbd_crash_test;

#define JBD2_MAGIC 0xC03B3998U
enum { JT_DESCRIPTOR = 1, JT_COMMIT, JT_SB_V1, JT_SB_V2, JT_REVOKE };
#define JF_ESCAPE    1
#define JF_SAME_UUID 2
#define JF_LAST_TAG  8
#define JI_REVOKE  0x1
#define JI_64BIT   0x2
#define JI_ASYNC   0x4
#define JI_CSUM_V2 0x8
#define JI_CSUM_V3 0x10
#define JI_FAST_COMMIT 0x20

static inline uint32_t be32(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
}

static inline void put_be32(void *p, uint32_t v)
{
    uint8_t *b = p;
    b[0] = v >> 24;
    b[1] = v >> 16;
    b[2] = v >> 8;
    b[3] = v;
}

/* journal superblock offsets */
#define S_BLOCKSIZE 12
#define S_MAXLEN    16
#define S_FIRST     20
#define S_SEQUENCE  24
#define S_START     28
#define S_ERRNO     32
#define S_INCOMPAT  40
#define S_UUID      48
#define S_CSUMTYPE  80
#define S_CHECKSUM  0xFC

/* ---------------- I/O ---------------- */

static int jread(struct jbd *j, uint32_t lblk, void *buf)
{
    if (lblk >= j->maxlen)
        return -EIO;
    uint32_t spb = j->bs / 512;
    return blk_read(j->dev, j->map[lblk] * spb, spb, buf);
}

/* Write n consecutive journal blocks from buf, in runs of physically contiguous blocks. */
static int jwrite(struct jbd *j, uint32_t lblk, uint32_t n, const void *buf)
{
    uint32_t spb = j->bs / 512;
    const uint8_t *p = buf;
    while (n) {
        uint32_t run = 1;
        while (run < n && j->map[lblk + run] == j->map[lblk] + run)
            run++;
        int r = blk_write(j->dev, j->map[lblk] * spb, run * spb, p);
        if (r < 0)
            return r;
        lblk += run;
        n -= run;
        p += (size_t)run * j->bs;
    }
    return 0;
}

static uint32_t csum(struct jbd *j, uint32_t seed, const void *p, size_t n)
{
    return j->crc(seed, p, n);
}

static bool sb_csum_ok(struct jbd *j)
{
    if (!j->csum)
        return true;
    uint8_t *s = j->sbuf;
    uint32_t want = be32(s + S_CHECKSUM);
    put_be32(s + S_CHECKSUM, 0);
    uint32_t got = csum(j, ~0U, s, 1024);
    put_be32(s + S_CHECKSUM, want);
    return got == want;
}

static int write_sb(struct jbd *j)
{
    uint8_t *s = j->sbuf;
    if (j->csum) {
        put_be32(s + S_CHECKSUM, 0);
        put_be32(s + S_CHECKSUM, csum(j, ~0U, s, 1024));
    }
    return jwrite(j, 0, 1, s);
}

/* ---------------- opening ---------------- */

int jbd_load(struct jbd *j, int dev, uint32_t bs, uint64_t *map, uint32_t nmap,
             uint32_t (*crc)(uint32_t, const void *, size_t))
{
    memset(j, 0, sizeof(*j));
    j->dev = dev;
    j->bs = bs;
    j->map = map;
    j->maxlen = nmap;
    j->crc = crc;
    j->sbuf = kmalloc(bs);
    j->io = kmalloc(bs);
    if (!j->sbuf || !j->io)
        return -ENOMEM;
    if (jread(j, 0, j->sbuf) < 0)
        return -EIO;
    uint8_t *s = j->sbuf;
    uint32_t type = be32(s + 4);
    if (be32(s) != JBD2_MAGIC || (type != JT_SB_V1 && type != JT_SB_V2)) {
        kprintf("jbd2: bad journal superblock\n");
        return -EINVAL;
    }
    if (be32(s + S_BLOCKSIZE) != bs) {
        kprintf("jbd2: journal block size %u differs from the file system's\n", be32(s + S_BLOCKSIZE));
        return -EINVAL;
    }
    uint32_t maxlen = be32(s + S_MAXLEN);
    if (maxlen < j->maxlen)
        j->maxlen = maxlen;
    j->first = be32(s + S_FIRST);
    uint32_t incompat = type == JT_SB_V2 ? be32(s + S_INCOMPAT) : 0;
    if (incompat & ~(JI_REVOKE | JI_64BIT | JI_ASYNC | JI_CSUM_V2 | JI_CSUM_V3)) {
        kprintf("jbd2: unsupported journal features %x\n", incompat);
        return -EINVAL;
    }
    j->b64 = incompat & JI_64BIT;
    j->csum3 = incompat & JI_CSUM_V3;
    j->csum = incompat & (JI_CSUM_V2 | JI_CSUM_V3);
    if (j->csum && !sb_csum_ok(j)) {
        kprintf("jbd2: journal superblock checksum mismatch\n");
        return -EINVAL;
    }
    memcpy(j->uuid, s + S_UUID, 16);
    j->cseed = csum(j, ~0U, j->uuid, 16);
    j->tag_bytes = j->csum3 ? 16 : (j->b64 ? 12 : 8) + (incompat & JI_CSUM_V2 ? 2 : 0);
    j->seq = be32(s + S_SEQUENCE);
    /* a transaction may use a quarter of the log (descriptors included) */
    j->limit = (j->maxlen - j->first) / 4;
    if (j->limit > 2048)
        j->limit = 2048;
    if (j->limit < 16) {
        kprintf("jbd2: journal too small (%u blocks)\n", j->maxlen);
        return -EINVAL;
    }
    return 0;
}

/* ---------------- recovery ---------------- */

struct revoke {
    uint64_t blk;
    uint32_t seq;
};

static uint32_t tags_per_desc(struct jbd *j)
{
    return j->bs - 12 - (j->csum ? 4 : 0);            /* header, block tail */
}

/* Walk descriptor tags: calls fn(j, blocknr, flags, data_lblk, arg) for each; returns blocks used. */
static int walk_tags(struct jbd *j, const uint8_t *d, uint32_t lblk, uint32_t seq,
                     void (*fn)(struct jbd *, uint64_t, uint32_t, uint32_t, uint32_t, void *), void *arg)
{
    uint32_t off = 12, space = 12 + tags_per_desc(j), n = 0;
    while (off + j->tag_bytes <= space) {
        const uint8_t *t = d + off;
        uint64_t blk = be32(t);
        uint32_t flags = j->csum3 ? be32(t + 4) : (uint32_t)(t[6] << 8 | t[7]);
        if (j->b64 || j->csum3)
            blk |= (uint64_t)be32(t + 8) << 32;
        n++;
        uint32_t data = j->first + (lblk - j->first + n) % (j->maxlen - j->first);
        if (fn)
            fn(j, blk, flags, data, seq, arg);
        off += j->tag_bytes;
        if (!(flags & JF_SAME_UUID))
            off += 16;
        if (flags & JF_LAST_TAG)
            break;
    }
    return n;
}

struct replay_arg {
    struct revoke *rv;
    int nrv;
    int replayed;
    bool failed;
};

static bool revoked(struct replay_arg *a, uint64_t blk, uint32_t seq)
{
    for (int i = 0; i < a->nrv; i++)
        if (a->rv[i].blk == blk && (int32_t)(seq - a->rv[i].seq) <= 0)
            return true;
    return false;
}

static void replay_one(struct jbd *j, uint64_t blk, uint32_t flags, uint32_t data, uint32_t seq, void *arg)
{
    struct replay_arg *a = arg;
    if (revoked(a, blk, seq))
        return;
    if (jread(j, data, j->io) < 0) {
        a->failed = true;
        return;
    }
    if (flags & JF_ESCAPE)
        put_be32(j->io, JBD2_MAGIC);
    uint32_t spb = j->bs / 512;
    if (blk_write(j->dev, blk * spb, spb, j->io) < 0)
        a->failed = true;
    else
        a->replayed++;
}

static uint32_t next(struct jbd *j, uint32_t lblk, uint32_t n)
{
    return j->first + (lblk - j->first + n) % (j->maxlen - j->first);
}

/* One pass over the log from s_start: 0 scan (returns the end sequence), 1 revoke, 2 replay. */
static int pass(struct jbd *j, int which, uint32_t *end_seq, struct replay_arg *a)
{
    uint8_t *s = j->sbuf;
    uint32_t lblk = be32(s + S_START), seq = be32(s + S_SEQUENCE);
    uint8_t *d = kmalloc(j->bs);
    if (!d)
        return -ENOMEM;
    for (uint32_t guard = 0; guard < j->maxlen; guard++) {
        if (which > 0 && seq == *end_seq)
            break;
        if (jread(j, lblk, d) < 0)
            break;
        if (be32(d) != JBD2_MAGIC || be32(d + 8) != seq)
            break;
        uint32_t type = be32(d + 4);
        if (type == JT_DESCRIPTOR) {
            if (j->csum) {
                uint32_t want = be32(d + j->bs - 4);
                put_be32(d + j->bs - 4, 0);
                if (csum(j, j->cseed, d, j->bs) != want)
                    break;                           /* a torn descriptor: the log ends here */
                put_be32(d + j->bs - 4, want);
            }
            int n = walk_tags(j, d, lblk, seq, which == 2 ? replay_one : NULL, a);
            lblk = next(j, lblk, n + 1);
        } else if (type == JT_COMMIT) {
            if (j->csum && which == 0) {
                uint32_t want = be32(d + 16);
                put_be32(d + 16, 0);
                bool ok = csum(j, j->cseed, d, j->bs) == want;
                put_be32(d + 16, want);
                if (!ok)
                    break;                           /* not committed after all */
            }
            seq++;
            lblk = next(j, lblk, 1);
        } else if (type == JT_REVOKE) {
            if (which == 1) {
                uint32_t count = be32(d + 12), rsz = j->b64 ? 8 : 4;
                for (uint32_t off = 16; off + rsz <= count && off + rsz <= j->bs; off += rsz) {
                    uint64_t blk = j->b64 ? ((uint64_t)be32(d + off) << 32 | be32(d + off + 4)) : be32(d + off);
                    if (a->nrv < 4096) {
                        a->rv[a->nrv].blk = blk;
                        a->rv[a->nrv++].seq = seq;
                    }
                }
            }
            lblk = next(j, lblk, 1);
        } else {
            break;
        }
    }
    kfree(d);
    if (which == 0)
        *end_seq = seq;
    return 0;
}

int jbd_recover(struct jbd *j)
{
    uint8_t *s = j->sbuf;
    if (be32(s + S_START) == 0)
        return 0;                                    /* clean */
    uint32_t start_seq = be32(s + S_SEQUENCE), end_seq = 0;
    struct replay_arg a = { kmalloc(4096 * sizeof(struct revoke)), 0, 0, false };
    if (!a.rv)
        return -ENOMEM;
    pass(j, 0, &end_seq, &a);
    pass(j, 1, &end_seq, &a);
    pass(j, 2, &end_seq, &a);
    kfree(a.rv);
    if (a.failed)
        return -EIO;
    kprintf("jbd2: recovered %u transaction%s, %d block%s\n", end_seq - start_seq, end_seq - start_seq == 1 ? "" : "s",
            a.replayed, a.replayed == 1 ? "" : "s");
    j->seq = end_seq;
    put_be32(s + S_SEQUENCE, j->seq);
    put_be32(s + S_START, 0);
    return write_sb(j) < 0 ? -EIO : a.replayed;
}

/* ---------------- transactions ---------------- */

bool jbd_has(struct jbd *j, struct buf *b)
{
    for (uint32_t i = 0; i < j->n; i++)
        if (j->blocks[i] == b)
            return true;
    return false;
}

int jbd_add(struct jbd *j, struct buf *b)
{
    if (jbd_has(j, b))
        return 0;
    if (j->n == j->cap) {
        uint32_t cap = j->cap ? j->cap * 2 : 64;
        struct buf **nb = kmalloc(cap * sizeof(*nb));
        if (!nb)
            return -ENOMEM;
        if (j->blocks) {
            memcpy(nb, j->blocks, j->n * sizeof(*nb));
            kfree(j->blocks);
        }
        j->blocks = nb;
        j->cap = cap;
    }
    b->ref++;                                        /* pinned until the checkpoint */
    j->blocks[j->n++] = b;
    if (j->n == 1)
        j->started = ticks;
    return 0;
}

void jbd_freed(struct jbd *j, uint64_t blk)
{
    if (j->nfreed == j->capfreed) {
        uint32_t cap = j->capfreed ? j->capfreed * 2 : 64;
        uint64_t *nf = kmalloc(cap * sizeof(*nf));
        if (!nf)
            return;
        if (j->freed) {
            memcpy(nf, j->freed, j->nfreed * sizeof(*nf));
            kfree(j->freed);
        }
        j->freed = nf;
        j->capfreed = cap;
    }
    j->freed[j->nfreed++] = blk;
}

bool jbd_is_freed(struct jbd *j, uint64_t blk)
{
    for (uint32_t i = 0; i < j->nfreed; i++)
        if (j->freed[i] == blk)
            return true;
    return false;
}

bool jbd_due(struct jbd *j)
{
    return j->n && (j->n >= j->limit || j->nfreed >= 4096 || ticks - j->started >= JBD_COMMIT_TICKS);
}

static int cmp_blk(const struct buf *a, const struct buf *b)
{
    return a->blockno < b->blockno ? -1 : a->blockno > b->blockno;
}

int jbd_commit(struct jbd *j)
{
    if (!j->n) {
        j->nfreed = 0;
        return 0;
    }
    uint32_t per = tags_per_desc(j) / j->tag_bytes;
    if (!per)
        return -EINVAL;
    /* one descriptor per 'per' blocks (the first tag also carries the UUID) */
    per = (tags_per_desc(j) - 16) / j->tag_bytes;
    uint32_t ndesc = (j->n + per - 1) / per, total = j->n + ndesc + 1;
    if (total > j->maxlen - j->first) {
        kprintf("jbd2: transaction of %u blocks does not fit the journal\n", j->n);
        return -ENOSPC;
    }
    int err = 0;
    uint32_t seq = j->seq;
    /* 1: the log is in use from s_first, with this transaction */
    put_be32(j->sbuf + S_SEQUENCE, seq);
    put_be32(j->sbuf + S_START, j->first);
    if (write_sb(j) < 0)
        return -EIO;
    /* 2: descriptors and copies, then the commit block */
    uint8_t *chunk = kmalloc((size_t)(per + 1) * j->bs);
    if (!chunk)
        return -ENOMEM;
    uint32_t lblk = j->first;
    for (uint32_t i = 0; i < j->n && !err; i += per) {
        uint32_t k = MIN(per, j->n - i);
        uint8_t *d = chunk;
        memset(d, 0, j->bs);
        put_be32(d, JBD2_MAGIC);
        put_be32(d + 4, JT_DESCRIPTOR);
        put_be32(d + 8, seq);
        uint32_t off = 12;
        for (uint32_t t = 0; t < k; t++) {
            struct buf *b = j->blocks[i + t];
            uint8_t *copy = chunk + (size_t)(t + 1) * j->bs;
            memcpy(copy, b->data, j->bs);
            uint32_t flags = t ? JF_SAME_UUID : 0;
            if (be32(copy) == JBD2_MAGIC) {          /* escape the magic number */
                put_be32(copy, 0);
                flags |= JF_ESCAPE;
            }
            if (t == k - 1)
                flags |= JF_LAST_TAG;
            uint8_t *tag = d + off;
            put_be32(tag, (uint32_t)b->blockno);
            if (j->csum3) {
                put_be32(tag + 4, flags);
                put_be32(tag + 8, (uint32_t)(b->blockno >> 32));
                uint8_t s4[4];
                put_be32(s4, seq);
                uint32_t c = csum(j, j->cseed, s4, 4);
                put_be32(tag + 12, csum(j, c, b->data, j->bs));   /* of the unescaped block */
            } else {
                uint16_t c16 = 0;
                if (j->csum) {
                    uint8_t s4[4];
                    put_be32(s4, seq);
                    c16 = (uint16_t)csum(j, csum(j, j->cseed, s4, 4), b->data, j->bs);
                }
                tag[4] = c16 >> 8;
                tag[5] = c16;
                tag[6] = flags >> 8;
                tag[7] = flags;
                if (j->b64)
                    put_be32(tag + 8, (uint32_t)(b->blockno >> 32));
            }
            off += j->tag_bytes;
            if (!t) {
                memcpy(d + off, j->uuid, 16);
                off += 16;
            }
        }
        if (j->csum)
            put_be32(d + j->bs - 4, csum(j, j->cseed, d, j->bs));
        if (jwrite(j, lblk, k + 1, chunk) < 0)
            err = -EIO;
        lblk += k + 1;
    }
    kfree(chunk);
    if (!err) {
        uint8_t *c = j->io;
        memset(c, 0, j->bs);
        put_be32(c, JBD2_MAGIC);
        put_be32(c + 4, JT_COMMIT);
        put_be32(c + 8, seq);
        uint64_t sec = kernel_time();
        put_be32(c + 48, (uint32_t)(sec >> 32));     /* h_commit_sec (be64), h_commit_nsec */
        put_be32(c + 52, (uint32_t)sec);
        if (j->csum)
            put_be32(c + 16, csum(j, j->cseed, c, j->bs));
        if (jwrite(j, lblk, 1, c) < 0)
            err = -EIO;
    }
    if (err) {
        kprintf("jbd2: journal write failed; the file system may need checking\n");
        return err;
    }
    if (jbd_crash_test) {                            /* committed, not checkpointed: power off */
        kprintf("jbd2: test: powering off after committing transaction %u (%u blocks)\n", seq, j->n);
        outw(0x604, 0x2000);
        outw(0xB004, 0x2000);
        cli();
        for (;;)
            hlt();
    }
    /* 3: checkpoint, in block order, in runs of consecutive blocks */
    for (uint32_t a = 1; a < j->n; a++) {             /* insertion sort */
        struct buf *b = j->blocks[a];
        uint32_t k = a;
        while (k && cmp_blk(j->blocks[k - 1], b) > 0) {
            j->blocks[k] = j->blocks[k - 1];
            k--;
        }
        j->blocks[k] = b;
    }
    uint32_t spb = j->bs / 512;
    uint8_t *run = kmalloc(64 * (size_t)j->bs);
    for (uint32_t i = 0; i < j->n;) {
        uint32_t k = 1;
        while (run && i + k < j->n && k < 64 && j->blocks[i + k]->blockno == j->blocks[i]->blockno + k)
            k++;
        int r;
        if (run && k > 1) {
            for (uint32_t t = 0; t < k; t++)
                memcpy(run + (size_t)t * j->bs, j->blocks[i + t]->data, j->bs);
            r = blk_write(j->dev, j->blocks[i]->blockno * spb, k * spb, run);
        } else {
            r = blk_write(j->dev, j->blocks[i]->blockno * spb, spb, j->blocks[i]->data);
        }
        if (r < 0)
            err = -EIO;
        i += k;
    }
    kfree(run);
    for (uint32_t i = 0; i < j->n; i++)
        brelse(j->blocks[i]);
    j->n = 0;
    j->nfreed = 0;
    j->commits++;
    if (err) {
        kprintf("jbd2: checkpoint failed; the journal is kept for recovery\n");
        j->seq = seq + 1;
        return err;
    }
    /* 4: the log is empty again */
    j->seq = seq + 1;
    put_be32(j->sbuf + S_SEQUENCE, j->seq);
    put_be32(j->sbuf + S_START, 0);
    return write_sb(j) < 0 ? -EIO : 0;
}

void jbd_close(struct jbd *j)
{
    kfree(j->sbuf);
    kfree(j->io);
    kfree(j->blocks);
    kfree(j->freed);
    kfree(j->map);
    memset(j, 0, sizeof(*j));
}
