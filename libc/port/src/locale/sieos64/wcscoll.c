/*
 * wcscoll.c - SIEOS: wcscoll by the locale's collation (__sieos_coll.c).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <wchar.h>
#include <locale.h>
#include "locale_impl.h"
#include "sieos_coll.h"

int __wcscoll_l(const wchar_t *l, const wchar_t *r, locale_t locale)
{
	return __sieos_coll_on(locale) ? __sieos_coll_cmp(l, r, 1) : wcscmp(l, r);
}

int wcscoll(const wchar_t *l, const wchar_t *r)
{
	return __wcscoll_l(l, r, CURRENT_LOCALE);
}

weak_alias(__wcscoll_l, wcscoll_l);
