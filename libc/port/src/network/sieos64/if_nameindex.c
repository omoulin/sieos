/*
 * if_nameindex.c - SIEOS: the network interfaces' names and indexes.
 *
 * musl asks Linux's netlink; SIEOS's kernel lists its interfaces with
 * netinfo (sieos/sysinfo.h): lo is index 1, eth0, eth1... (netinfo's 0,
 * 1...) are 2, 3... - the indexes IPv6 scope ids and IPV6_MULTICAST_IF use.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#define _GNU_SOURCE
#include <net/if.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "syscall.h"
#include "sieos/abi.h"

/* Interface index i's name into name (IF_NAMESIZE); 0 if there is none. */
hidden int __sieos_ifname(unsigned i, char *name)
{
	if (i == 1) {
		strcpy(name, "lo");
		return 1;
	}
	struct sieos_netinfo ni;
	if (i < 2 || __syscall(SIEOS_SYS_netinfo, &ni, (long)(i - 2)) < 0)
		return 0;
	memcpy(name, ni.name, sizeof ni.name);
	name[sizeof ni.name < IF_NAMESIZE ? sizeof ni.name : IF_NAMESIZE - 1] = 0;
	return 1;
}

struct if_nameindex *if_nameindex()
{
	char name[IF_NAMESIZE];
	unsigned n = 0;
	while (__sieos_ifname(n + 1, name))
		n++;
	struct if_nameindex *r = malloc((n + 1) * sizeof *r + n * IF_NAMESIZE);
	if (!r) {
		errno = ENOBUFS;
		return 0;
	}
	char *names = (char *)(r + n + 1);
	for (unsigned i = 0; i < n; i++) {
		r[i].if_index = i + 1;
		r[i].if_name = names + i * IF_NAMESIZE;
		__sieos_ifname(i + 1, r[i].if_name);
	}
	r[n].if_index = 0;
	r[n].if_name = 0;
	return r;
}
