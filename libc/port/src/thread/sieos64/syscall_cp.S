/*
 * Cancellable system calls.  Everything between __cp_begin and __cp_end
 * may be redirected to __cp_cancel by the cancellation signal handler.
 * FUTEX_WAIT (the waits of condition variables and semaphores) goes to
 * lwp_umtx_wait inside that range; other emulated calls run in C.
 */
#include "sieos/syscall.h"
#include "sieos_emu.h"

.text
.global __cp_begin
.hidden __cp_begin
.global __cp_end
.hidden __cp_end
.global __cp_cancel
.hidden __cp_cancel
.hidden __cancel
.hidden __sieos_emu
.global __syscall_cp_asm
.hidden __syscall_cp_asm
.type   __syscall_cp_asm,@function
__syscall_cp_asm:

__cp_begin:
	mov (%rdi),%eax
	test %eax,%eax
	jnz __cp_cancel
	cmp $__EMU_futex,%rsi
	jne 2f
	mov %ecx,%eax
	and $127,%eax
	jnz 3f
	mov %rdx,%rdi                   /* lwp_umtx_wait(addr, val, timeout, flags) */
	mov %r8,%rsi
	mov %r9,%rdx
	mov %ecx,%r10d
	shr $7,%r10d
	and $1,%r10d                    /* FUTEX_PRIVATE -> SIEOS_UMTX_PRIVATE */
	mov $SIEOS_SYS_lwp_umtx_wait,%eax
	jmp 1f
2:	cmp $__SIEOS_EMU_BASE,%rsi
	jae 3f
	mov %rdi,%r11
	mov %rsi,%rax
	mov %rdx,%rdi
	mov %rcx,%rsi
	mov %r8,%rdx
	mov %r9,%r10
	mov 8(%rsp),%r8
	mov 16(%rsp),%r9
	mov %r11,8(%rsp)
1:	syscall
__cp_end:
	jnc 4f
	neg %rax
4:	ret
3:	mov %rsi,%rdi                   /* __sieos_emu(nr, u, v, w, x, y, z) */
	mov %rdx,%rsi
	mov %rcx,%rdx
	mov %r8,%rcx
	mov %r9,%r8
	mov 8(%rsp),%r9
	mov 16(%rsp),%rax
	push %rax
	call __sieos_emu
	add $8,%rsp
	ret
__cp_cancel:
	jmp __cancel
