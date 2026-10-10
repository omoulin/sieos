/*
 * boot.S - From the boot loader to C, in four moves:
 *   1. the boot loader (QEMU or GRUB, following the "multiboot" standard)
 *      finds our header, loads the kernel at 1 MiB and jumps to _start in
 *      32-bit protected mode, paging off;
 *   2. we build the first page tables;
 *   3. we switch the CPU to 64-bit "long mode" (which requires paging);
 *   4. we jump to the higher half and call kmain() in main.c.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#define KVMA 0xFFFFFFFF80000000
#define PHYS(x) ((x) - KVMA)            /* physical address of a higher-half symbol */

/* ---- 1. The multiboot header: "this file is a kernel, load it like this".
 * Flags: modules page-aligned (bit 0), give us the memory map (bit 1), and
 * the load addresses are in this header (bit 16), so the file can be a flat
 * binary rather than an ELF file. */
    .section .multiboot, "a"
    .align 4
mb_header:
    .long 0x1BADB002                    /* magic */
    .long 0x00010003                    /* flags */
    .long -(0x1BADB002 + 0x00010003)    /* checksum: the three add up to 0 */
    .long mb_header                     /* where this header is loaded */
    .long mb_header                     /* the file is loaded from here ... */
    .long _load_end                     /* ... to here */
    .long _bss_end                      /* zeroed up to here (the .bss) */
    .long _start                        /* entry point */
/* ... and ours, for SIEOS's UEFI loader (boot/uefi, mk/boot.h): the 64-bit
 * entry point, which it finds by the word "SIE6" right after this header. */
    .long 0x36454953                    /* "SIE6" */
    .long _start64

    .section .boot, "ax"
    .code32
    .global _start
_start:                                 /* eax = 0x2BADB002, ebx = boot information */
    cli
    mov  $PHYS(boot_stack_top), %esp
    mov  %eax, %ebp                     /* keep both for kmain() */
    mov  %ebx, %esi

    /* Clear the .bss ourselves (it holds the page tables below). */
    mov  $_bss_start, %edi
    mov  $_bss_end, %ecx
    sub  %edi, %ecx
    shr  $2, %ecx
    xor  %eax, %eax
    rep  stosl

/* ---- 2. Page tables. x86-64 translates a virtual address through 4 levels
 * of tables (PML4 -> PDPT -> PD -> PT), 512 entries each. A PD entry with
 * bit 7 ("huge") maps 2 MiB at once, so one PD maps 1 GiB and four map
 * the first 4 GiB of physical memory. We use those four PDs three times:
 *   PML4[0]   -> the first GiB at virtual 0: "identity", so this code keeps
 *                running when paging starts (removed later, in mem.c;
 *                smp.c borrows it again to start the other CPUs);
 *   PML4[256] -> all 4 GiB at 0xFFFF800000000000: the kernel reaches any
 *                physical address there (the "direct map"; mem.c adds
 *                the RAM above 4 GiB);
 *   PML4[511] -> the first GiB at 0xFFFFFFFF80000000: the kernel image.
 * Entry bits: 1 = present, 2 = writable, 0x80 = huge, 0x100 = global (kept
 * in the TLB, the CPU's translation cache, when switching address spaces). */
    mov  $PHYS(boot_pml4), %edi
    movl $PHYS(pdpt_low) + 3, (%edi)
    movl $PHYS(pdpt_dmap) + 3, 256*8(%edi)
    movl $PHYS(pdpt_kern) + 3, 511*8(%edi)
    movl $PHYS(pd) + 3, PHYS(pdpt_low)
    movl $PHYS(pd) + 3, PHYS(pdpt_kern) + 510*8

    mov  $PHYS(pdpt_dmap), %edi         /* pdpt_dmap[0..3] -> the 4 PDs */
    mov  $PHYS(pd) + 3, %eax
    mov  $4, %ecx
1:  mov  %eax, (%edi)
    add  $4096, %eax
    add  $8, %edi
    loop 1b

    mov  $PHYS(pd), %edi                /* 2048 entries x 2 MiB = 4 GiB */
    mov  $0x183, %eax
    mov  $2048, %ecx
2:  mov  %eax, (%edi)
    add  $0x200000, %eax
    add  $8, %edi
    loop 2b

/* ---- 3. Long mode: enable PAE (64-bit page table entries), point CR3 at
 * the PML4, set EFER.LME (long mode), EFER.SCE (the SYSCALL instruction)
 * and EFER.NXE (pages can be non-executable), then enable paging. */
    mov  %cr4, %eax
    or   $0x20, %eax                    /* CR4.PAE */
    mov  %eax, %cr4
    mov  $PHYS(boot_pml4), %eax
    mov  %eax, %cr3
    mov  $0xC0000080, %ecx              /* the EFER register */
    rdmsr
    or   $0x901, %eax                   /* SCE | LME | NXE */
    wrmsr
    mov  %cr0, %eax
    or   $0x80010001, %eax              /* PG (paging) | WP (protect read-only pages from the kernel too) | PE */
    mov  %eax, %cr0

    lgdt boot_gdtr                      /* a 64-bit code segment ... */
    ljmp $0x08, $long64                 /* ... and jump into it */

    .code64
long64:
    mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %ss
    xor  %eax, %eax
    mov  %ax, %fs
    mov  %ax, %gs

/* ---- 4. Into the higher half, and C. */
#ifdef STACKCHECK
    movabs $boot_stack, %rdi            /* fill the stack with the pattern (task.c) */
    mov  $4096 / 8, %ecx
    movabs $0x5AFE5AFE5AFE5AFE, %rax
    rep  stosq
#endif
    movabs $boot_stack_top, %rsp
    mov  %ebp, %edi                     /* kmain(magic, boot information) */
    mov  %esi, %esi
    movabs $kmain, %rax
    call *%rax
3:  hlt
    jmp  3b

/* ---- From SIEOS's UEFI loader: already in 64-bit mode, paging on (the
 * firmware's identity map), interrupts off; edi = SIEOS_BOOT_MAGIC, rsi =
 * its information (below 4 GiB). The same steps as above, in 64-bit code,
 * then the common path at long64. */
    .global _start64
_start64:
    cli
    mov  %edi, %ebp                     /* keep both for kmain() (esi stays as is) */
    mov  $PHYS(boot_stack_top), %esp
    mov  $_bss_start, %edi              /* clear the .bss (the loader did too: be sure) */
    mov  $_bss_end, %ecx
    sub  %edi, %ecx
    shr  $2, %ecx
    xor  %eax, %eax
    rep  stosl

    mov  $PHYS(boot_pml4), %edi         /* the same page tables as above */
    movl $PHYS(pdpt_low) + 3, (%rdi)
    movl $PHYS(pdpt_dmap) + 3, 256*8(%rdi)
    movl $PHYS(pdpt_kern) + 3, 511*8(%rdi)
    mov  $PHYS(pdpt_low), %edi
    movl $PHYS(pd) + 3, (%rdi)
    mov  $PHYS(pdpt_kern), %edi
    movl $PHYS(pd) + 3, 510*8(%rdi)
    mov  $PHYS(pdpt_dmap), %edi
    mov  $PHYS(pd) + 3, %eax
    mov  $4, %ecx
4:  mov  %eax, (%rdi)
    add  $4096, %eax
    add  $8, %rdi
    dec  %ecx
    jnz  4b
    mov  $PHYS(pd), %edi
    mov  $0x183, %eax
    mov  $2048, %ecx
5:  mov  %eax, (%rdi)
    add  $0x200000, %eax
    add  $8, %rdi
    dec  %ecx
    jnz  5b

    /* The firmware may have left global pages (CR4.PGE) and SMEP/SMAP on:
     * off, so switching to our table forgets all its translations, and
     * mem.c turns PGE on again later, as after multiboot. */
    mov  %cr4, %rax
    and  $~0x300080, %eax               /* PGE (7), SMEP (20), SMAP (21) */
    mov  %rax, %cr4
    mov  $PHYS(boot_pml4), %eax
    mov  %rax, %cr3
    mov  $0xC0000080, %ecx              /* EFER: SCE | LME | NXE (LME is already on) */
    rdmsr
    or   $0x901, %eax
    wrmsr
    mov  %cr0, %rax
    or   $0x10000, %eax                 /* WP */
    mov  %rax, %cr0
    lgdt boot_gdtr64
    pushq $0x08                         /* reload cs with our 64-bit code segment */
    pushq $long64
    .byte 0x48, 0xCB                    /* lretq (as bytes: both assemblers take them) */

/* The segment descriptors needed to enter 64-bit mode (cpu.c installs the
 * final ones, with the same 0x08 and 0x10 selectors). */
    .align 8
boot_gdt:
    .quad 0
    .quad 0x00AF9A000000FFFF            /* 0x08: kernel code, 64-bit */
    .quad 0x00CF92000000FFFF            /* 0x10: kernel data */
boot_gdtr:
    .word 3*8 - 1
    .long boot_gdt
boot_gdtr64:                            /* (lgdt in 64-bit mode reads a 64-bit address) */
    .word 3*8 - 1
    .quad boot_gdt

/* Zeroed memory: page tables, and the boot stack (later the idle stack). */
    .bss
    .align 4096
    .global boot_pml4
boot_pml4:  .skip 4096
    .global pdpt_low
pdpt_low:   .skip 4096
pdpt_dmap:  .skip 4096
pdpt_kern:  .skip 4096
pd:         .skip 4 * 4096
    .global boot_stack, boot_stack_top
boot_stack: .skip 4096                  /* deepest use measured: 600 bytes (make STACKCHECK=1) */
boot_stack_top:
