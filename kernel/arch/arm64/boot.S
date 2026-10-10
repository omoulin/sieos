/*
 * boot.S (arm64) - From the boot loader to C.
 *
 *  1. The boot loader (QEMU's -kernel, a Raspberry Pi's firmware) finds the
 *     "Image" header below, puts the file somewhere in RAM (any 2 MiB
 *     boundary: we find out where with adr), and jumps to it with the MMU
 *     off, x0 = the physical address of the device tree, at EL2 or EL1.
 *  2. From EL2 (a hypervisor's level, where the Pis start) we drop to EL1.
 *  3. We build the first page tables: the kernel image at KVMA, the GiB(s)
 *     of RAM holding the kernel and the device tree in the direct map (the
 *     rest of RAM: mmu.c, once the device tree is read), and an "identity"
 *     map so the instructions right after turning the MMU on still run.
 *  4. MMU on, jump to the kernel's high address, call arm64_main.
 * The other CPUs (smp.c, PSCI or spin table) enter at secondary_start and
 * take the same steps with the same tables.
 *
 * Until the MMU is on, everything is addressed relative to the program
 * counter (adr/adrp), which gives physical addresses.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#define KVMA_HI   0xFFFF            /* KVMA = 0xFFFFFFFF80000000, built 16 bits at a time */
#define MAIR      0x4400FF          /* attribute 0: normal, write-back; 1: device nGnRnE; 2: normal uncached */
#define SCTLR     0x34D09805        /* reserved-one bits | UCI (programs may clean/invalidate the data
                                       cache: drivers of devices that do not see the caches, the Pis')
                                       | UCT (programs may read the cache line size) | I | C | M (MMU) */
#define DESC_TBL  3                 /* a table descriptor */
#define DESC_GB   0x60000000000701  /* a 1 GiB block of RAM: valid, normal, shareable, accessed, XN */
#define DESC_GBX  0x701             /*   the same, executable (identity map) */
#define DESC_PAGE 0x703             /* a kernel page: valid, normal, shareable, accessed, executable */

    .section .text.boot, "ax"
    .global _start
_start:                             /* ---- the Image header (64 bytes) */
    b     start                     /* code0: jump over the header */
    .long 0                         /* code1 */
    .quad 0                         /* text_offset: 0 (from a 2 MiB boundary) */
    .quad _image_size               /* image_size: in memory, .bss included */
    .quad 0xA                       /* flags: little-endian, 4 KiB pages, anywhere in RAM */
    .quad 0, 0, 0
    .ascii "ARM\x64"                /* magic */
    .long 0

start:
    msr   daifset, #15              /* no interrupts until we are ready */
    mov   x19, x0                   /* the device tree, physical */
    adr   x20, _start               /* where the image is, physical */
    /* diag.c (boot steps on a Pi's screen) is C: it needs a stack, which
     * lives in .data (boot.S clears .bss) */
    adrp  x0, diag_stack_top
    add   x0, x0, :lo12:diag_stack_top
    mov   sp, x0
    mov   x0, x19
    bl    diag_init                 /* step 1: entered the kernel */
    bl    to_el1
    adrp  x0, diag_stack_top        /* (EL1 has its own stack pointer) */
    add   x0, x0, :lo12:diag_stack_top
    mov   sp, x0
    adrp  x0, early_vectors         /* until the real ones: faults painted by diag.c */
    add   x0, x0, :lo12:early_vectors
    msr   vbar_el1, x0
    isb
    mov   x0, #2
    bl    diag_stage                /* step 2: at EL1 */
    bl    zero_bss
    mov   x0, #3
    bl    diag_stage                /* step 3: .bss cleared */
    bl    build_tables
    mov   x0, #4
    bl    diag_stage                /* step 4: page tables built */
    mov   x22, #1                   /* (mmu_on: the first CPU shows step 5) */
    bl    mmu_on
    /* now at the kernel's high address: adrp gives virtual addresses */
    mov   x0, x20
    mov   x1, x19
    bl    diag_high                 /* step 6: running at the kernel's high address */
    adrp  x0, boot_stack_top
    add   x0, x0, :lo12:boot_stack_top
    mov   sp, x0
    adrp  x0, vectors
    add   x0, x0, :lo12:vectors
    msr   vbar_el1, x0
    isb
    mov   x0, x19
    mov   x1, x20
    bl    arm64_main                /* platform.c: never returns */
1:  wfi
    b     1b

/* ---- EL2 -> EL1: let EL1 use the timers and the FP registers, run EL1 in
 * 64-bit mode, and "return" to EL1 with interrupts masked. (At EL1 already:
 * nothing to do. QEMU's virt starts there; the Pis' firmware at EL2.) */
to_el1:
    mrs   x0, currentel
    lsr   x0, x0, #2
    cmp   x0, #2
    b.ne  1f
    mov   x0, #3
    msr   cnthctl_el2, x0           /* EL1 may read the counter and use the physical timer */
    msr   cntvoff_el2, xzr          /* virtual time = physical time */
    mov   x0, #0x33FF
    msr   cptr_el2, x0              /* no FP/SIMD traps to EL2 */
    movz  x0, #0x8000, lsl #16
    msr   hcr_el2, x0               /* EL1 is AArch64; nothing else trapped */
    mrs   x0, midr_el1
    msr   vpidr_el2, x0
    mrs   x0, mpidr_el1
    msr   vmpidr_el2, x0
    movz  x0, #0x0800
    movk  x0, #0x30D0, lsl #16
    msr   sctlr_el1, x0             /* EL1's control register: reserved-one bits, MMU off */
    mov   x0, #0x3C5
    msr   spsr_el2, x0              /* EL1h, interrupts masked */
    msr   elr_el2, x30
    eret                            /* returns to our caller, at EL1 */
1:  ret

zero_bss:
    adrp  x0, _bss_start
    add   x0, x0, :lo12:_bss_start
    adrp  x1, _bss_end
    add   x1, x1, :lo12:_bss_end
1:  cmp   x0, x1
    b.hs  2f
    str   xzr, [x0], #8
    b     1b
    /* written with the caches off: drop any stale cached copy of it, so
     * the zeros are what the CPU sees once the caches are on */
2:  adrp  x0, _bss_start
    add   x0, x0, :lo12:_bss_start
3:  dc    ivac, x0
    add   x0, x0, #64
    cmp   x0, x1
    b.lo  3b
    dsb   sy
    ret

/* ---- The page tables (in .bss, zeroed). x20: the image's physical base.
 *   ttbr1_l0[256] -> dmap_l1: 1 GiB blocks for the GiB of the kernel and
 *                    the GiB of the device tree (the direct map: DMAP + pa)
 *   ttbr1_l0[511] -> kern_l1; kern_l1[510] -> kern_l2; kern_l2[0] -> kern_l3:
 *                    the image's pages at KVMA (at most 2 MiB)
 *   id_l0[pa >> 39] -> id_l1: the kernel's GiB at its own address (identity) */
build_tables:
    adrp  x0, ttbr1_l0
    add   x0, x0, :lo12:ttbr1_l0
    adrp  x1, dmap_l1
    add   x1, x1, :lo12:dmap_l1
    orr   x2, x1, #DESC_TBL
    str   x2, [x0, #256 * 8]
    adrp  x3, kern_l1
    add   x3, x3, :lo12:kern_l1
    orr   x2, x3, #DESC_TBL
    str   x2, [x0, #511 * 8]
    /* direct map: the kernel's GiB, then the device tree's */
    movz  x4, #0x0701
    movk  x4, #0x0060, lsl #48      /* DESC_GB */
    lsr   x5, x20, #30
    lsl   x6, x5, #30
    orr   x6, x6, x4
    str   x6, [x1, x5, lsl #3]
    lsr   x5, x19, #30
    lsl   x6, x5, #30
    orr   x6, x6, x4
    str   x6, [x1, x5, lsl #3]
    /* the image at KVMA, page by page */
    adrp  x4, kern_l2
    add   x4, x4, :lo12:kern_l2
    orr   x2, x4, #DESC_TBL
    str   x2, [x3, #510 * 8]
    adrp  x5, kern_l3
    add   x5, x5, :lo12:kern_l3
    orr   x2, x5, #DESC_TBL
    str   x2, [x4]
    adrp  x6, _kernel_end
    add   x6, x6, :lo12:_kernel_end
    adr   x7, _start
    sub   x6, x6, x7                /* the image's size in memory */
    mov   x7, x20
    movz  x8, #DESC_PAGE
1:  orr   x9, x7, x8
    str   x9, [x5], #8
    add   x7, x7, #4096
    sub   x6, x6, #4096
    cmp   x6, #0
    b.gt  1b
    /* the identity map */
    adrp  x0, id_l0
    add   x0, x0, :lo12:id_l0
    adrp  x1, id_l1
    add   x1, x1, :lo12:id_l1
    lsr   x2, x20, #39
    orr   x3, x1, #DESC_TBL
    str   x3, [x0, x2, lsl #3]
    lsr   x2, x20, #30
    and   x2, x2, #511
    lsr   x3, x20, #30
    lsl   x3, x3, #30
    movz  x4, #DESC_GBX
    orr   x3, x3, x4
    str   x3, [x1, x2, lsl #3]
    /* the tables were written with the MMU (and caches) off: drop any
     * stale cached copy of them, so the table walker reads what we wrote */
    adrp  x0, ttbr1_l0
    add   x0, x0, :lo12:ttbr1_l0
    adrp  x1, tables_end
    add   x1, x1, :lo12:tables_end
2:  dc    ivac, x0
    add   x0, x0, #64
    cmp   x0, x1
    b.lo  2b
    dsb   sy
    ret

/* ---- MMU on (x20: the physical base), then continue at the caller's
 * return address + (KVMA - x20): the same code, at its high address. */
mmu_on:
    movz  x0, #0x00FF
    movk  x0, #0x0044, lsl #16
    msr   mair_el1, x0
    /* TCR: 48-bit halves (T0SZ = T1SZ = 16), 4 KiB granules, tables cached
     * write-back and shareable, physical address size from the CPU */
    mrs   x1, id_aa64mmfr0_el1
    and   x1, x1, #7
    movz  x0, #0x3510
    movk  x0, #0xB510, lsl #16
    orr   x0, x0, x1, lsl #32
    msr   tcr_el1, x0
    adrp  x0, id_l0
    add   x0, x0, :lo12:id_l0
    msr   ttbr0_el1, x0
    adrp  x0, ttbr1_l0
    add   x0, x0, :lo12:ttbr1_l0
    msr   ttbr1_el1, x0
    isb
    tlbi  vmalle1                   /* no stale translations ... */
    ic    iallu                     /* ... nor instructions cached before (by the firmware's code) */
    dsb   nsh
    isb
    movz  x0, #0x9805               /* SCTLR (above): the MMU and caches on, and programs may */
    movk  x0, #0x34D0, lsl #16      /* read the cache type and clean the data cache (UCT, UCI) */
    msr   sctlr_el1, x0
    isb
    cbz   x22, 1f                   /* the first CPU: step 5, MMU on (still at the */
    mov   x23, x30                  /* physical address, through the identity map) */
    mov   x0, #5
    bl    diag_stage
    mov   x30, x23
1:  movz  x1, #0x0000
    movk  x1, #0x8000, lsl #16
    movk  x1, #KVMA_HI, lsl #32
    movk  x1, #KVMA_HI, lsl #48     /* KVMA */
    sub   x1, x1, x20
    add   x30, x30, x1
    ret

/* ---- Another CPU (smp.c started it with PSCI or the spin table): x0 = the
 * physical address of its start block {entry stack top (virtual), cpu_t
 * (virtual), the image's physical base}. Same steps as above, with the
 * tables CPU 0 built. */
    .global secondary_spin
secondary_spin:                     /* from a spin table: no x0, find the block (MMU off: physical) */
    adrp  x0, ap_boot
    add   x0, x0, :lo12:ap_boot
    .global secondary_start
secondary_start:
    msr   daifset, #15
    mov   x22, #0                   /* (no screen steps for the other CPUs) */
    mov   x21, x0
    ldr   x20, [x21, #16]           /* the physical base */
    bl    to_el1
    bl    mmu_on
    sub   x21, x21, x20             /* the start block, now through KVMA */
    movz  x1, #0x0000
    movk  x1, #0x8000, lsl #16
    movk  x1, #KVMA_HI, lsl #32
    movk  x1, #KVMA_HI, lsl #48
    add   x21, x21, x1
    ldr   x0, [x21]
    mov   sp, x0
    adrp  x0, vectors
    add   x0, x0, :lo12:vectors
    msr   vbar_el1, x0
    isb
    ldr   x0, [x21, #8]
    bl    ap_main                   /* smp.c: never returns */
1:  wfi
    b     1b

/* ---- Exceptions before the kernel's own vectors (boot.S above, until
 * the high address): diag.c paints the red band, then we stop. 16 entries
 * of 128 bytes, 2 KiB aligned, all alike. */
    .balign 2048
early_vectors:
    .rept 16
    b     early_exception
    .balign 128
    .endr
early_exception:
    mrs   x1, esr_el1
    mrs   x2, elr_el1
    mrs   x3, far_el1
    adrp  x0, diag_stack_top
    add   x0, x0, :lo12:diag_stack_top
    mov   sp, x0
    mov   x0, #1
    bl    diag_fault
1:  wfe
    b     1b

/* ---- PSCI calls (the firmware's interface for CPUs and power): x0 = the
 * function, x1-x3 its arguments; through "hvc" or "smc" as the device
 * tree's /psci "method" says. */
    .text
    .global psci_hvc
psci_hvc:
    hvc   #0
    ret
    .global psci_smc
psci_smc:
    smc   #0
    ret

    .data                           /* (not cleared: usable before zero_bss) */
    .balign 16
diag_stack:  .skip 4096
diag_stack_top:

    .bss
    .balign 4096
    .global ttbr1_l0, id_l0
ttbr1_l0: .skip 4096
dmap_l1:  .skip 4096
kern_l1:  .skip 4096
kern_l2:  .skip 4096
kern_l3:  .skip 4096
id_l0:    .skip 4096
id_l1:    .skip 4096
tables_end:
    .global boot_stack, boot_stack_top
boot_stack: .skip 16384
boot_stack_top:
