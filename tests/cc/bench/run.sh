#!/bin/sh
# Code speed: each benchmark built by the host compiler (-O2) and by sicc,
# best of 3 runs each. Usage: run.sh SICC. Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
SICC=$1; D=$(dirname "$0"); OUT=${OUT:-/tmp/sicc-bench}; mkdir -p "$OUT"
best() { b=999999; for k in 1 2 3; do s=$(date +%s%N); "$1" > /dev/null; e=$(( ($(date +%s%N) - s) / 1000000 )); [ $e -lt $b ] && b=$e; done; echo $b; }
printf "%-8s %8s %8s %6s   %8s %8s\n" bench "gcc ms" "sicc ms" ratio "gcc B" "sicc B"
for t in "$D"/*.c; do
    n=$(basename "$t" .c)
    gcc -O2 -w -o "$OUT/$n.gcc" "$t" -lm || exit 1
    "$SICC" -O2 -c -o "$OUT/$n.o" "$t" && gcc -no-pie -o "$OUT/$n.sicc" "$OUT/$n.o" -lm || exit 1
    "$OUT/$n.gcc" > "$OUT/$n.a"; "$OUT/$n.sicc" > "$OUT/$n.b"; cmp -s "$OUT/$n.a" "$OUT/$n.b" || { echo "$n: different output"; exit 1; }
    g=$(best "$OUT/$n.gcc"); s=$(best "$OUT/$n.sicc")
    gs=$(size "$OUT/$n.o" | awk 'NR==2{print $1}'); gcc -O2 -w -c -o "$OUT/$n.g.o" "$t"; gg=$(size "$OUT/$n.g.o" | awk 'NR==2{print $1}')
    printf "%-8s %8d %8d %6s   %8d %8d\n" "$n" "$g" "$s" "$(awk "BEGIN{printf \"%.2f\", $s/$g}")" "$gg" "$gs"
done
