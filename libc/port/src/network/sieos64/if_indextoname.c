/*
 * if_indextoname.c - SIEOS: an interface's name by its index (if_nameindex.c).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#define _GNU_SOURCE
#include <net/if.h>
#include <errno.h>

hidden int __sieos_ifname(unsigned, char *);

char *if_indextoname(unsigned index, char *name)
{
	if (!__sieos_ifname(index, name)) {
		errno = ENXIO;
		return 0;
	}
	return name;
}
