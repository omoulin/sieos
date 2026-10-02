/*
 * wcsxfrm.c - SIEOS: wide sort keys by the locale's collation
 * (__sieos_coll.c: a key's bytes, one to a wide character).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <stdlib.h>
#include <wchar.h>
#include <locale.h>
#include "locale_impl.h"
#include "sieos_coll.h"

size_t __wcsxfrm_l(wchar_t *restrict dest, const wchar_t *restrict src, size_t n, locale_t loc)
{
	if (__sieos_coll_on(loc)) {
		size_t l = __sieos_coll_key(src, 1, 0, 0);
		if (l < n) {
			unsigned char *k = malloc(l ? l : 1);
			if (!k) return (size_t)-1;
			__sieos_coll_key(src, 1, k, l);
			for (size_t i = 0; i < l; i++) dest[i] = k[i];
			dest[l] = 0;
			free(k);
		}
		return l;
	}
	size_t l = wcslen(src);
	if (l < n) {
		wmemcpy(dest, src, l+1);
	} else if (n) {
		wmemcpy(dest, src, n-1);
		dest[n-1] = 0;
	}
	return l;
}

size_t wcsxfrm(wchar_t *restrict dest, const wchar_t *restrict src, size_t n)
{
	return __wcsxfrm_l(dest, src, n, CURRENT_LOCALE);
}

weak_alias(__wcsxfrm_l, wcsxfrm_l);
