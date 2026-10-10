/*
 * arch_defs.h (x86-64) - The types, constants and tiny inline helpers of
 * this processor, which the portable core (the .c files of kernel/) uses through the
 * names it expects (see kernel/arch.h). Included first by kernel.h.
 *
 * Virtual memory layout. Each process has its own lower half; the upper
 * half is the same everywhere and reachable only in kernel mode.
 *   0x0000000000400000             the user program (code, data, then heap)
 *   0x0000600000000000             device memory and DMA buffers of drivers
 *   0x00007FFFFFFFF000             top of the main thread's stack (grows down)
 *   0xFFFF800000000000 (+ all RAM) all physical memory: physical address p
 *                                  is at P2V(p) ("direct map")
 *   0xFFFFFF0000000000             the kernel's own device registers (APICs), uncached
 *   0xFFFFFFFF80000000             the kernel itself
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

#define ARCH_NAME   "x86_64"
#define ELF_MACHINE 62                /* EM_X86_64: the programs SIEOS runs */

#define KVMA      0xFFFFFFFF80000000UL
#define DMAP      0xFFFF800000000000UL
#define KDEV      0xFFFFFF0000000000UL  /* PML4 entry 510 */
#define P2V(p)    ((void *)((uint64_t)(p) + DMAP))
#define V2P(v)    ((uint64_t)(v) - DMAP)
#define PAGE      4096UL
#define MMIO_BASE 0x0000600000000000UL
#define TRAMP     0x8000UL            /* where the other CPUs start (smp.c, ap.S) */
#define STACK_TOP 0x00007FFFFFFFF000UL
#define USER_TOP  0x0000800000000000UL
#define MAXCPU    256                 /* xAPIC ids are 8 bits */
#define MAXIRQ    64                  /* interrupt lines (IOAPIC inputs, "GSIs") */
#define ARCH_IRQ_RESERVED(irq) ((irq) == 2)   /* IRQ 2: where the old PICs were chained */

/* Interrupt vectors: 0-31 CPU exceptions, 32-47 the (masked) 8259 PICs,
 * then ours, then device lines from VEC_IRQ0 on. */
#define VEC_TIMER    48               /* each CPU's local APIC timer */
#define VEC_RESCHED  49               /* "there is work, or your timer must change" */
#define VEC_TLB      50               /* "flush your TLB" (shootdown) */
#define VEC_IRQ0     64               /* interrupt line n arrives as vector 64 + n */
#define VEC_SPURIOUS 255

/* Page table entry bits. */
#define PTE_P     1UL                 /* present */
#define PTE_W     2UL                 /* writable */
#define PTE_U     4UL                 /* user mode may use it */
#define PTE_PWT   0x08UL              /* write-through ... */
#define PTE_PCD   0x10UL              /* ... and no caching: for device registers */
#define PTE_PS    0x80UL              /* a 2 MiB or 1 GiB page */
#define PTE_G     0x100UL             /* global: kept in the TLB across CR3 changes */
#define PTE_DEV   (1UL << 9)          /* (ours) device memory: never freed */
#define PTE_DMA   (1UL << 10)         /* (ours) a DMA buffer: quarantined when its driver dies */
#define PTE_NX    (1UL << 63)         /* not executable */
#define PTE_ADDR  0x000FFFFFFFFFF000UL

/* The core's mapping flags (vm_map): here simply the PTE bits themselves,
 * so nothing is translated. Another processor maps them to its own format. */
#define VM_W      PTE_W               /* writable */
#define VM_U      PTE_U               /* user mode may use it */
#define VM_NX     PTE_NX              /* not executable */
#define VM_UC     (PTE_PCD | PTE_PWT) /* uncached: device registers */
#define VM_DEV    PTE_DEV             /* device memory: never freed */
#define VM_DMA    PTE_DMA             /* a DMA buffer: quarantined when its driver dies */
#define PTE_WCX   (1UL << 11)         /* (ours) asked for write-combining: see vm_map */
#define VM_WC     PTE_WCX             /* with VM_UC: write-combining (frame buffers): vm_map keeps
                                         only PWT, which the PAT (cpu.c) turns into "WC" */

/* The registers of an interrupted program, as entry.S saves them. */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vec, err;                          /* pushed by the entry stub */
    uint64_t rip, cs, rflags, rsp, ss;          /* pushed by the CPU */
} regs_t;

/* A system call's number, arguments and result in regs_t (SYSCALL: rax,
 * then rdi, rsi, rdx, r10, r8, r9; result in rax). */
#define SC_NR(r)     ((r)->rax)
#define SC_ARG0(r)   ((r)->rdi)
#define SC_ARG1(r)   ((r)->rsi)
#define SC_ARG2(r)   ((r)->rdx)
#define SC_ARG3(r)   ((r)->r10)
#define SC_ARG4(r)   ((r)->r8)
#define SC_ARG5(r)   ((r)->r9)
#define SC_RET(r)    ((r)->rax)

/* The TSS: the stack the CPU switches to on an interrupt in user mode
 * (rsp0), and where the I/O permission bitmap is (iomap: an offset from
 * the TSS; cpu.c places one bitmap, shared by all CPUs, right after it). */
typedef struct __attribute__((packed)) {
    uint32_t r0;
    uint64_t rsp0, rsp1, rsp2, r1, ist[7], r2;
    uint16_t r3, iomap;
} tss_t;

/* This processor's part of cpu_t (kernel.h puts it first). entry.S uses
 * the first three fields at fixed offsets from GS: in kernel mode GS's
 * base points at the running CPU's cpu_t (SWAPGS exchanges it with the
 * user's on entry and exit). */
typedef struct {
    struct cpu *self;           /* %gs:0  */
    uint64_t kstack_top;        /* %gs:8  the running thread's kernel stack */
    uint64_t user_rsp;          /* %gs:16 syscall_entry's scratch */
    int apic_id;
    int ts;                     /* CR0.TS is set: no thread's registers are loaded (fpu.c) */
    uint64_t fs_loaded;         /* the FS base this CPU holds now */
    uint64_t gdt[7];
    tss_t *tss;                 /* at the end of this cpu_t's page (cpu.c) */
} arch_cpu_t;

static inline struct cpu *this_cpu(void)   /* volatile: re-read after a switch, the thread may have moved */
{
    struct cpu *c;
    asm volatile("mov %%gs:0, %0" : "=r"(c));
    return c;
}

/* Port I/O, control registers, model-specific registers, the time stamp counter. */
static inline void outb(uint16_t p, uint8_t v)  { asm volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { asm volatile("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void write_cr3(uint64_t v) { asm volatile("mov %0, %%cr3" : : "r"(v) : "memory"); }
static inline void wrmsr(uint32_t msr, uint64_t v)
{
    asm volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline uint64_t rdtsc(void) { uint32_t lo, hi; asm volatile("rdtsc" : "=a"(lo), "=d"(hi)); return (uint64_t)hi << 32 | lo; }
static inline void cpuid(uint32_t leaf, uint32_t r[4])
{
    asm volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}

/* ---- What the core calls, inline (see kernel/arch.h for the contract). */
static inline uint64_t ticks(void) { return rdtsc(); }         /* the fast clock: the TSC */
static inline void arch_relax(void) { asm volatile("pause"); } /* in a spin loop */
/* DMA memory just zeroed: x86 devices see the caches, nothing to do. */
static inline void arch_dma_clean(uint64_t pa, uint64_t len) { (void)pa; (void)len; }
static inline void arch_wait_irq(void) { asm volatile("sti; hlt; cli"); }   /* idle: sleep until an interrupt */
static inline __attribute__((noreturn)) void arch_halt_forever(void) { for (;;) asm volatile("cli; hlt"); }
static inline void arch_as_load(uint64_t as) { write_cr3(as); }              /* switch address spaces */
static inline void arch_tls_switch(arch_cpu_t *a, uint64_t base)            /* a thread's FS base */
{
    if (base != a->fs_loaded) wrmsr(0xC0000100, a->fs_loaded = base);
}
/* A CPU waiting for the kernel lock still answers TLB shootdowns (smp.c):
 * the holder may be waiting for exactly that. */
#define arch_lock_wait(c) do { if ((c)->tlb_req) tlb_ack(c); arch_relax(); } while (0)
