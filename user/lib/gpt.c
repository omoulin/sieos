/*
 * gpt.c - A GUID partition table (mk/gpt.h): read and checked, written
 * back safely, its last partition grown to the disk's end.
 *
 * Writing order: the backup (its entries, then its header at the disk's
 * last sector), flush, then the primary (entries, header), flush. A power
 * cut leaves one valid table at least: the primary until the backup is
 * whole, the backup until the primary is; gpt_read takes either, and
 * rewrites the primary from the backup.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk/gpt.h"
#if __STDC_HOSTED__
#include <string.h>                                     /* (tests/gpt: on the host) */
#else
#include "mk/lib.h"
#endif

uint32_t gpt_crc(const void *p, unsigned n)
{
    const uint8_t *b = p;
    uint32_t c = ~0u;
    while (n--) { c ^= *b++; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320 & -(c & 1)); }
    return ~c;
}

static unsigned esize(const gpt_header_t *h) { return h->nentries * h->entsize; }
static uint32_t esect(const gpt_t *g) { return (esize(&g->h) + g->sector - 1) / g->sector; }

/* The header at lba, and its entries, checked. */
static int load(gpt_t *g, uint64_t lba)
{
    gpt_header_t *h = &g->h;
    if (g->read(g->ctx, lba, 1, g->sec)) return -1;
    memcpy(h, g->sec, sizeof *h);
    uint32_t crc = h->hcrc;
    h->hcrc = 0;
    if (memcmp(h->sig, "EFI PART", 8) || h->hsize != sizeof *h || gpt_crc(h, sizeof *h) != crc ||
        h->self != lba || h->entsize != sizeof(gpt_entry_t) || !h->nentries || esize(h) > GPT_ENTS_MAX ||
        esize(h) % g->sector || h->first_usable > h->last_usable || h->last_usable >= g->sectors) return -1;
    h->hcrc = crc;
    if (g->read(g->ctx, h->entries_lba, esect(g), g->ents) || gpt_crc(g->ents, esize(h)) != h->entcrc) return -1;
    return 0;
}

int gpt_read(gpt_t *g)
{
    if (g->sector > sizeof g->sec || g->sectors < 64) return -1;
    if (!load(g, 1)) return 0;
    if (load(g, g->sectors - 1)) return -1;
    return gpt_write(g);                                /* (the backup is whole: the primary again) */
}

static int put(gpt_t *g, uint64_t self, uint64_t alt, uint64_t ents)
{
    gpt_header_t *h = &g->h;
    h->self = self; h->alt = alt; h->entries_lba = ents;
    h->hcrc = 0;
    h->hcrc = gpt_crc(h, sizeof *h);
    memset(g->sec, 0, g->sector);
    memcpy(g->sec, h, sizeof *h);
    return g->write(g->ctx, ents, esect(g), g->ents) || g->write(g->ctx, self, 1, g->sec) || g->flush(g->ctx);
}

int gpt_write(gpt_t *g)
{
    uint64_t last = g->sectors - 1;
    g->h.last_usable = last - 1 - esect(g);
    g->h.entcrc = gpt_crc(g->ents, esize(&g->h));
    if (put(g, last, 1, last - esect(g)) || put(g, 1, last, 2)) return -1;
    if (g->read(g->ctx, 0, 1, g->sec)) return -1;      /* the protective MBR: over the whole disk */
    if (g->sec[450] == 0xEE) {
        uint32_t n = last > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)last;
        memcpy(g->sec + 458, &n, 4);
        if (g->write(g->ctx, 0, 1, g->sec) || g->flush(g->ctx)) return -1;
    }
    return 0;
}

int gpt_grow(gpt_t *g, uint32_t i)
{
    gpt_entry_t e, o;
    uint64_t mib = (1 << 20) / g->sector, end = (g->sectors - 1 - esect(g)) / mib * mib;   /* (exclusive) */
    if (i >= g->h.nentries || g->h.alt >= g->sectors - 1) return 0;
    memcpy(&e, g->ents + i * sizeof e, sizeof e);
    for (uint32_t j = 0; j < g->h.nentries; j++) {      /* the last partition? */
        memcpy(&o, g->ents + j * sizeof o, sizeof o);
        if (j != i && (o.first || o.last) && o.last >= e.first) return 0;
    }
    if (e.last + 1 >= end || e.last > g->h.last_usable) return 0;
    e.last = end - 1;
    memcpy(g->ents + i * sizeof e, &e, sizeof e);
    return gpt_write(g) ? -1 : 1;
}
