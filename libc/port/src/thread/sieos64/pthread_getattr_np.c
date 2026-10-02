/*
 * pthread_getattr_np - SIEOS: a thread's attributes, its stack included.
 *
 * musl measures the main thread's stack by probing with mremap() until
 * Linux answers ENOMEM below the stack; SIEOS's mremap does not answer so,
 * and the stack came out as one page (Python 3.14, which checks its stack
 * depth against it, stopped at once: "stack overflow").  On SIEOS the main
 * stack is the region the kernel reserves below the top of the address
 * space, RLIMIT_STACK long (8 MiB, demand-paged), whose top 128 KiB at most
 * hold the arguments and environment: the stack is reported from the
 * arguments' page down, that size less those 128 KiB.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#define _GNU_SOURCE
#include "pthread_impl.h"
#include "libc.h"
#include <sys/resource.h>

#define ARGS_AREA (128 * 1024)                 /* the kernel's USER_STACK_INIT */

int pthread_getattr_np(pthread_t t, pthread_attr_t *a)
{
	*a = (pthread_attr_t){0};
	a->_a_detach = t->detach_state>=DT_DETACHED;
	a->_a_guardsize = t->guard_size;
	if (t->stack) {
		a->_a_stackaddr = (uintptr_t)t->stack;
		a->_a_stacksize = t->stack_size;
	} else {
		char *p = (void *)libc.auxv;
		p += -(uintptr_t)p & PAGE_SIZE-1;
		size_t l = 8 * 1024 * 1024;
		struct rlimit rl;
		if (!getrlimit(RLIMIT_STACK, &rl) && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur > 2 * ARGS_AREA)
			l = rl.rlim_cur;
		a->_a_stackaddr = (uintptr_t)p;
		a->_a_stacksize = l - ARGS_AREA;
	}
	return 0;
}
