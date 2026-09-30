/*
 * ipfrag.c - Reassembly of fragmented IPv4 and IPv6 datagrams.
 *
 * A datagram is identified by its addresses, identification and (IPv4)
 * protocol.  Fragments are copied into a 64 KB buffer at their offset and
 * counted in 8-byte units; the datagram is complete once the last fragment
 * (no "more fragments") and every unit before it have arrived.  An IPv6
 * datagram whose fragments overlap is dropped (RFC 5722); IPv4 fragments
 * may overlap, the later data wins.  Incomplete datagrams are dropped after
 * 30 s (IPv4) or 60 s (IPv6), and at most NFRAG are reassembled at once.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "net.h"
#include "mm.h"

#define NFRAG    16
#define FRAG_MAX 65536                   /* the largest datagram payload */
#define UNITS    (FRAG_MAX / 8)

static struct frag {
    bool used;
    struct frag_key key;
    uint8_t *buf;
    uint8_t have[UNITS / 8];             /* 8-byte units received */
    uint32_t total;                      /* payload length, once the last fragment came; 0 before */
    uint64_t deadline;
} frags[NFRAG];

uint64_t frag_reassembled, frag_dropped;

static bool same_key(const struct frag_key *a, const struct frag_key *b)
{
    return a->v6 == b->v6 && a->id == b->id && a->proto == b->proto && na_eq(&a->src, &b->src) &&
           na_eq(&a->dst, &b->dst);
}

static void release(struct frag *f)
{
    kfree(f->buf);
    memset(f, 0, sizeof(*f));
}

static struct frag *find(const struct frag_key *k)
{
    struct frag *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < NFRAG; i++) {
        struct frag *f = &frags[i];
        if (f->used && same_key(&f->key, k))
            return f;
        if (!f->used && !free_slot)
            free_slot = f;
        if (f->used && (!oldest || f->deadline < oldest->deadline))
            oldest = f;
    }
    struct frag *f = free_slot;
    if (!f) {                                    /* all busy: the oldest makes way */
        release(oldest);
        frag_dropped++;
        f = oldest;
    }
    f->buf = kmalloc(FRAG_MAX);
    if (!f->buf)
        return NULL;
    f->used = true;
    f->key = *k;
    memset(f->have, 0, sizeof(f->have));
    f->total = 0;
    f->deadline = ticks + (k->v6 ? 60 : 30) * TIMER_HZ;
    return f;
}

static bool has_unit(struct frag *f, uint32_t u) { return f->have[u / 8] & (1 << (u % 8)); }

int frag_add(const struct frag_key *k, uint32_t off, const uint8_t *data, size_t len, bool more, uint8_t **out,
             size_t *outlen)
{
    if (off + len > FRAG_MAX || (more && (len % 8 || !len)))
        return -1;                               /* too big, or a middle fragment not a multiple of 8 */
    struct frag *f = find(k);
    if (!f)
        return -1;
    uint32_t u0 = off / 8, u1 = (off + len + 7) / 8;
    if (k->v6)
        for (uint32_t u = u0; u < u1; u++)
            if (has_unit(f, u)) {                /* overlap: the datagram is dropped */
                release(f);
                frag_dropped++;
                return -1;
            }
    if (!more) {
        if (f->total && f->total != off + len) {
            release(f);
            frag_dropped++;
            return -1;
        }
        f->total = off + len;
    } else if (f->total && off + len > f->total) {
        release(f);
        frag_dropped++;
        return -1;
    }
    memcpy(f->buf + off, data, len);
    for (uint32_t u = u0; u < u1; u++)
        f->have[u / 8] |= 1 << (u % 8);
    if (!f->total)
        return 0;
    for (uint32_t u = 0; u < (f->total + 7) / 8; u++)
        if (!has_unit(f, u))
            return 0;
    *out = f->buf;                               /* complete: the caller frees it */
    *outlen = f->total;
    f->buf = NULL;
    release(f);
    frag_reassembled++;
    return 1;
}

void frag_tick(void)
{
    for (int i = 0; i < NFRAG; i++)
        if (frags[i].used && ticks >= frags[i].deadline) {
            release(&frags[i]);
            frag_dropped++;
        }
}
