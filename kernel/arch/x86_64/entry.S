/*
 * entry.S - Every way into and out of the kernel:
 *   - interrupts and CPU exceptions: isr_N -> isr_common -> trap() (cpu.c)
 *   - system calls:  SYSCALL -> syscall_entry -> syscall() (syscall.c)
 *   - switching from one task to another: switch_to (task.c)
 * Both entries save the program's registers in the same layout, regs_t in
 * kernel.h, at the top of the running task's kernel stack.
 *
 * GS: in kernel mode, GS's base points at this CPU's cpu_t (kernel.h).
 * User programs have their own GS, so coming from user mode we exchange
 * the two with SWAPGS, and exchange them back on the way out.
 *
 * The kernel lock: trap() and syscall() take it; kernel_exit() (task.c)
 * gives it back just before the registers are restored. (trap() returns 0
 * when it did not take it: NMIs and TLB shootdowns.) A task switch
 * happens with the lock held, and the task that resumes releases it.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
    .text
    .code64

.macro PUSHALL                          /* regs_t, from rax (highest) to r15 */
    push %rax; push %rbx; push %rcx; push %rdx; push %rsi; push %rdi; push %rbp
    push %r8;  push %r9;  push %r10; push %r11; push %r12; push %r13; push %r14; push %r15
.endm
.macro POPALL
    pop %r15; pop %r14; pop %r13; pop %r12; pop %r11; pop %r10; pop %r9; pop %r8
    pop %rbp; pop %rdi; pop %rsi; pop %rdx; pop %rcx; pop %rbx; pop %rax
.endm

/* ---- Interrupts. On an interrupt the CPU switches to the kernel stack
 * (TSS.rsp0, set by cpu.c for the running task) and pushes ss, rsp, rflags,
 * cs, rip, and for some exceptions an error code. Each stub pushes a 0 for
 * the others, then its vector number, so the frame is always the same. */
.macro ISR n
isr_\n:
    .if !(\n == 8 || (\n >= 10 && \n <= 14) || \n == 17 || \n == 21 || \n == 29 || \n == 30)
    push $0
    .endif
    push $\n
    jmp  isr_common
.endm
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,255
    ISR \n
.endr

isr_common:
    testb $3, 24(%rsp)                  /* the interrupted cs: from user mode? */
    jz   1f
    swapgs
1:  PUSHALL
    mov  %rsp, %rdi                     /* trap(regs_t *) */
    call trap
    test %eax, %eax                     /* did trap() take the kernel lock? */
    jz   3f
    .global trap_return
trap_return:                            /* new threads also start here (task.c) */
    call kernel_exit
3:  POPALL
    add  $16, %rsp                      /* vector and error code */
    testb $3, 8(%rsp)                   /* going back to user mode? */
    jz   2f
    swapgs
2:  iretq

/* ---- System calls. SYSCALL jumps here in kernel mode but does not switch
 * stacks: it leaves the return address in rcx and the flags in r11. We
 * switch to the task's kernel stack (from this CPU's cpu_t), build the same
 * frame as an interrupt, and return with SYSRET, the fast way back to user
 * mode. Interrupts stay off meanwhile (cpu.c masks IF on SYSCALL). */
    .global syscall_entry
syscall_entry:
    swapgs
    mov  %rsp, %gs:16                   /* cpu_t.user_rsp */
    mov  %gs:8, %rsp                    /* cpu_t.kstack_top */
    push $0x1B                          /* ss: user data */
    push %gs:16                         /* rsp */
    push %r11                           /* rflags */
    push $0x23                          /* cs: user code */
    push %rcx                           /* rip */
    push $0                             /* error code */
    push $0                             /* vector */
    PUSHALL
    mov  %rsp, %rdi                     /* syscall(regs_t *) */
    call syscall
    call kernel_exit
    POPALL
    add  $16, %rsp
    pop  %rcx                           /* rip, for SYSRET */
    add  $8, %rsp
    pop  %r11                           /* rflags, for SYSRET */
    pop  %rsp                           /* the user's stack */
    swapgs
    sysretq

/* ---- switch_to(&old->ksp, new->ksp): save the registers C expects to
 * survive a call on the old task's kernel stack, remember that stack,
 * switch to the new one and restore its registers. The "ret" then returns
 * wherever the new task was when it called switch_to, or to trap_return
 * for a task that never ran. */
    .global switch_to
switch_to:
    push %rbp; push %rbx; push %r12; push %r13; push %r14; push %r15
    mov  %rsp, (%rdi)
    mov  %rsi, %rsp
    pop  %r15; pop %r14; pop %r13; pop %r12; pop %rbx; pop %rbp
    ret

    .section .rodata
    .global isr_table                   /* the stubs' addresses, for the IDT */
isr_table:                              /* vectors 0-127, then 255 */
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,255
    .quad isr_\n
.endr
