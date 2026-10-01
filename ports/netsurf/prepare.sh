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
# NetSurf: netsurf-curl.patch (the fetcher resolves IPv4 only when the host has
# no global IPv6 address; curl's long options given longs), netsurf-title.patch
# (the page's title to the surface: nsfb_set_parameters "title="),
# netsurf-clip.patch (a page's plotting kept inside its widget: nothing drawn
# over the toolbar, scroll bars or status bar), netsurf-clipboard.patch (copy
# and paste through the desktop's clipboard, libfacet's), netsurf-textcopy.patch
# (the address bar: Ctrl+C and Ctrl+X copy it, Ctrl+V pastes),
# netsurf-dom-dispatch.patch (libdom: an event reached its target twice, at
# target and again bubbling: a script's click handler ran twice), and
# Makefile.config.
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

# JavaScript on by default (NetSurf's default is off; ~/.netsurf/Choices can say
# enable_javascript:0)
sed -i 's/^NSOPTION_BOOL(enable_javascript, false)$/NSOPTION_BOOL(enable_javascript, true)/' "$TREE/netsurf/desktop/options.h"
grep -q '^NSOPTION_BOOL(enable_javascript, true)$' "$TREE/netsurf/desktop/options.h"

# NetSurf's patches (patch -p1 in TREE)
for p in "$HERE"/netsurf-*.patch; do
    patch -d "$TREE" -p1 -s -N < "$p"
done

# NetSurf's build options (Makefile.config): text through FreeType with the
# DejaVu fonts of /usr/share/fonts/dejavu; JavaScript with Duktape (its
# bindings made by nsgenbind on the build host); no formats or
# features without a port (WebP, JPEG XL, PDF export, RISC OS sprites); curl
# does TLS (Mbed TLS), so no OpenSSL; iconv is the C library's.
cat > "$TREE/netsurf/Makefile.config" <<'CFG'
override NETSURF_FB_FONTLIB := freetype
override NETSURF_FB_FONTPATH := /usr/share/fonts/dejavu
override NETSURF_USE_DUKTAPE := YES
override NETSURF_USE_WEBP := NO
override NETSURF_USE_JPEGXL := NO
override NETSURF_USE_HARU_PDF := NO
override NETSURF_USE_ROSPRITE := NO
override NETSURF_USE_VIDEO := NO
override NETSURF_USE_OPENSSL := NO
override NETSURF_USE_LIBICONV_PLUG := YES
override NETSURF_FB_FONT_CURSIVE := DejaVuSans.ttf
override NETSURF_FB_FONT_FANTASY := DejaVuSans.ttf
CFG
