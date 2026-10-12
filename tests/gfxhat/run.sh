#!/bin/sh
# run.sh - gfxhat's text part (tests/gfxhat/text.c) on the development machine.
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
set -e
out=${OUT:-build/gfxhat-test}
mkdir -p "$out"
${HOSTCC:-cc} -O1 -Wall -Wextra -Werror -o "$out/text" tests/gfxhat/text.c
"$out/text" "$@"
