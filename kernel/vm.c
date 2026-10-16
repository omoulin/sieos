/*
 * vm.c - Anonymous memory given on first touch, and waiting on an address.
 *
 * A process may reserve regions of its address space (SYS_MMAP): nothing
 * is allocated then. The first access to a page of a region, by the
 * program (a page fault: vm_fault) or by the kernel copying a message into
 * it (mem.c), gets a zeroed page with the region's rights. SYS_MUNMAP
 * removes a region or gives its pages back (UNMAP_KEEP: the region stays,
 * its pages come back zeroed when touched again: Linux's MADV_DONTNEED),
 * SYS_MPROTECT changes rights. This is the memory big ported programs
 * expect (WebKit, JavaScriptCore's JIT, musl's threads and their stacks:
 * docs/webkit-plan.md).
 *
 * Regions are a sorted list per process (under its lock), between
 * MAP_BASE and the drivers' device area (MMIO_BASE). The kernel only ever
 * finds a process from an address space here (vm_as_*), for its copies.
 *
 * SYS_FUTEX: a thread waits while a 32-bit word holds a value, until
 * another wakes it (musl's mutexes, condition variables, pthread_join).
 * Waiters are keyed by the word's physical address, so a futex in memory
 * two processes share works too. A waiter sleeps as SYS_SLEEP does, and is
 * woken as SYS_WAKE does (a wake that comes before the sleep is not lost).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"

#define MAP_BASE 0x0000100000000000UL   /* 16 TiB: below, the program and its heap */

typedef struct region { uint64_t start, end, prot; struct region *next; } region_t;

/* ---- Address space -> process, for the kernel's copies (mem.c). */
#define NAS 64
static proc_t *as_hash[NAS];
static spinlock_t as_lock;             /* (only around this table: taken alone) */
static unsigned as_slot(uint64_t as) { return (as >> 12) % NAS; }
void vm_as_add(proc_t *p)
{
    spin_lock(&as_lock);
    p->asnext = as_hash[as_slot(p->as)];
    as_hash[as_slot(p->as)] = p;
    spin_unlock(&as_lock);
}
void vm_as_del(proc_t *p)
{
    spin_lock(&as_lock);
    for (proc_t **q = &as_hash[as_slot(p->as)]; *q; q = &(*q)->asnext)
        if (*q == p) { *q = p->asnext; break; }
    spin_unlock(&as_lock);
}

static uint64_t vm_flags(uint64_t prot)       /* PROT_* -> the page's VM_* (PROT_NONE: kernel-only) */
{
    return (prot & (PROT_READ | PROT_WRITE | PROT_EXEC) ? VM_U : 0) | (prot & PROT_WRITE ? VM_W : 0)
         | (prot & PROT_EXEC ? 0 : VM_NX);
}

static region_t *find(proc_t *p, uint64_t va)   /* (under p->lock) */
{
    for (region_t *r = p->regions; r && r->start <= va; r = r->next)
        if (va < r->end) return r;
    return 0;
}

/* Give page va of p the zeroed page its region promises, if the access
 * (PROT_READ, PROT_WRITE or PROT_EXEC) is allowed. -> 1: done (or another
 * thread was faster, or a stale translation on this CPU: try again), 0: a
 * real fault. */
static int fault(proc_t *p, uint64_t va, int access)
{
    va &= ~(PAGE - 1);
    spin_lock(&p->lock);
    region_t *r = find(p, va);
    int ok = r && !p->exiting && (r->prot & access) == (uint64_t)access;
    if (ok && !arch_uaddr(p->as, va, 3)) {          /* (there: its rights are the region's, try again) */
        uint64_t pa = p->quota && p->pages >= p->quota ? 0 : page_alloc(1);
        if (!pa) { ok = 0; kprintf("mk: %s (pid %d): out of memory\n", p->name, p->pid); }
        else if (vm_map(p->as, va, pa, vm_flags(r->prot))) page_free(pa, 1);   /* (there already) */
        else {
            p->pages++;
            if (r->prot & PROT_EXEC) arch_sync_code(P2V(pa), PAGE);   /* (old instructions of that page, elsewhere) */
        }
    }
    spin_unlock(&p->lock);
    return ok;
}

/* A page fault from the running program (the architecture's trap). */
int vm_fault(uint64_t va, int access) { return va < USER_TOP && fault(cur->proc, va, access); }

/* The kernel found no page at va in address space as (mem.c's copies). */
int vm_fault_as(uint64_t as, uint64_t va, int write)
{
    proc_t *p;
    spin_lock(&as_lock);
    for (p = as_hash[as_slot(as)]; p && p->as != as; p = p->asnext) ;
    spin_unlock(&as_lock);
    return p && va < USER_TOP && fault(p, va, write ? PROT_WRITE : PROT_READ);
}

/* Cut regions at a and b, so that [a, b) is made of whole regions. */
static int split(proc_t *p, uint64_t at)
{
    region_t *r = find(p, at);
    if (!r || r->start == at) return 0;
    region_t *n = kzalloc(sizeof *n);
    if (!n) return -ENOMEM;
    *n = (region_t){ at, r->end, r->prot, r->next };
    r->end = at;
    r->next = n;
    return 0;
}

/* Give back the pages in [a, b) (they are p's own: never device memory). */
static void drop_pages(proc_t *p, uint64_t a, uint64_t b)
{
    for (uint64_t va = a; va < b; va += PAGE)       /* (count first: vm_unmap frees them) */
        if (arch_uaddr(p->as, va, 3)) p->pages--;
    vm_unmap(p, a, b);
}

/* Remove the regions in [a, b) and their pages (under p->lock). */
static int unmap_locked(proc_t *p, uint64_t a, uint64_t b, int keep)
{
    if (split(p, a) || split(p, b)) return -ENOMEM;
    for (region_t **q = &p->regions; *q; ) {
        region_t *r = *q;
        if (r->end <= a || r->start >= b) { q = &r->next; continue; }
        drop_pages(p, r->start, r->end);
        if (keep) { q = &r->next; continue; }
        *q = r->next;
        kfree(r);
    }
    return 0;
}

/* SYS_MMAP: reserve size bytes (at addr with MAP_FIXED, replacing what was
 * there; else wherever there is room). -> the address. */
long vm_mmap(uint64_t addr, uint64_t size, uint64_t flags)
{
    proc_t *p = cur->proc;
    uint64_t prot = flags & (PROT_READ | PROT_WRITE | PROT_EXEC);
    size = (size + PAGE - 1) & ~(PAGE - 1);
    if (!size || size > MMIO_BASE - MAP_BASE || (flags & ~(prot | MAP_FIXED))) return -EINVAL;
    if ((flags & MAP_FIXED) && (addr % PAGE || addr < MAP_BASE || addr > MMIO_BASE - size)) return -EINVAL;
    region_t *n = kzalloc(sizeof *n);
    if (!n) return -ENOMEM;
    spin_lock(&p->lock);
    region_t **q = &p->regions;
    long e = 0;
    if (flags & MAP_FIXED) {
        e = unmap_locked(p, addr, addr + size, 0);
        while (*q && (*q)->start < addr) q = &(*q)->next;
    } else {                                    /* the first gap that fits */
        addr = MAP_BASE;
        for (; *q && (*q)->start - addr < size; q = &(*q)->next) addr = (*q)->end;
        if (MMIO_BASE - addr < size) e = -ENOMEM;
    }
    if (!e) { *n = (region_t){ addr, addr + size, prot, *q }; *q = n; }
    spin_unlock(&p->lock);
    if (e) kfree(n);
    return e ? e : (long)addr;
}

static int range_ok(uint64_t a, uint64_t size)
{
    return a % PAGE == 0 && a >= MAP_BASE && size && size <= MMIO_BASE - a;
}

/* SYS_MUNMAP: remove [addr, addr + size) (UNMAP_KEEP: only its pages). */
long vm_munmap(uint64_t addr, uint64_t size, uint64_t flags)
{
    proc_t *p = cur->proc;
    size = (size + PAGE - 1) & ~(PAGE - 1);
    if (!range_ok(addr, size) || (flags & ~UNMAP_KEEP)) return -EINVAL;
    spin_lock(&p->lock);
    long e = unmap_locked(p, addr, addr + size, flags & UNMAP_KEEP);
    spin_unlock(&p->lock);
    return e;
}

/* SYS_MPROTECT: new rights for the regions in [addr, addr + size); pages
 * already there are mapped again with them (every CPU then forgets the
 * old ones). Every page of the range must be in a region. */
long vm_mprotect(uint64_t addr, uint64_t size, uint64_t prot)
{
    proc_t *p = cur->proc;
    size = (size + PAGE - 1) & ~(PAGE - 1);
    if (!range_ok(addr, size) || (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC))) return -EINVAL;
    uint64_t b = addr + size;
    spin_lock(&p->lock);
    long e = 0;
    for (uint64_t at = addr; at < b && !e; ) {      /* all covered? */
        region_t *r = find(p, at);
        if (!r) e = -ENOMEM; else at = r->end;
    }
    if (!e && (split(p, addr) || split(p, b))) e = -ENOMEM;
    for (region_t *r = p->regions; r && !e; r = r->next) {
        if (r->end <= addr || r->start >= b) continue;
        r->prot = prot;
        for (uint64_t va = r->start; va < r->end; va += PAGE) {
            uint64_t fl, pa = arch_uaddr(p->as, va, 3) ? arch_vm_unmap(p->as, va, &fl) : 0;
            if (pa) vm_map(p->as, va, pa, vm_flags(prot));   /* (the same page: a table entry exists) */
        }
    }
    if (!e) tlb_shootdown(p);
    spin_unlock(&p->lock);
    return e;
}

/* The process has ended: its regions (their pages went with its address space). */
void vm_regions_free(proc_t *p)
{
    for (region_t *r = p->regions, *n; r; r = n) { n = r->next; kfree(r); }
    p->regions = 0;
}

/* ---- Futexes. */
typedef struct fwait { struct fwait *next; uint64_t key; int tid; } fwait_t;
#define NFUT 64
static struct { spinlock_t lock; fwait_t *head; } fut[NFUT];   /* (each taken alone) */
#define FUT(key) (&fut[((key) >> 2) % NFUT])

/* The kernel's address of the word at uaddr, made present first. 0: bad address. */
static volatile uint32_t *word(uint64_t uaddr)
{
    uint32_t v;
    if (uaddr % 4 || vm_copy(0, &v, cur->proc->as, (void *)uaddr, 4)) return 0;
    return (volatile uint32_t *)arch_uaddr(cur->proc->as, uaddr, 0);
}

static long futex_wait(uint64_t uaddr, uint32_t val, uint64_t ns)
{
    volatile uint32_t *w = word(uaddr);
    if (!w) return -EFAULT;
    fwait_t me = { 0, (uint64_t)w, cur->tid };
    spin_lock(&FUT(me.key)->lock);
    if (*w != val) { spin_unlock(&FUT(me.key)->lock); return -EAGAIN; }
    me.next = FUT(me.key)->head;
    FUT(me.key)->head = &me;
    spin_unlock(&FUT(me.key)->lock);
    /* (the page cannot go meanwhile without our process's own doing; if it
     * does, nobody wakes this key: the time limit or a signal ends the wait) */
    long r;
    do r = sleep_ns(ns ? ns : 1UL << 52); while (!ns && r == 0);   /* (0: no limit: ~52 days, again) */
    int queued = 0;
    spin_lock(&FUT(me.key)->lock);
    for (fwait_t **q = &FUT(me.key)->head; *q; q = &(*q)->next)
        if (*q == &me) { *q = me.next; queued = 1; break; }
    spin_unlock(&FUT(me.key)->lock);
    if (r < 0) return r;                         /* -EINTR, -EPIPE */
    return queued && r == 0 ? -ETIMEDOUT : 0;
}

/* Wake up to n waiters on the word at kernel address key. -> how many. */
static long futex_wake_key(uint64_t key, long n)
{
    int tids[16], k = 0;
    spin_lock(&FUT(key)->lock);
    for (fwait_t **q = &FUT(key)->head; *q && k < n && k < 16; ) {
        if ((*q)->key != key) { q = &(*q)->next; continue; }
        tids[k++] = (*q)->tid;
        *q = (*q)->next;
    }
    spin_unlock(&FUT(key)->lock);
    for (int i = 0; i < k; i++) thread_wake(tids[i]);
    return k;
}

long futex(int op, uint64_t uaddr, uint64_t val, uint64_t ns)
{
    if (op == FUTEX_WAIT) return futex_wait(uaddr, (uint32_t)val, ns);
    if (op != FUTEX_WAKE) return -EINVAL;
    volatile uint32_t *w = word(uaddr);
    if (!w) return -EFAULT;
    long n = (long)val, done = 0, k;
    do done += k = futex_wake_key((uint64_t)w, n - done); while (k == 16 && done < n);
    return done;
}

/* A thread that set a word with SYS_THREAD_SELF ends: 0 there, one waiter woken. */
void vm_thread_end(task_t *t)
{
    uint32_t zero = 0;
    if (!t->clear_tid || t->proc->exiting) return;
    if (vm_copy(t->proc->as, (void *)t->clear_tid, 0, &zero, 4)) return;
    volatile uint32_t *w = (volatile uint32_t *)arch_uaddr(t->proc->as, t->clear_tid, 0);
    if (w) futex_wake_key((uint64_t)w, 1);
}
