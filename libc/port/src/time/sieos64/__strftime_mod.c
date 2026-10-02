/*
 * __strftime_mod.c - SIEOS: strftime's E and O modifiers, as glibc has them.
 *
 * musl reads and ignores them.  With a locale of the database (its ERA and
 * ALT_DIGITS, src/locale/sieos64/langinfo.c):
 *   %O followed by a numeric conversion: the number in the locale's
 *      alternative digits (ja_JP's kanji, fa_IR's Persian digits...);
 *   %Ec %Ex %EX: the era's date and time formats; %EC the era's name; %Ey
 *      the year in the era; %EY the era's own representation.
 * Without them, the conversion as without the modifier.  Also glibc's %k, %l
 * and %P, which locales' formats use.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <langinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "locale_impl.h"
#include "../time_impl.h"

/* The n-th of a semicolon-separated list: its start and length, or 0. */
static const char *nth(const char *list, long n, size_t *len)
{
	for (; n > 0 && *list; n--) {
		list = strchr(list, ';');
		if (!list) return 0;
		list++;
	}
	if (n || !*list) return 0;
	*len = strcspn(list, ";");
	return list;
}

/* A field of an era segment (direction:offset:start:end:name:format). */
static const char *field(const char *seg, int i, size_t *len)
{
	for (; i > 0; i--) {
		seg = strchr(seg, ':');
		if (!seg) return 0;
		seg++;
	}
	*len = strcspn(seg, ":;");
	return seg;
}

/* yyyy/mm/dd as a comparable number; -* and +* the ends of time. */
static long long era_date(const char *s, int *year)
{
	if (s[0] == '-' && s[1] == '*') return -(1LL << 62);
	if (s[0] == '+' && s[1] == '*') return 1LL << 62;
	char *e;
	long y = strtol(s, &e, 10), m = 1, d = 1;
	if (*e == '/') m = strtol(e+1, &e, 10);
	if (*e == '/') d = strtol(e+1, &e, 10);
	if (year) *year = y;
	return y * 10000LL + (y < 0 ? -1 : 1) * (m * 100 + d);
}

/* The era tm falls in: its segment, or 0. */
static const char *find_era(const char *eras, const struct tm *tm)
{
	long long t = (tm->tm_year + 1900LL) * 10000 + (tm->tm_mon + 1) * 100 + tm->tm_mday;
	for (const char *seg = eras; seg && *seg; seg = strchr(seg, ';') ? strchr(seg, ';') + 1 : 0) {
		size_t l;
		const char *s = field(seg, 2, &l), *e = field(seg, 3, &l);
		if (!s || !e) continue;
		long long a = era_date(s, 0), b = era_date(e, 0);
		if (a > b) { long long x = a; a = b; b = x; }
		if (t >= a && t <= b) return seg;
	}
	return 0;
}

const char *__strftime_fmt_mod(char (*s)[400], size_t *l, int mod, int f, const struct tm *tm, locale_t loc, int pad)
{
	/* glibc's own conversions, which locales use: %k and %l (the hour, 24 and 12, padded
	 * with a space), %P (am or pm, in lower case) */
	if (f == 'k' || f == 'l') {
		int h = tm->tm_hour;
		if (f == 'l') h = h % 12 ? h % 12 : 12;
		*l = snprintf(*s, sizeof *s, pad == '-' ? "%d" : pad == '0' ? "%02d" : "%2d", h);
		return *s;
	}
	if (f == 'P') {
		const char *ap = __nl_langinfo_l(tm->tm_hour >= 12 ? PM_STR : AM_STR, loc);
		size_t n = strlen(ap);
		if (n >= sizeof *s) n = sizeof *s - 1;
		for (size_t i = 0; i < n; i++)
			(*s)[i] = ap[i] >= 'A' && ap[i] <= 'Z' ? ap[i] + 32 : ap[i];
		(*s)[n] = 0;
		*l = n;
		return *s;
	}
	if (mod == 'O' && strchr("CdeHImMSuUVwWy", f)) {
		const char *alt = __nl_langinfo_l(ALT_DIGITS, loc), *num, *a;
		size_t k, al;
		if (*alt && (num = __strftime_fmt_1(s, &k, f, tm, loc, '-'))) {
			long v = strtol(num, 0, 10);
			if (v >= 0 && (a = nth(alt, v, &al)) && al < sizeof *s) {
				memcpy(*s, a, al);
				(*s)[al] = 0;
				*l = al;
				return *s;
			}
		}
	}
	if (mod == 'E' && strchr("cCxXyY", f)) {
		const char *eras = __nl_langinfo_l(ERA, loc), *seg;
		if (*eras && (seg = find_era(eras, tm))) {
			size_t fl;
			const char *fmt = 0, *p;
			char tmp[400];
			switch (f) {
			case 'c': fmt = __nl_langinfo_l(ERA_D_T_FMT, loc); break;
			case 'x': fmt = __nl_langinfo_l(ERA_D_FMT, loc); break;
			case 'X': fmt = __nl_langinfo_l(ERA_T_FMT, loc); break;
			case 'C':
				if (!(p = field(seg, 4, &fl)) || fl >= sizeof *s) break;
				memcpy(*s, p, fl);
				(*s)[fl] = 0;
				*l = fl;
				return *s;
			case 'y': {
				int start;
				size_t ol;
				const char *dir = field(seg, 0, &fl), *off = field(seg, 1, &ol), *st = field(seg, 2, &fl);
				if (!dir || !off || !st) break;
				era_date(st, &start);
				long y = tm->tm_year + 1900L, o = strtol(off, 0, 10);
				long ey = *dir == '-' ? start - y + o : y - start + o;
				*l = snprintf(*s, sizeof *s, "%ld", ey);
				return *s;
			}
			case 'Y':
				if (!(p = field(seg, 5, &fl)) || fl >= sizeof tmp) break;
				memcpy(tmp, p, fl);
				tmp[fl] = 0;
				fmt = tmp;
				break;
			}
			if (fmt && *fmt) {
				if (fmt != tmp) {
					strncpy(tmp, fmt, sizeof tmp - 1);
					tmp[sizeof tmp - 1] = 0;
				}
				*l = __strftime_l(*s, sizeof *s, tmp, tm, loc);
				return *l ? *s : 0;
			}
		}
	}
	return __strftime_fmt_1(s, l, f, tm, loc, pad);
}
