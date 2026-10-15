/*
 * crypt.c - Encrypted volumes: sealing and opening tree nodes and data
 * blocks, and the volume key's life (unlock, lock, a new password).
 *
 * A volume's key is 32 random bytes. Every node of its tree is sealed
 * with ChaCha20-Poly1305 except its 32-byte header (level, count, commit,
 * tree id: they stay readable, and are authenticated as associated data);
 * the 16-byte tag goes in the pointer to the node (bptr_t.mac), the salt
 * of its nonce in the pointer's flags. Every data block is sealed whole,
 * its tag in the extent, the salt once per extent. A nonce is the block's
 * number and a salt: a random one per commit for nodes, a counter from a
 * random start for data writes (NSALT_NODE keeps them apart), so the same
 * block is never sealed twice with the same nonce, crashes included (a
 * commit redone after a crash draws a new salt). Checksums are of what is
 * on the disk, so a volume checks without its key.
 *
 * The key itself is kept in the root tree (volkey_t), sealed with a
 * key-encryption key that the caller derives from the user's password
 * with Argon2id: changing the password seals the same key anew.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "siefs_int.h"

#define BODY (BS - sizeof(nhdr_t))

static void nonce(uint8_t n[12], uint64_t blk, uint32_t salt) { memcpy(n, &blk, 8); memcpy(n + 8, &salt, 4); }

uint32_t crypt_salt(siefs_t *fs)
{
    uint32_t s = 0;
    if (!fs->env.random || fs->env.random(&s, sizeof s)) s = (uint32_t)(fs->env.now() ^ fs->txn * 0x9E3779B9u);
    return s;
}

static int scratch(siefs_t *fs) { return fs->ctmp || (fs->ctmp = fs->env.alloc(BS + 16)) ? 0 : E(ENOMEM); }

/* The key of the volume a node belongs to (its tree id), or 0: not sealed. */
static volx_t *keyed(siefs_t *fs, uint64_t tree)
{
    volx_t *v = tree ? vol_get(fs, tree) : 0;
    return v && (v->v.flags & V_ENC) ? v : 0;
}

int node_open(siefs_t *fs, node_t *n, const bptr_t *p)
{
    volx_t *v = keyed(fs, NH(n)->tree);
    uint8_t nc[12];
    int e;
    if (!v) return 0;
    if (!v->key) return E(EACCES);
    if ((e = scratch(fs))) return e;
    memcpy(fs->ctmp, n->d + sizeof(nhdr_t), BODY);
    memcpy(fs->ctmp + BODY, p->mac, 16);
    nonce(nc, p->blk, p->flags);
    return aead_open(v->key, nc, n->d, sizeof(nhdr_t), fs->ctmp, BODY + 16, n->d + sizeof(nhdr_t)) ? E(EIO) : 0;
}

int node_seal(siefs_t *fs, const node_t *n, uint8_t *out, bptr_t *p)
{
    volx_t *v = keyed(fs, NH(n)->tree);
    uint8_t nc[12];
    int e;
    memcpy(out, n->d, BS);
    if (!v) return 0;
    if (!v->key) return E(EACCES);
    if ((e = scratch(fs))) return e;
    p->flags = fs->nsalt;
    nonce(nc, p->blk, p->flags);
    aead_seal(v->key, nc, n->d, sizeof(nhdr_t), n->d + sizeof(nhdr_t), BODY, fs->ctmp);
    memcpy(out + sizeof(nhdr_t), fs->ctmp, BODY);
    memcpy(p->mac, fs->ctmp + BODY, 16);
    return 0;
}

int data_seal(siefs_t *fs, volx_t *v, uint64_t blk, uint32_t salt, uint8_t *b, uint8_t mac[16])
{
    uint8_t nc[12];
    int e;
    if (!v->key) return E(EACCES);
    if ((e = scratch(fs))) return e;
    nonce(nc, blk, salt);
    aead_seal(v->key, nc, &v->id, sizeof v->id, b, BS, fs->ctmp);
    memcpy(b, fs->ctmp, BS);
    memcpy(mac, fs->ctmp + BS, 16);
    return 0;
}

int data_open(siefs_t *fs, volx_t *v, uint64_t blk, uint32_t salt, uint8_t *b, const uint8_t mac[16])
{
    uint8_t nc[12];
    uint64_t id = v->v.flags & V_SNAP ? v->v.parent : v->id;   /* (sealed by the head) */
    int e;
    if (!v->key) return E(EACCES);
    if ((e = scratch(fs))) return e;
    memcpy(fs->ctmp, b, BS);
    memcpy(fs->ctmp + BS, mac, 16);
    nonce(nc, blk, salt);
    return aead_open(v->key, nc, &id, sizeof id, fs->ctmp, BS + 16, b) ? E(EIO) : 0;
}

/* The volume key is known: v and its snapshots are open. */
int vol_secret(siefs_t *fs, volx_t *v, const uint8_t key[32])
{
    for (uint32_t i = 0; i < fs->nvols; i++) {
        volx_t *w = fs->vols[i];
        if (w != v && !((w->v.flags & V_SNAP) && w->v.parent == v->id)) continue;
        if (!w->key && !(w->key = fs->env.alloc(sizeof(aead_t)))) return E(ENOMEM);
        aead_init(w->key, AEAD_CHACHA20_POLY1305, key);
        w->locked = 0;
    }
    return 0;
}

/* ---- The key's record, and what callers ask. */
static int head_of(siefs_t *fs, uint64_t g, volx_t **h)
{
    volx_t *v = vol_get(fs, VOLOF(g));
    if (v && (v->v.flags & V_SNAP)) v = vol_get(fs, v->v.parent);
    if (!v) return E(ENOENT);
    if (!(v->v.flags & V_ENC)) return E(EINVAL);
    *h = v;
    return 0;
}
static int volkey(siefs_t *fs, volx_t *v, volkey_t *k)
{
    int n = t_get(fs, &fs->rt, K(v->id, T_VOLKEY, 0), k, sizeof *k);
    return n < 0 ? n : n == sizeof *k ? 0 : E(EIO);
}

/* Seal key with kek into k (a fresh nonce). */
static int wrap(siefs_t *fs, volx_t *v, volkey_t *k, const siefs_kdf_t *kdf, const uint8_t kek[32], const uint8_t key[32])
{
    aead_t a;
    if (!fs->env.random || fs->env.random(k->nonce, sizeof k->nonce)) return E(EINVAL);
    memcpy(k->salt, kdf->salt, 16);
    k->t = kdf->t; k->m_kib = kdf->m_kib; k->version = 1;
    aead_init(&a, AEAD_CHACHA20_POLY1305, kek);
    aead_seal(&a, k->nonce, &v->id, sizeof v->id, key, 32, k->wrapped);
    memset(&a, 0, sizeof a);
    return 0;
}

/* A new encrypted volume's key: random, recorded wrapped. (fs.c makes the
 * volume, then calls this before its first node is written.) */
int vol_new_key(siefs_t *fs, volx_t *v, const siefs_kdf_t *kdf, const uint8_t kek[32], uint32_t mode, uint32_t uid,
                uint32_t gid, uint64_t parent)
{
    uint8_t key[32];
    volkey_t k = { .mode = mode, .uid = uid, .gid = gid, .parent = parent };
    int e;
    if (!fs->env.random || fs->env.random(key, sizeof key)) return E(EINVAL);
    v->v.flags |= V_ENC;
    memcpy(v->v.key_id, kdf->salt, 16);
    if (!(e = wrap(fs, v, &k, kdf, kek, key)) && !(e = vol_secret(fs, v, key)))
        e = t_put(fs, &fs->rt, K(v->id, T_VOLKEY, 0), &k, sizeof k);
    memset(key, 0, sizeof key);
    return e;
}

/* What a locked volume's root shows: its owner's directory. */
int vol_locked_stat(siefs_t *fs, volx_t *v, siefs_stat_t *st)
{
    volkey_t k;
    int e = volkey(fs, v, &k);
    if (e) return e;
    *st = (siefs_stat_t){ .ino = v->id << VSHIFT | SIEFS_ROOT, .parent = k.parent, .mode = SIEFS_IFDIR | (k.mode & 07777),
                          .uid = k.uid, .gid = k.gid, .nlink = 2, .btime = v->v.created, .mtime = v->v.created,
                          .ctime = v->v.created, .atime = v->v.created };
    return 0;
}

int siefs_kdf(siefs_t *fs, uint64_t g, siefs_kdf_t *kdf)
{
    volx_t *v;
    volkey_t k;
    int e;
    if ((e = head_of(fs, g, &v)) || (e = volkey(fs, v, &k))) return e;
    memcpy(kdf->salt, k.salt, 16);
    kdf->t = k.t; kdf->m_kib = k.m_kib;
    return 0;
}

int siefs_unlock(siefs_t *fs, uint64_t g, const uint8_t kek[32])
{
    volx_t *v;
    volkey_t k;
    uint8_t key[32];
    aead_t a;
    int e;
    if ((e = head_of(fs, g, &v)) || (e = volkey(fs, v, &k))) return e;
    if (!v->locked) return 0;
    aead_init(&a, AEAD_CHACHA20_POLY1305, kek);
    e = aead_open(&a, k.nonce, &v->id, sizeof v->id, k.wrapped, sizeof k.wrapped, key) ? E(EACCES) : vol_secret(fs, v, key);
    memset(&a, 0, sizeof a);
    memset(key, 0, sizeof key);
    if (!e) {                                           /* (opens the tree now: a wrong key would not get here) */
        node_t *n;
        e = t_get_node(fs, &v->ft.root, &n);
    }
    return e;
}

int siefs_rekey(siefs_t *fs, uint64_t g, const siefs_kdf_t *kdf, const uint8_t kek[32])
{
    volx_t *v;
    volkey_t k;
    int e;
    if ((e = head_of(fs, g, &v)) || (e = volkey(fs, v, &k))) return e;
    if (v->locked) return E(EACCES);
    fs->opmod = 0;
    if ((e = wrap(fs, v, &k, kdf, kek, ((aead_t *)v->key)->key))) return e;
    memcpy(v->v.key_id, kdf->salt, 16);
    v->dirty = 1;
    fs->opmod = 1;
    if ((e = t_update(fs, &fs->rt, K(v->id, T_VOLKEY, 0), &k, sizeof k))) { fs->broken = 1; return e; }
    fs->changed = 1;
    return commit(fs, 1);
}

int siefs_lock(siefs_t *fs, uint64_t g)
{
    volx_t *v;
    int e;
    if ((e = head_of(fs, g, &v))) return e;
    if (v->locked) return 0;
    if ((e = commit(fs, 0))) return e;                  /* (its changes, sealed, on the disk) */
    for (uint32_t i = 0; i < fs->nvols; i++) {
        volx_t *w = fs->vols[i];
        if (w != v && !((w->v.flags & V_SNAP) && w->v.parent == v->id)) continue;
        if (w->key) { memset(w->key, 0, sizeof(aead_t)); fs->env.free(w->key); w->key = 0; }
        w->locked = 1;
    }
    uint32_t keep = fs->env.cache_nodes;                /* what was read from it, opened, goes too */
    fs->env.cache_nodes = 0;
    cache_trim(fs);
    fs->env.cache_nodes = keep;
    if (fs->cv && fs->cv->locked) fs->cv = vol_get(fs, 1);
    return 0;
}
