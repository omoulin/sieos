/*
 * mmu.c (arm64) - Page tables: the direct map of all RAM, the kernel's own
 * device mappings, and the address spaces. 4 KiB granule, 4 levels (L0 ->
 * L1 -> L2 -> L3), 512 entries each; a level-1 entry may map 1 GiB at
 * once, a level-2 entry 2 MiB.
 *
 * Two roots: TTBR1 (the kernel's half, boot.S's ttbr1_l0, the same for
 * every process) and TTBR0 (a process's half: its own L0 table, which is
 * what the core calls its "address space"). So, unlike x86-64, a new
 * address space copies nothing of the kernel's.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"
#include "arm64.h"

extern uint64_t ttbr1_l0[512];       /* boot.S: the kernel's half */
uint64_t kernel_as;                  /* an empty TTBR0 table: idle runs on it */

#define K_ATTR   (D_AF | D_ISH | D_PXN | D_UXN)            /* kernel data: not executable */
#define LVL_MASK(shift) ((1UL << (shift)) - 1)

static inline void dsb_ishst(void) { asm volatile("dsb ishst; isb" : : : "memory"); }

/* A table for the kernel's half. The first few come from a small pool in
 * .bss: kmap works before the page allocator does (platform.c maps the
 * UART first thing). */
static uint64_t pool[4][512] __attribute__((aligned(4096)));
static int npool;
static uint64_t table_page(void)
{
    if (npool < 4) return (uint64_t)pool[npool++] - KVMA + phys_base;
    uint64_t pa = page_alloc(1);
    if (!pa) panic("no memory for page tables\n");
    return pa;
}
/* The physical address a pool page or a direct-map page is at. */
static uint64_t *tvirt(uint64_t pa)
{
    uint64_t pool_pa = (uint64_t)pool - KVMA + phys_base;
    return pa >= pool_pa && pa < pool_pa + sizeof pool ? (uint64_t *)((uint64_t)pool + (pa - pool_pa)) : P2V(pa);
}
/* The next-level table of entry e, created if needed. */
static uint64_t *next_table(uint64_t *e)
{
    if (!(*e & D_VALID)) *e = table_page() | D_TABLE;
    return tvirt(*e & D_ADDR);
}

/* ---- The direct map: RAM range [s, e) at DMAP + pa, with the biggest
 * blocks that fit (boot.S already mapped one or two GiB: skipped). */
static void dmap_range(uint64_t s, uint64_t e)
{
    for (uint64_t pa = s & ~(PAGE - 1); pa < e; ) {
        uint64_t va = DMAP + pa;
        uint64_t *l1 = &next_table(&ttbr1_l0[va >> 39 & 511])[va >> 30 & 511];
        if ((*l1 & 3) == D_BLOCK) { pa = (pa | LVL_MASK(30)) + 1; continue; }
        if (!(pa & LVL_MASK(30)) && e - pa >= 1UL << 30 && !(*l1 & D_VALID)) {
            *l1 = pa | D_BLOCK | K_ATTR; pa += 1UL << 30; continue;
        }
        uint64_t *l2 = &next_table(l1)[va >> 21 & 511];
        if ((*l2 & 3) == D_BLOCK) { pa = (pa | LVL_MASK(21)) + 1; continue; }
        if (!(pa & LVL_MASK(21)) && e - pa >= 1UL << 21 && !(*l2 & D_VALID)) {
            *l2 = pa | D_BLOCK | K_ATTR; pa += 1UL << 21; continue;
        }
        next_table(l2)[va >> 12 & 511] = pa | D_PAGE | K_ATTR;
        pa += PAGE;
    }
}

/* Once the page allocator works (mem.c): map all of RAM, and give TTBR0
 * an empty table (boot.S's identity map is not needed any more by this
 * CPU; the others use it once, when they start). */
void arch_mem_init(uint64_t top)
{
    (void)top;
    for (int i = 0; i < boot_bi->nram; i++) dmap_range(boot_bi->ram[i].start, boot_bi->ram[i].end);
    dsb_ishst();
    if (!(kernel_as = page_alloc(1))) panic("no memory\n");
    arch_as_load(kernel_as);
}

/* ---- The kernel's own mappings (KDEV), one after the other: device
 * registers (VM_UC: device memory, never cached). Two calls in a row give
 * adjacent addresses. */
void *kmap(uint64_t pa, uint64_t size, uint64_t flags)
{
    static uint64_t next = KDEV;
    uint64_t va = next + pa % PAGE;
    for (uint64_t p = pa & ~(PAGE - 1); p < pa + size; p += PAGE, next += PAGE) {
        uint64_t *t = ttbr1_l0;
        for (int shift = 39; shift > 12; shift -= 9) t = next_table(&t[next >> shift & 511]);
        t[next >> 12 & 511] = p | D_PAGE | K_ATTR | D_ATTR(flags & VM_UC ? M_DEVICE : M_NORMAL);
    }
    dsb_ishst();
    return (void *)va;
}

/* SYS_MAP_PHYS never maps the interrupt controller (the kernel's own). */
/* ... but may map the firmware's frame buffer ("simple-framebuffer"
 * nodes: the Pi 5's), even where it lies in what the device tree calls RAM
 * (the kernel never allocates it: platform.c reserves it). */
int arch_phys_screen(uint64_t pa, uint64_t size)
{
    for (int i = 0; i < nscreen; i++)
        if (pa >= screen[i].start && pa + size <= screen[i].end) return 1;
    return 0;
}
int arch_phys_forbidden(uint64_t pa, uint64_t size)
{
    for (int i = 0; i < nforbid; i++)
        if (pa < forbid[i].end && pa + size > forbid[i].start) return 1;
    return 0;
}

/* ---- Address spaces: a process's own L0 table, for TTBR0. */
uint64_t vm_new(void) { return page_alloc(1); }

/* The level-3 entry for va (creating the tables on the way if asked). */
static uint64_t *walk(uint64_t as, uint64_t va, int create)
{
    uint64_t *t = P2V(as);
    for (int shift = 39; shift > 12; shift -= 9) {
        uint64_t *e = &t[va >> shift & 511];
        if (!(*e & D_VALID)) {
            uint64_t pa;
            if (!create || !(pa = page_alloc(1))) return 0;
            *e = pa | D_TABLE;
        }
        t = P2V(*e & D_ADDR);
    }
    return &t[va >> 12 & 511];
}

/* The core's VM_* flags <-> a user page's descriptor. Every user page is
 * "not global" (it belongs to the address space loaded now) and never
 * executable by the kernel. */
static uint64_t to_desc(uint64_t f)
{
    return D_PAGE | D_AF | D_ISH | D_NG | D_PXN | D_ATTR(f & VM_UC ? (f & VM_WC ? M_NC : M_DEVICE) : M_NORMAL)
         | (f & VM_U ? D_USER : 0) | (f & VM_W ? 0 : D_RO) | (f & VM_NX ? D_UXN : 0)
         | (f & VM_DEV ? D_SWDEV : 0) | (f & VM_DMA ? D_SWDMA : 0);
}
static uint64_t to_flags(uint64_t d)
{
    return (d & D_USER ? VM_U : 0) | (d & D_RO ? 0 : VM_W) | (d & D_UXN ? VM_NX : 0)
         | ((d >> 2 & 7) == M_DEVICE ? VM_UC : (d >> 2 & 7) == M_NC ? VM_UC | VM_WC : 0) | (d & D_SWDEV ? VM_DEV : 0) | (d & D_SWDMA ? VM_DMA : 0);
}

int vm_map(uint64_t as, uint64_t va, uint64_t pa, uint64_t flags)
{
    uint64_t *e = walk(as, va, 1);
    if (!e) return -ENOMEM;
    if (*e & D_VALID) return -EEXIST;
    *e = pa | to_desc(flags);
    asm volatile("dsb ishst" : : : "memory");   /* the table walker sees it before the program runs */
    return 0;
}

/* Remove a mapping: returns the page that was there (0: none) and its
 * flags. "tlbi vale1is" makes every CPU forget the translation (the
 * hardware broadcasts it): no shootdown interrupt is needed on arm64. */
uint64_t arch_vm_unmap(uint64_t as, uint64_t va, uint64_t *flags)
{
    uint64_t *e = walk(as, va, 0);
    if (!e || !(*e & D_VALID)) return 0;
    uint64_t old = *e;
    *e = 0;
    asm volatile("dsb ishst; tlbi vale1is, %0; dsb ish; isb" : : "r"(va >> 12) : "memory");
    *flags = to_flags(old);
    return old & D_ADDR;
}

/* Free an address space's tables; each user page goes to leaf(). */
static void free_table(uint64_t pa, int level, void (*leaf)(uint64_t, uint64_t))
{
    uint64_t *e = P2V(pa);
    for (int i = 0; i < 512; i++) {
        if (!(e[i] & D_VALID)) continue;
        if (level < 3) free_table(e[i] & D_ADDR, level + 1, leaf);
        else leaf(e[i] & D_ADDR, to_flags(e[i]));
    }
    page_free(pa, 1);
}
void arch_vm_free(uint64_t as, void (*leaf)(uint64_t pa, uint64_t flags)) { free_table(as, 0, leaf); }

/* Where the kernel finds user address va of address space as: 0 unless it
 * is mapped for user mode (and writable, if write == 1; write == 2: the
 * kernel itself writes, read-only pages included, to load a program). */
char *arch_uaddr(uint64_t as, uint64_t va, int write)
{
    uint64_t *e = va < USER_TOP ? walk(as, va, 0) : 0;
    if (!e || !(*e & D_VALID) || !(*e & D_USER) || (write == 1 && (*e & D_RO))) return 0;
    return (char *)P2V(*e & D_ADDR) + va % PAGE;
}

/* A program's code, just written through the data cache: the instruction
 * cache does not see it by itself (QEMU has no caches, a real Pi does).
 * Clean each data line to where both caches meet (PoU), then drop any old
 * instruction lines, on every CPU (inner shareable). CTR_EL0 gives the
 * smallest line sizes. */
void arch_sync_code(void *p, uint64_t n)
{
    uint64_t ctr, a = (uint64_t)p, end = a + n;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t dl = 4UL << (ctr >> 16 & 15), il = 4UL << (ctr & 15);
    for (uint64_t x = a & ~(dl - 1); x < end; x += dl) asm volatile("dc cvau, %0" : : "r"(x) : "memory");
    asm volatile("dsb ish" : : : "memory");
    for (uint64_t x = a & ~(il - 1); x < end; x += il) asm volatile("ic ivau, %0" : : "r"(x) : "memory");
    asm volatile("dsb ish; isb" : : : "memory");
}
