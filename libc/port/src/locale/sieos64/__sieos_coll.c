/*
 * __sieos_coll.c - SIEOS: collation for the locales of the database.
 *
 * A simplified Unicode collation, for every locale of the database (its
 * catalog says "@collate"): strings compare by their letters' base letters
 * (accents and case ignored: "à" before "b", "B" with "b"), then their
 * accents, then their case, then their code points.  A sort key holds the
 * four levels in turn, separated by a byte lower than any of theirs, so that
 * keys compare as the strings collate.  The letters with accents and their
 * base letters (Latin, Greek, Cyrillic) are generated from Unicode's data
 * (sieos_collate.h, libc/sieos-port.py); others sort by their lower case.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "locale_impl.h"
#include "sieos_coll.h"
#include "sieos_collate.h"

int __sieos_coll_on(locale_t loc)
{
	const struct __locale_map *lm = loc->cat[LC_COLLATE];
	return lm && lm->map && __mo_lookup(lm->map, lm->map_size, "@collate");
}

struct cpiter { const unsigned char *s; const wchar_t *w; };

/* The next code point, 0 at the end (UTF-8: a byte that does not decode is U+DC00+byte). */
static unsigned next(struct cpiter *it)
{
	if (it->w) return *it->w ? (unsigned)*it->w++ : 0;
	unsigned c = *it->s;
	if (!c) return 0;
	int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
	if (c >= 0x80 && !n) { it->s++; return 0xdc00 + c; }
	unsigned v = n ? c & (0x3f >> n) : c;
	for (int i = 1; i <= n; i++) {
		if ((it->s[i] & 0xc0) != 0x80) { it->s++; return 0xdc00 + c; }
		v = v << 6 | (it->s[i] & 0x3f);
	}
	it->s += n + 1;
	return v;
}

struct weights { unsigned p1, p2; unsigned char acc, upper, mark; };

static void weigh(unsigned c, struct weights *w)
{
	size_t lo = 0, hi = sizeof __coll_tab / sizeof __coll_tab[0];
	w->p2 = 0;
	w->mark = c >= 0x300 && c < 0x370;           /* a combining accent: no letter of its own */
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (__coll_tab[mid].cp == c) {
			w->p1 = __coll_tab[mid].p1;
			w->p2 = __coll_tab[mid].p2;
			w->acc = __coll_tab[mid].acc;
			w->upper = __coll_tab[mid].upper;
			return;
		}
		if (__coll_tab[mid].cp < c) lo = mid + 1;
		else hi = mid;
	}
	unsigned l = towlower(c);
	w->p1 = l;
	w->acc = w->mark ? c - 0x2ff : 0;
	w->upper = l != c;
}

struct out { unsigned char *b; size_t cap, len; };

static void put(struct out *o, unsigned v)
{
	if (o->len < o->cap) o->b[o->len] = v;
	o->len++;
}

static void put3(struct out *o, unsigned v)           /* 21 bits, 7 to a byte, each >= 2 */
{
	put(o, (v >> 14 & 0x7f) + 2);
	put(o, (v >> 7 & 0x7f) + 2);
	put(o, (v & 0x7f) + 2);
}

size_t __sieos_coll_key(const void *s, int wide, unsigned char *out, size_t cap)
{
	struct out o = { out, cap, 0 };
	for (int level = 0; level < 4; level++) {
		struct cpiter it = { wide ? 0 : s, wide ? s : 0 };
		unsigned c;
		struct weights w;
		if (level) put(&o, 1);
		while ((c = next(&it))) {
			weigh(c, &w);
			switch (level) {
			case 0:
				if (w.mark) break;
				put3(&o, w.p1);
				if (w.p2) put3(&o, w.p2);
				break;
			case 1:
				put(&o, w.acc + 2);
				if (w.p2) put(&o, 2);
				break;
			case 2:
				if (w.mark) break;
				put(&o, w.upper + 2);
				if (w.p2) put(&o, w.upper + 2);
				break;
			default:
				put3(&o, c);
			}
		}
	}
	return o.len;
}

/* A key in buf (cap bytes) or allocated; NULL if out of memory. */
static unsigned char *key(const void *s, int wide, unsigned char *buf, size_t cap)
{
	size_t n = __sieos_coll_key(s, wide, buf, cap - 1);
	if (n < cap) {
		buf[n] = 0;
		return buf;
	}
	unsigned char *k = malloc(n + 1);
	if (!k) return 0;
	__sieos_coll_key(s, wide, k, n);
	k[n] = 0;
	return k;
}

int __sieos_coll_cmp(const void *l, const void *r, int wide)
{
	unsigned char bl[256], br[256];
	unsigned char *kl = key(l, wide, bl, sizeof bl), *kr = key(r, wide, br, sizeof br);
	int d = kl && kr ? strcmp((char *)kl, (char *)kr)
	        : wide ? wcscmp(l, r) : strcmp(l, r);
	if (kl != bl) free(kl);
	if (kr != br) free(kr);
	return d;
}
