/*
 * syscall_cp.c - musl's cancellable system calls on SIEOS (replaces
 * src/thread/ARCH/syscall_cp.s). A cancellation already asked is acted on
 * before the call; one asked during a blocking call takes effect at the
 * next cancellation point (SIEOS sends no cancel signal), so the labels
 * musl's signal handler compares the interrupted address with are empty.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "pthread_impl.h"
#include "syscall.h"

hidden long __cancel(void);

__asm__(".text\n.global __cp_begin\n.hidden __cp_begin\n.global __cp_end\n.hidden __cp_end\n"
        ".global __cp_cancel\n.hidden __cp_cancel\n__cp_begin:\n__cp_end:\n__cp_cancel:\n");

hidden long __syscall_cp_asm(volatile int *cancel, syscall_arg_t nr, syscall_arg_t u, syscall_arg_t v,
                             syscall_arg_t w, syscall_arg_t x, syscall_arg_t y, syscall_arg_t z)
{
    if (*cancel) return __cancel();
    return __syscall6(nr, u, v, w, x, y, z);
}
