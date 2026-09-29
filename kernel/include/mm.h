/*
 * mm.h - Physical memory, virtual memory and kernel heap.
 */
#ifndef SIEOS_MM_H
#define SIEOS_MM_H

#include "kernel.h"

/* Page table entry flags */
#define PTE_P    0x001
#define PTE_W    0x002
#define PTE_U    0x004
#define PTE_PS   0x080
#define PTE_NX   (1UL << 63)       /* no-execute (only when EFER.NXE is on: use pte_nx) */
#define PTE_DEVICE 0x200            /* software bit: MMIO/framebuffer, not owned RAM */
#define PTE_COW    0x400            /* software bit: private page shared after fork */
#define PTE_SHARED 0x800            /* software bit: MAP_SHARED memory, stays shared */
#define PTE_WANTW  (1UL << 52)      /* software bit: logically writable (COW pages have W clear) */
#define PTE_NOACC  (1UL << 53)      /* software bit: PROT_NONE (U cleared) */
#define PTE_ADDR  0x000FFFFFFFFFF000UL
#define PTE_FLAGS (0xFFFUL | PTE_NX | PTE_WANTW | PTE_NOACC)   /* bits kept by translate/copy */

/* User address space layout */
#define USER_BASE       0x0000000000400000UL
#define USER_STACK_TOP  0x00007FFFFFFFF000UL
#define USER_STACK_SIZE (8UL * 1024 * 1024)     /* reserved, demand-paged */
#define USER_STACK_INIT (128 * 1024)            /* mapped at exec (arguments, environment) */
#define USER_MMAP_TOP   (USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE)
#define USER_LIMIT      0x0000800000000000UL

/* pmm.c */
void     pmm_init(uint64_t mb_info_phys);
uint64_t pmm_alloc(void);                 /* zeroed 4K frame, 0 on failure */
uint64_t pmm_alloc_contig(size_t npages); /* zeroed, physically contiguous */
void     pmm_free(uint64_t pa);
void     pmm_free_contig(uint64_t pa, size_t npages);
void     pmm_ref(uint64_t pa);                /* another address space maps this frame */
void     pmm_unref(uint64_t pa);              /* frees it when nobody maps it any more */
int      pmm_refcount(uint64_t pa);
uint64_t pmm_free_pages(void);
uint64_t pmm_total_pages(void);
uint64_t pmm_kernel_pages(void);

/* vmm.c */
extern uint64_t kernel_pml4_phys;
void     vmm_init(void);
void     vmm_identity_map(bool on);
void     vmm_set_uncached(uint64_t pa);
/* Boot: add RAM [start, end) above 4 GiB to the direct map (2 MiB pages; tables from alloc). */
void     vmm_direct_map(uint64_t start, uint64_t end, uint64_t (*alloc)(void));
/* Device memory (a PCI BAR) at pa, uncached, in the kernel's address space:
 * through the direct map below 4 GiB, else in the MMIO window.  NULL if the
 * window is full. */
void    *mmio_map(uint64_t pa, size_t size);
/* The same, write-combining (framebuffers); uncached without PAT. */
void    *mmio_map_wc(uint64_t pa, size_t size);
#define PTE_WC   0x008                     /* PWT: write-combining once pat_wc (PAT entry 1) */
uint64_t vmm_new_space(void);
int      vmm_map(uint64_t pml4, uint64_t va, uint64_t pa, uint64_t flags);
uint64_t vmm_translate(uint64_t pml4, uint64_t va, uint64_t *flags);
int      vmm_alloc_range(uint64_t pml4, uint64_t start, uint64_t end, uint64_t flags);
void     vmm_widen(uint64_t pml4, uint64_t va, bool write, bool exec);
void     vmm_free_space(uint64_t pml4);
uint64_t vmm_user_pages(uint64_t pml4);
bool     user_range_ok(uint64_t pml4, uint64_t va, uint64_t len, bool write);
uint64_t *vmm_pte(uint64_t pml4, uint64_t va, bool create);   /* NULL if no table */
long     user_strlen(uint64_t pml4, const char *s, size_t max);

/* kmalloc.c */
void  *kmalloc(size_t size);
void  *kzalloc(size_t size);
void   kfree(void *p);
size_t kheap_used(void);

#endif
