/*
 * mmu.c (x86-64) - Page tables: the direct map of all RAM, the kernel's own
 * device mappings, and the address spaces (4 levels: PML4 -> PDPT -> PD ->
 * PT, 512 entries each). The portable core (mem.c) decides what to map and
 * when to free; this file knows the format.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"
#include "x86.h"

extern uint64_t boot_pml4[512];     /* boot.S: the kernel's page table */
uint64_t kernel_as;                 /* ... and its physical address */

/* boot.S maps the first 4 GiB in the direct map. Map the rest of RAM the
 * same way: with 1 GiB pages if the CPU has them (CPUID 0x80000001,
 * EDX bit 26), else 2 MiB pages. Its tables come from page_alloc, which
 * hands out low pages first, already mapped. */
static void dmap_extend(uint64_t top)
{
    uint32_t r[4];
    cpuid(0x80000001, r);
    int big = r[3] >> 26 & 1;
    for (uint64_t gb = 4; gb < (top + (1UL << 30) - 1) >> 30; gb++) {
        uint64_t *l4 = &boot_pml4[256 + gb / 512];          /* 512 GiB per PML4 entry */
        if (!(*l4 & PTE_P)) *l4 = page_alloc(1) | PTE_P | PTE_W;
        uint64_t *l3 = (uint64_t *)P2V(*l4 & PTE_ADDR) + gb % 512;
        if (big) { *l3 = gb << 30 | PTE_P | PTE_W | PTE_PS | PTE_G; continue; }
        uint64_t pd = page_alloc(1);
        for (int i = 0; i < 512; i++)
            ((uint64_t *)P2V(pd))[i] = ((gb << 30) + ((uint64_t)i << 21)) | PTE_P | PTE_W | PTE_PS | PTE_G;
        *l3 = pd | PTE_P | PTE_W;
    }
}

/* Once the page allocator works (mem.c): map all RAM below top, prepare
 * the KDEV area, drop boot.S's identity map (so a null pointer in the
 * kernel faults), then let the CPU keep the kernel's "global" translations
 * in its TLB across address-space switches (CR4.PGE). */
void arch_mem_init(uint64_t top)
{
    dmap_extend(top);
    boot_pml4[510] = page_alloc(1) | PTE_P | PTE_W;   /* for kmap_dev, before any address space copies the upper half */
    kernel_as = (uint64_t)boot_pml4 - KVMA;
    boot_pml4[0] = 0;
    write_cr3(kernel_as);
    uint64_t cr4;
    asm volatile("mov %%cr4, %0; or $0x80, %0; mov %0, %%cr4" : "=r"(cr4));
}

/* ---- The kernel's own mappings in the KDEV area, one after the other:
 * the APICs' registers, uncached (kmap_dev: PCD and PWT; the direct map is
 * cacheable, and device registers must never be cached, or writes are
 * delayed and reads stale), and each CPU's page followed by the shared I/O
 * bitmap (cpu.c). Two calls in a row give adjacent addresses. */
void *kmap(uint64_t pa, uint64_t size, uint64_t flags)
{
    static uint64_t next = KDEV;
    uint64_t va = next + pa % PAGE;
    for (uint64_t p = pa & ~(PAGE - 1); p < pa + size; p += PAGE, next += PAGE) {
        uint64_t *t = boot_pml4;
        for (int shift = 39; shift > 12; shift -= 9) {
            uint64_t *e = &t[next >> shift & 511];
            if (!(*e & PTE_P)) *e = page_alloc(1) | PTE_P | PTE_W;
            t = P2V(*e & PTE_ADDR);
        }
        t[next >> 12 & 511] = p | PTE_P | PTE_W | PTE_G | flags;
    }
    return (void *)va;
}

/* SYS_MAP_PHYS never maps the interrupt controllers (the kernel's own). */
int arch_phys_forbidden(uint64_t pa, uint64_t size) { return pa < 0xFF000000 && pa + size > 0xFEC00000; }
/* The firmware's screen (a UEFI loader's frame buffer) may sit in what the
 * memory map calls RAM: only that range may then be mapped by the desktop. */
int arch_phys_screen(uint64_t pa, uint64_t size)
{
    return x86_fb.pa && pa >= x86_fb.pa && pa + size <= x86_fb.pa + ((x86_fb.size + PAGE - 1) & ~(PAGE - 1));
}

/* ---- Address spaces. An address space is a PML4 page: its lower half
 * (entries 0-255) is the process's own; its upper half points at the
 * kernel's tables, shared by all. */
uint64_t vm_new(void)
{
    uint64_t as = page_alloc(1);
    if (as) memcpy((uint64_t *)P2V(as) + 256, boot_pml4 + 256, 256 * 8);
    return as;
}

/* The page table entry for va (creating the tables on the way if asked). */
static uint64_t *walk(uint64_t as, uint64_t va, int create)
{
    uint64_t *t = P2V(as);
    for (int shift = 39; shift > 12; shift -= 9) {
        uint64_t *e = &t[va >> shift & 511];
        if (!(*e & PTE_P)) {
            uint64_t pa;
            if (!create || !(pa = page_alloc(1))) return 0;
            *e = pa | PTE_P | PTE_W | PTE_U;    /* the last level decides the rights */
        }
        t = P2V(*e & PTE_ADDR);
    }
    return &t[va >> 12 & 511];
}

int vm_map(uint64_t as, uint64_t va, uint64_t pa, uint64_t flags)
{
    uint64_t *e = walk(as, va, 1);
    if (!e) return -ENOMEM;
    if (*e & PTE_P) return -EEXIST;
    if (flags & PTE_WCX) flags &= ~PTE_PCD;     /* PAT entry 1 (PWT alone): write-combining (cpu.c) */
    *e = pa | flags | PTE_P;
    return 0;
}

/* Remove a mapping: returns the page that was there (0: none) and its
 * flags. This CPU forgets the old translation at once (invlpg); for the
 * others, see tlb_shootdown (smp.c). */
uint64_t arch_vm_unmap(uint64_t as, uint64_t va, uint64_t *flags)
{
    uint64_t *e = walk(as, va, 0);
    if (!e || !(*e & PTE_P)) return 0;
    uint64_t old = *e;
    *e = 0;
    asm volatile("invlpg (%0)" : : "r"(va) : "memory");
    *flags = old & ~PTE_ADDR;
    return old & PTE_ADDR;
}

/* Free an address space's tables; each user page goes to leaf() (level 4 = PML4). */
static void free_table(uint64_t pa, int level, void (*leaf)(uint64_t, uint64_t))
{
    uint64_t *e = P2V(pa);
    for (int i = 0; i < (level == 4 ? 256 : 512); i++) {
        if (!(e[i] & PTE_P)) continue;
        if (level > 1) free_table(e[i] & PTE_ADDR, level - 1, leaf);
        else leaf(e[i] & PTE_ADDR, e[i] & ~PTE_ADDR);
    }
    page_free(pa, 1);
}
void arch_vm_free(uint64_t as, void (*leaf)(uint64_t pa, uint64_t flags)) { free_table(as, 4, leaf); }

/* Where the kernel finds user address va of address space as: 0 unless it
 * is mapped for user mode (and writable, if write == 1; write == 2: the
 * kernel itself writes, read-only pages included, to load a program). */
char *arch_uaddr(uint64_t as, uint64_t va, int write)
{
    uint64_t *e = va < USER_TOP ? walk(as, va, 0) : 0;
    if (!e || !(*e & PTE_P) || !(*e & PTE_U) || (write == 1 && !(*e & PTE_W))) return 0;
    return (char *)P2V(*e & PTE_ADDR) + va % PAGE;
}

/* x86 keeps its instruction cache coherent with memory by itself. */
void arch_sync_code(void *p, uint64_t n) { (void)p; (void)n; }
