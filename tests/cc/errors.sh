#!/bin/sh
# Bad programs: sicc must refuse each one, at the right place, with the expected message
# (the first line of each file: "expect: line:col: error: text").
# Usage: errors.sh SICC [sicc options, e.g. --target=aarch64]
# Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
SICC=$1; shift; D=$(dirname "$0")/errors; pass=0; fail=0
for t in "$D"/*.c; do
    want=$(sed -n '1s/.*expect[^:]*: \(.*\) \*\/$/\1/p' "$t")
    flags=""; head -1 "$t" | grep -q general-regs-only && flags=-mgeneral-regs-only
    got=$("$SICC" "$@" $flags -c -o /dev/null "$t" 2>&1) && { echo "FAIL (accepted): $t"; fail=$((fail+1)); continue; }
    case "$got" in *"$(basename "$t"):$want"*) pass=$((pass+1));; *) echo "FAIL: $t"; echo "  want: $want"; echo "  got:  $(echo "$got" | head -1)"; fail=$((fail+1));; esac
done
echo "sicc error tests${*:+ ($*)}: $pass passed, $fail failed"
[ $fail = 0 ]
