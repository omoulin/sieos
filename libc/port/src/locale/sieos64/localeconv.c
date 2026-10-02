/*
 * localeconv.c - SIEOS: struct lconv from the locale database.
 *
 * LC_NUMERIC's fields (decimal_point, thousands_sep, grouping) and
 * LC_MONETARY's come from the locale each category is set to (the catalog's
 * "@lc.FIELD" keys, tools/mklocales.py), else the C locale's.  printf and
 * strtod keep the C locale's decimal point, as musl's.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <locale.h>
#include <limits.h>
#include <stdlib.h>
#include "locale_impl.h"

static const struct lconv posix_lconv = {
	.decimal_point = ".",
	.thousands_sep = "",
	.grouping = "",
	.int_curr_symbol = "",
	.currency_symbol = "",
	.mon_decimal_point = "",
	.mon_thousands_sep = "",
	.mon_grouping = "",
	.positive_sign = "",
	.negative_sign = "",
	.int_frac_digits = CHAR_MAX,
	.frac_digits = CHAR_MAX,
	.p_cs_precedes = CHAR_MAX,
	.p_sep_by_space = CHAR_MAX,
	.n_cs_precedes = CHAR_MAX,
	.n_sep_by_space = CHAR_MAX,
	.p_sign_posn = CHAR_MAX,
	.n_sign_posn = CHAR_MAX,
	.int_p_cs_precedes = CHAR_MAX,
	.int_p_sep_by_space = CHAR_MAX,
	.int_n_cs_precedes = CHAR_MAX,
	.int_n_sep_by_space = CHAR_MAX,
	.int_p_sign_posn = CHAR_MAX,
	.int_n_sign_posn = CHAR_MAX,
};

static struct lconv lc;

static char *str(const struct __locale_map *lm, const char *key, char *dflt)
{
	const char *s = lm && lm->map ? __mo_lookup(lm->map, lm->map_size, key) : 0;
	return s ? (char *)s : dflt;
}

static char num(const struct __locale_map *lm, const char *key)
{
	const char *s = lm && lm->map ? __mo_lookup(lm->map, lm->map_size, key) : 0;
	int v = s ? atoi(s) : -1;
	return v < 0 || v > CHAR_MAX ? CHAR_MAX : v;
}

struct lconv *localeconv(void)
{
	locale_t loc = CURRENT_LOCALE;
	const struct __locale_map *n = loc->cat[LC_NUMERIC], *m = loc->cat[LC_MONETARY];
	if (!n && !m) return (void *)&posix_lconv;
	lc = posix_lconv;
	lc.decimal_point = str(n, "@lc.decimal_point", ".");
	lc.thousands_sep = str(n, "@lc.thousands_sep", "");
	lc.grouping = str(n, "@lc.grouping", "");
	if (!m) return &lc;
	lc.int_curr_symbol = str(m, "@lc.int_curr_symbol", "");
	lc.currency_symbol = str(m, "@lc.currency_symbol", "");
	lc.mon_decimal_point = str(m, "@lc.mon_decimal_point", "");
	lc.mon_thousands_sep = str(m, "@lc.mon_thousands_sep", "");
	lc.mon_grouping = str(m, "@lc.mon_grouping", "");
	lc.positive_sign = str(m, "@lc.positive_sign", "");
	lc.negative_sign = str(m, "@lc.negative_sign", "");
	lc.int_frac_digits = num(m, "@lc.int_frac_digits");
	lc.frac_digits = num(m, "@lc.frac_digits");
	lc.p_cs_precedes = num(m, "@lc.p_cs_precedes");
	lc.p_sep_by_space = num(m, "@lc.p_sep_by_space");
	lc.n_cs_precedes = num(m, "@lc.n_cs_precedes");
	lc.n_sep_by_space = num(m, "@lc.n_sep_by_space");
	lc.p_sign_posn = num(m, "@lc.p_sign_posn");
	lc.n_sign_posn = num(m, "@lc.n_sign_posn");
	lc.int_p_cs_precedes = num(m, "@lc.int_p_cs_precedes");
	lc.int_p_sep_by_space = num(m, "@lc.int_p_sep_by_space");
	lc.int_n_cs_precedes = num(m, "@lc.int_n_cs_precedes");
	lc.int_n_sep_by_space = num(m, "@lc.int_n_sep_by_space");
	lc.int_p_sign_posn = num(m, "@lc.int_p_sign_posn");
	lc.int_n_sign_posn = num(m, "@lc.int_n_sign_posn");
	return &lc;
}
