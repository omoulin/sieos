/*
 * strxfrm.c - SIEOS: sort keys by the locale's collation (__sieos_coll.c),
 * the string itself in the C locale.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <string.h>
#include <locale.h>
#include "locale_impl.h"
#include "sieos_coll.h"

size_t __strxfrm_l(char *restrict dest, const char *restrict src, size_t n, locale_t loc)
{
	if (__sieos_coll_on(loc)) {
		size_t l = __sieos_coll_key(src, 0, (unsigned char *)dest, n);
		if (l < n) dest[l] = 0;
		return l;
	}
	size_t l = strlen(src);
	if (n > l) strcpy(dest, src);
	return l;
}

size_t strxfrm(char *restrict dest, const char *restrict src, size_t n)
{
	return __strxfrm_l(dest, src, n, CURRENT_LOCALE);
}

weak_alias(__strxfrm_l, strxfrm_l);
