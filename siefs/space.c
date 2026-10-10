/*
 * space.c - Free space, the commit, formatting and mounting.
 *
 * Disk layout (4 KiB blocks):
 *   0          unused (room for a boot record)
 *   1, 2       superblock slots 0 and 1
 *   idx[0]     bitmap copy 0: its index (the checksum of each bitmap block)
 *   bm[0]      bitmap copy 0: one bit per block, 1 = in use
 *   idx[1], bm[1]   the same, copy 1
 *   data_start ...  tree nodes and file data, anywhere
 *
 * Commit number s writes bitmap copy s % 2, then superblock slot s % 2: the
 * previous commit's slot and bitmap are not touched, so a crash at any
 * moment leaves at least the previous commit whole.
 *
 * Freed blocks: a block the last commit still uses may not be overwritten
 * before the next commit lands, or a crash would find it changed. We go
 * further: a block freed by transaction t becomes usable only after
 * commit t + 1, so the two superblocks on disk always describe two intact
 * states (the newest, and the one before as a fallback).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "siefs_int.h"

int dev_read(siefs_t *fs, uint64_t b, uint32_t n, void *buf)
{
    return b + n > fs->sb.nblocks || fs->env.read(fs->env.ctx, b, n, buf) ? E(EIO) : 0;
}
int dev_write(siefs_t *fs, uint64_t b, uint32_t n, const void *buf)
{
    return b + n > fs->sb.nblocks || fs->env.write(fs->env.ctx, b, n, buf) ? E(EIO) : 0;
}
static int dev_flush(siefs_t *fs) { return fs->env.flush(fs->env.ctx) ? E(EIO) : 0; }

static void set1(uint8_t *m, uint64_t b) { m[b >> 3] |= 1 << (b & 7); }
static void set0(uint8_t *m, uint64_t b) { m[b >> 3] &= ~(1 << (b & 7)); }

/* The bitmap that the next commit writes: remember which of its blocks
 * changed, for each of the two copies. */
static void map_mark(siefs_t *fs, uint64_t b, int used)
{
    if (used) set1(fs->map, b); else set0(fs->map, b);
    set1(fs->bmdirty[0], b / BPB);
    set1(fs->bmdirty[1], b / BPB);
}

static void run_add(siefs_t *fs, runs_t *r, uint64_t b, uint64_t n)
{
    if (r->n && r->r[r->n - 1].b + r->r[r->n - 1].n == b) { r->r[r->n - 1].n += n; return; }
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 64;
        run_t *nr = fs->env.alloc(cap * sizeof *nr);
        if (!nr) { fs->broken = 1; return; }            /* cannot remember: stop before damage */
        if (r->n) memcpy(nr, r->r, r->n * sizeof *nr);
        fs->env.free(r->r);
        r->r = nr; r->cap = cap;
    }
    r->r[r->n++] = (run_t){ b, n };
}

/* Up to want consecutive free blocks: the first run that long, else the
 * longest one found. Returns its first block, and its length in *got
 * (0: the disk is full). Searching continues from the last allocation, so
 * successive allocations land next to each other. */
uint64_t blk_alloc(siefs_t *fs, uint64_t want, uint64_t *got)
{
    uint64_t nb = fs->sb.nblocks, b = fs->hint, best = 0, bestn = 0;
    *got = 0;
    if (!fs->nfree || !want) return 0;
    for (uint64_t seen = 0; seen < nb && bestn < want; ) {
        if (b >= nb) b = fs->sb.data_start;
        if (!(b & 7) && fs->busy[b >> 3] == 0xFF) { b += 8; seen += 8; continue; }
        if (bit(fs->busy, b)) { b++; seen++; continue; }
        uint64_t s = b;
        while (b < nb && b - s < want && !bit(fs->busy, b)) b++;
        seen += b - s;
        if (b - s > bestn) { best = s; bestn = b - s; }
    }
    if (!bestn) return 0;
    for (uint64_t i = best; i < best + bestn; i++) { set1(fs->busy, i); set1(fs->fresh, i); map_mark(fs, i, 1); }
    run_add(fs, &fs->fruns, best, bestn);
    fs->nfree -= bestn;
    fs->hint = best + bestn;
    *got = bestn;
    return best;
}

/* Blocks no longer used by the trees. Allocated in this same transaction:
 * nothing on disk refers to them, they are free at once. Otherwise they
 * wait (see the top of this file). */
void blk_free(siefs_t *fs, uint64_t b, uint64_t n)
{
    for (uint64_t i = b; i < b + n; i++) {
        map_mark(fs, i, 0);
        if (bit(fs->fresh, i)) { set0(fs->fresh, i); set0(fs->busy, i); fs->nfree++; }
        else { run_add(fs, &fs->pend[0], i, 1); fs->npend++; }
    }
}

/* A block for a node being written by the commit: from the run reserved
 * at its start (so nodes land together), else wherever. */
uint64_t commit_block(siefs_t *fs)
{
    uint64_t got, b;
    if (fs->pren) { fs->pren--; return fs->pre++; }
    b = blk_alloc(fs, 1, &got);
    return got ? b : 0;
}

/* Make sure an operation will have room before it changes anything:
 * need blocks for its data, plus room for the commit's nodes. If space is
 * short but blocks are waiting to be freed, two commits release them. */
int space_reserve(siefs_t *fs, uint64_t need)
{
    need += fs->ndirty + 64;
    if (fs->nfree >= need) return 0;
    if (fs->npend) { commit(fs, 1); commit(fs, 1); fs->opmod = 0; }   /* whole commits: this operation changed nothing yet */
    return fs->broken ? E(EIO) : fs->nfree >= need ? 0 : E(ENOSPC);
}

/* Write bitmap copy s: the blocks that changed since it was last written,
 * then its index. The new superblock gets the index checksums. */
static int write_bitmap(siefs_t *fs, int s)
{
    int any = 0, e;
    for (uint64_t i = 0; i < fs->sb.nbm; i++) {
        if (!bit(fs->bmdirty[s], i)) continue;
        if ((e = dev_write(fs, fs->sb.bm[s] + i, 1, fs->map + i * BS))) return e;
        fs->bmsum[s][i] = siefs_crc32c(0, fs->map + i * BS, BS);
        set0(fs->bmdirty[s], i);
        any = 1;
    }
    for (uint32_t j = 0; any && j < fs->sb.nidx; j++) {
        uint32_t *idx = (uint32_t *)fs->io;
        memset(idx, 0, BS);
        for (uint64_t i = j * SUMS_PER_IDX; i < fs->sb.nbm && i < (j + 1) * SUMS_PER_IDX; i++)
            idx[i % SUMS_PER_IDX] = fs->bmsum[s][i];
        if ((e = dev_write(fs, fs->sb.idx[s] + j, 1, idx))) return e;
        fs->idxsum[s][j] = siefs_crc32c(0, idx, BS);
    }
    return 0;
}

static uint32_t sb_sum(sb_t *sb)
{
    uint32_t c = sb->csum, r;
    sb->csum = 0;
    r = siefs_crc32c(0, sb, BS);
    sb->csum = c;
    return r;
}

/* ---- The commit: everything changed since the last one reaches the disk
 * at once. The order is what makes it safe:
 *   1. new tree nodes (file data was already written by the operations);
 *   2. flush: all of it is really on the disk;
 *   3. this slot's bitmap copy; flush;
 *   4. this slot's superblock, pointing at all of it; flush. Until this
 *      single block is written, the disk still describes the old state. */
int commit(siefs_t *fs, int force)
{
    uint64_t got;
    int e, s = fs->txn & 1;
    if (fs->broken) return E(EIO);
    if (!force && !fs->changed) return 0;
    fs->pre = blk_alloc(fs, fs->ndirty + 8, &got);
    fs->pren = got;
    if ((e = t_flush(fs, &fs->ft.root))) goto fail;
    fs->vol.root = fs->ft.root;
    if ((e = t_wflush(fs)) || (e = t_update(fs, &fs->rt, K(1, T_VOLUME, 0), &fs->vol, sizeof fs->vol)) ||
        (e = t_flush(fs, &fs->rt.root)) || (e = t_wflush(fs))) goto fail;
    if (fs->pren) { blk_free(fs, fs->pre, fs->pren); fs->pren = 0; }
    if ((e = dev_flush(fs)) || (e = write_bitmap(fs, s)) || (e = dev_flush(fs))) goto fail;

    sb_t *nsb = (sb_t *)fs->io;
    *nsb = fs->sb;
    nsb->seq = fs->txn;
    nsb->root = fs->rt.root;
    nsb->used = fs->sb.nblocks - fs->nfree - fs->npend;
    memcpy(nsb->idxsum, fs->idxsum[s], sizeof nsb->idxsum);
    nsb->csum = sb_sum(nsb);
    if ((e = dev_write(fs, 1 + s, 1, nsb)) || (e = dev_flush(fs))) goto fail;
    fs->sb = *nsb;

    /* Committed. Blocks freed by the previous transaction can now be
     * reused; this one's wait for the next commit. */
    runs_t old = fs->pend[1];
    for (size_t i = 0; i < old.n; i++)
        for (uint64_t b = old.r[i].b; b < old.r[i].b + old.r[i].n; b++) { set0(fs->busy, b); fs->nfree++; fs->npend--; }
    fs->pend[1] = fs->pend[0];
    fs->pend[0] = (runs_t){ old.r, 0, old.cap };
    for (size_t i = 0; i < fs->fruns.n; i++)
        for (uint64_t b = fs->fruns.r[i].b; b < fs->fruns.r[i].b + fs->fruns.r[i].n; b++) set0(fs->fresh, b);
    fs->fruns.n = 0;
    fs->txn++;
    fs->changed = 0;
    cache_trim(fs);
    return 0;
fail:
    fs->broken = 1;      /* the disk still holds the last commit: remount to go back to it */
    return e;
}

/* ---- Setting up the in-memory state for a superblock. */
static void fs_close(siefs_t *fs)
{
    if (!fs) return;
    void (*fr)(void *) = fs->env.free;
    if (fs->dirty) nodes_free(fs);
    void *p[] = { fs->map, fs->busy, fs->fresh, fs->bmdirty[0], fs->bmdirty[1], fs->bmsum[0], fs->bmsum[1],
                  fs->pend[0].r, fs->pend[1].r, fs->fruns.r, fs->dirty, fs->hash, fs->io };
    for (size_t i = 0; i < sizeof p / sizeof *p; i++) if (p[i]) fr(p[i]);
    fr(fs);
}

static void *zalloc(siefs_t *fs, size_t n)
{
    void *p = fs->env.alloc(n);
    if (p) memset(p, 0, n);
    return p;
}

static int fs_open(const siefs_env_t *env, const sb_t *sb, siefs_t **out)
{
    siefs_t *fs = env->alloc(sizeof *fs);
    if (!fs) return E(ENOMEM);
    memset(fs, 0, sizeof *fs);
    fs->env = *env;
    if (!fs->env.cache_nodes) fs->env.cache_nodes = 128;
    if (!fs->env.dirty_limit) fs->env.dirty_limit = 256;
    if (!fs->env.commit_ns) fs->env.commit_ns = 5000000000LL;
    fs->sb = *sb;
    fs->lru.next = fs->lru.prev = &fs->lru;
    fs->hbits = 10;
    while ((1u << fs->hbits) < fs->env.cache_nodes * 2 && fs->hbits < 20) fs->hbits++;
    size_t mapb = sb->nbm * BS, dirtyb = (sb->nbm + 7) / 8;
    fs->map = zalloc(fs, mapb); fs->busy = zalloc(fs, mapb); fs->fresh = zalloc(fs, mapb);
    fs->bmdirty[0] = zalloc(fs, dirtyb); fs->bmdirty[1] = zalloc(fs, dirtyb);
    fs->bmsum[0] = zalloc(fs, sb->nbm * 4); fs->bmsum[1] = zalloc(fs, sb->nbm * 4);
    fs->hash = zalloc(fs, sizeof(node_t *) << fs->hbits);
    fs->io = zalloc(fs, EXT_MAX * BS);
    fs->hint = sb->data_start;
    fs->rt.id = 0; fs->ft.id = 1;
    if (!fs->map || !fs->busy || !fs->fresh || !fs->bmdirty[0] || !fs->bmdirty[1] || !fs->bmsum[0] ||
        !fs->bmsum[1] || !fs->hash || !fs->io) { fs_close(fs); return E(ENOMEM); }
    *out = fs;
    return 0;
}

static uint64_t popcount8(uint8_t x) { x = x - (x >> 1 & 0x55); x = (x & 0x33) + (x >> 2 & 0x33); return (x + (x >> 4)) & 0x0F; }

/* ---- Format: lay out the disk, then make an empty file system (a root
 * directory) through the normal code, and commit twice so that both
 * superblock slots are valid. */
int siefs_format(const siefs_env_t *env, const char *label)
{
    uint64_t nb = env->nblocks, nbm = (nb + BPB - 1) / BPB, nidx = (nbm + SUMS_PER_IDX - 1) / SUMS_PER_IDX;
    if (nidx > MAX_IDX) return E(EFBIG);
    sb_t *sb = env->alloc(sizeof *sb);
    if (!sb) return E(ENOMEM);
    memset(sb, 0, sizeof *sb);
    sb->magic = SB_MAGIC; sb->version = 1; sb->bs = BS;
    sb->nblocks = nb; sb->nbm = nbm; sb->nidx = (uint32_t)nidx;
    sb->idx[0] = 3;               sb->bm[0] = sb->idx[0] + nidx;
    sb->idx[1] = sb->bm[0] + nbm; sb->bm[1] = sb->idx[1] + nidx;
    sb->data_start = sb->bm[1] + nbm;
    sb->created = env->now();
    for (int i = 0; label && label[i] && i < 31; i++) sb->label[i] = label[i];
    uint64_t u[2] = { (uint64_t)sb->created, nb };
    for (int i = 0; i < 4; i++) { uint32_t c = siefs_crc32c((uint32_t)i, u, sizeof u); memcpy(sb->uuid + 4 * i, &c, 4); }
    if (sb->data_start + 32 > nb) { env->free(sb); return E(EINVAL); }

    siefs_t *fs;
    int e = fs_open(env, sb, &fs);
    env->free(sb);
    if (e) return e;
    for (uint64_t b = 0; b < nbm * BPB; b++)            /* the layout, and past the end: never free */
        if (b < fs->sb.data_start || b >= nb) { set1(fs->map, b); set1(fs->busy, b); }
    fs->nfree = nb - fs->sb.data_start;
    memset(fs->bmdirty[0], 0xFF, (nbm + 7) / 8);
    memset(fs->bmdirty[1], 0xFF, (nbm + 7) / 8);
    fs->txn = 1;

    int64_t t = env->now();
    inode_t root = { .mode = SIEFS_IFDIR | 0755, .nlink = 2, .parent = SIEFS_ROOT, .atime = t, .mtime = t, .ctime = t, .btime = t };
    fs->vol = (vol_t){ .next_id = 16, .inodes = 1 };
    if (!(e = t_new_root(fs, &fs->rt)) && !(e = t_new_root(fs, &fs->ft)) &&
        !(e = t_insert(fs, &fs->ft, K(SIEFS_ROOT, T_INODE, 0), &root, sizeof root)) &&
        !(e = t_insert(fs, &fs->rt, K(1, T_VOLUME, 0), &fs->vol, sizeof fs->vol)) &&
        !(e = commit(fs, 1)))
        e = commit(fs, 1);
    fs_close(fs);
    return e;
}

static int sb_valid(const siefs_env_t *env, sb_t *sb)
{
    return sb->magic == SB_MAGIC && sb->version == 1 && sb->bs == BS && sb->csum == sb_sum(sb) &&
           sb->nblocks <= env->nblocks && sb->nbm == (sb->nblocks + BPB - 1) / BPB &&
           sb->nidx == (sb->nbm + SUMS_PER_IDX - 1) / SUMS_PER_IDX && sb->nidx <= MAX_IDX &&
           sb->data_start == 3 + 2 * (sb->nidx + sb->nbm) && sb->data_start < sb->nblocks;
}

/* Load the state a superblock describes: its bitmap copy (every block
 * checked against the index, the index against the superblock), then the
 * root tree and the volume. */
static int load(siefs_t *fs)
{
    int s = fs->sb.seq & 1, e;
    for (uint32_t j = 0; j < fs->sb.nidx; j++) {
        uint32_t *idx = (uint32_t *)fs->io;
        if ((e = dev_read(fs, fs->sb.idx[s] + j, 1, idx))) return e;
        if (siefs_crc32c(0, idx, BS) != fs->sb.idxsum[j]) return E(EIO);
        for (uint64_t i = j * SUMS_PER_IDX; i < fs->sb.nbm && i < (j + 1) * SUMS_PER_IDX; i++)
            fs->bmsum[s][i] = idx[i % SUMS_PER_IDX];
        fs->idxsum[s][j] = fs->sb.idxsum[j];
    }
    for (uint64_t i = 0; i < fs->sb.nbm; i++) {
        if ((e = dev_read(fs, fs->sb.bm[s] + i, 1, fs->map + i * BS))) return e;
        if (siefs_crc32c(0, fs->map + i * BS, BS) != fs->bmsum[s][i]) return E(EIO);
    }
    memcpy(fs->busy, fs->map, fs->sb.nbm * BS);
    uint64_t used = 0;
    for (uint64_t i = 0; i < fs->sb.nbm * BS; i++) used += popcount8(fs->map[i]);
    fs->nfree = fs->sb.nbm * BPB - used;
    memset(fs->bmdirty[!s], 0xFF, (fs->sb.nbm + 7) / 8);   /* the other copy: rewrite it entirely */
    fs->rt.root = fs->sb.root;
    fs->txn = fs->sb.seq + 1;
    if ((e = t_get(fs, &fs->rt, K(1, T_VOLUME, 0), &fs->vol, sizeof fs->vol)) < 0) return e;
    if (e != sizeof fs->vol) return E(EIO);
    fs->ft.root = fs->vol.root;
    node_t *n;
    return t_get_node(fs, &fs->ft.root, &n);
}

/* Mount: of the two superblocks, use the newest valid one whose state
 * loads; if it does not, fall back to the other. */
int siefs_mount(const siefs_env_t *env, siefs_t **out)
{
    sb_t *sb = env->alloc(2 * sizeof *sb);
    int e = E(EINVAL), ok[2];
    if (!sb) return E(ENOMEM);
    for (int i = 0; i < 2; i++)
        ok[i] = !env->read(env->ctx, 1 + i, 1, &sb[i]) && sb_valid(env, &sb[i]);
    int first = ok[0] && (!ok[1] || sb[0].seq > sb[1].seq) ? 0 : 1;
    for (int k = 0; k < 2; k++) {
        int i = k ? !first : first;
        siefs_t *fs;
        if (!ok[i]) continue;
        if ((e = fs_open(env, &sb[i], &fs))) break;
        if (!(e = load(fs))) { env->free(sb); *out = fs; return 0; }
        fs_close(fs);
    }
    env->free(sb);
    return e;
}

int siefs_unmount(siefs_t *fs)
{
    int e = fs->broken ? 0 : commit(fs, 0);
    fs_close(fs);
    return e;
}

int siefs_sync(siefs_t *fs) { return commit(fs, 0); }

int siefs_tick(siefs_t *fs)
{
    if (fs->changed && fs->env.now() - fs->first_change >= fs->env.commit_ns) return commit(fs, 0);
    return fs->broken ? E(EIO) : 0;
}

int siefs_statfs(siefs_t *fs, siefs_statfs_t *st)
{
    st->blocks = fs->sb.nblocks;
    st->free = fs->nfree;
    st->pending = fs->npend;
    st->inodes = fs->vol.inodes;
    st->commits = fs->sb.seq;
    memcpy(st->label, fs->sb.label, sizeof st->label);
    st->label[31] = 0;
    return 0;
}
