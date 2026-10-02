/*
 * strcoll.c - SIEOS: strcoll by the locale's collation (__sieos_coll.c),
 * byte order in the C locale.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <string.h>
#include <locale.h>
#include "locale_impl.h"
#include "sieos_coll.h"

int __strcoll_l(const char *l, const char *r, locale_t loc)
{
	return __sieos_coll_on(loc) ? __sieos_coll_cmp(l, r, 0) : strcmp(l, r);
}

int strcoll(const char *l, const char *r)
{
	return __strcoll_l(l, r, CURRENT_LOCALE);
}

weak_alias(__strcoll_l, strcoll_l);
