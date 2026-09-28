/*
 * pmm.c - Physical frame allocator (bitmap based).
 *
 * Only memory covered by the direct map (the first 4 GiB) is managed.
 */
#include "mm.h"
#include "blkdev.h"

#define MAX_FRAMES (DIRECT_MAP_SIZE / PAGE_SIZE)

static uint64_t bitmap[MAX_FRAMES / 64];   /* 1 = used */
static uint64_t nframes;                   /* frames up to highest usable address */
static uint64_t free_frames;
static uint64_t total_usable;
static uint64_t kernel_frames;
static uint64_t search_hint;
static uint16_t refcnt[MAX_FRAMES];        /* sharers of user frames (copy-on-write) */

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

static void mark_range(uint64_t start, uint64_t end, bool used)
{
    for (uint64_t f = start / PAGE_SIZE; f < PAGE_ALIGN_UP(end) / PAGE_SIZE && f < MAX_FRAMES; f++) {
        if (used && !frame_used(f)) {
            frame_set(f);
            free_frames--;
        } else if (!used && frame_used(f)) {
            frame_clr(f);
            free_frames++;
        }
    }
}

void pmm_init(uint64_t mb_info_phys)
{
    memset(bitmap, 0xFF, sizeof(bitmap));
    free_frames = 0;

    uint32_t total = *(uint32_t *)P2V(mb_info_phys);
    uint8_t *p = (uint8_t *)P2V(mb_info_phys) + 8;
    uint8_t *end = (uint8_t *)P2V(mb_info_phys) + total;
    bool have_mmap = false;
    uint64_t mod_start = 0, mod_end = 0;

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
                if (s >= DIRECT_MAP_SIZE)
                    continue;
                if (t > DIRECT_MAP_SIZE)
                    t = DIRECT_MAP_SIZE;
                if (t <= s)
                    continue;
                mark_range(s, t, false);
                total_usable += (t - s) / PAGE_SIZE;
                if (t / PAGE_SIZE > nframes)
                    nframes = t / PAGE_SIZE;
                have_mmap = true;
            }
        }
        if (tag->type == 3 && !mod_end) {          /* first module = root fs image */
            mod_start = *(uint32_t *)(p + 8);
            mod_end = *(uint32_t *)(p + 12);
        }
        p += (tag->size + 7) & ~7;
    }
    if (!have_mmap)
        panic("no multiboot2 memory map");

    /* Reserve: low memory (BIOS, VGA), the kernel image, the multiboot info. */
    uint64_t before = free_frames;
    mark_range(0, 0x100000, true);
    mark_range(0x100000, (uint64_t)_kernel_phys_end, true);
    kernel_frames = before - free_frames;
    mark_range(mb_info_phys, mb_info_phys + total, true);
    if (mod_end > mod_start) {
        mark_range(mod_start, mod_end, true);
        blk_set_ramdisk(mod_start, mod_end - mod_start);
    }
    search_hint = 0;
}

uint64_t pmm_alloc(void)
{
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
            memset(P2V(f * PAGE_SIZE), 0, PAGE_SIZE);
            return f * PAGE_SIZE;
        }
    }
    return 0;
}

uint64_t pmm_alloc_contig(size_t n)
{
    if (n == 1)
        return pmm_alloc();
    uint64_t run = 0;
    for (uint64_t f = 256; f < nframes; f++) {
        if (frame_used(f)) {
            run = 0;
            continue;
        }
        if (++run == n) {
            uint64_t start = f + 1 - n;
            for (uint64_t k = start; k <= f; k++)
                frame_set(k);
            free_frames -= n;
            memset(P2V(start * PAGE_SIZE), 0, n * PAGE_SIZE);
            return start * PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free(uint64_t pa)
{
    uint64_t f = pa / PAGE_SIZE;
    if (f >= nframes || !frame_used(f))
        panic("pmm_free: bad frame %lx", pa);
    frame_clr(f);
    refcnt[f] = 0;
    free_frames++;
    if (f < search_hint)
        search_hint = f;
}

/* Reference counting for frames mapped by several address spaces. */
void pmm_ref(uint64_t pa)
{
    uint64_t f = pa / PAGE_SIZE;
    if (f < nframes && frame_used(f))
        refcnt[f]++;
}

void pmm_unref(uint64_t pa)
{
    uint64_t f = pa / PAGE_SIZE;
    if (f >= nframes || !frame_used(f))
        panic("pmm_unref: bad frame %lx", pa);
    if (refcnt[f] > 1)
        refcnt[f]--;
    else
        pmm_free(pa);
}

int pmm_refcount(uint64_t pa)
{
    uint64_t f = pa / PAGE_SIZE;
    return f < nframes ? refcnt[f] : 0;
}

void pmm_free_contig(uint64_t pa, size_t n)
{
    for (size_t i = 0; i < n; i++)
        pmm_free(pa + i * PAGE_SIZE);
}

uint64_t pmm_free_pages(void)   { return free_frames; }
uint64_t pmm_total_pages(void)  { return total_usable; }
uint64_t pmm_kernel_pages(void) { return kernel_frames; }
