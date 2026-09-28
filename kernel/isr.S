/*
 * isr.S - Interrupt entry stubs, trap return and context switch.
 */

.macro ISR_NOERR n
    .global isr\n
isr\n:
    pushq $0
    pushq $\n
    jmp isr_common
.endm

.macro ISR_ERR n
    .global isr\n
isr\n:
    pushq $\n
    jmp isr_common
.endm

    .altmacro
.macro ISR_REF n
    .quad isr\n
.endm

    .text
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31
.set i, 32
.rept 16
    ISR_NOERR %i
    .set i, i + 1
.endr
ISR_NOERR 128
ISR_NOERR 64
ISR_NOERR 65
ISR_NOERR 66
ISR_NOERR 255

/*
 * Stack at entry: int_no, err_code, rip, cs, rflags, rsp, ss.
 * When coming from user mode, swapgs makes %gs point at this CPU's
 * struct cpu (and stashes the user GS base in KERNEL_GS_BASE).
 */
isr_common:
    cld
    testb $3, 24(%rsp)
    jz isr_save
    swapgs
isr_save:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    call trap_handler
    .global trapret
trapret:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp          /* int_no, err_code */
    testb $3, 8(%rsp)       /* returning to user mode? */
    jz 2f
    swapgs
2:
    iretq

/*
 * ABI v2 entry: the syscall instruction (rcx = user rip, r11 = user rflags,
 * interrupts masked by FMASK).  Switch to the process's kernel stack and
 * build the same frame an interrupt would, so the return path, signals and
 * restarts are shared; the return is always iretq.
 */
    .global syscall_entry
syscall_entry:
    swapgs
    movq %rsp, %gs:8
    movq %gs:16, %rsp
    pushq $0x1b                  /* ss: USER_DS */
    pushq %gs:8                  /* rsp */
    pushq %r11                   /* rflags */
    pushq $0x23                  /* cs: USER_CS */
    pushq %rcx                   /* rip */
    pushq $0                     /* err_code */
    pushq $0x81                  /* int_no: T_SYSCALL2 */
    cld
    jmp isr_save

/* First run of a new process: drop the big kernel lock, then return to user. */
    .global forkret
forkret:
    call lwp_first_run
    call bkl_unlock
    jmp trapret

/*
 * void switch_context(uint64_t *old_rsp, uint64_t new_rsp)
 * Saves callee-saved registers on the current stack, stores rsp in
 * *old_rsp, switches to new_rsp and restores registers from there.
 */
    .global switch_context
switch_context:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

    .section .rodata
    .align 8
    .global isr_table
isr_table:
.set i, 0
.rept 48
    ISR_REF %i
    .set i, i + 1
.endr

    .section .note.GNU-stack, "", @progbits
