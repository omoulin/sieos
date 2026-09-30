#!/bin/sh
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
# fetch.sh URL OUT
#
# Download URL to OUT, unless OUT exists.  A stalled transfer is abandoned
# (no progress for a minute) and retried; a GNU tarball that ftp.gnu.org does
# not serve is fetched through the ftpmirror.gnu.org redirector.  OUT appears
# only once complete (the checksum is the caller's).
set -e
URL=$1
OUT=$2
[ -f "$OUT" ] && exit 0
get() {
    curl -sSfL --connect-timeout 30 --speed-limit 1024 --speed-time 60 \
        --retry 1 --retry-delay 5 -o "$OUT.part" "$1"
}
if ! get "$URL"; then
    case $URL in
    https://ftp.gnu.org/gnu/*)
        echo "fetch: $URL failed, trying ftpmirror.gnu.org" >&2
        get "https://ftpmirror.gnu.org/${URL#https://ftp.gnu.org/gnu/}" ;;
    *) exit 1 ;;
    esac
fi
mv "$OUT.part" "$OUT"
