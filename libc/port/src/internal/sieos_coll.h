/*
 * sieos_coll.h - SIEOS: collation for the locales of the database
 * (src/locale/sieos64/__sieos_coll.c).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_COLL_H
#define SIEOS_COLL_H

#include <stddef.h>
#include <locale.h>
#include "libc.h"

/* Does loc's LC_COLLATE order by Unicode (a locale of the database)? */
hidden int __sieos_coll_on(locale_t loc);
/* s's sort key (UTF-8, or wide if wide): its length, out filled up to cap
 * bytes (never NUL: keys compare with strcmp once terminated). */
hidden size_t __sieos_coll_key(const void *s, int wide, unsigned char *out, size_t cap);
/* strcoll / wcscoll by keys */
hidden int __sieos_coll_cmp(const void *l, const void *r, int wide);

#endif
