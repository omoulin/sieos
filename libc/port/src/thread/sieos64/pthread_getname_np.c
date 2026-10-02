/*
 * pthread_getname_np - SIEOS: a thread's name (see pthread_setname_np.c).
 * A thread not named yet has the program's, as on Linux.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#define _GNU_SOURCE
#include <string.h>
#include "pthread_impl.h"

extern char *__progname;

int pthread_getname_np(pthread_t thread, char *name, size_t len)
{
	const char *s = thread->name[0] ? thread->name : __progname ? __progname : "";
	size_t n = strnlen(s, sizeof thread->name - 1);
	if (len < sizeof thread->name) return ERANGE;
	memcpy(name, s, n);
	name[n] = 0;
	return 0;
}
