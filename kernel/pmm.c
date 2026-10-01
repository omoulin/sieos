/*
 * pmm.c - Physical frame allocator (bitmap based).
 *
 * Every usable range of the firmware's memory map is managed, up to
 * DIRECT_MAP_MAX: RAM above 4 GiB is added to the direct map (2 MiB pages)
 * here, before the allocator starts.  The bitmap and the reference counts
 * are sized for the highest usable address and placed in RAM themselves.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "mm.h"
#include "smp.h"
#include "blkdev.h"

static uint64_t *bitmap;                   /* 1 = used */
static uint64_t nframes;                   /* frames up to highest usable address */
static uint64_t free_frames;
static uint64_t total_usable;
static uint64_t kernel_frames;
static uint64_t search_hint;
static uint16_t *refcnt;                   /* sharers of user frames (copy-on-write) */

extern char _kernel_phys_end[];

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

struct mb2_mmap_entry {
    uint64_t base;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
};

static inline bool frame_used(uint64_t f) { return bitmap[f / 64] & (1UL << (f % 64)); }
static inline void frame_set(uint64_t f)  { bitmap[f / 64] |= 1UL << (f % 64); }
static inline void frame_clr(uint64_t f)  { bitmap[f / 64] &= ~(1UL << (f % 64)); }

/*
 * Boot-time memory: the usable ranges minus what is in use (low memory, the
 * kernel, the multiboot information and module).  The allocator's own
 * tables and the direct map's page directories are carved from them; what
 * is left becomes the free frames.
 */
#define NPIECES 160
static struct range { uint64_t start, end; } pieces[NPIECES];
static int npieces;

static void piece_add(uint64_t s, uint64_t e)
{
    if (e > s && npieces < NPIECES)
        pieces[npieces++] = (struct range){ s, e };
}

/* Remove [s, e) from every piece. */
static void piece_cut(uint64_t s, uint64_t e)
{
    s = PAGE_ALIGN_DOWN(s);
    e = PAGE_ALIGN_UP(e);
    for (int i = 0; i < npieces; i++) {
        struct range *r = &pieces[i];
        if (e <= r->start || s >= r->end)
            continue;
        uint64_t tail = r->end;
        if (s > r->start)
            r->end = s;                          /* keep the part below */
        else
            r->end = r->start;
        if (e < tail) {
            if (r->end == r->start)
                r->start = e, r->end = tail;     /* only the part above is left */
            else
                piece_add(e, tail);
        }
    }
}

/* size bytes, page aligned, from the pieces (below 4 GiB if low: mapped from the start). */
static uint64_t early_alloc(uint64_t size, bool low)
{
    size = PAGE_ALIGN_UP(size);
    for (int i = 0; i < npieces; i++) {
        struct range *r = &pieces[i];
        if (r->end - r->start < size || (low && r->start + size > DIRECT_MAP_SIZE))
            continue;
        uint64_t pa = r->start;
        r->start += size;
        memset(P2V(pa), 0, size);
        return pa;
    }
    panic("pmm: no memory for %lu KiB of boot tables", size / 1024);
}

static uint64_t early_table(void)
{
    return early_alloc(PAGE_SIZE, true);
}

uint64_t boot_archive_pa, boot_archive_size;   /* the drivers' boot archive (modload.c) */

void pmm_init(uint64_t mb_info_phys)
{
    uint32_t total = *(uint32_t *)P2V(mb_info_phys);
    uint8_t *p = (uint8_t *)P2V(mb_info_phys) + 8;
    uint8_t *end = (uint8_t *)P2V(mb_info_phys) + total;
    bool have_mmap = false;
    uint64_t mod_start = 0, mod_end = 0, top = 0, arch_start = 0, arch_end = 0;

    while (p < end) {
        struct mb2_tag *tag = (struct mb2_tag *)p;
        if (tag->type == 0)
            break;
        if (tag->type == 6) {
            uint32_t esize = *(uint32_t *)(p + 8);
            for (uint8_t *e = p + 16; e < p + tag->size; e += esize) {
                struct mb2_mmap_entry *m = (struct mb2_mmap_entry *)e;
                if (m->type != 1)
                    continue;
                uint64_t s = PAGE_ALIGN_UP(m->base);
                uint64_t t = PAGE_ALIGN_DOWN(m->base + m->len);
                if (s >= DIRECT_MAP_MAX)
                    continue;
                if (t > DIRECT_MAP_MAX)
                    t = DIRECT_MAP_MAX;
                if (t <= s)
                    continue;
                piece_add(s, t);
                total_usable += (t - s) / PAGE_SIZE;
                if (t > top)
                    top = t;
                have_mmap = true;
            }
        }
        if (tag->type == 3) {                      /* a module: the boot archive, or the root fs image */
            uint64_t s = *(uint32_t *)(p + 8), e = *(uint32_t *)(p + 12);
            if (!strcmp((const char *)p + 16, "boot_archive"))
                arch_start = s, arch_end = e;
            else if (!mod_end)                     /* ("rootfs", or the first other) */
                mod_start = s, mod_end = e;
        }
        p += (tag->size + 7) & ~7;
    }
    if (!have_mmap)
        panic("no multiboot2 memory map");

    /* Reserve: low memory (BIOS, VGA), the kernel image, the multiboot info, the module. */
    piece_cut(0, 0x100000);
    piece_cut(0x100000, (uint64_t)_kernel_phys_end);
    kernel_frames = (PAGE_ALIGN_UP((uint64_t)_kernel_phys_end) - 0x100000) / PAGE_SIZE;
    piece_cut(mb_info_phys, mb_info_phys + total);
    if (mod_end > mod_start)
        piece_cut(mod_start, mod_end);
    if (arch_end > arch_start) {
        piece_cut(arch_start, arch_end);
        boot_archive_pa = arch_start;
        boot_archive_size = arch_end - arch_start;
    }

    /* RAM above 4 GiB joins the direct map before anything lives there. */
    for (int i = 0; i < npieces; i++)
        if (pieces[i].end > DIRECT_MAP_SIZE)
            vmm_direct_map(MAX(pieces[i].start, DIRECT_MAP_SIZE), pieces[i].end, early_table);

    nframes = top / PAGE_SIZE;
    uint64_t words = (nframes + 63) / 64;
    bitmap = P2V(early_alloc(words * 8, false));
    refcnt = P2V(early_alloc(nframes * sizeof(uint16_t), false));
    memset(bitmap, 0xFF, words * 8);
    free_frames = 0;
    for (int i = 0; i < npieces; i++)
        for (uint64_t f = pieces[i].start / PAGE_SIZE; f < pieces[i].end / PAGE_SIZE; f++) {
            frame_clr(f);
            free_frames++;
        }
    if (mod_end > mod_start)
        blk_set_ramdisk(mod_start, mod_end - mod_start);
    /* Single frames come from above 4 GiB first: the low memory stays for 32-bit DMA. */
    search_hint = nframes > DIRECT_MAP_SIZE / PAGE_SIZE ? DIRECT_MAP_SIZE / PAGE_SIZE : 0;
}

/*
 * The frame allocator has its own lock (the innermost one: page faults on
 * anonymous memory allocate without the big kernel lock).  Callers run with
 * interrupts disabled.
 */
static struct spinlock pmm_lock;

uint64_t pmm_alloc(void)
{
    uint64_t pa = 0;
    spin_lock(&pmm_lock);
    for (uint64_t i = 0; i < nframes; i++) {
        uint64_t f = (search_hint + i) % nframes;
        if (bitmap[f / 64] == ~0UL) {
            i += 63 - (f % 64);
            continue;
        }
        if (!frame_used(f)) {
            frame_set(f);
            free_frames--;
            search_hint = f + 1;
            refcnt[f] = 1;
            pa = f * PAGE_SIZE;
            break;
        }
    }
    spin_unlock(&pmm_lock);
    if (pa)
        memset(P2V(pa), 0, PAGE_SIZE);           /* ours now: cleared outside the lock */
    return pa;
}

uint64_t pmm_alloc_contig(size_t n)
{
    uint64_t run = 0, start = 0;
    spin_lock(&pmm_lock);
    for (uint64_t f = 256; f < nframes; f++) {      /* lowest first: early callers get 32-bit DMA memory */
        if (bitmap[f / 64] == ~0UL) {
            run = 0;
            f |= 63;
            continue;
        }
        if (frame_used(f)) {
            run = 0;
            continue;
        }
        if (++run == n) {
            start = f + 1 - n;
            for (uint64_t k = start; k <= f; k++)
                frame_set(k);
            free_frames -= n;
            break;
        }
    }
    spin_unlock(&pmm_lock);
    if (!start)
        return 0;
    memset(P2V(start * PAGE_SIZE), 0, n * PAGE_SIZE);
    return start * PAGE_SIZE;
}

static void free_locked(uint64_t pa)
{
    uint64_t f = pa / PAGE_SIZE;
    if (f >= nframes || !frame_used(f))
        panic("pmm_free: bad frame %lx", pa);
    frame_clr(f);
    refcnt[f] = 0;
    free_frames++;
    if (f < search_hint && (f >= DIRECT_MAP_SIZE / PAGE_SIZE || nframes <= DIRECT_MAP_SIZE / PAGE_SIZE))
        search_hint = f;
}

void pmm_free(uint64_t pa)
{
    spin_lock(&pmm_lock);
    free_locked(pa);
    spin_unlock(&pmm_lock);
}

/* Reference counting for frames mapped by several address spaces. */
void pmm_ref(uint64_t pa)
{
    uint64_t f = pa / PAGE_SIZE;
    spin_lock(&pmm_lock);
    if (f < nframes && frame_used(f))
        refcnt[f]++;
    spin_unlock(&pmm_lock);
}

void pmm_unref(uint64_t pa)
{
    uint64_t f = pa / PAGE_SIZE;
    spin_lock(&pmm_lock);
    if (f >= nframes || !frame_used(f))
        panic("pmm_unref: bad frame %lx", pa);
    if (refcnt[f] > 1)
        refcnt[f]--;
    else
        free_locked(pa);
    spin_unlock(&pmm_lock);
}

int pmm_refcount(uint64_t pa)
{
    uint64_t f = pa / PAGE_SIZE;
    return f < nframes ? refcnt[f] : 0;
}

void pmm_free_contig(uint64_t pa, size_t n)
{
    spin_lock(&pmm_lock);
    for (size_t i = 0; i < n; i++)
        free_locked(pa + i * PAGE_SIZE);
    spin_unlock(&pmm_lock);
}

uint64_t pmm_free_pages(void)   { return free_frames; }
uint64_t pmm_total_pages(void)  { return total_usable; }
uint64_t pmm_kernel_pages(void) { return kernel_frames; }
