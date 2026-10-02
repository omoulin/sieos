/*
 * kmalloc.c - Kernel heap.
 *
 * Small requests are served from per-size-class free lists carved out of
 * 4K pages; large requests get physically contiguous page runs.  Every
 * allocation is preceded by a 16-byte header describing it.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "mm.h"
#include "smp.h"

#define HDR_MAGIC 0xA1E05A11
#define NCLASSES  8                 /* 32 .. 4096 bytes including header */

struct header {
    uint32_t magic;
    uint32_t cls;                   /* size class, or NCLASSES for large */
    uint64_t npages;                /* large allocations only */
};

struct freeblk {
    struct freeblk *next;
};

static struct freeblk *freelist[NCLASSES];
static size_t heap_used;
static struct spinlock kmem_lock;           /* the free lists and the count (then the frame allocator's lock) */

static size_t class_size(int c) { return 32UL << c; }

void *kmalloc(size_t size)
{
    size_t need = size + sizeof(struct header);
    for (int c = 0; c < NCLASSES - 1; c++) {
        if (need > class_size(c))
            continue;
        spin_lock(&kmem_lock);
        if (!freelist[c]) {
            spin_unlock(&kmem_lock);
            uint64_t pa = pmm_alloc();               /* (a page cut in blocks: outside the lock) */
            if (!pa)
                return NULL;
            char *page = P2V(pa);
            struct freeblk *first = NULL, *last = NULL;
            for (size_t off = 0; off + class_size(c) <= PAGE_SIZE; off += class_size(c)) {
                struct freeblk *b = (struct freeblk *)(page + off);
                b->next = first;
                first = b;
                if (!last)
                    last = b;
            }
            spin_lock(&kmem_lock);
            last->next = freelist[c];
            freelist[c] = first;
        }
        struct freeblk *b = freelist[c];
        freelist[c] = b->next;
        __atomic_add_fetch(&heap_used, class_size(c), __ATOMIC_RELAXED);
        spin_unlock(&kmem_lock);
        struct header *h = (struct header *)b;
        h->magic = HDR_MAGIC;
        h->cls = c;
        h->npages = 0;
        return h + 1;
    }
    size_t npages = PAGE_ALIGN_UP(need) / PAGE_SIZE;
    uint64_t pa = pmm_alloc_contig(npages);
    if (!pa)
        return NULL;
    struct header *h = P2V(pa);
    h->magic = HDR_MAGIC;
    h->cls = NCLASSES;
    h->npages = npages;
    __atomic_add_fetch(&heap_used, npages * PAGE_SIZE, __ATOMIC_RELAXED);
    return h + 1;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *p)
{
    if (!p)
        return;
    struct header *h = (struct header *)p - 1;
    if (h->magic != HDR_MAGIC)
        panic("kfree: bad pointer %p", p);
    h->magic = 0;
    if (h->cls == NCLASSES) {
        __atomic_sub_fetch(&heap_used, h->npages * PAGE_SIZE, __ATOMIC_RELAXED);
        pmm_free_contig(V2P(h), h->npages);
        return;
    }
    int c = h->cls;
    struct freeblk *b = (struct freeblk *)h;
    spin_lock(&kmem_lock);
    __atomic_sub_fetch(&heap_used, class_size(c), __ATOMIC_RELAXED);
    b->next = freelist[c];
    freelist[c] = b;
    spin_unlock(&kmem_lock);
}

size_t kheap_used(void)
{
    return heap_used;
}
