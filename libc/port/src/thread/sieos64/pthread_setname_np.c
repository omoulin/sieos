/*
 * pthread_setname_np - SIEOS: a thread's name.
 *
 * musl names threads through Linux's prctl(PR_SET_NAME) and
 * /proc/self/task/TID/comm, which SIEOS does not have: the name is kept in
 * the thread's own structure (pthread_impl.h, name), for
 * pthread_getname_np.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#define _GNU_SOURCE
#include <string.h>
#include "pthread_impl.h"

int pthread_setname_np(pthread_t thread, const char *name)
{
	size_t len = strnlen(name, sizeof thread->name);
	if (len >= sizeof thread->name) return ERANGE;
	memcpy(thread->name, name, len);
	thread->name[len] = 0;
	return 0;
}
