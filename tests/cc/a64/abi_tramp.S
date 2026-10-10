// abi_tramp.S - The hand-written side of the AArch64 calling-convention
// checker (abi_a64.py): registers and stack exactly as AAPCS64 lays them out,
// independent of sicc's code generator.
//
// The records (struct Regs in the generated C file):
//   0 x0-x7, 64 x8, 72 (pad), 80 v0-v7 (16 bytes each), 208 stack (256 bytes),
//   464 mem size, 472 x19-x28 and d8-d15 (18 words), 616 sp after, 624 sp before
//
// Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

    .text
    .globl abi_rec
// abi_rec: a function of any type. Records its arguments in abi_in (registers,
// and the first 256 bytes of its stack arguments), then returns abi_out:
// x0, x1, v0-v3, and (if abi_out's mem size is not 0) that many bytes of
// abi_out's stack area copied to the result buffer at x8.
abi_rec:
    adrp x9, abi_in
    add x9, x9, :lo12:abi_in
    stp x0, x1, [x9]
    stp x2, x3, [x9, #16]
    stp x4, x5, [x9, #32]
    stp x6, x7, [x9, #48]
    str x8, [x9, #64]
    stp q0, q1, [x9, #80]
    stp q2, q3, [x9, #112]
    stp q4, q5, [x9, #144]
    stp q6, q7, [x9, #176]
    mov x10, sp
    add x11, x9, #208
    mov x12, #32
.Lrec_stack:
    ldr x13, [x10], #8
    str x13, [x11], #8
    subs x12, x12, #1
    b.ne .Lrec_stack
    adrp x9, abi_out
    add x9, x9, :lo12:abi_out
    ldr x12, [x9, #464]
    add x11, x9, #208
    mov x10, x8
    cbz x12, .Lrec_regs
.Lrec_mem:
    ldrb w13, [x11], #1
    strb w13, [x10], #1
    subs x12, x12, #1
    b.ne .Lrec_mem
.Lrec_regs:
    ldp x0, x1, [x9]
    ldp q0, q1, [x9, #80]
    ldp q2, q3, [x9, #112]
    ret

    .globl abi_call
// abi_call(fn): calls fn with the registers and stack arguments of abi_in
// (x0-x7, x8, v0-v7, 256 bytes of stack), and known values in x19-x28 and
// d8-d15. Records in abi_ret: x0, x1, v0-v3, the callee-saved registers as fn
// left them, and sp before and after.
abi_call:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #160
    stp x19, x20, [sp]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp d8, d9, [sp, #80]
    stp d10, d11, [sp, #96]
    stp d12, d13, [sp, #112]
    stp d14, d15, [sp, #128]
    mov x16, x0
    sub sp, sp, #256
    adrp x9, abi_in
    add x9, x9, :lo12:abi_in
    add x10, x9, #208
    mov x11, sp
    mov x12, #32
.Lcall_stack:
    ldr x13, [x10], #8
    str x13, [x11], #8
    subs x12, x12, #1
    b.ne .Lcall_stack
    movz x19, #0x1919, lsl #16          // x(19+k) = 0x1919_0000 + 0x101 * k: known values
    mov x20, #0x101
    add x20, x19, x20
    add x21, x20, #0x101
    add x22, x21, #0x101
    add x23, x22, #0x101
    add x24, x23, #0x101
    add x25, x24, #0x101
    add x26, x25, #0x101
    add x27, x26, #0x101
    add x28, x27, #0x101
    fmov d8, x19                       // d(8+k) = the bits of x(19+k)
    fmov d9, x20
    fmov d10, x21
    fmov d11, x22
    fmov d12, x23
    fmov d13, x24
    fmov d14, x25
    fmov d15, x26
    mov x17, sp
    adrp x10, abi_ret
    add x10, x10, :lo12:abi_ret
    str x17, [x10, #624]
    ldp x0, x1, [x9]
    ldp x2, x3, [x9, #16]
    ldp x4, x5, [x9, #32]
    ldp x6, x7, [x9, #48]
    ldr x8, [x9, #64]
    ldp q0, q1, [x9, #80]
    ldp q2, q3, [x9, #112]
    ldp q4, q5, [x9, #144]
    ldp q6, q7, [x9, #176]
    blr x16
    adrp x9, abi_ret
    add x9, x9, :lo12:abi_ret
    stp x0, x1, [x9]
    stp q0, q1, [x9, #80]
    stp q2, q3, [x9, #112]
    add x10, x9, #472                  // (beyond stp's reach from x9)
    stp x19, x20, [x10]
    stp x21, x22, [x10, #16]
    stp x23, x24, [x10, #32]
    stp x25, x26, [x10, #48]
    stp x27, x28, [x10, #64]
    stp d8, d9, [x10, #80]
    stp d10, d11, [x10, #96]
    stp d12, d13, [x10, #112]
    stp d14, d15, [x10, #128]
    mov x17, sp
    str x17, [x9, #616]
    add sp, sp, #256
    ldp x19, x20, [sp]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp d8, d9, [sp, #80]
    ldp d10, d11, [sp, #96]
    ldp d12, d13, [sp, #112]
    ldp d14, d15, [sp, #128]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret
