/*
 * dev.c - /dev/nvgpuN (sieos/nvgpu.h): what a process asks of the GPU.
 *
 * Per open file: its memory objects, its reserved VA ranges, its timeline
 * semaphores, its channels.  Done now: the device's information, the GPU's
 * clock, memory in system memory (pages the GPU will reach; mapped into the
 * process by mmap at the object's offset), VA reservations (the VA space's
 * bookkeeping), timeline semaphores (values, signaled and waited on by the
 * processor).  Memory in VRAM, binding into the GPU's page tables, channels
 * and execution come with stage 4 (GSP-RM's VA space and channels): they
 * answer ENOSYS until then.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "pci.h"
#include "mm.h"
#include "fs.h"
#include "proc.h"
#include "poll.h"
#include "abi2.h"
#include "sieos/nvgpu.h"
#include "nvgpu.h"

#define NMEM   4096
#define NSYNC  1024
#define NVA    1024
#define MAP_SHIFT 40                             /* mmap offsets: (handle << 40) | offset in the object */

struct mem {
    bool used;
    uint64_t size;
    uint32_t flags;
    uint64_t *pages;                             /* system memory: its pages */
    uint64_t npages;
};
struct va { uint64_t addr, size; };
struct sync { bool used; uint64_t value; };

struct client {
    struct mem mem[NMEM];
    struct va va[NVA];
    int nva;
    struct sync sync[NSYNC];
    uint64_t va_next;                            /* the next free VA (a bump allocator over va_start..va_end) */
};

static struct client *client(struct file *f) { return f->priv; }

/* ---------------------------------------------------------------- memory */

static long mem_new(struct client *c, struct sieos_nvgpu_mem *a)
{
    if (!a->size || a->size > (1ULL << 34))
        return -EINVAL;
    if (!(a->flags & SIEOS_NVGPU_MEM_GART))
        return -ENOSYS;                          /* (VRAM: stage 4, GSP-RM's memory) */
    int h;
    for (h = 1; h < NMEM && c->mem[h].used; h++)
        ;
    if (h == NMEM)
        return -ENOSPC;
    struct mem *m = &c->mem[h];
    m->npages = (a->size + PAGE_SIZE - 1) / PAGE_SIZE;
    m->pages = kmalloc(m->npages * sizeof(uint64_t));
    if (!m->pages)
        return -ENOMEM;
    for (uint64_t i = 0; i < m->npages; i++)
        if (!(m->pages[i] = pmm_alloc())) {
            while (i--)
                pmm_free(m->pages[i]);
            kfree(m->pages);
            return -ENOMEM;
        }
    m->used = true;
    m->size = m->npages * PAGE_SIZE;
    m->flags = a->flags;
    a->handle = h;
    a->map_offset = (uint64_t)h << MAP_SHIFT;
    return 0;
}

static void mem_free(struct client *c, uint32_t h)
{
    if (h >= NMEM || !c->mem[h].used)
        return;
    struct mem *m = &c->mem[h];
    for (uint64_t i = 0; i < m->npages; i++)
        pmm_free(m->pages[i]);
    kfree(m->pages);
    memset(m, 0, sizeof(*m));
}

/* mmap's page: the object's page at the offset (handle in the high bits). */
static uint64_t dev_page(struct file *f, uint64_t off, bool *wc)
{
    struct client *c = client(f);
    uint64_t h = off >> MAP_SHIFT, o = off & ((1ULL << MAP_SHIFT) - 1);
    if (h >= NMEM || !c->mem[h].used || o >= c->mem[h].size)
        return 0;
    *wc = false;                                 /* (system memory: cached, the GPU snoops) */
    return c->mem[h].pages[o / PAGE_SIZE];
}

/* ---------------------------------------------------------------- the VA space */

static long va_alloc(struct client *c, struct sieos_nvgpu_va *a)
{
    const struct sieos_nvgpu_info *in = nvgpu_info();
    uint64_t align = a->align < 0x10000 ? 0x10000 : a->align;
    if (!a->size || (align & (align - 1)) || c->nva == NVA)
        return -EINVAL;
    uint64_t addr;
    if (a->flags & SIEOS_NVGPU_VA_FIXED) {
        addr = a->addr;
        if (addr < in->va_start || addr + a->size > in->va_end || (addr & 0xffff))
            return -EINVAL;
        for (int i = 0; i < c->nva; i++)
            if (addr < c->va[i].addr + c->va[i].size && c->va[i].addr < addr + a->size)
                return -EBUSY;
    } else {
        addr = (c->va_next + align - 1) & ~(align - 1);
        if (addr + a->size > in->va_end)
            return -ENOMEM;                      /* (a bump allocator: freed ranges are not reused yet) */
        c->va_next = addr + a->size;
    }
    c->va[c->nva].addr = addr;
    c->va[c->nva].size = a->size;
    c->nva++;
    a->addr = addr;
    return 0;
}

static long va_free(struct client *c, struct sieos_nvgpu_va *a)
{
    for (int i = 0; i < c->nva; i++)
        if (c->va[i].addr == a->addr) {
            c->va[i] = c->va[--c->nva];
            return 0;
        }
    return -EINVAL;
}

/* ---------------------------------------------------------------- timeline semaphores */

static struct sync *sync_of(struct client *c, uint32_t h)
{
    return h && h < NSYNC && c->sync[h].used ? &c->sync[h] : NULL;
}

static long sync_wait(struct client *c, struct sieos_nvgpu_wait *w)
{
    if (!w->count || w->count > 256)
        return -EINVAL;
    struct sieos_nvgpu_sync_point *pts = (void *)w->points_ptr;
    if (!user_ok(pts, w->count * sizeof(*pts), false))
        return -EFAULT;
    uint64_t deadline = 0;
    if (w->timeout_ns >= 0) {
        int64_t left = w->timeout_ns - (int64_t)hrtime();
        if (left < 0)
            left = 0;
        deadline = ticks + (uint64_t)left / (1000000000 / TIMER_HZ) + 1;
    }
    for (;;) {
        uint32_t reached = 0, first = ~0U;
        for (uint32_t i = 0; i < w->count; i++) {
            struct sync *s = sync_of(c, pts[i].handle);
            if (!s)
                return -EINVAL;
            if (s->value >= pts[i].value) {
                reached++;
                if (first == ~0U)
                    first = i;
            }
        }
        if ((w->flags & SIEOS_NVGPU_WAIT_ALL) ? reached == w->count : reached > 0) {
            w->first = first;
            return 0;
        }
        if (deadline && ticks >= deadline)
            return -ETIME;
        if (signal_pending(current))
            return -EINTR;
        poll_sleep(deadline);
    }
}

/* ---------------------------------------------------------------- the device's file */

static long dev_ioctl(struct file *f, unsigned long cmd, void *arg)
{
    struct client *c = client(f);
    size_t sz;
    switch (cmd) {
    case SIEOS_NVGPU_INFO: sz = sizeof(struct sieos_nvgpu_info); break;
    case SIEOS_NVGPU_MEM_NEW: case SIEOS_NVGPU_MEM_FREE: sz = sizeof(struct sieos_nvgpu_mem); break;
    case SIEOS_NVGPU_VA_ALLOC: case SIEOS_NVGPU_VA_FREE: sz = sizeof(struct sieos_nvgpu_va); break;
    case SIEOS_NVGPU_BIND: sz = sizeof(struct sieos_nvgpu_bind); break;
    case SIEOS_NVGPU_CHAN_NEW: case SIEOS_NVGPU_CHAN_FREE: sz = sizeof(struct sieos_nvgpu_chan); break;
    case SIEOS_NVGPU_EXEC: sz = sizeof(struct sieos_nvgpu_exec); break;
    case SIEOS_NVGPU_SYNC_NEW: case SIEOS_NVGPU_SYNC_FREE: case SIEOS_NVGPU_SYNC_SIGNAL:
    case SIEOS_NVGPU_SYNC_QUERY: sz = sizeof(struct sieos_nvgpu_sync); break;
    case SIEOS_NVGPU_SYNC_WAIT: sz = sizeof(struct sieos_nvgpu_wait); break;
    case SIEOS_NVGPU_TIMESTAMP: sz = sizeof(uint64_t); break;
    default: return -ENOTTY;
    }
    if (!user_ok(arg, sz, true))
        return -EFAULT;
    switch (cmd) {
    case SIEOS_NVGPU_INFO:
        memcpy(arg, nvgpu_info(), sz);
        return 0;
    case SIEOS_NVGPU_TIMESTAMP:
        *(uint64_t *)arg = nvgpu_timestamp();
        return 0;
    case SIEOS_NVGPU_MEM_NEW:
        return mem_new(c, arg);
    case SIEOS_NVGPU_MEM_FREE:
        mem_free(c, ((struct sieos_nvgpu_mem *)arg)->handle);
        return 0;
    case SIEOS_NVGPU_VA_ALLOC:
        return va_alloc(c, arg);
    case SIEOS_NVGPU_VA_FREE:
        return va_free(c, arg);
    case SIEOS_NVGPU_SYNC_NEW: {
        struct sieos_nvgpu_sync *s = arg;
        for (uint32_t h = 1; h < NSYNC; h++)
            if (!c->sync[h].used) {
                c->sync[h].used = true;
                c->sync[h].value = s->value;
                s->handle = h;
                return 0;
            }
        return -ENOSPC;
    }
    case SIEOS_NVGPU_SYNC_FREE: {
        struct sync *s = sync_of(c, ((struct sieos_nvgpu_sync *)arg)->handle);
        if (!s)
            return -EINVAL;
        s->used = false;
        return 0;
    }
    case SIEOS_NVGPU_SYNC_SIGNAL: {
        struct sieos_nvgpu_sync *a = arg;
        struct sync *s = sync_of(c, a->handle);
        if (!s)
            return -EINVAL;
        if (a->value > s->value)
            s->value = a->value;
        poll_wakeup();
        return 0;
    }
    case SIEOS_NVGPU_SYNC_QUERY: {
        struct sieos_nvgpu_sync *a = arg;
        struct sync *s = sync_of(c, a->handle);
        if (!s)
            return -EINVAL;
        a->value = s->value;
        return 0;
    }
    case SIEOS_NVGPU_SYNC_WAIT:
        return sync_wait(c, arg);
    case SIEOS_NVGPU_BIND:
    case SIEOS_NVGPU_CHAN_NEW:
    case SIEOS_NVGPU_CHAN_FREE:
    case SIEOS_NVGPU_EXEC:
        return -ENOSYS;                          /* (stage 4) */
    }
    return -ENOTTY;
}

static void dev_close(struct file *f)
{
    struct client *c = client(f);
    for (uint32_t h = 1; h < NMEM; h++)
        mem_free(c, h);
    kfree(c);
}

static const struct file_ops nvgpu_ops = { "nvgpu", NULL, NULL, NULL, dev_close, dev_ioctl, dev_page };

static int dev_open(struct file *f, int minor)
{
    if (minor != 0 || !nvgpu_info())
        return -ENXIO;
    struct client *c = kmalloc(sizeof(*c));
    if (!c)
        return -ENOMEM;
    memset(c, 0, sizeof(*c));
    c->va_next = nvgpu_info()->va_start;
    f->type = FD_OPS;
    f->ops = &nvgpu_ops;
    f->priv = c;
    return 0;
}

void nvgpu_dev_init(void)
{
    if (cdev_register(SIEOS_DEV_NVGPU_MAJOR, dev_open) == 0)
        dev_node("/dev/nvgpu0", S_IFCHR | 0666, MKDEV(SIEOS_DEV_NVGPU_MAJOR, 0));
}
