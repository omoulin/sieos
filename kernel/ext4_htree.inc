/*
 * ext4_htree.inc - Hashed (htree) directories, included by ext4.c.
 *
 * Lookups and readdir walk directory blocks linearly, which works on hashed
 * directories too: their index blocks read as empty entries.  Adding an
 * entry keeps the index valid: the name's hash (legacy, half-MD4 or TEA, as
 * the directory's root says) selects a leaf through the root and at most one
 * level of index nodes; a full leaf is split by hash and the new leaf goes
 * into the parent index.  When that index block is full too (a directory of
 * some hundred thousand entries), the directory is turned into a plain linear
 * one, which ext4 and e2fsck accept.
 */

#define DX_HASH_LEGACY   0
#define DX_HASH_HALF_MD4 1
#define DX_HASH_TEA      2
#define EXT2_FLAGS_UNSIGNED_HASH 0x0002

static inline uint32_t rol32(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

static uint32_t dx_hack_hash(const char *name, int len, bool unsig)
{
    uint32_t hash, hash0 = 0x12a3fe2d, hash1 = 0x37abe8f9;
    while (len--) {
        int c = unsig ? (int)(unsigned char)*name : (int)(signed char)*name;
        name++;
        hash = hash1 + (hash0 ^ (uint32_t)(c * 7152373));
        if (hash & 0x80000000)
            hash -= 0x7fffffff;
        hash1 = hash0;
        hash0 = hash;
    }
    return hash0 << 1;
}

static void str2hashbuf(const char *msg, int len, uint32_t *buf, int num, bool unsig)
{
    uint32_t pad = (uint32_t)len | ((uint32_t)len << 8), val;
    pad |= pad << 16;
    val = pad;
    if (len > num * 4)
        len = num * 4;
    for (int i = 0; i < len; i++) {
        int c = unsig ? (int)(unsigned char)msg[i] : (int)(signed char)msg[i];
        val = (uint32_t)c + (val << 8);
        if ((i % 4) == 3) {
            *buf++ = val;
            val = pad;
            num--;
        }
    }
    if (--num >= 0)
        *buf++ = val;
    while (--num >= 0)
        *buf++ = pad;
}

#define F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define G(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define ROUND(f, a, b, c, d, x, s) (a += f(b, c, d) + (x), a = rol32(a, s))
#define K2 013240474631U
#define K3 015666365641U

static void half_md4_transform(uint32_t buf[4], const uint32_t in[8])
{
    uint32_t a = buf[0], b = buf[1], c = buf[2], d = buf[3];
    ROUND(F, a, b, c, d, in[0], 3);
    ROUND(F, d, a, b, c, in[1], 7);
    ROUND(F, c, d, a, b, in[2], 11);
    ROUND(F, b, c, d, a, in[3], 19);
    ROUND(F, a, b, c, d, in[4], 3);
    ROUND(F, d, a, b, c, in[5], 7);
    ROUND(F, c, d, a, b, in[6], 11);
    ROUND(F, b, c, d, a, in[7], 19);
    ROUND(G, a, b, c, d, in[1] + K2, 3);
    ROUND(G, d, a, b, c, in[3] + K2, 5);
    ROUND(G, c, d, a, b, in[5] + K2, 9);
    ROUND(G, b, c, d, a, in[7] + K2, 13);
    ROUND(G, a, b, c, d, in[0] + K2, 3);
    ROUND(G, d, a, b, c, in[2] + K2, 5);
    ROUND(G, c, d, a, b, in[4] + K2, 9);
    ROUND(G, b, c, d, a, in[6] + K2, 13);
    ROUND(H, a, b, c, d, in[3] + K3, 3);
    ROUND(H, d, a, b, c, in[7] + K3, 9);
    ROUND(H, c, d, a, b, in[2] + K3, 11);
    ROUND(H, b, c, d, a, in[6] + K3, 15);
    ROUND(H, a, b, c, d, in[1] + K3, 3);
    ROUND(H, d, a, b, c, in[5] + K3, 9);
    ROUND(H, c, d, a, b, in[0] + K3, 11);
    ROUND(H, b, c, d, a, in[4] + K3, 15);
    buf[0] += a;
    buf[1] += b;
    buf[2] += c;
    buf[3] += d;
}

static void tea_transform(uint32_t buf[4], const uint32_t in[4])
{
    uint32_t sum = 0, b0 = buf[0], b1 = buf[1];
    uint32_t a = in[0], b = in[1], c = in[2], d = in[3];
    for (int n = 16; n; n--) {
        sum += 0x9E3779B9;
        b0 += ((b1 << 4) + a) ^ (b1 + sum) ^ ((b1 >> 5) + b);
        b1 += ((b0 << 4) + c) ^ (b0 + sum) ^ ((b0 >> 5) + d);
    }
    buf[0] += b0;
    buf[1] += b1;
}

/* The directory hash of a name (ext4's, low bit clear), for hash version ver (0-5). */
static uint32_t dx_hash(const char *name, int len, int ver)
{
    uint32_t buf[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 }, in[8], hash;
    if (sb.s_hash_seed[0] | sb.s_hash_seed[1] | sb.s_hash_seed[2] | sb.s_hash_seed[3])
        memcpy(buf, sb.s_hash_seed, sizeof(buf));
    bool unsig = ver >= 3;
    switch (ver % 3) {
    case DX_HASH_LEGACY:
        hash = dx_hack_hash(name, len, unsig);
        break;
    case DX_HASH_HALF_MD4:
        for (const char *p = name; len > 0; len -= 32, p += 32) {
            str2hashbuf(p, len, in, 8, unsig);
            half_md4_transform(buf, in);
        }
        hash = buf[1];
        break;
    default:
        for (const char *p = name; len > 0; len -= 16, p += 16) {
            str2hashbuf(p, len, in, 4, unsig);
            tea_transform(buf, in);
        }
        hash = buf[0];
        break;
    }
    hash &= ~1U;
    if (hash == (0x7fffffffU << 1))
        hash = (0x7fffffffU - 1) << 1;
    return hash;
}

/* ---------------- index blocks ---------------- */

struct dx_entry {
    uint32_t hash;                   /* (entries[0]: the limit and count instead) */
    uint32_t block;                  /* logical block in the directory */
};

struct dx_countlimit {
    uint16_t limit, count;
};

/* Where an index block's count/limit and entries start, and how many fit. */
static uint32_t dx_offset(bool root)
{
    return root ? 32 : 8;            /* after "." ".." and the root info, or a fake empty entry */
}

static void dx_csum(struct inode *dir, uint8_t *data, bool root)
{
    if (!f_csum)
        return;
    uint32_t off = dx_offset(root);
    struct dx_countlimit *cl = (struct dx_countlimit *)(data + off);
    uint8_t *tail = data + off + cl->limit * 8;                  /* dt_reserved, dt_checksum */
    if (tail + 8 > data + bs)
        return;
    uint32_t zero = 0;
    uint32_t c = crc32c(inode_seed(dir->ino, dir->raw), data, off + cl->count * 8);
    c = crc32c(c, tail, 4);
    c = crc32c(c, &zero, 4);
    memcpy(tail + 4, &c, 4);
}

static int dx_version(const uint8_t *root)
{
    int ver = root[28];
    if (ver <= DX_HASH_TEA && (sb.s_flags & EXT2_FLAGS_UNSIGNED_HASH))
        ver += 3;
    return ver;
}

/* The entry of an index block whose range holds hash (binary search). */
static int dx_find(const struct dx_entry *e, int count, uint32_t hash)
{
    int lo = 1, hi = count - 1, found = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (e[mid].hash <= hash) {
            found = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return found;
}

/* ---------------- un-indexing ---------------- */

/* Rewrite an index block as a directory block with no entries (keeping "." ".." in the root). */
static void dx_to_linear(struct inode *dir, uint8_t *data, bool root)
{
    uint32_t limit = dir_block_limit();
    if (root) {
        struct ext4_dirent *dot = (struct ext4_dirent *)data;
        struct ext4_dirent *dotdot = (struct ext4_dirent *)(data + dot->rec_len);
        dotdot->rec_len = limit - dot->rec_len;
        memset(data + dot->rec_len + 12, 0, limit - dot->rec_len - 12);
    } else {
        memset(data, 0, bs);
        ((struct ext4_dirent *)data)->rec_len = limit;
    }
    dirblock_init_tail(data);
    dirblock_csum(dir, data);
}

/* Turn a hashed directory into a linear one: its index blocks become empty entries. */
static int dx_unindex(struct inode *dir)
{
    uint64_t pblk;
    if (bmap(dir, 0, &pblk) < 0 || !pblk)
        return -EIO;
    struct buf *b = vbread(pblk);
    if (!b)
        return -EIO;
    int levels = b->data[30];
    struct dx_countlimit *cl = (struct dx_countlimit *)(b->data + dx_offset(true));
    struct dx_entry *e = (struct dx_entry *)cl;
    uint32_t nodes[512];
    int nn = 0;
    if (levels >= 1)
        for (int i = 0; i < cl->count && nn < 512; i++)
            nodes[nn++] = e[i].block;
    dx_to_linear(dir, b->data, true);
    vbwrite(b);
    brelse(b);
    for (int i = 0; i < nn; i++) {
        if (bmap(dir, nodes[i], &pblk) < 0 || !pblk)
            continue;
        struct buf *nb = vbread(pblk);
        if (!nb)
            continue;
        dx_to_linear(dir, nb->data, false);
        vbwrite(nb);
        brelse(nb);
    }
    DI(dir)->i_flags &= ~EXT4_INDEX_FL;
    kprintf("ext4: directory %u: its hash index is full; it is a linear directory now\n", dir->ino);
    return ext4_update(dir);
}

/* ---------------- adding ---------------- */

struct dx_ent {
    uint32_t hash;
    uint16_t off, len;               /* the entry in the old block, its name length */
};

/* Rewrite block data with the given entries (from old), compactly, with a tail. */
static void leaf_fill(struct inode *dir, uint8_t *data, const uint8_t *old, const struct dx_ent *ents, int n)
{
    uint32_t limit = dir_block_limit(), off = 0;
    memset(data, 0, bs);
    struct ext4_dirent *last = NULL;
    for (int i = 0; i < n; i++) {
        const struct ext4_dirent *src = (const struct ext4_dirent *)(old + ents[i].off);
        struct ext4_dirent *de = (struct ext4_dirent *)(data + off);
        uint32_t rl = rec_len_for(src->name_len);
        memcpy(de, src, 8 + src->name_len);
        de->rec_len = rl;
        last = de;
        off += rl;
    }
    if (last)
        last->rec_len += limit - off;
    else
        ((struct ext4_dirent *)data)->rec_len = limit;
    dirblock_init_tail(data);
    dirblock_csum(dir, data);
}

/* Add to one directory block; 1 if it fit. */
static int leaf_add(struct inode *dir, uint64_t pblk, struct add_arg *a)
{
    struct buf *b = vbread(pblk);
    if (!b)
        return -EIO;
    bool dirty = false;
    int done = 0;
    struct ext4_dirent *prev = NULL;
    for (uint32_t off = 0; off + 8 <= bs && !done;) {
        struct ext4_dirent *de = (struct ext4_dirent *)(b->data + off);
        if (de->rec_len < 8 || off + de->rec_len > bs)
            break;
        done = add_cb(de, prev, a, &dirty);
        prev = de;
        off += de->rec_len;
    }
    if (dirty) {
        dirblock_csum(dir, b->data);
        vbwrite(b);
    }
    brelse(b);
    return done;
}

static int ent_cmp(const struct dx_ent *x, const struct dx_ent *y)
{
    return x->hash < y->hash ? -1 : x->hash > y->hash;
}

/* Insert {hash, block} after entry pos of the index block (which has room). */
static void dx_insert(struct inode *dir, uint8_t *data, bool root, int pos, uint32_t hash, uint32_t block)
{
    struct dx_countlimit *cl = (struct dx_countlimit *)(data + dx_offset(root));
    struct dx_entry *e = (struct dx_entry *)cl;
    memmove(&e[pos + 2], &e[pos + 1], (cl->count - pos - 1) * sizeof(*e));
    e[pos + 1].hash = hash;
    e[pos + 1].block = block;
    cl->count++;
    dx_csum(dir, data, root);
}

static int dx_add(struct inode *dir, struct add_arg *a)
{
    uint64_t rootp, pblk;
    if (bmap(dir, 0, &rootp) < 0 || !rootp)
        return -EIO;
    struct buf *rb = vbread(rootp);
    if (!rb)
        return -EIO;
    int ver = dx_version(rb->data), levels = rb->data[30];
    if (rb->data[29] != 8 || levels > 1 || ver > 5) {
        brelse(rb);                                  /* an index we do not know: go linear */
        int r = dx_unindex(dir);
        return r < 0 ? r : 0;
    }
    uint32_t hash = dx_hash(a->name, a->len, ver);
    /* down the index: the root, then at most one node */
    struct buf *ib = rb;
    bool iroot = true;
    struct dx_countlimit *cl = (struct dx_countlimit *)(rb->data + dx_offset(true));
    struct dx_entry *e = (struct dx_entry *)cl;
    int pos = dx_find(e, cl->count, hash);
    if (levels == 1) {
        if (bmap(dir, e[pos].block, &pblk) < 0 || !pblk) {
            brelse(rb);
            return -EIO;
        }
        ib = vbread(pblk);
        if (!ib) {
            brelse(rb);
            return -EIO;
        }
        iroot = false;
        cl = (struct dx_countlimit *)(ib->data + dx_offset(false));
        e = (struct dx_entry *)cl;
        pos = dx_find(e, cl->count, hash);
    }
    uint32_t leaf = e[pos].block;
    int r;
    if (bmap(dir, leaf, &pblk) < 0 || !pblk) {
        r = -EIO;
        goto out;
    }
    r = leaf_add(dir, pblk, a);
    if (r != 0)
        goto out;                                    /* added (1), or an error */
    if (cl->count >= cl->limit) {                    /* no room for another leaf in the index */
        if (ib != rb)
            brelse(ib);
        brelse(rb);
        r = dx_unindex(dir);
        return r < 0 ? r : 0;                        /* (the caller adds linearly) */
    }
    /* split the leaf by hash into itself and a new block at the end of the directory */
    struct buf *lb = vbread(pblk);
    if (!lb) {
        r = -EIO;
        goto out;
    }
    uint8_t *old = kmalloc(bs);
    struct dx_ent *ents = kmalloc((bs / 12 + 1) * sizeof(*ents));
    if (!old || !ents) {
        kfree(old);
        kfree(ents);
        brelse(lb);
        r = -ENOMEM;
        goto out;
    }
    memcpy(old, lb->data, bs);
    int n = 0;
    for (uint32_t off = 0; off + 8 <= dir_block_limit();) {
        struct ext4_dirent *de = (struct ext4_dirent *)(old + off);
        if (de->rec_len < 8 || off + de->rec_len > bs)
            break;
        if (de->inode) {
            ents[n].hash = dx_hash(de->name, de->name_len, ver);
            ents[n].off = off;
            ents[n++].len = de->name_len;
        }
        off += de->rec_len;
    }
    for (int i = 1; i < n; i++) {                    /* by hash */
        struct dx_ent t = ents[i];
        int k = i;
        while (k && ent_cmp(&ents[k - 1], &t) > 0) {
            ents[k] = ents[k - 1];
            k--;
        }
        ents[k] = t;
    }
    int m = n / 2;
    while (m > 1 && ents[m].hash == ents[m - 1].hash)
        m--;                                         /* keep equal hashes together if we can */
    uint32_t split = ents[m].hash | (m && ents[m].hash == ents[m - 1].hash ? 1 : 0);
    uint64_t newl = (inode_size(dir) + bs - 1) / bs, newp;
    r = bmap_alloc(dir, newl, &newp);
    if (r < 0) {
        kfree(old);
        kfree(ents);
        brelse(lb);
        goto out;
    }
    struct buf *nb = vbread(newp);
    if (!nb) {
        kfree(old);
        kfree(ents);
        brelse(lb);
        r = -EIO;
        goto out;
    }
    leaf_fill(dir, lb->data, old, ents, m);
    leaf_fill(dir, nb->data, old, ents + m, n - m);
    vbwrite(lb);
    vbwrite(nb);
    brelse(lb);
    brelse(nb);
    kfree(old);
    kfree(ents);
    inode_set_size(dir, (newl + 1) * bs);
    dx_insert(dir, ib->data, iroot, pos, split, (uint32_t)newl);
    vbwrite(ib);
    /* and now the name goes where its hash says */
    r = leaf_add(dir, hash >= split ? newp : pblk, a);
    if (r == 0)
        r = -ENOSPC;                                 /* (cannot happen: each half has room) */
out:
    if (ib != rb)
        brelse(ib);
    brelse(rb);
    return r;
}
