/*
 * langinfo.c - SIEOS: nl_langinfo from the locale database.
 *
 * An item of a category set to a locale of the database (locale_map.c) is
 * that locale's: the catalog's key "@CAT.INDEX" (tools/mklocales.py); lists
 * (ALT_DIGITS, ERA) are semicolon-separated, as POSIX has them.  Otherwise,
 * the C locale's, as musl gives them.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <locale.h>
#include <langinfo.h>
#include "locale_impl.h"

static const char c_time[] =
	"Sun\0" "Mon\0" "Tue\0" "Wed\0" "Thu\0" "Fri\0" "Sat\0"
	"Sunday\0" "Monday\0" "Tuesday\0" "Wednesday\0"
	"Thursday\0" "Friday\0" "Saturday\0"
	"Jan\0" "Feb\0" "Mar\0" "Apr\0" "May\0" "Jun\0"
	"Jul\0" "Aug\0" "Sep\0" "Oct\0" "Nov\0" "Dec\0"
	"January\0"   "February\0" "March\0"    "April\0"
	"May\0"       "June\0"     "July\0"     "August\0"
	"September\0" "October\0"  "November\0" "December\0"
	"AM\0" "PM\0"
	"%a %b %e %T %Y\0"
	"%m/%d/%y\0"
	"%H:%M:%S\0"
	"%I:%M:%S %p\0"
	"\0"
	"\0"
	"%m/%d/%y\0"
	"\0"                  /* ALT_DIGITS: none (glibc's C locale) */
	"%a %b %e %T %Y\0"
	"%H:%M:%S";

static const char c_messages[] = "^[yY]\0" "^[nN]\0" "yes\0" "no";
static const char c_numeric[] = ".\0" "";

/* The database's value of item idx of category cat, or 0. */
static const char *db_item(const struct __locale_map *lm, int cat, int idx)
{
	char key[16], *k = key + sizeof key;
	if (!lm || !lm->map) return 0;
	*--k = 0;
	do *--k = '0' + idx % 10; while (idx /= 10);
	*--k = '.';
	*--k = '0' + cat;
	*--k = '@';
	return __mo_lookup(lm->map, lm->map_size, k);
}

char *__nl_langinfo_l(nl_item item, locale_t loc)
{
	int cat = item >> 16;
	int idx = item & 65535;
	const char *str;

	if (item == CODESET) return loc->cat[LC_CTYPE] ? "UTF-8" : "ASCII";

	/* _NL_LOCALE_NAME extension */
	if (idx == 65535 && cat < LC_ALL)
		return loc->cat[cat] ? (char *)loc->cat[cat]->name : "C";

	if (cat < LC_ALL && cat != LC_CTYPE && (str = db_item(loc->cat[cat], cat, idx)))
		return (char *)str;

	switch (cat) {
	case LC_NUMERIC:
		if (idx > 1) return "";
		str = c_numeric;
		break;
	case LC_TIME:
		if (idx > 0x31) return "";
		str = c_time;
		break;
	case LC_MONETARY:
		if (idx > 0) return "";
		str = "";
		break;
	case LC_MESSAGES:
		if (idx > 3) return "";
		str = c_messages;
		break;
	default:
		return "";
	}

	for (; idx; idx--, str++) for (; *str; str++);
	return (char *)str;
}

char *__nl_langinfo(nl_item item)
{
	return __nl_langinfo_l(item, CURRENT_LOCALE);
}

weak_alias(__nl_langinfo, nl_langinfo);
weak_alias(__nl_langinfo_l, nl_langinfo_l);
