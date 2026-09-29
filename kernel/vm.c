/*
 * vm.c - Process address spaces (ABI v2 milestone 4).
 *
 *   0x400000         program image, then the brk heap
 *   ...              mmap areas, allocated top-down below the stack
 *   USER_MMAP_TOP    guard page
 *   USER_STACK_TOP   8 MiB stack, demand-paged
 *
 * fork shares every private page copy-on-write: both page tables lose the
 * W bit and get PTE_COW (PTE_WANTW remembers that the page is logically
 * writable); the first write copies the frame, or just takes it back when
 * nobody else maps it any more.  MAP_SHARED pages (PTE_SHARED) stay shared.
 * Anonymous private memory is zero-filled on first touch.
 *
 * Locking: page faults from user mode run without the big kernel lock, under
 * the process's vmlock only (vm_space_lock), so that the LWPs of a process, and
 * different processes, fault in parallel.  The fault path reads the area list
 * and changes page tables; everything else runs under the kernel lock and
 * takes vmlock around its own page-table changes and area-list changes (never
 * across anything that sleeps), and frees areas only after unlinking them
 * under it.  Order: kernel lock, vmlock, the frame allocator's lock.
 */
#include "proc.h"
#include "mm.h"
#include "vm.h"
#include "smp.h"
#include "fs.h"
#include "abi2.h"
#include "sieos/mman.h"
#include "sieos/time.h"

#define MMAP_FLOOR_GAP (64UL * 1024 * 1024)   /* room left above the heap for brk */

static uint64_t prot_flags(int prot)
{
    uint64_t f = PTE_U;
    if (prot & SIEOS_PROT_WRITE)
        f |= PTE_W;
    if (!(prot & SIEOS_PROT_EXEC))
        f |= pte_nx;
    return f;
}

static struct vm_area *find_area(struct proc *p, uint64_t va)
{
    for (struct vm_area *a = p->areas; a; a = a->next)
        if (va >= a->start && va < a->end)
            return a;
    return NULL;
}

static void insert_area(struct proc *p, struct vm_area *n)
{
    struct vm_area **pp = &p->areas;
    while (*pp && (*pp)->start < n->start)
        pp = &(*pp)->next;
    n->next = *pp;
    *pp = n;
}

/* An area's references: the mapped file and the System V segment. */
static void area_hold(struct vm_area *a)
{
    if (a->ip)
        idup(a->ip);
    if (a->shm)
        shm_attach_ref(a->shm, 1);
}

/* Part [s, e) of an area goes away: write shared file pages back. */
static void area_writeback(struct vm_area *a, uint64_t s, uint64_t e)
{
    if (!a->ip || !(a->flags & SIEOS_MAP_SHARED))
        return;                                  /* private file pages never go back */
    for (uint64_t va = s; va < e; va += PAGE_SIZE)
        vfs_writeback(a->ip, (a->off + (va - a->start)) / PAGE_SIZE);
}

/* The area (already unmapped) is gone. */
static void area_free(struct vm_area *a)
{
    if (a->ip) {
        vfs_pcache_trim(a->ip);
        iput(a->ip);
    }
    if (a->shm)
        shm_attach_ref(a->shm, -1);
    kfree(a);
}

/* Split a at address 'at': a keeps [start, at), the new area gets [at, end). */
static struct vm_area *area_split(struct vm_area *a, uint64_t at)
{
    struct vm_area *b = kmalloc(sizeof(*b));
    if (!b)
        return NULL;
    *b = *a;
    b->start = at;
    b->off = a->off + (at - a->start);
    a->end = at;
    b->next = a->next;
    a->next = b;
    area_hold(b);
    return b;
}

void vm_space_lock(struct proc *p)
{
    if (!__atomic_exchange_n(&p->vmlock.locked, 1, __ATOMIC_ACQUIRE))
        return;
    /* While we spin, a TLB shootdown for this space must not wait for us
     * (interrupts are off): catch up on it once we have the lock. */
    struct cpu *c = mycpu();
    c->bkl_waiting = 1;
    spin_lock(&p->vmlock);
    c->bkl_waiting = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (c->tlb_flush) {
        c->tlb_flush = 0;
        write_cr3(read_cr3());
    }
}

void vm_space_unlock(struct proc *p)
{
    spin_unlock(&p->vmlock);
}

static void flush_if_current(uint64_t pml4)
{
    tlb_shootdown(pml4);                         /* this CPU and the others running this space */
}

/* ---------------- fork and teardown ---------------- */

uint64_t vm_space_copy(struct proc *child, struct proc *parent)
{
    uint64_t src = parent->pml4, dst = vmm_new_space();
    if (!dst)
        return 0;
    vm_space_lock(parent);
    uint64_t *l4 = P2V(src & PTE_ADDR);
    for (uint64_t i = 0; i < 256; i++) {
        if (!(l4[i] & PTE_P))
            continue;
        uint64_t *l3 = P2V(l4[i] & PTE_ADDR);
        for (uint64_t j = 0; j < 512; j++) {
            if (!(l3[j] & PTE_P))
                continue;
            uint64_t *l2 = P2V(l3[j] & PTE_ADDR);
            for (uint64_t k = 0; k < 512; k++) {
                if (!(l2[k] & PTE_P))
                    continue;
                uint64_t *l1 = P2V(l2[k] & PTE_ADDR);
                for (uint64_t m = 0; m < 512; m++) {
                    uint64_t e = l1[m];
                    if (!(e & PTE_P))
                        continue;
                    uint64_t va = (i << 39) | (j << 30) | (k << 21) | (m << 12);
                    uint64_t pa = e & PTE_ADDR, fl = e & PTE_FLAGS;
                    if (!(e & (PTE_DEVICE | PTE_SHARED))) {
                        /* private: share copy-on-write */
                        if (e & PTE_W)
                            fl |= PTE_WANTW;
                        fl = (fl & ~PTE_W) | PTE_COW;
                        l1[m] = pa | fl;
                    }
                    if (vmm_map(dst, va, pa, fl & ~PTE_P) < 0) {
                        flush_if_current(src);
                        vm_space_unlock(parent);
                        vm_space_free(child, dst);
                        return 0;
                    }
                    if (!(e & PTE_DEVICE))
                        pmm_ref(pa);
                }
            }
        }
    }
    flush_if_current(src);
    vm_space_unlock(parent);
    /* the child gets copies of the area descriptions */
    for (struct vm_area *a = parent->areas; a; a = a->next) {
        struct vm_area *n = kmalloc(sizeof(*n));
        if (!n)
            break;
        *n = *a;
        n->next = NULL;
        area_hold(n);
        insert_area(child, n);
    }
    return dst;
}

void vm_space_free(struct proc *p, uint64_t pml4)
{
    vmm_free_space(pml4);
    vm_exec_reset(p);
}

void vm_exec_reset(struct proc *p)
{
    vm_space_lock(p);
    struct vm_area *list = p->areas;
    p->areas = NULL;
    vm_space_unlock(p);
    while (list) {
        struct vm_area *a = list;
        list = a->next;
        area_writeback(a, a->start, a->end);
        area_free(a);
    }
}

/* ---------------- faults ---------------- */

/* Give the writer its own copy of a copy-on-write page. */
static bool cow_break(uint64_t pml4, uint64_t va, uint64_t *pte)
{
    uint64_t pa = *pte & PTE_ADDR, fl = *pte & PTE_FLAGS;
    fl = (fl & ~PTE_COW) | PTE_W;
    if (!(fl & PTE_SHARED))
        fl &= ~PTE_WANTW;
    if (pmm_refcount(pa) == 1) {
        *pte = pa | fl;                           /* last user: take it back */
    } else {
        uint64_t n = pmm_alloc();
        if (!n)
            return false;
        memcpy(P2V(n), P2V(pa), PAGE_SIZE);
        *pte = n | fl;
        pmm_unref(pa);
    }
    (void)va;
    tlb_shootdown(pml4);
    return true;
}

/* *frame: one allocated (and cleared) before the fault took vmlock, or 0 */
static bool zero_page(uint64_t pml4, uint64_t va, uint64_t flags, uint64_t *frame)
{
    uint64_t pa = *frame ? *frame : pmm_alloc();
    *frame = 0;
    if (!pa)
        return false;
    if (vmm_map(pml4, va, pa, flags) < 0) {
        pmm_free(pa);
        return false;
    }
    return true;
}

static bool fault(struct proc *p, uint64_t addr, uint64_t err, uint64_t *frame);

/* Also called without the kernel lock, for faults from user mode (see above). */
bool vm_fault(uint64_t addr, uint64_t err, bool from_user)
{
    (void)from_user;
    struct lwp *l = curlwp;
    if (!l || l->is_idle || addr >= USER_LIMIT || addr < USER_BASE)
        return false;
    struct proc *p = l->proc;
    /* A fault on a missing page usually needs a zeroed frame: clear it
     * before taking the lock, so that the LWPs of a process overlap there. */
    uint64_t frame = 0;
    if (!(err & 1) && p->pml4)
        frame = pmm_alloc();
    vm_space_lock(p);
    bool ok = fault(p, addr, err, &frame);
    vm_space_unlock(p);
    if (frame)
        pmm_free(frame);                         /* not needed after all */
    return ok;
}

static bool fault(struct proc *p, uint64_t addr, uint64_t err, uint64_t *frame)
{
    uint64_t pml4 = p->pml4, va = PAGE_ALIGN_DOWN(addr);
    if (!pml4 || pml4 == kernel_pml4_phys)
        return false;
    uint64_t *pte = vmm_pte(pml4, va, false);
    if (pte && (*pte & PTE_P)) {
        /* present: a write to a copy-on-write page is ours to fix */
        if ((err & 2) && (*pte & PTE_COW) && (*pte & PTE_WANTW) && (*pte & PTE_U))
            return cow_break(pml4, va, pte);
        /* another CPU fixed the page while we waited for the lock, or our TLB entry is
         * stale: if the access is allowed now, retry it */
        bool ok = (*pte & PTE_U) && !(*pte & PTE_NOACC) && (!(err & 2) || (*pte & PTE_W)) &&
                  !((err & 0x10) && (*pte & pte_nx));
        if (ok)
            __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
        return ok;
    }
    if (err & 1)
        return false;
    /* the stack: 8 MiB (or RLIMIT_STACK), demand-paged */
    uint64_t limit = MIN(p->rlim_cur[3] ? p->rlim_cur[3] : USER_STACK_SIZE, USER_STACK_SIZE);
    if (va >= USER_STACK_TOP - limit && va < USER_STACK_TOP)
        return zero_page(pml4, va, PTE_U | PTE_W | pte_nx, frame);
    struct vm_area *a = find_area(p, va);
    if (a && a->prot != SIEOS_PROT_NONE && !(a->flags & SIEOS_MAP_SHARED)) {
        if ((err & 2) && !(a->prot & SIEOS_PROT_WRITE))
            return false;
        return zero_page(pml4, va, prot_flags(a->prot), frame);
    }
    return false;
}

/* ---------------- mapping ---------------- */

/* The first mapped user page in [va, end), or end: empty tables are skipped whole. */
static uint64_t next_mapped(uint64_t pml4, uint64_t va, uint64_t end)
{
    while (va < end) {
        uint64_t *l4 = P2V(pml4 & PTE_ADDR);
        uint64_t e4 = l4[(va >> 39) & 511];
        if (!(e4 & PTE_P)) {
            va = ((va >> 39) + 1) << 39;
            continue;
        }
        uint64_t *l3 = P2V(e4 & PTE_ADDR);
        uint64_t e3 = l3[(va >> 30) & 511];
        if (!(e3 & PTE_P)) {
            va = ((va >> 30) + 1) << 30;
            continue;
        }
        uint64_t *l2 = P2V(e3 & PTE_ADDR);
        uint64_t e2 = l2[(va >> 21) & 511];
        if (!(e2 & PTE_P)) {
            va = ((va >> 21) + 1) << 21;
            continue;
        }
        uint64_t *l1 = P2V(e2 & PTE_ADDR);
        if (l1[(va >> 12) & 511] & PTE_P)
            return va;
        va += PAGE_SIZE;
    }
    return end;
}

static void unmap_pages(struct proc *p, uint64_t start, uint64_t end)
{
    uint64_t pml4 = p->pml4;
    vm_space_lock(p);
    for (uint64_t va = next_mapped(pml4, start, end); va < end; va = next_mapped(pml4, va + PAGE_SIZE, end)) {
        uint64_t *pte = vmm_pte(pml4, va, false);
        if (!pte || !(*pte & PTE_P))
            continue;
        if (!(*pte & PTE_DEVICE))
            pmm_unref(*pte & PTE_ADDR);
        *pte = 0;
    }
    flush_if_current(pml4);
    vm_space_unlock(p);
}

/* Remove [start, end) (already unmapped) from the area list, splitting areas that straddle it. */
static int cut_areas(struct proc *p, uint64_t start, uint64_t end)
{
    /* shared file pages go back first: that sleeps */
    for (struct vm_area *a = p->areas; a; a = a->next)
        if (a->end > start && a->start < end)
            area_writeback(a, MAX(a->start, start), MIN(a->end, end));
    struct vm_area *gone = NULL;
    struct inode *trim[2] = { NULL, NULL };
    int ntrim = 0, r = 0;
    vm_space_lock(p);
    struct vm_area **pp = &p->areas;
    while (*pp) {
        struct vm_area *a = *pp;
        if (a->end <= start || a->start >= end) {
            pp = &a->next;
            continue;
        }
        if (a->start < start && a->end > end) {         /* hole in the middle: split */
            if (!area_split(a, end)) {
                r = -ENOMEM;
                break;
            }
            a->end = start;
            if (a->ip && ntrim < 2)
                trim[ntrim++] = a->ip;
            break;
        }
        if (a->start < start) {
            a->end = start;
            if (a->ip && ntrim < 2)
                trim[ntrim++] = a->ip;
            pp = &a->next;
        } else if (a->end > end) {
            a->off += end - a->start;
            a->start = end;
            if (a->ip && ntrim < 2)
                trim[ntrim++] = a->ip;
            pp = &a->next;
        } else {
            *pp = a->next;
            a->next = gone;
            gone = a;
        }
    }
    vm_space_unlock(p);
    for (int i = 0; i < ntrim; i++)
        vfs_pcache_trim(trim[i]);                    /* (the areas still hold the inodes) */
    while (gone) {
        struct vm_area *a = gone;
        gone = a->next;
        area_free(a);
    }
    return r;
}

static bool range_free(struct proc *p, uint64_t start, uint64_t end)
{
    for (struct vm_area *a = p->areas; a; a = a->next)
        if (a->start < end && a->end > start)
            return false;
    return next_mapped(p->pml4, start, end) == end;
}

/* Highest free range of len bytes (aligned) above floor, below USER_MMAP_TOP. */
static uint64_t find_space_above(struct proc *p, uint64_t len, uint64_t align, uint64_t floor)
{
    uint64_t end = USER_MMAP_TOP;
    for (;;) {
        if (end < floor + len)
            return 0;
        uint64_t start = (end - len) & ~(align - 1);
        if (start < floor)
            return 0;
        struct vm_area *hit = NULL;
        for (struct vm_area *a = p->areas; a; a = a->next)
            if (a->start < start + len && a->end > start && (!hit || a->start < hit->start))
                hit = a;
        if (!hit)
            return start;
        end = hit->start;
    }
}

/* Leave room for the brk heap to grow, unless the address space is full. */
static uint64_t find_space(struct proc *p, uint64_t len, uint64_t align)
{
    uint64_t heap = PAGE_ALIGN_UP(p->brk);
    uint64_t r = find_space_above(p, len, align, heap + MMAP_FLOOR_GAP);
    return r ? r : find_space_above(p, len, align, heap);
}

/*
 * RLIMIT_VMEM: may the address space grow by n bytes?  It counts the
 * mappings and the brk heap (the program's own segments and the stack are
 * small next to the limits it is used for).
 */
bool vm_vmem_ok(struct proc *p, uint64_t n)
{
    uint64_t lim = p->rlim_cur[SIEOS_RLIMIT_VMEM];
    if (lim == SIEOS_RLIM_INFINITY)
        return true;
    uint64_t size = p->brk > p->heap_start ? p->brk - p->heap_start : 0;
    for (struct vm_area *a = p->areas; a; a = a->next)
        size += a->end - a->start;
    return size + n <= lim;
}

/* May the brk heap grow to [old, new)?  Not over a mapping. */
bool vm_brk_ok(struct proc *p, uint64_t old, uint64_t new)
{
    old = PAGE_ALIGN_UP(old);
    new = PAGE_ALIGN_UP(new);
    for (struct vm_area *a = p->areas; a; a = a->next)
        if (a->start < new && a->end > old)
            return false;
    return true;
}

static int map_locked(struct proc *p, uint64_t va, uint64_t pa, uint64_t fl)
{
    vm_space_lock(p);
    int r = vmm_map(p->pml4, va, pa, fl);
    vm_space_unlock(p);
    return r;
}

long vm_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off)
{
    struct proc *p = current;
    int type = flags & SIEOS_MAP_TYPE;
    if (!len || (type != SIEOS_MAP_SHARED && type != SIEOS_MAP_PRIVATE) || (off & (PAGE_SIZE - 1)) ||
        (prot & ~(SIEOS_PROT_READ | SIEOS_PROT_WRITE | SIEOS_PROT_EXEC)))
        return -EINVAL;
    if (flags & ~(SIEOS_MAP_TYPE | SIEOS_MAP_FIXED | SIEOS_MAP_NORESERVE | SIEOS_MAP_ANON | SIEOS_MAP_ALIGN |
                  SIEOS_MAP_TEXT | SIEOS_MAP_INITDATA))
        return -EINVAL;
    len = PAGE_ALIGN_UP(len);
    if (!vm_vmem_ok(p, len))
        return -ENOMEM;
    bool anon = flags & SIEOS_MAP_ANON;
    struct file *f = NULL;
    if (!anon) {
        f = fd_file(fd);
        if (!f)
            return -EBADF;
        if (f->type != FD_INODE || !S_ISREG(inode_mode(f->ip)))
            return -ENODEV;
        if ((f->flags & O_ACCMODE) == O_WRONLY)
            return -EACCES;
        if (type == SIEOS_MAP_SHARED && (prot & SIEOS_PROT_WRITE) && (f->flags & O_ACCMODE) != O_RDWR)
            return -EACCES;
    }
    uint64_t start;
    if (flags & SIEOS_MAP_FIXED) {
        if ((addr & (PAGE_SIZE - 1)) || addr < USER_BASE || addr + len > USER_MMAP_TOP || addr + len < addr)
            return -EINVAL;
        start = addr;
        unmap_pages(p, start, start + len);
        if (cut_areas(p, start, start + len) < 0)
            return -ENOMEM;
    } else {
        uint64_t align = PAGE_SIZE;
        if (flags & SIEOS_MAP_ALIGN) {
            if (addr && (addr & (addr - 1)))
                return -EINVAL;
            align = MAX(addr, PAGE_SIZE);
            addr = 0;
        }
        start = 0;
        if (addr && !(addr & (PAGE_SIZE - 1)) && addr >= USER_BASE && addr + len <= USER_MMAP_TOP &&
            range_free(p, addr, addr + len))
            start = addr;                            /* the hint is free */
        if (!start)
            start = find_space(p, len, align);
        if (!start)
            return -ENOMEM;
    }
    /* heuristic overcommit: a private writable mapping larger than memory cannot succeed */
    if (type == SIEOS_MAP_PRIVATE && (prot & SIEOS_PROT_WRITE) && !(flags & SIEOS_MAP_NORESERVE) &&
        len / PAGE_SIZE > pmm_total_pages())
        return -ENOMEM;
    /* shared anonymous and file mappings are filled now: they must fit in memory */
    if ((type == SIEOS_MAP_SHARED || !anon) && len / PAGE_SIZE + 64 > pmm_free_pages())
        return -ENOMEM;
    struct vm_area *a = kmalloc(sizeof(*a));
    if (!a)
        return -ENOMEM;
    a->start = start;
    a->end = start + len;
    a->prot = prot;
    a->flags = flags & (SIEOS_MAP_TYPE | SIEOS_MAP_ANON);
    a->ip = f ? idup(f->ip) : NULL;
    a->off = off;
    a->shm = NULL;
    vm_space_lock(p);
    insert_area(p, a);
    vm_space_unlock(p);

    /*
     * Shared anonymous and all file mappings are filled now; private anonymous
     * on demand.  A file mapping maps the file's page-cache pages: shared ones
     * as they are, private ones copy-on-write (always PTE_COW, so that a write,
     * or mprotect to writable, copies the page first).
     */
    if (type == SIEOS_MAP_SHARED || !anon) {
        uint64_t fl = prot_flags(prot) | (type == SIEOS_MAP_SHARED ? PTE_SHARED : 0);
        if (type == SIEOS_MAP_SHARED && (prot & SIEOS_PROT_WRITE))
            fl |= PTE_WANTW;
        if (type != SIEOS_MAP_SHARED && f) {
            if (fl & PTE_W)
                fl |= PTE_WANTW;
            fl = (fl & ~PTE_W) | PTE_COW;
        }
        if (prot == SIEOS_PROT_NONE)
            fl = (fl & ~PTE_U) | PTE_NOACC;
        uint64_t fsize = f ? inode_size(f->ip) : 0;
        for (uint64_t va = start; va < start + len; va += PAGE_SIZE) {
            uint64_t pa = 0;
            if (a->ip) {                             /* the file's page */
                if (vfs_getpage(a->ip, (off + (va - start)) / PAGE_SIZE, &pa) < 0)
                    pa = 0;
                if (pa && map_locked(p, va, pa, fl) < 0) {
                    pmm_unref(pa);
                    pa = 0;
                }
                if (!pa) {
                    unmap_pages(p, start, start + len);
                    cut_areas(p, start, start + len);
                    return -ENOMEM;
                }
                continue;
            }
            pa = pmm_alloc();
            if (!pa || map_locked(p, va, pa, fl) < 0) {
                if (pa)
                    pmm_free(pa);
                unmap_pages(p, start, start + len);
                cut_areas(p, start, start + len);
                return -ENOMEM;
            }
            uint64_t fo = off + (va - start);
            if (f && fo < fsize) {
                long n = readi(f->ip, P2V(pa), fo, MIN(PAGE_SIZE, fsize - fo));
                if (n < 0) {
                    unmap_pages(p, start, start + len);
                    cut_areas(p, start, start + len);
                    return -EIO;
                }
            }
        }
    }
    return (long)start;
}

long vm_munmap(uint64_t addr, uint64_t len)
{
    struct proc *p = current;
    if ((addr & (PAGE_SIZE - 1)) || !len || addr < USER_BASE || addr + len > USER_LIMIT || addr + len < addr)
        return -EINVAL;
    len = PAGE_ALIGN_UP(len);
    unmap_pages(p, addr, addr + len);
    return cut_areas(p, addr, addr + len);
}

/* Is every page of [start, end) mapped, inside an area, or in the stack region? */
static bool range_mapped(struct proc *p, uint64_t start, uint64_t end)
{
    for (uint64_t va = start; va < end;) {
        struct vm_area *a = find_area(p, va);
        if (a) {
            va = a->end;                         /* areas are covered whole */
        } else if (va >= USER_STACK_TOP - USER_STACK_SIZE && va < USER_STACK_TOP) {
            va = USER_STACK_TOP;
        } else if (vmm_translate(p->pml4, va, NULL)) {
            va += PAGE_SIZE;
        } else {
            return false;
        }
    }
    return true;
}

long vm_mprotect(uint64_t addr, uint64_t len, int prot)
{
    struct proc *p = current;
    if ((addr & (PAGE_SIZE - 1)) || addr < USER_BASE || addr + len > USER_LIMIT || addr + len < addr ||
        (prot & ~(SIEOS_PROT_READ | SIEOS_PROT_WRITE | SIEOS_PROT_EXEC)))
        return -EINVAL;
    len = PAGE_ALIGN_UP(len);
    uint64_t end = addr + len;
    if (!range_mapped(p, addr, end))
        return -ENOMEM;
    /* split the areas at the range edges, then set their protection */
    vm_space_lock(p);
    for (struct vm_area *a = p->areas; a; a = a->next) {
        if (a->end <= addr || a->start >= end)
            continue;
        if (a->start < addr) {
            if (!area_split(a, addr)) {
                vm_space_unlock(p);
                return -ENOMEM;
            }
            continue;                                /* the new area is visited next */
        }
        if (a->end > end && !area_split(a, end)) {
            vm_space_unlock(p);
            return -ENOMEM;
        }
        a->prot = prot;
    }
    for (uint64_t va = next_mapped(p->pml4, addr, end); va < end; va = next_mapped(p->pml4, va + PAGE_SIZE, end)) {
        uint64_t *pte = vmm_pte(p->pml4, va, false);
        if (!pte || !(*pte & PTE_P) || (*pte & PTE_DEVICE))
            continue;
        uint64_t e = *pte & ~(PTE_W | PTE_WANTW | PTE_NX | PTE_NOACC | PTE_U);
        if (prot == SIEOS_PROT_NONE) {
            e |= PTE_NOACC | pte_nx;
        } else {
            e |= PTE_U;
            if (prot & SIEOS_PROT_WRITE)
                e |= (e & PTE_COW) ? PTE_WANTW : (PTE_W | ((e & PTE_SHARED) ? PTE_WANTW : 0));
            if (!(prot & SIEOS_PROT_EXEC))
                e |= pte_nx;
        }
        *pte = e;
    }
    flush_if_current(p->pml4);
    vm_space_unlock(p);
    return 0;
}

long vm_mincore(uint64_t addr, uint64_t len, char *vec)
{
    if ((addr & (PAGE_SIZE - 1)) || addr + len < addr)
        return -EINVAL;
    uint64_t n = PAGE_ALIGN_UP(len) / PAGE_SIZE;
    if (!user_ok(vec, n, true))
        return -EFAULT;
    if (!range_mapped(current, addr, addr + n * PAGE_SIZE))
        return -ENOMEM;
    for (uint64_t i = 0; i < n; i++)
        vec[i] = vmm_translate(current->pml4, addr + i * PAGE_SIZE, NULL) ? 1 : 0;
    return 0;
}

long vm_memcntl(uint64_t addr, uint64_t len, int cmd, uint64_t arg)
{
    struct proc *p = current;
    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;
    len = PAGE_ALIGN_UP(len);
    switch (cmd) {
    case SIEOS_MC_SYNC:                              /* msync: shared file pages go to the file */
        for (struct vm_area *a = p->areas; a; a = a->next)
            if (a->end > addr && a->start < addr + len)
                area_writeback(a, MAX(a->start, addr), MIN(a->end, addr + len));
        if (!range_mapped(p, addr, addr + len))
            return -ENOMEM;
        vfs_sync();
        return 0;
    case SIEOS_MC_LOCK:
    case SIEOS_MC_UNLOCK:
    case SIEOS_MC_LOCKAS:
    case SIEOS_MC_UNLOCKAS:                          /* nothing is ever paged out */
        return range_mapped(p, addr, addr + len) || cmd == SIEOS_MC_LOCKAS || cmd == SIEOS_MC_UNLOCKAS ? 0 : -ENOMEM;
    case SIEOS_MC_ADVISE:
        if (arg == SIEOS_MADV_DONTNEED || arg == SIEOS_MADV_FREE) {
            /* private anonymous pages are dropped and come back zero-filled */
            for (struct vm_area *a = p->areas; a; a = a->next)
                if (a->end > addr && a->start < addr + len && (a->flags & SIEOS_MAP_ANON) &&
                    !(a->flags & SIEOS_MAP_SHARED))
                    unmap_pages(p, MAX(a->start, addr), MIN(a->end, addr + len));
            return 0;
        }
        return arg <= SIEOS_MADV_FREE ? 0 : -EINVAL;
    }
    return -EINVAL;
}

/* ---------------- System V shared memory ---------------- */

long vm_map_shm(struct shmseg *seg, uint64_t addr, uint64_t len, int prot, bool fixed)
{
    struct proc *p = current;
    len = PAGE_ALIGN_UP(len);
    uint64_t start;
    if (fixed) {
        if ((addr & (PAGE_SIZE - 1)) || addr < USER_BASE || addr + len > USER_MMAP_TOP || !range_free(p, addr, addr + len))
            return -EINVAL;
        start = addr;
    } else if (!(start = find_space(p, len, PAGE_SIZE))) {
        return -ENOMEM;
    }
    struct vm_area *a = kzalloc(sizeof(*a));
    if (!a)
        return -ENOMEM;
    a->start = start;
    a->end = start + len;
    a->prot = prot;
    a->flags = SIEOS_MAP_SHARED;
    a->shm = seg;
    shm_attach_ref(seg, 1);
    vm_space_lock(p);
    insert_area(p, a);
    uint64_t fl = prot_flags(prot) | PTE_SHARED | ((prot & SIEOS_PROT_WRITE) ? PTE_WANTW : 0);
    for (uint64_t va = start; va < start + len; va += PAGE_SIZE) {
        uint64_t pa = shm_frame(seg, (va - start) / PAGE_SIZE);
        pmm_ref(pa);
        if (vmm_map(p->pml4, va, pa, fl) < 0) {
            pmm_unref(pa);
            vm_space_unlock(p);
            unmap_pages(p, start, start + len);
            cut_areas(p, start, start + len);
            return -ENOMEM;
        }
    }
    vm_space_unlock(p);
    return start;
}

long vm_unmap_shm(uint64_t addr)
{
    struct proc *p = current;
    struct vm_area *a = find_area(p, addr);
    if (!a || !a->shm || a->start != addr)
        return -EINVAL;
    struct shmseg *seg = a->shm;
    uint64_t start = a->start, end = a->end;
    /* the whole attachment, including pieces split off by mprotect */
    for (struct vm_area *b = a->next; b && b->shm == seg && b->start == end; b = b->next)
        end = b->end;
    unmap_pages(p, start, end);
    return cut_areas(p, start, end);
}

/* An area for memory the kernel mapped itself (the dynamic linker at exec). */
void vm_add_area(struct proc *p, uint64_t start, uint64_t end, int prot, int flags)
{
    struct vm_area *a = kzalloc(sizeof(*a));
    if (!a)
        return;
    a->start = start;
    a->end = end;
    a->prot = prot;
    a->flags = flags;
    vm_space_lock(p);
    insert_area(p, a);
    vm_space_unlock(p);
}
