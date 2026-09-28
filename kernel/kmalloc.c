/*
 * kmalloc.c - Kernel heap.
 *
 * Small requests are served from per-size-class free lists carved out of
 * 4K pages; large requests get physically contiguous page runs.  Every
 * allocation is preceded by a 16-byte header describing it.
 */
#include "mm.h"

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

static size_t class_size(int c) { return 32UL << c; }

void *kmalloc(size_t size)
{
    size_t need = size + sizeof(struct header);
    for (int c = 0; c < NCLASSES - 1; c++) {
        if (need > class_size(c))
            continue;
        if (!freelist[c]) {
            uint64_t pa = pmm_alloc();
            if (!pa)
                return NULL;
            char *page = P2V(pa);
            for (size_t off = 0; off + class_size(c) <= PAGE_SIZE; off += class_size(c)) {
                struct freeblk *b = (struct freeblk *)(page + off);
                b->next = freelist[c];
                freelist[c] = b;
            }
        }
        struct freeblk *b = freelist[c];
        freelist[c] = b->next;
        struct header *h = (struct header *)b;
        h->magic = HDR_MAGIC;
        h->cls = c;
        h->npages = 0;
        heap_used += class_size(c);
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
    heap_used += npages * PAGE_SIZE;
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
        heap_used -= h->npages * PAGE_SIZE;
        pmm_free_contig(V2P(h), h->npages);
        return;
    }
    int c = h->cls;
    heap_used -= class_size(c);
    struct freeblk *b = (struct freeblk *)h;
    b->next = freelist[c];
    freelist[c] = b;
}

size_t kheap_used(void)
{
    return heap_used;
}
