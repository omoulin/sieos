/*
 * arch_defs.h (arm64) - The types, constants and tiny inline helpers of
 * AArch64 processors (ARMv8-A: QEMU's "virt" machine, the Raspberry Pi 4
 * and 5), which the portable core uses through the names it expects (see
 * kernel/arch.h). Included first by kernel.h.
 *
 * Privilege: the kernel runs at EL1, programs at EL0. Two page tables are
 * in use at once: TTBR0 translates the lower half (a process's own
 * addresses, switched with the process) and TTBR1 the upper half (the
 * kernel's, the same for everyone; the processor refuses it to EL0).
 *
 * Virtual memory layout (48-bit addresses, the same as on x86-64):
 *   0x0000000000400000             the user program (code, data, then heap)
 *   0x0000600000000000             device memory and DMA buffers of drivers
 *   0x00007FFFFFFFF000             top of the main thread's stack (grows down)
 *   0xFFFF800000000000 (+ RAM)     all RAM: physical address p is at P2V(p)
 *   0xFFFFFF0000000000             the kernel's own device registers (GIC, UART)
 *   0xFFFFFFFF80000000             the kernel image (wherever the boot loader
 *                                  put it physically: boot.S maps it here)
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

#define ARCH_NAME   "arm64"
#define ELF_MACHINE 183               /* EM_AARCH64: the programs SIEOS runs */

#define KVMA      0xFFFFFFFF80000000UL
#define DMAP      0xFFFF800000000000UL
#define KDEV      0xFFFFFF0000000000UL
#define P2V(p)    ((void *)((uint64_t)(p) + DMAP))
#define V2P(v)    ((uint64_t)(v) - DMAP)
#define PAGE      4096UL
#define MMIO_BASE 0x0000600000000000UL
#define STACK_TOP 0x00007FFFFFFFF000UL
#define USER_TOP  0x0000800000000000UL
#define MAXCPU    8                   /* GICv2 delivers to at most 8 processors */
#define MAXIRQ    256                 /* the GIC's shared lines ("SPI" 0-255 = interrupt ids 32-287) */
#define ARCH_KSTACK (2 * PAGE)        /* each thread's kernel stack (the frames sicc makes for
                                         AArch64 are bigger than x86-64's; make STACKCHECK=1
                                         measures the deepest use) */
#define ARCH_IRQ_RESERVED(irq) 0      /* (x86-64 reserves IRQ 2) */
/* random.c's timing samples: the generic counter runs at tens of MHz, too
 * coarse for jitter; the performance monitor's cycle counter (PMCCNTR_EL0,
 * started by cpu.c) counts the processor's cycles. */
#define ARCH_JITTER_LOOP 256
static inline uint64_t arch_cycles(void) { uint64_t v; asm volatile("isb; mrs %0, s3_3_c9_c13_0" : "=r"(v)); return v; }
#define ARCH_FINE_TICKS() arch_cycles()

/* Interrupt ids of the GIC: 0-15 "software generated" (between CPUs),
 * 16-31 per CPU (its timers), 32+ shared lines from devices. */
#define SGI_KICK   1                  /* "there is work, or your timer must change" */
#define SGI_HALT   2                  /* "stop" (power off, panic) */
#define PPI_VTIMER 27                 /* each CPU's virtual timer (the generic timer) */
#define GIC_SPI0   32                 /* SPI n is interrupt id 32 + n */

/* Page table entry ("descriptor") bits, 4 KiB granule. */
#define D_VALID   1UL
#define D_TABLE   3UL                 /* levels 0-2: points to the next table */
#define D_PAGE    3UL                 /* level 3: a 4 KiB page */
#define D_BLOCK   1UL                 /* levels 1-2: a 1 GiB or 2 MiB block */
#define D_ATTR(n) ((uint64_t)(n) << 2)/* memory type, an index into MAIR_EL1 (boot.S): */
#define M_NORMAL  0                   /*   ordinary memory, cached */
#define M_DEVICE  1                   /*   device registers: never cached, never reordered */
#define M_NC      2                   /*   normal memory, uncached (writes may be gathered) */
#define D_USER    (1UL << 6)          /* AP[1]: EL0 may use it */
#define D_RO      (1UL << 7)          /* AP[2]: read-only */
#define D_ISH     (3UL << 8)          /* shareable between the CPUs */
#define D_AF      (1UL << 10)         /* "accessed" (we set it: no access-flag faults) */
#define D_NG      (1UL << 11)         /* not global: belongs to the current address space */
#define D_PXN     (1UL << 53)         /* EL1 may not execute it */
#define D_UXN     (1UL << 54)         /* EL0 may not execute it */
#define D_SWDEV   (1UL << 55)         /* (ours, software bits) device memory: never freed */
#define D_SWDMA   (1UL << 56)         /* (ours) a DMA buffer: quarantined when its driver dies */
#define D_ADDR    0x0000FFFFFFFFF000UL

/* The core's mapping flags (vm_map): ours, translated by mmu.c to the
 * descriptor format, and given back the same way by arch_vm_unmap. */
#define VM_W      (1UL << 1)          /* writable */
#define VM_U      (1UL << 2)          /* user mode may use it */
#define VM_UC     (1UL << 4)          /* uncached: device registers */
#define VM_DEV    (1UL << 9)          /* device memory: never freed */
#define VM_DMA    (1UL << 10)         /* a DMA buffer: quarantined when its driver dies */
#define VM_WC     (1UL << 5)          /* with VM_UC: normal memory, uncached: writes gathered (frame buffers) */
#define VM_NX     (1UL << 63)         /* not executable */

/* The registers of an interrupted program, as entry.S saves them. */
typedef struct {
    uint64_t x[31];                   /* x0-x30 (x30: the link register) */
    uint64_t sp;                      /* the program's stack pointer (SP_EL0) */
    uint64_t pc;                      /* where it continues (ELR_EL1) */
    uint64_t pstate;                  /* its flags and mode (SPSR_EL1) */
} regs_t;

/* A system call's number, arguments and result in regs_t ("svc #0":
 * number in x8, arguments x0-x5, result in x0). */
#define SC_NR(r)     ((r)->x[8])
#define SC_ARG0(r)   ((r)->x[0])
#define SC_ARG1(r)   ((r)->x[1])
#define SC_ARG2(r)   ((r)->x[2])
#define SC_ARG3(r)   ((r)->x[3])
#define SC_ARG4(r)   ((r)->x[4])
#define SC_ARG5(r)   ((r)->x[5])
#define SC_RET(r)    ((r)->x[0])

/* This processor's part of cpu_t (kernel.h puts it first). TPIDR_EL1 holds
 * the running CPU's cpu_t: this_cpu() reads it. */
typedef struct {
    uint64_t mpidr;             /* its affinity (MPIDR_EL1): what PSCI and the device tree call it */
    uint8_t gic_mask;           /* its bit for the GIC (interrupt targets, software interrupts) */
    int fpen;                   /* EL0 may use the FP/SIMD registers now (fpu.c) */
    uint64_t tls_loaded;        /* the TPIDR_EL0 this CPU holds now */
} arch_cpu_t;

static inline struct cpu *this_cpu(void)   /* volatile: re-read after a switch, the thread may have moved */
{
    struct cpu *c;
    asm volatile("mrs %0, tpidr_el1" : "=r"(c));
    return c;
}

/* ---- What the core calls, inline (see kernel/arch.h for the contract). */
static inline uint64_t ticks(void)          /* the fast clock: the generic timer's virtual count */
{
    uint64_t v;
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
static inline void arch_relax(void) { asm volatile("yield"); }   /* in a spin loop */
/* DMA memory just zeroed (through the cached direct map): write those
 * lines to memory and drop them, for devices that do not see the caches
 * (the Pis' SD controllers and firmware). "dc civac" per cache line (the
 * smallest line size is in CTR_EL0); harmless where devices are coherent. */
static inline void arch_dma_clean(uint64_t pa, uint64_t len)
{
    uint64_t ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4UL << (ctr >> 16 & 15);
    for (uint64_t a = (uint64_t)P2V(pa) & ~(line - 1); a < (uint64_t)P2V(pa) + len; a += line)
        asm volatile("dc civac, %0" : : "r"(a) : "memory");
    asm volatile("dsb sy" : : : "memory");
}
/* Idle: sleep until an interrupt. wfi wakes on a pending interrupt even
 * while they are masked, so masking stays on until it returns: then a
 * short unmasked window lets the interrupt in. No wake-up can be missed. */
static inline void arch_wait_irq(void)
{
    asm volatile("dsb sy; wfi; msr daifclr, #2; isb; msr daifset, #2" : : : "memory");
}
void diag_halt(void);       /* diag.c: on a Pi before any program ran, show it */
static inline __attribute__((noreturn)) void arch_halt_forever(void)
{
    diag_halt();
    for (;;) asm volatile("msr daifset, #15; wfi");
}
/* Switch address spaces: TTBR0 is the process's table. All processes use
 * address space id 0, so this CPU then forgets its translations for id 0
 * (the kernel's are "global" and stay). */
static inline void arch_as_load(uint64_t as)
{
    uint64_t z = 0;
    asm volatile("msr ttbr0_el1, %0; isb; tlbi aside1, %1; dsb nsh; isb" : : "r"(as), "r"(z) : "memory");
}
static inline void arch_tls_switch(arch_cpu_t *a, uint64_t base)   /* a thread's thread pointer */
{
    if (base != a->tls_loaded) asm volatile("msr tpidr_el0, %0" : : "r"(a->tls_loaded = base));
}
/* TLB invalidations are broadcast to every CPU by the hardware (tlbi ...is):
 * nothing to answer while waiting for the kernel lock. */
#define arch_lock_wait(c) arch_relax()
