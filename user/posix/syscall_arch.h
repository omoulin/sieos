/*
 * syscall_arch.h - musl's system calls on SIEOS (docs/webkit-plan.md): the
 * one musl file replaced (for x86_64 and aarch64 alike). A Linux system
 * call does not trap into a Linux kernel: it calls the Linux personality,
 * __sieos_syscall (linux.c), which does it with SIEOS's own services.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

long __sieos_syscall(long n, long a, long b, long c, long d, long e, long f);

static __inline long __syscall0(long n) { return __sieos_syscall(n, 0, 0, 0, 0, 0, 0); }
static __inline long __syscall1(long n, long a) { return __sieos_syscall(n, a, 0, 0, 0, 0, 0); }
static __inline long __syscall2(long n, long a, long b) { return __sieos_syscall(n, a, b, 0, 0, 0, 0); }
static __inline long __syscall3(long n, long a, long b, long c) { return __sieos_syscall(n, a, b, c, 0, 0, 0); }
static __inline long __syscall4(long n, long a, long b, long c, long d) { return __sieos_syscall(n, a, b, c, d, 0, 0); }
static __inline long __syscall5(long n, long a, long b, long c, long d, long e) { return __sieos_syscall(n, a, b, c, d, e, 0); }
static __inline long __syscall6(long n, long a, long b, long c, long d, long e, long f) { return __sieos_syscall(n, a, b, c, d, e, f); }

#define IPC_64 0
