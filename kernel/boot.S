/*
 * boot.S - Multiboot2 entry point.
 *
 * GRUB drops us here in 32-bit protected mode with paging disabled.
 * We build a minimal set of page tables, switch to long mode and jump
 * to the higher-half kernel.
 *
 * Boot page table layout (2 MiB pages, 4 page directories = 4 GiB):
 *   PML4[0]   -> identity map of 0..4G      (temporary, removed in C)
 *   PML4[256] -> direct map  0xFFFF800000000000 -> phys 0..4G
 *   PML4[511] -> kernel map  0xFFFFFFFF80000000 -> phys 0..2G
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */

#define KERNEL_VBASE 0xFFFFFFFF80000000

    .section .multiboot2, "a"
    .align 8
mb2_header:
    .long 0xE85250D6                    /* magic */
    .long 0                             /* architecture: i386 */
    .long mb2_header_end - mb2_header   /* header length */
    .long -(0xE85250D6 + 0 + (mb2_header_end - mb2_header))
    .align 8
    .short 5                            /* framebuffer request (optional) */
    .short 1
    .long 20
    .long 1024                          /* width  */
    .long 768                           /* height */
    .long 32                            /* depth  */
    .align 8
    .short 0                            /* end tag */
    .short 0
    .long 8
mb2_header_end:

    .section .boot.text, "ax"
    .code32
    .global _start
_start:
    cli
    movl $boot_stack_top, %esp
    movl %eax, mb_magic
    movl %ebx, mb_info

    /* Check for long mode support via CPUID. */
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb no_long_mode
    movl $0x80000001, %eax
    cpuid
    testl $(1 << 29), %edx
    jz no_long_mode

    /* Fill the 4 page directories: entry i maps i * 2MiB. */
    movl $boot_pd, %edi
    xorl %ecx, %ecx
1:  movl %ecx, %eax
    shll $21, %eax
    orl  $0x83, %eax                    /* present | writable | huge */
    movl %eax, (%edi, %ecx, 8)
    movl %ecx, %eax
    shrl $11, %eax
    movl %eax, 4(%edi, %ecx, 8)
    incl %ecx
    cmpl $2048, %ecx
    jne 1b

    /* PDPT for identity + direct map: 4 entries -> the 4 PDs. */
    movl $boot_pdpt_low, %edi
    movl $boot_pd, %eax
    orl  $0x3, %eax
    movl $4, %ecx
2:  movl %eax, (%edi)
    addl $4096, %eax
    addl $8, %edi
    loop 2b

    /* PDPT for the kernel image: -2G -> 0..1G, -1G -> 1..2G */
    movl $boot_pd, %eax
    orl  $0x3, %eax
    movl %eax, boot_pdpt_high + 510 * 8
    addl $4096, %eax
    movl %eax, boot_pdpt_high + 511 * 8

    /* PML4 */
    movl $boot_pdpt_low, %eax
    orl  $0x3, %eax
    movl %eax, boot_pml4
    movl %eax, boot_pml4 + 256 * 8
    movl $boot_pdpt_high, %eax
    orl  $0x3, %eax
    movl %eax, boot_pml4 + 511 * 8

    /* Enable PAE */
    movl %cr4, %eax
    orl  $(1 << 5), %eax
    movl %eax, %cr4

    movl $boot_pml4, %eax
    movl %eax, %cr3

    /* EFER.LME */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $(1 << 8), %eax
    wrmsr

    /* Enable paging + write protect */
    movl %cr0, %eax
    orl  $((1 << 31) | (1 << 16)), %eax
    movl %eax, %cr0

    lgdt boot_gdt_ptr
    ljmp $0x08, $long_mode_entry

no_long_mode:
    movl $0xB8000, %edi
    movl $no_lm_msg, %esi
3:  lodsb
    testb %al, %al
    jz 4f
    movb %al, (%edi)
    movb $0x4F, 1(%edi)
    addl $2, %edi
    jmp 3b
4:  hlt
    jmp 4b

    .code64
long_mode_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs
    movabsq $higher_half, %rax
    jmp *%rax

    .section .boot.data, "aw"
no_lm_msg:
    .asciz "SIEOS: CPU does not support 64-bit long mode"
    .align 16
boot_gdt:
    .quad 0
    .quad 0x00AF9A000000FFFF            /* 0x08: 64-bit code */
    .quad 0x00CF92000000FFFF            /* 0x10: data */
boot_gdt_end:
boot_gdt_ptr:
    .short boot_gdt_end - boot_gdt - 1
    .long boot_gdt
    .global mb_magic, mb_info
mb_magic: .long 0
mb_info:  .long 0

    .section .boot.bss, "aw", @nobits
    .align 4096
    .global boot_pml4, boot_pdpt_low
boot_pml4:      .space 4096
boot_pdpt_low:  .space 4096
boot_pdpt_high: .space 4096
boot_pd:        .space 4096 * 4
boot_stack:     .space 4096
boot_stack_top:

    /* ---------------- higher half ---------------- */
    .text
    .code64
higher_half:
    movabsq $kernel_stack_top, %rsp
    xorq %rbp, %rbp
    movl mb_magic, %edi
    movl mb_info, %esi
    call kmain
5:  cli
    hlt
    jmp 5b

    .bss
    .align 16
kernel_stack:
    .space 16384
    .global kernel_stack_top
kernel_stack_top:

    .section .note.GNU-stack, "", @progbits
