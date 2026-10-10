/*
 * start.S - The doors between the UEFI firmware's calling convention
 * (Microsoft x64: arguments in rcx, rdx, r8, r9, then the stack above 32
 * bytes of "shadow space") and ours (System V: rdi, rsi, rdx, rcx, r8, r9).
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
    .text
    .global efi_start, efi_call, efi_jump

/* The firmware starts the loader here: rcx = our image handle, rdx = the
 * system table. efi_main returns a status for the firmware. */
efi_start:
    push %rbp                       /* (also aligns the stack to 16 for the call) */
    mov  %rsp, %rbp
    push %rdi                       /* rdi and rsi belong to the caller in its convention */
    push %rsi
    mov  %rcx, %rdi
    mov  %rdx, %rsi
    call efi_main
    pop  %rsi
    pop  %rdi
    pop  %rbp
    ret

/* efi_call(fn, a1, a2, a3, a4, a5, a6): fn in rdi, a1-a5 in rsi, rdx, rcx,
 * r8, r9, a6 on our stack. */
efi_call:
    push %rbp
    mov  %rsp, %rbp
    sub  $48, %rsp                  /* shadow space (32) + a5, a6 (the stack stays 16-aligned) */
    mov  16(%rbp), %rax             /* a6 */
    mov  %rax, 40(%rsp)
    mov  %r9, 32(%rsp)              /* a5 */
    mov  %rdi, %rax                 /* fn */
    mov  %r8, %r9                   /* a4 */
    mov  %rcx, %r8                  /* a3 */
    mov  %rsi, %rcx                 /* a1 (a2 is in rdx already) */
    call *%rax
    leave
    ret

/* efi_jump(entry, magic, info): to the kernel, rdi = magic, rsi = info. */
efi_jump:
    cli
    mov  %rdi, %rax
    mov  %rsi, %rdi
    mov  %rdx, %rsi
    jmp  *%rax
