/*
 * ap_boot.S - Application processor start-up trampoline.
 *
 * Copied to physical address AP_TRAMPOLINE (0x8000) and entered in real
 * mode by the Startup IPI.  It switches to protected mode, enables PAE
 * and long mode using the kernel page tables (with the temporary identity
 * map in place), and jumps to ap_main(cpu) on the stack prepared by the
 * BSP in the parameter block at the end of the trampoline.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#define BASE 0x8000
#define ADDR(x) (BASE + ((x) - ap_trampoline))

    .section .rodata
    .global ap_trampoline, ap_trampoline_end
    .global ap_param_cr3, ap_param_stack, ap_param_entry, ap_param_cpu

    .code16
ap_trampoline:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    lgdtl ADDR(ap_gdt_ptr)
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmpl $0x08, $ADDR(ap_pm32)

    .code32
ap_pm32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl %cr4, %eax
    orl $(1 << 5), %eax                 /* PAE */
    movl %eax, %cr4
    movl ADDR(ap_param_cr3), %eax
    movl %eax, %cr3
    movl $0xC0000080, %ecx              /* EFER.LME */
    rdmsr
    orl $(1 << 8), %eax
    wrmsr
    movl %cr0, %eax
    orl $((1 << 31) | (1 << 16)), %eax  /* PG | WP */
    movl %eax, %cr0
    ljmpl $0x18, $ADDR(ap_lm64)

    .code64
ap_lm64:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movq ADDR(ap_param_stack), %rsp
    movq ADDR(ap_param_cpu), %rdi
    movq ADDR(ap_param_entry), %rax
    xorq %rbp, %rbp
    jmp *%rax

    .align 16
ap_gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF            /* 0x08: 32-bit code */
    .quad 0x00CF92000000FFFF            /* 0x10: data */
    .quad 0x00AF9A000000FFFF            /* 0x18: 64-bit code */
ap_gdt_end:
ap_gdt_ptr:
    .short ap_gdt_end - ap_gdt - 1
    .long ADDR(ap_gdt)

    .align 8
ap_param_cr3:   .quad 0
ap_param_stack: .quad 0
ap_param_entry: .quad 0
ap_param_cpu:   .quad 0
ap_trampoline_end:

    .section .note.GNU-stack, "", @progbits
