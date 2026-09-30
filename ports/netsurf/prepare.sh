#!/bin/sh
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
# prepare.sh TREE - the SIEOS changes to an unpacked netsurf-all release.
#
# libnsfb: the Facet surface (nsfb-facet.c, as src/surface/facet.c), always
# built; its type, NSFB_SURFACE_FACET, before NSFB_SURFACE_RAM, so that
# NetSurf, which takes the lowest type built as its default, opens a Facet
# window; and libfacet (shared, /usr/lib/libfacet.so.1) in libnsfb's link.
set -e
TREE=$1
HERE=$(cd "$(dirname "$0")" && pwd)
NSFB=$TREE/libnsfb

cp "$HERE/nsfb-facet.c" "$NSFB/src/surface/facet.c"
grep -q 'facet\.c' "$NSFB/src/surface/Makefile" ||
    sed -i 's/^SURFACE_HANDLER_yes := surface.c ram.c$/SURFACE_HANDLER_yes := surface.c ram.c facet.c/' \
        "$NSFB/src/surface/Makefile"
grep -q 'NSFB_SURFACE_FACET' "$NSFB/include/libnsfb.h" ||
    sed -i 's|^    NSFB_SURFACE_RAM, /\*\*< RAM surface \*/|    NSFB_SURFACE_FACET, /**< SIEOS Facet window */\n&|' \
        "$NSFB/include/libnsfb.h"
grep -q -- '-lfacet' "$NSFB/libnsfb.pc.in" ||
    sed -i 's|^Libs: -L${libdir} -lnsfb$|Libs: -L${libdir} -lnsfb -lfacet|' "$NSFB/libnsfb.pc.in"

# each change must have taken
grep -q 'facet\.c' "$NSFB/src/surface/Makefile"
grep -q 'NSFB_SURFACE_FACET' "$NSFB/include/libnsfb.h"
grep -q -- '-lfacet' "$NSFB/libnsfb.pc.in"
