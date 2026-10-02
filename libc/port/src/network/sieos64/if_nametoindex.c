/*
 * if_nametoindex.c - SIEOS: an interface's index by its name (if_nameindex.c).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#define _GNU_SOURCE
#include <net/if.h>
#include <errno.h>
#include <string.h>

hidden int __sieos_ifname(unsigned, char *);

unsigned if_nametoindex(const char *name)
{
	char n[IF_NAMESIZE];
	for (unsigned i = 1; __sieos_ifname(i, n); i++)
		if (!strncmp(n, name, IF_NAMESIZE))
			return i;
	errno = ENXIO;
	return 0;
}
