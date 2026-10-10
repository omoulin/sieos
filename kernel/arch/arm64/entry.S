/*
 * entry.S (arm64) - Every way into and out of the kernel:
 *   - the exception vector table (VBAR_EL1): an exception (system call
 *     "svc", fault, interrupt) from EL0 or EL1 lands at one of its entries,
 *     which saves the registers as a regs_t (arch_defs.h) on the kernel
 *     stack and calls the C handler (cpu.c);
 *   - switching from one thread to another (switch_to);
 *   - saving and loading the FP/SIMD registers of user threads (fpu.c).
 *
 * The kernel stack: an exception from EL0 runs on SP_EL1, which is the
 * running thread's kernel stack top whenever the thread is in user mode
 * (its frame was popped on the way out). From EL1 (only idle can be
 * interrupted: the kernel runs with interrupts masked) it is the current
 * stack.
 *
 * The kernel lock: the C handlers take it; they return 1 if they did, and
 * kernel_exit() (task.c) gives it back on the way out. A thread switch
 * happens with the lock held; the thread that resumes releases it.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#define FRAME 272                   /* sizeof(regs_t): x0-x30, sp, pc, pstate */

.macro VENT target
    .balign 128
    sub   sp, sp, #FRAME
    stp   x0, x1, [sp, #0]
    b     \target
.endm

.macro SAVEALL                      /* (x0 and x1 are saved by VENT) */
    stp   x2, x3, [sp, #16]
    stp   x4, x5, [sp, #32]
    stp   x6, x7, [sp, #48]
    stp   x8, x9, [sp, #64]
    stp   x10, x11, [sp, #80]
    stp   x12, x13, [sp, #96]
    stp   x14, x15, [sp, #112]
    stp   x16, x17, [sp, #128]
    stp   x18, x19, [sp, #144]
    stp   x20, x21, [sp, #160]
    stp   x22, x23, [sp, #176]
    stp   x24, x25, [sp, #192]
    stp   x26, x27, [sp, #208]
    stp   x28, x29, [sp, #224]
    mrs   x21, sp_el0
    stp   x30, x21, [sp, #240]
    mrs   x22, elr_el1
    mrs   x23, spsr_el1
    stp   x22, x23, [sp, #256]
.endm

.macro RESTOREALL
    ldp   x22, x23, [sp, #256]
    msr   elr_el1, x22
    msr   spsr_el1, x23
    ldp   x30, x21, [sp, #240]
    msr   sp_el0, x21
    ldp   x0, x1, [sp, #0]
    ldp   x2, x3, [sp, #16]
    ldp   x4, x5, [sp, #32]
    ldp   x6, x7, [sp, #48]
    ldp   x8, x9, [sp, #64]
    ldp   x10, x11, [sp, #80]
    ldp   x12, x13, [sp, #96]
    ldp   x14, x15, [sp, #112]
    ldp   x16, x17, [sp, #128]
    ldp   x18, x19, [sp, #144]
    ldp   x20, x21, [sp, #160]
    ldp   x22, x23, [sp, #176]
    ldp   x24, x25, [sp, #192]
    ldp   x26, x27, [sp, #208]
    ldp   x28, x29, [sp, #224]
    add   sp, sp, #FRAME
    eret
.endm

    .text
/* ---- The vector table: 16 entries of 128 bytes, 2 KiB aligned. Four
 * groups: from EL1 with SP_EL0 (unused), from EL1 with SP_EL1 (the
 * kernel), from EL0 in 64-bit mode (programs), from EL0 in 32-bit mode
 * (not supported); in each: synchronous, IRQ, FIQ, SError. */
    .balign 2048
    .global vectors
vectors:
    VENT  bad_entry
    VENT  bad_entry
    VENT  bad_entry
    VENT  bad_entry
    VENT  el1_sync
    VENT  el1_irq
    VENT  bad_entry
    VENT  bad_entry
    VENT  el0_sync
    VENT  el0_irq
    VENT  bad_entry
    VENT  bad_entry
    VENT  bad_entry
    VENT  bad_entry
    VENT  bad_entry
    VENT  bad_entry

el0_sync:                           /* system call, fault, or first FP use */
    SAVEALL
    mov   x0, sp
    bl    trap_sync
    b     trap_out
el0_irq:
el1_irq:                            /* (from EL1: only idle's wait) */
    SAVEALL
    mov   x0, sp
    bl    trap_irq
    b     trap_out
el1_sync:                           /* a bug in the kernel */
    SAVEALL
    mov   x0, sp
    bl    trap_kernel
    b     trap_out
bad_entry:
    SAVEALL
    mov   x0, sp
    bl    trap_bad

trap_out:
    cbz   w0, 1f                    /* did the handler take the kernel lock? */
    .global trap_return
trap_return:                        /* new threads also start here (cpu.c) */
    bl    kernel_exit
1:  RESTOREALL

/* ---- switch_to(&old->ksp, new->ksp): save the registers C expects to
 * survive a call (x19-x30) on the old thread's kernel stack, remember that
 * stack, switch to the new one and restore its registers. The "ret" then
 * returns wherever the new thread was when it called switch_to, or to
 * trap_return for a thread that never ran. */
    .global switch_to
switch_to:
    sub   sp, sp, #96
    stp   x19, x20, [sp, #0]
    stp   x21, x22, [sp, #16]
    stp   x23, x24, [sp, #32]
    stp   x25, x26, [sp, #48]
    stp   x27, x28, [sp, #64]
    stp   x29, x30, [sp, #80]
    mov   x9, sp
    str   x9, [x0]
    mov   sp, x1
    ldp   x19, x20, [sp, #0]
    ldp   x21, x22, [sp, #16]
    ldp   x23, x24, [sp, #32]
    ldp   x25, x26, [sp, #48]
    ldp   x27, x28, [sp, #64]
    ldp   x29, x30, [sp, #80]
    add   sp, sp, #96
    ret

/* ---- A user thread's FP/SIMD registers (fpu.c): q0-q31 (512 bytes),
 * then FPCR and FPSR. The area is 16-byte aligned. */
    .global fpu_save
fpu_save:
    stp   q0, q1, [x0, #0]
    stp   q2, q3, [x0, #32]
    stp   q4, q5, [x0, #64]
    stp   q6, q7, [x0, #96]
    stp   q8, q9, [x0, #128]
    stp   q10, q11, [x0, #160]
    stp   q12, q13, [x0, #192]
    stp   q14, q15, [x0, #224]
    stp   q16, q17, [x0, #256]
    stp   q18, q19, [x0, #288]
    stp   q20, q21, [x0, #320]
    stp   q22, q23, [x0, #352]
    stp   q24, q25, [x0, #384]
    stp   q26, q27, [x0, #416]
    stp   q28, q29, [x0, #448]
    stp   q30, q31, [x0, #480]
    mrs   x1, fpcr
    mrs   x2, fpsr
    str   x1, [x0, #512]
    str   x2, [x0, #520]
    ret
    .global fpu_load
fpu_load:
    ldp   q0, q1, [x0, #0]
    ldp   q2, q3, [x0, #32]
    ldp   q4, q5, [x0, #64]
    ldp   q6, q7, [x0, #96]
    ldp   q8, q9, [x0, #128]
    ldp   q10, q11, [x0, #160]
    ldp   q12, q13, [x0, #192]
    ldp   q14, q15, [x0, #224]
    ldp   q16, q17, [x0, #256]
    ldp   q18, q19, [x0, #288]
    ldp   q20, q21, [x0, #320]
    ldp   q22, q23, [x0, #352]
    ldp   q24, q25, [x0, #384]
    ldp   q26, q27, [x0, #416]
    ldp   q28, q29, [x0, #448]
    ldp   q30, q31, [x0, #480]
    ldr   x1, [x0, #512]
    ldr   x2, [x0, #520]
    msr   fpcr, x1
    msr   fpsr, x2
    ret
