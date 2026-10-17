/*
 * __set_thread_area.c - the thread pointer on SIEOS (replaces musl's
 * src/thread/ARCH/__set_thread_area.*): the kernel keeps it per thread
 * (SYS_SET_FS: x86-64's FS base, arm64's TPIDR_EL0), through linux.c.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "pthread_impl.h"

long __sieos_set_tp(void *p);

int __set_thread_area(void *p) { return __sieos_set_tp(p); }
