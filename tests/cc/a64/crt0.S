/* crt0.S - where an AArch64 test program starts, under the user-mode emulator:
 * the stack holds argc, then the argv pointers, a null, the environment.
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
    .text
    .globl _start
_start:
    mov x29, #0
    mov x30, #0
    ldr x0, [sp]                /* argc */
    add x1, sp, #8              /* argv */
    add x2, x1, x0, lsl #3
    add x2, x2, #8              /* envp: after argv's null */
    bl __libc_start
    brk #0
