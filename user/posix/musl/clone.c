/*
 * clone.c - musl's thread start on SIEOS (replaces src/thread/ARCH/clone.s):
 * only threads (CLONE_VM | CLONE_THREAD), started by the Linux personality
 * (linux.c) with SYS_THREAD_CREATE; no fork-like clones (-ENOSYS).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdarg.h>
#include "pthread_impl.h"

long __sieos_clone(int (*fn)(void *), void *stack, int flags, void *arg, int *ptid, void *tls, int *ctid);

int __clone(int (*fn)(void *), void *stack, int flags, void *arg, ...)
{
    va_list ap;
    va_start(ap, arg);
    int *ptid = va_arg(ap, int *);
    void *tls = va_arg(ap, void *);
    int *ctid = va_arg(ap, int *);
    va_end(ap);
    return __sieos_clone(fn, stack, flags, arg, ptid, tls, ctid);
}
