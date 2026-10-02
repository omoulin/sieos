/*
 * locale_map.c - SIEOS: the locales a program can set.
 *
 * musl's own: any name is accepted, with the C locale's data.  SIEOS's: the
 * locales of its database (/usr/lib/locale/NAME, tools/mklocales.py: gettext
 * catalogs with the locales' numbers, money, dates, times and collation),
 * UTF-8 ones: LANG_TERRITORY[.UTF-8][@MODIFIER].  A name without data, or
 * with another character set (en_US.ISO-8859-1), is refused, as glibc does:
 * setlocale returns NULL.  MUSL_LOCPATH, if set, is searched first.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <locale.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <stdlib.h>
#include "locale_impl.h"
#include "libc.h"
#include "lock.h"
#include "fork_impl.h"

#define malloc __libc_malloc
#define calloc undef
#define realloc undef
#define free undef

#define SIEOS_LOCALE_DIR "/usr/lib/locale"

const char *__lctrans_impl(const char *msg, const struct __locale_map *lm)
{
	const char *trans = 0;
	if (lm) trans = __mo_lookup(lm->map, lm->map_size, msg);
	return trans ? trans : msg;
}

static const char envvars[][12] = {
	"LC_CTYPE",
	"LC_NUMERIC",
	"LC_TIME",
	"LC_COLLATE",
	"LC_MONETARY",
	"LC_MESSAGES",
};

volatile int __locale_lock[1];
volatile int *const __locale_lockptr = __locale_lock;

static int is_utf8(const char *s, size_t n)
{
	return (n == 5 && !strncasecmp(s, "UTF-8", 5)) || (n == 4 && !strncasecmp(s, "UTF8", 4));
}

/* The database's file in dir: buf; 0 if it does not fit. */
static int file_name(char *buf, size_t size, const char *dir, size_t dl,
	const char *lang, size_t ll, const char *mod, size_t ml)
{
	if (dl + 1 + ll + (ml ? ml + 1 : 0) + 1 > size) return 0;
	memcpy(buf, dir, dl);
	buf[dl] = '/';
	memcpy(buf+dl+1, lang, ll);
	size_t l = dl+1+ll;
	if (ml) {
		buf[l++] = '@';
		memcpy(buf+l, mod, ml);
		l += ml;
	}
	buf[l] = 0;
	return 1;
}

const struct __locale_map *__get_locale(int cat, const char *val)
{
	static void *volatile loc_head;
	const struct __locale_map *p;
	struct __locale_map *new = 0;
	const char *path = 0, *z;
	char buf[256];
	size_t n;

	if (!*val) {
		(val = getenv("LC_ALL")) && *val ||
		(val = getenv(envvars[cat])) && *val ||
		(val = getenv("LANG")) && *val ||
		(val = "C.UTF-8");
	}

	/* Limit name length and forbid leading dot or any slashes. */
	for (n=0; n<LOCALE_NAME_MAX && val[n] && val[n]!='/'; n++);
	if (val[0]=='.' || val[n]) return LOC_MAP_FAILED;

	/* LANG_TERRITORY.CODESET@MODIFIER */
	const char *dot = memchr(val, '.', n), *at = memchr(val, '@', n);
	if (dot && at && at < dot) dot = 0;
	size_t ll = dot ? (size_t)(dot-val) : at ? (size_t)(at-val) : n;
	const char *cs = dot ? dot+1 : 0;
	size_t cl = dot ? (at ? (size_t)(at-cs) : n-(ll+1)) : 0;
	const char *mod = at ? at+1 : 0;
	size_t ml = at ? n-(size_t)(mod-val) : 0;
	if (cs && !is_utf8(cs, cl)) return LOC_MAP_FAILED;

	if ((ll == 1 && val[0] == 'C') || (ll == 5 && !memcmp(val, "POSIX", 5))) {
		if (mod) return LOC_MAP_FAILED;
		if (cs) return cat == LC_CTYPE ? (void *)&__c_dot_utf8 : 0;
		return 0;
	}

	for (p=loc_head; p; p=p->next)
		if (!strcmp(val, p->name)) return p;

	if (!libc.secure) path = getenv("MUSL_LOCPATH");
	for (int pass = 0; !new && pass < 2; pass++) {
		if (pass) path = SIEOS_LOCALE_DIR;
		if (!path) continue;
		for (; *path && !new; path=z+!!*z) {
			z = __strchrnul(path, ':');
			if (!file_name(buf, sizeof buf, path, z-path, val, ll, mod, ml))
				continue;
			size_t map_size;
			const void *map = __map_file(buf, &map_size);
			if (!map) continue;
			new = malloc(sizeof *new);
			if (!new) {
				__munmap((void *)map, map_size);
				return LOC_MAP_FAILED;
			}
			new->map = map;
			new->map_size = map_size;
			memcpy(new->name, val, n);
			new->name[n] = 0;
			new->next = loc_head;
			loc_head = new;
		}
	}
	return new ? new : LOC_MAP_FAILED;
}
