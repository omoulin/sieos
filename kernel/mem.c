/*
 * mem.c - Memory: which physical pages are free, small kernel objects,
 * and the policy of address spaces (what is freed, quarantined, copied).
 * The page tables' format is the architecture's (arch/NAME/mmu.c).
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"

uint64_t pages_total, pages_free;

/* ---- Physical pages: a bitmap, one bit per 4 KiB page, 1 = in use.
 * 32 KiB of bitmap per GiB of RAM. */
static uint8_t *bitmap;
static uint64_t npages, hint;       /* hint: where the last allocation ended */

/* The boot information (the architecture's, kept for the whole run): the
 * RAM ranges (SYS_MAP_PHYS refuses them), what the boot loader left for us
 * (its information, the modules' names), kept until kernel_main has read it
 * (mem_boot_done), and what is never ours. */
static boot_info_t *bi;

static int used(uint64_t pg) { return bitmap[pg / 8] >> (pg % 8) & 1; }

static void mark(uint64_t first, uint64_t end, int use)   /* pages [first, end) */
{
    for (uint64_t pg = first; pg < end && pg < npages; pg++)
        if (used(pg) != use) {
            bitmap[pg / 8] ^= 1 << (pg % 8);
            pages_free += use ? -1 : 1;
        }
}

#define UP(x) (((x) + PAGE - 1) / PAGE)     /* bytes -> pages, rounding up */
#define MAX(a, b) ((a) > (b) ? (a) : (b))

void mem_init(boot_info_t *info)
{
    /* 1. The RAM ranges, and the highest RAM address: the bitmap covers all of it. */
    uint64_t top = 0;
    bi = info;
    for (int i = 0; i < bi->nram; i++) top = MAX(top, bi->ram[i].end);
    npages = top / PAGE;

    /* 2. Memory in use before we start: the kernel, the modules (kept: init
     * restarts servers from them), and what the boot loader left for us. */
    uint64_t end = bi->kernel_end;
    for (int i = 0; i < bi->nmod; i++) end = MAX(end, bi->mod[i].pa + bi->mod[i].len);
    for (int i = 0; i < bi->nkeep; i++) end = MAX(end, bi->keep[i].end);

    /* 3. The bitmap goes after all of it (it must be in the part of RAM the
     * boot code already mapped): all used, then free the RAM, then take back
     * what is in use (including the bitmap itself). */
    uint64_t bm = UP(end) * PAGE;
    bitmap = P2V(bm);
    if (bm + (npages + 7) / 8 > bi->dmap_boot) panic("too much memory for the page bitmap\n");
    memset(bitmap, 0xFF, (npages + 7) / 8);
    for (int i = 0; i < bi->nram; i++) mark(UP(bi->ram[i].start), bi->ram[i].end / PAGE, 0);
    pages_total = pages_free;
    for (int i = 0; i < bi->nrsvd; i++) mark(bi->rsvd[i].start / PAGE, UP(bi->rsvd[i].end), 1);
    mark(bi->kernel_start / PAGE, UP(bi->kernel_end), 1);                /* the kernel, */
    for (int i = 0; i < bi->nmod; i++)                                      /* the modules, */
        mark(bi->mod[i].pa / PAGE, UP(bi->mod[i].pa + bi->mod[i].len), 1);
    mark(bm / PAGE, UP(bm + (npages + 7) / 8), 1);                          /* the bitmap (each where it is:
                                                                               a boot loader may leave gaps) */
    for (int i = 0; i < bi->nkeep; i++) mark(bi->keep[i].start / PAGE, UP(bi->keep[i].end), 1);

    /* 4. The architecture maps the rest of RAM and switches to the kernel's
     * own page table. */
    arch_mem_init(top);
}

static int reserved(uint64_t pg)
{
    for (int i = 0; i < bi->nrsvd; i++) if (pg >= bi->rsvd[i].start / PAGE && pg < UP(bi->rsvd[i].end)) return 1;
    return 0;
}

/* A page the kernel, a module or the bitmap still use (a boot loader's
 * information may share pages with them). */
static int still_used(uint64_t pg)
{
    uint64_t bm = V2P(bitmap) / PAGE;
    if (pg >= bi->kernel_start / PAGE && pg < UP(bi->kernel_end)) return 1;
    if (pg >= bm && pg < bm + UP((npages + 7) / 8)) return 1;
    for (int i = 0; i < bi->nmod; i++)
        if (pg >= bi->mod[i].pa / PAGE && pg < UP(bi->mod[i].pa + bi->mod[i].len)) return 1;
    return 0;
}

/* The boot loader's information has been read: free its pages (except
 * page 0, the reserved ones, and those still used). */
void mem_boot_done(void)
{
    for (int i = 0; i < bi->nkeep; i++)
        for (uint64_t pg = bi->keep[i].start / PAGE; pg < UP(bi->keep[i].end); pg++)
            if (pg && !still_used(pg) && !reserved(pg)) mark(pg, pg + 1, 0);
    bi->nkeep = 0;
}

/* n physically contiguous pages, zeroed, or 0 if none. Zeroing matters:
 * memory one user freed must never show up in another user's program. */
uint64_t page_alloc(uint64_t n)
{
    uint64_t pg = hint, run = 0;
    for (uint64_t i = 0; i < npages + n; i++, pg++) {
        if (pg >= npages) pg = run = 0;          /* wrap around */
        run = used(pg) ? 0 : run + 1;
        if (run == n) {
            uint64_t first = pg + 1 - n;
            mark(first, pg + 1, 1);
            hint = pg + 1;
            memset(P2V(first * PAGE), 0, n * PAGE);
            return first * PAGE;
        }
    }
    return 0;    /* page 0 is never free, so 0 can mean "none" */
}

/* The same, below physical address `limit` (DMA_LOW): a plain scan from
 * the start, rare (a few buffers at a driver's start). */
uint64_t page_alloc_low(uint64_t n, uint64_t limit)
{
    for (uint64_t pg = 1, run = 0; pg < npages && pg < limit / PAGE; pg++) {
        run = used(pg) ? 0 : run + 1;
        if (run == n) {
            mark(pg + 1 - n, pg + 1, 1);
            memset(P2V((pg + 1 - n) * PAGE), 0, n * PAGE);
            return (pg + 1 - n) * PAGE;
        }
    }
    return 0;
}

void page_free(uint64_t pa, uint64_t n) { mark(pa / PAGE, pa / PAGE + n, 0); }

int is_ram(uint64_t pa, uint64_t size)
{
    for (int i = 0; i < bi->nram; i++)
        if (pa < bi->ram[i].end && pa + size > bi->ram[i].start) return 1;
    return 0;
}

/* ---- Small kernel objects (processes, ports): blocks of 32, 64, ... 2048
 * bytes. Each page holds blocks of one size behind a small header that
 * counts the blocks in use; pages with free blocks are on their size's
 * list. A page whose last block is freed goes back to page_alloc. */
typedef struct slab { struct slab *next, *prev; void *free; int used, cls; } slab_t;
#define SLAB_HDR 64                          /* header size, keeps blocks aligned */
static slab_t *partial[7];

static int size_class(size_t n) { int c = 0; while ((32UL << c) < n) c++; return c; }
static void slab_unlink(slab_t *s)
{
    if (s->prev) s->prev->next = s->next; else partial[s->cls] = s->next;
    if (s->next) s->next->prev = s->prev;
    s->next = s->prev = 0;
}
static void slab_link(slab_t *s)
{
    s->prev = 0;
    s->next = partial[s->cls];
    if (s->next) s->next->prev = s;
    partial[s->cls] = s;
}

void *kzalloc(size_t n)
{
    int c = size_class(n);
    slab_t *s = partial[c];
    if (!s) {
        uint64_t pa = page_alloc(1);
        if (!pa) return 0;
        s = P2V(pa);
        s->cls = c;
        for (char *b = (char *)s + PAGE - (32 << c); b >= (char *)s + SLAB_HDR; b -= 32 << c) {
            *(void **)b = s->free;
            s->free = b;
        }
        slab_link(s);
    }
    void *p = s->free;
    s->free = *(void **)p;
    if (!s->free) slab_unlink(s);            /* full */
    s->used++;
    return memset(p, 0, 32 << c);
}

void kfree(void *p)
{
    slab_t *s = (slab_t *)((uint64_t)p & ~(PAGE - 1));
    if (!s->free) slab_link(s);              /* was full: it has room again */
    *(void **)p = s->free;
    s->free = p;
    if (--s->used == 0) { slab_unlink(s); page_free(V2P(s), 1); }
}

/* Remove a mapping, freeing the page unless it is a device's. This CPU
 * forgets the old translation at once; other CPUs running threads of the
 * same process may still hold it in their TLB: the caller must call
 * tlb_shootdown once done, before releasing the kernel lock (so no one can
 * get the freed pages before every CPU has forgotten them). */
void vm_unmap(uint64_t as, uint64_t va)
{
    uint64_t flags, pa = arch_vm_unmap(as, va, &flags);
    if (pa && !(flags & VM_DEV)) page_free(pa, 1);
}

/* ---- DMA quarantine. When a driver dies, its device may still be in the
 * middle of a transfer into the driver's DMA buffers. Those pages must not
 * be given to anyone else at once: they wait here, and are freed once
 * QUARANTINE_NS has passed (a device finishes a request in microseconds,
 * and a restarted driver resets its device first). Checked when programs
 * start and when drivers allocate DMA memory: no timer needed. */
#define QUARANTINE_NS 100000000UL    /* 100 ms */
typedef struct qpage { uint64_t pa, since; struct qpage *next; } qpage_t;
static qpage_t *quarantine;

static void quarantine_add(uint64_t pa)
{
    qpage_t *q = kzalloc(sizeof *q);
    if (!q) return;                      /* no memory for the note: lose the page rather than risk it */
    q->pa = pa;
    q->since = now_ns();
    q->next = quarantine;
    quarantine = q;
}

void quarantine_release(void)
{
    uint64_t t = now_ns();
    for (qpage_t **p = &quarantine; *p; ) {
        qpage_t *q = *p;
        if (t - q->since < QUARANTINE_NS) { p = &q->next; continue; }
        *p = q->next;
        page_free(q->pa, 1);
        kfree(q);
    }
}

/* Free an address space: its tables, and each page by its kind: a DMA
 * buffer waits in quarantine, a device's registers are not ours to free. */
static void free_leaf(uint64_t pa, uint64_t flags)
{
    if (flags & VM_DMA) quarantine_add(pa);
    else if (!(flags & VM_DEV)) page_free(pa, 1);
}
void vm_free(uint64_t as) { arch_vm_free(as, free_leaf); }

/* Copy n bytes from src in address space sas to dst in das (0 = a kernel
 * address), page by page, checking every user page: a bad pointer from a
 * program gives -EFAULT, never a crash of the kernel. This is how IPC moves
 * data directly from one process to another. */
static int copy(uint64_t das, void *dst, uint64_t sas, const void *src, uint64_t n, int dmode)
{
    uint64_t d = (uint64_t)dst, s = (uint64_t)src;
    while (n) {
        uint64_t c = n;
        char *dp = (char *)d, *sp = (char *)s;
        if (das) { if (c > PAGE - d % PAGE) c = PAGE - d % PAGE; if (!(dp = arch_uaddr(das, d, dmode))) return -EFAULT; }
        if (sas) { if (c > PAGE - s % PAGE) c = PAGE - s % PAGE; if (!(sp = arch_uaddr(sas, s, 0))) return -EFAULT; }
        memcpy(dp, sp, c);
        if (dmode == 2) arch_sync_code(dp, c);   /* loading a program: its code must be runnable */
        d += c; s += c; n -= c;
    }
    return 0;
}
int vm_copy(uint64_t das, void *dst, uint64_t sas, const void *src, uint64_t n) { return copy(das, dst, sas, src, n, 1); }
int vm_load(uint64_t das, void *dst, uint64_t sas, const void *src, uint64_t n) { return copy(das, dst, sas, src, n, 2); }
