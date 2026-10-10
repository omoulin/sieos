/*
 * crt0.S - Where every program starts. The kernel passes the argument
 * string ("name arg1 arg2") as the first argument (x86-64: rdi; arm64:
 * x0); __start (lib.c) splits it, calls main and exits with its result.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
    .text
    .global _start
_start:
#if defined(__aarch64__)
    mov  x29, #0            /* the first frame: no caller */
    mov  x30, #0
    bl   __start            /* the kernel left sp 16-byte aligned */
    udf  #0                 /* never reached */
#else
    and  $-16, %rsp         /* the C convention: a 16-byte aligned stack */
    call __start
    ud2                     /* never reached */
#endif
