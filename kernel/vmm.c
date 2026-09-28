/*
 * vmm.c - 4-level paging and per-process address spaces.
 *
 * The upper half (PML4 entries 256..511) is shared by every address
 * space and holds the direct map and the kernel image.  The lower half
 * belongs to user space and is built from 4K pages.
 */
#include "mm.h"
#include "vm.h"

extern uint64_t boot_pml4[];
extern uint64_t boot_pdpt_low[];
uint64_t kernel_pml4_phys;

#define PML4_IDX(va) (((va) >> 39) & 0x1FF)
#define PDPT_IDX(va) (((va) >> 30) & 0x1FF)
#define PD_IDX(va)   (((va) >> 21) & 0x1FF)
#define PT_IDX(va)   (((va) >> 12) & 0x1FF)

static inline uint64_t *table(uint64_t pa) { return (uint64_t *)P2V(pa & PTE_ADDR); }

void vmm_init(void)
{
    kernel_pml4_phys = (uint64_t)boot_pml4;   /* .boot.bss is linked at its physical address */
    vmm_identity_map(false);                  /* drop the boot identity map */
}

/* The low identity map is only needed while application processors start. */
void vmm_identity_map(bool on)
{
    uint64_t *pml4 = table(kernel_pml4_phys);
    pml4[0] = on ? ((uint64_t)boot_pdpt_low | PTE_P | PTE_W) : 0;
    write_cr3(kernel_pml4_phys);
}

/* Make the 2 MiB direct-map page containing pa uncacheable (for MMIO). */
void vmm_set_uncached(uint64_t pa)
{
    uint64_t va = (uint64_t)P2V(pa);
    uint64_t *l4 = table(kernel_pml4_phys);
    uint64_t *l3 = table(l4[PML4_IDX(va)]);
    uint64_t *l2 = table(l3[PDPT_IDX(va)]);
    l2[PD_IDX(va)] |= 0x18;                   /* PCD | PWT */
    invlpg(va);
}

/* Return a pointer to the PTE for va, allocating intermediate tables if asked. */
uint64_t *vmm_pte(uint64_t pml4, uint64_t va, bool create);
static uint64_t *walk(uint64_t pml4, uint64_t va, bool create)
{
    uint64_t *t = table(pml4);
    int idx[3] = { PML4_IDX(va), PDPT_IDX(va), PD_IDX(va) };
    for (int level = 0; level < 3; level++) {
        uint64_t e = t[idx[level]];
        if (!(e & PTE_P)) {
            if (!create)
                return NULL;
            uint64_t pa = pmm_alloc();
            if (!pa)
                return NULL;
            t[idx[level]] = pa | PTE_P | PTE_W | PTE_U;
            e = t[idx[level]];
        } else if (e & PTE_PS) {
            return NULL;                      /* huge pages only in kernel half */
        }
        t = table(e);
    }
    return &t[PT_IDX(va)];
}

uint64_t *vmm_pte(uint64_t pml4, uint64_t va, bool create)
{
    return walk(pml4, va, create);
}

uint64_t vmm_new_space(void)
{
    uint64_t pa = pmm_alloc();
    if (!pa)
        return 0;
    uint64_t *n = table(pa), *k = table(kernel_pml4_phys);
    for (int i = 256; i < 512; i++)
        n[i] = k[i];
    return pa;
}

int vmm_map(uint64_t pml4, uint64_t va, uint64_t pa, uint64_t flags)
{
    uint64_t *pte = walk(pml4, va, true);
    if (!pte)
        return -ENOMEM;
    *pte = (pa & PTE_ADDR) | flags | PTE_P;
    if (read_cr3() == pml4)
        invlpg(va);
    return 0;
}

uint64_t vmm_translate(uint64_t pml4, uint64_t va, uint64_t *flags)
{
    uint64_t *pte = walk(pml4, va, false);
    if (!pte || !(*pte & PTE_P))
        return 0;
    if (flags)
        *flags = *pte & PTE_FLAGS;
    return (*pte & PTE_ADDR) | (va & 0xFFF);
}

/* Widen the permissions of an existing user page: add W, drop NX as asked. */
void vmm_widen(uint64_t pml4, uint64_t va, bool write, bool exec)
{
    uint64_t *pte = walk(pml4, va, false);
    if (!pte || !(*pte & PTE_P))
        return;
    if (write)
        *pte |= PTE_W;
    if (exec)
        *pte &= ~PTE_NX;
    if (read_cr3() == pml4)
        invlpg(va);
}

/* Map fresh zeroed pages over [start, end) wherever nothing is mapped yet. */
int vmm_alloc_range(uint64_t pml4, uint64_t start, uint64_t end, uint64_t flags)
{
    for (uint64_t va = PAGE_ALIGN_DOWN(start); va < end; va += PAGE_SIZE) {
        if (vmm_translate(pml4, va, NULL))
            continue;
        uint64_t pa = pmm_alloc();
        if (!pa)
            return -ENOMEM;
        if (vmm_map(pml4, va, pa, flags) < 0) {
            pmm_free(pa);
            return -ENOMEM;
        }
    }
    return 0;
}

void vmm_free_space(uint64_t pml4)
{
    uint64_t *l4 = table(pml4);
    for (int i = 0; i < 256; i++) {
        if (!(l4[i] & PTE_P))
            continue;
        uint64_t *l3 = table(l4[i]);
        for (int j = 0; j < 512; j++) {
            if (!(l3[j] & PTE_P))
                continue;
            uint64_t *l2 = table(l3[j]);
            for (int k = 0; k < 512; k++) {
                if (!(l2[k] & PTE_P))
                    continue;
                uint64_t *l1 = table(l2[k]);
                for (int m = 0; m < 512; m++)
                    if ((l1[m] & PTE_P) && !(l1[m] & PTE_DEVICE))
                        pmm_unref(l1[m] & PTE_ADDR);
                pmm_free(l2[k] & PTE_ADDR);
            }
            pmm_free(l3[j] & PTE_ADDR);
        }
        pmm_free(l4[i] & PTE_ADDR);
    }
    pmm_free(pml4);
}

uint64_t vmm_user_pages(uint64_t pml4)
{
    uint64_t n = 0;
    uint64_t *l4 = table(pml4);
    for (int i = 0; i < 256; i++) {
        if (!(l4[i] & PTE_P))
            continue;
        uint64_t *l3 = table(l4[i]);
        for (int j = 0; j < 512; j++) {
            if (!(l3[j] & PTE_P))
                continue;
            uint64_t *l2 = table(l3[j]);
            for (int k = 0; k < 512; k++) {
                if (!(l2[k] & PTE_P))
                    continue;
                uint64_t *l1 = table(l2[k]);
                for (int m = 0; m < 512; m++)
                    if ((l1[m] & PTE_P) && !(l1[m] & PTE_DEVICE))
                        n++;
            }
        }
    }
    return n;
}

bool user_range_ok(uint64_t pml4, uint64_t va, uint64_t len, bool write)
{
    if (va >= USER_LIMIT || len > USER_LIMIT || va + len > USER_LIMIT || va + len < va)
        return false;
    for (uint64_t p = PAGE_ALIGN_DOWN(va); p < va + len; p += PAGE_SIZE) {
        uint64_t fl;
        bool present = vmm_translate(pml4, p, &fl) != 0;
        if (present && (fl & PTE_U) && (!write || (fl & PTE_W)))
            continue;
        /* a demand-zero or copy-on-write page the kernel is about to touch */
        if (read_cr3() != pml4 || !vm_fault(p, (present ? 1 : 0) | (write ? 2 : 0), false))
            return false;
        if (!vmm_translate(pml4, p, &fl) || !(fl & PTE_U) || (write && !(fl & PTE_W)))
            return false;
    }
    return true;
}

/* Length of a user string, or -EFAULT if it is not fully mapped / too long. */
long user_strlen(uint64_t pml4, const char *s, size_t max)
{
    uint64_t va = (uint64_t)s;
    for (size_t n = 0; n < max; n++, va++) {
        if (n == 0 || (va & 0xFFF) == 0)
            if (!user_range_ok(pml4, va, 1, false))
                return -EFAULT;
        if (*(const char *)va == 0)
            return n;
    }
    return -ENAMETOOLONG;
}
