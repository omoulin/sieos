/*
 * SIEOS ABI v2 system calls: the syscall instruction, carry flag set on
 * failure with a positive errno in rax (converted here to musl's -errno),
 * rdx possibly clobbered (second return value).  Numbers from
 * __SIEOS_EMU_BASE up are Linux calls emulated in libc (src/sieos/emu.c);
 * from __SIEOS_NOSYS_BASE up they do not exist (ENOSYS).
 */
#include "sieos_emu.h"

#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)
#define SYSCALL_RLIM_INFINITY (-3ULL)

hidden long __sieos_emu(long, long, long, long, long, long, long);

#define __SIEOS_RET(ret, cf) ((cf) ? -(long)(ret) : (long)(ret))

static __inline long __syscall0(long n)
{
	unsigned long ret;
	unsigned char cf;
	if (n >= __SIEOS_EMU_BASE) return __sieos_emu(n, 0, 0, 0, 0, 0, 0);
	__asm__ __volatile__ ("syscall; setc %1" : "=a"(ret), "=r"(cf) : "a"(n)
			      : "rcx", "r11", "rdx", "memory", "cc");
	return __SIEOS_RET(ret, cf);
}

static __inline long __syscall1(long n, long a1)
{
	unsigned long ret;
	unsigned char cf;
	if (n >= __SIEOS_EMU_BASE) return __sieos_emu(n, a1, 0, 0, 0, 0, 0);
	__asm__ __volatile__ ("syscall; setc %1" : "=a"(ret), "=r"(cf) : "a"(n), "D"(a1)
			      : "rcx", "r11", "rdx", "memory", "cc");
	return __SIEOS_RET(ret, cf);
}

static __inline long __syscall2(long n, long a1, long a2)
{
	unsigned long ret;
	unsigned char cf;
	if (n >= __SIEOS_EMU_BASE) return __sieos_emu(n, a1, a2, 0, 0, 0, 0);
	__asm__ __volatile__ ("syscall; setc %1" : "=a"(ret), "=r"(cf) : "a"(n), "D"(a1), "S"(a2)
			      : "rcx", "r11", "rdx", "memory", "cc");
	return __SIEOS_RET(ret, cf);
}

static __inline long __syscall3(long n, long a1, long a2, long a3)
{
	unsigned long ret;
	unsigned char cf;
	if (n >= __SIEOS_EMU_BASE) return __sieos_emu(n, a1, a2, a3, 0, 0, 0);
	__asm__ __volatile__ ("syscall; setc %1" : "=a"(ret), "=r"(cf), "+d"(a3) : "a"(n), "D"(a1), "S"(a2)
			      : "rcx", "r11", "memory", "cc");
	return __SIEOS_RET(ret, cf);
}

static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{
	unsigned long ret;
	unsigned char cf;
	register long r10 __asm__("r10") = a4;
	if (n >= __SIEOS_EMU_BASE) return __sieos_emu(n, a1, a2, a3, a4, 0, 0);
	__asm__ __volatile__ ("syscall; setc %1" : "=a"(ret), "=r"(cf), "+d"(a3) : "a"(n), "D"(a1), "S"(a2),
			      "r"(r10) : "rcx", "r11", "memory", "cc");
	return __SIEOS_RET(ret, cf);
}

static __inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
	unsigned long ret;
	unsigned char cf;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	if (n >= __SIEOS_EMU_BASE) return __sieos_emu(n, a1, a2, a3, a4, a5, 0);
	__asm__ __volatile__ ("syscall; setc %1" : "=a"(ret), "=r"(cf), "+d"(a3) : "a"(n), "D"(a1), "S"(a2),
			      "r"(r10), "r"(r8) : "rcx", "r11", "memory", "cc");
	return __SIEOS_RET(ret, cf);
}

static __inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	unsigned long ret;
	unsigned char cf;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	register long r9 __asm__("r9") = a6;
	if (n >= __SIEOS_EMU_BASE) return __sieos_emu(n, a1, a2, a3, a4, a5, a6);
	__asm__ __volatile__ ("syscall; setc %1" : "=a"(ret), "=r"(cf), "+d"(a3) : "a"(n), "D"(a1), "S"(a2),
			      "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory", "cc");
	return __SIEOS_RET(ret, cf);
}

#define IPC_64 0

/* socket timeouts are already timevals: no time64 conversion */
#define SO_RCVTIMEO_OLD 0x1006
#define SO_SNDTIMEO_OLD 0x1005
