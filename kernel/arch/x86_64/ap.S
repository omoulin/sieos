/*
 * ap.S - The "trampoline": how another processor gets from power-on to
 * the kernel. smp.c copies this code to physical address 0x8000 and wakes
 * the processor there, in 16-bit real mode, like a PC in 1981. It then
 * climbs the same steps as boot.S: 32-bit protected mode, then 64-bit
 * mode with paging, and calls ap_main(cpu) on its own stack.
 *
 * The code runs at 0x8000, not where it is linked, so every address is
 * computed relative to ap_start: TR(x) is where x is once copied.
 * smp.c fills in ap_cr3, ap_stack and ap_cpu before each start.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#define TR(x) ((x) - ap_start + 0x8000)

    .section .rodata
    .global ap_start, ap_end, ap_cr3, ap_stack, ap_cpu
    .code16
ap_start:
    cli
    xor  %ax, %ax
    mov  %ax, %ds
    lgdtl TR(ap_gdtr)
    mov  %cr0, %eax
    or   $1, %eax                       /* protected mode */
    mov  %eax, %cr0
    ljmpl $0x18, $TR(ap32)

    .code32
ap32:
    mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %ss
    mov  %cr4, %eax
    or   $0x20, %eax                    /* PAE (not PGE yet: ap_main, after leaving this page table) */
    mov  %eax, %cr4
    mov  TR(ap_cr3), %eax
    mov  %eax, %cr3
    mov  $0xC0000080, %ecx              /* EFER: SCE | LME | NXE, as on the first CPU */
    rdmsr
    or   $0x901, %eax
    wrmsr
    mov  %cr0, %eax
    or   $0x80010000, %eax              /* paging, write protection */
    mov  %eax, %cr0
    ljmp $0x08, $TR(ap64)

    .code64
ap64:
    mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %ss
    xor  %eax, %eax
    mov  %ax, %fs
    mov  %ax, %gs
    mov  TR(ap_stack), %rsp
    mov  TR(ap_cpu), %rdi
    movabs $ap_main, %rax
    call *%rax

    .align 8
ap_gdt:
    .quad 0
    .quad 0x00AF9A000000FFFF            /* 0x08: 64-bit code */
    .quad 0x00CF92000000FFFF            /* 0x10: data */
    .quad 0x00CF9A000000FFFF            /* 0x18: 32-bit code */
ap_gdtr:
    .word 4*8 - 1
    .long TR(ap_gdt)
    .align 8
ap_cr3:   .quad 0
ap_stack: .quad 0
ap_cpu:   .quad 0
ap_end:
