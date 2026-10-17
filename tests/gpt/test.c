/*
 * test.c - user/lib/gpt.c on the host (make gpt-test): a key image's GPT
 * (an EFI partition, then SieFS up to the image's end) on a disk four
 * times bigger, grown; with a power cut after every sector write of the
 * grow, the table must read back (primary, or the backup), old or grown,
 * and grow fully next time; nothing outside the tables written; a
 * partition that is not the last one, or a table already at the end, left
 * alone.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mk/gpt.h"

#define SS 512
#define IMG (64u << 11)                     /* the image: 64 MiB, in sectors */
#define DISK (4 * IMG)
static uint8_t *disk, *orig;
static long writes, cut = -1, fails;
static uint64_t touched_lo = ~0ULL, touched_hi;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int rd(void *c, uint64_t lba, uint32_t n, void *b) { (void)c; if (lba + n > DISK) return -1; memcpy(b, disk + lba * SS, n * SS); return 0; }
static int wr(void *c, uint64_t lba, uint32_t n, const void *b)
{
    (void)c;
    if (lba + n > DISK) return -1;
    for (uint32_t i = 0; i < n; i++, writes++) {        /* (one sector at a time: the power can go between) */
        if (cut >= 0 && writes >= cut) return 0;
        memcpy(disk + (lba + i) * SS, (const uint8_t *)b + i * SS, SS);
        uint64_t s = lba + i;
        if (s > 33 && s < DISK - 33) { if (s < touched_lo) touched_lo = s; if (s > touched_hi) touched_hi = s; }
    }
    return 0;
}
static int fl(void *c) { (void)c; return 0; }
static gpt_t g;
static void open_disk(uint64_t sectors) { memset(&g, 0, sizeof g); g.read = rd; g.write = wr; g.flush = fl; g.sector = SS; g.sectors = sectors; }

/* What mkusb.py makes: the GPT for IMG sectors (made by gpt_write on an IMG-sized disk). */
static void make_image(int two_last)
{
    memset(disk, 0, (size_t)DISK * SS);
    disk[450] = 0xEE; disk[510] = 0x55; disk[511] = 0xAA;
    open_disk(IMG);
    memcpy(g.h.sig, "EFI PART", 8);
    g.h.rev = 0x10000; g.h.hsize = 92; g.h.first_usable = 34; g.h.nentries = 128; g.h.entsize = 128;
    gpt_entry_t *e = (gpt_entry_t *)g.ents;
    e[0].type[0] = 0x28; e[0].first = 2048; e[0].last = 4095;                       /* "EFI" */
    e[1].type[0] = 0x5e; e[1].guid[0] = 1; e[1].first = 4096; e[1].last = (IMG - 34) / 8 * 8 - 1;   /* SieFS */
    if (two_last) { e[1].last = 8191; e[2].type[0] = 0x0f; e[2].first = 8192; e[2].last = 10239; }
    CHECK(gpt_write(&g) == 0);
    memset(disk + 4096 * SS, 0xAB, 8192 * SS);         /* data in the partitions: never touched */
}

static int valid_at_end(void)                          /* both copies valid, the backup at the disk's end */
{
    gpt_header_t p = g.h;
    open_disk(DISK);
    if (gpt_read(&g) || g.h.alt != DISK - 1) return 0;
    gpt_header_t b;
    memcpy(&b, disk + (uint64_t)(DISK - 1) * SS, sizeof b);
    (void)p;
    return !memcmp(b.sig, "EFI PART", 8) && b.self == DISK - 1 && b.entcrc == g.h.entcrc &&
           !memcmp(disk + b.entries_lba * SS, g.ents, GPT_ENTS_MAX);
}

int main(void)
{
    disk = malloc((size_t)DISK * SS); orig = malloc((size_t)DISK * SS);
    gpt_entry_t *e = (gpt_entry_t *)g.ents;

    /* grown at once */
    make_image(0);
    open_disk(DISK);
    CHECK(gpt_read(&g) == 0 && g.h.alt == IMG - 1);
    gpt_entry_t efi = e[0];
    writes = 0; touched_hi = 0;
    CHECK(gpt_grow(&g, 1) == 1);
    long total = writes;
    CHECK(valid_at_end() && e[1].last == (uint64_t)(DISK - 34) / 2048 * 2048 - 1 && e[1].first == 4096);
    CHECK(!memcmp(&e[0], &efi, sizeof efi));
    CHECK(touched_hi == 0 && disk[4096 * SS] == 0xAB && disk[12287 * SS] == 0xAB);   /* only the tables */
    CHECK(disk[458] == ((DISK - 1) & 0xFF) && disk[459] == ((DISK - 1) >> 8 & 0xFF));     /* protective MBR */
    open_disk(DISK);
    CHECK(gpt_read(&g) == 0 && gpt_grow(&g, 1) == 0);                                     /* once */

    /* a power cut after every sector written */
    long runs = 0, grown = 0;
    make_image(0);
    memcpy(orig, disk, (size_t)DISK * SS);
    for (cut = 0; cut <= total; cut++) {
        memcpy(disk, orig, (size_t)DISK * SS);
        open_disk(DISK);
        writes = 0;
        CHECK(gpt_read(&g) == 0);
        gpt_grow(&g, 1);
        long c = cut; cut = -1;                         /* the power is back */
        open_disk(DISK);
        if (gpt_read(&g)) { CHECK(!"table lost"); cut = c; continue; }
        CHECK(e[1].last == (uint64_t)(IMG - 34) / 8 * 8 - 1 || e[1].last == (uint64_t)(DISK - 34) / 2048 * 2048 - 1);
        grown += e[1].last > IMG;
        gpt_grow(&g, 1);
        CHECK(valid_at_end() && e[1].last == (uint64_t)(DISK - 34) / 2048 * 2048 - 1 && !memcmp(&e[0], &efi, sizeof efi));
        cut = c;
        runs++;
    }
    cut = -1;

    /* left alone: not the last partition; a table already at the disk's end */
    make_image(1);
    open_disk(DISK);
    CHECK(gpt_read(&g) == 0 && gpt_grow(&g, 1) == 0 && gpt_grow(&g, 2) == 1);
    open_disk(DISK);
    CHECK(gpt_read(&g) == 0 && e[1].last == 8191 && gpt_grow(&g, 1) == 0 && gpt_grow(&g, 2) == 0);
    make_image(0);
    memset(disk + SS, 0, SS);                           /* a damaged primary, the backup not at the end: odd */
    open_disk(DISK);
    CHECK(gpt_read(&g) == -1);

    printf("gpt test: grown from %u to %u MiB in %ld sector writes; %ld power cuts (%ld came back grown), "
           "all readable and grown after%s\n", IMG >> 11, DISK >> 11, total, runs, grown, fails ? "" : "; passed");
    return fails != 0;
}
