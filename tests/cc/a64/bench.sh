#!/bin/sh
# Code size and (emulated, so only indicative) speed of sicc's AArch64 code:
# each benchmark of ../bench built by sicc for AArch64 (run under QEMU), and,
# for comparison, by sicc and by the host compiler for x86-64 (run natively).
# Sizes: .text of the benchmark's object. Times: best of 3, milliseconds.
# Usage: bench.sh SICC QEMU. Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
SICC=$1; QEMU=$2; D=$(cd "$(dirname "$0")" && pwd); B="$D/../bench"; OUT=${OUT:-/tmp/sicc-a64-bench}; RT="$OUT/rt"; mkdir -p "$RT"
A="$SICC --target=aarch64"
$A -c -o "$RT/crt0.o" "$D/crt0.S" && for f in libc fmt; do $A -O2 -c -o "$RT/$f.o" "$D/$f.c" || exit 1; done && $A -O2 -c -o "$RT/rt.o" "$D/../../../cc/rt/rt.c" || exit 1
best() { b=999999; for k in 1 2 3; do s=$(date +%s%N); "$@" > /dev/null; e=$(( ($(date +%s%N) - s) / 1000000 )); [ $e -lt $b ] && b=$e; done; echo $b; }
text() { size -A "$1" | awk '$1 ~ /^\.text/ {t += $2} END {print t}'; }
printf "%-8s %9s %9s %9s   %9s %9s %9s\n" bench "a64 B" "x86 B" "gcc B" "a64 ms" "x86 ms" "gcc ms"
for t in "$B"/*.c; do
    n=$(basename "$t" .c)
    $A -O2 -c -o "$OUT/$n.a.o" "$t" && $A -nostdlib -T "$D/prog.ld" -o "$OUT/$n.a" "$RT/crt0.o" "$OUT/$n.a.o" "$RT/libc.o" "$RT/fmt.o" "$RT/rt.o" && chmod +x "$OUT/$n.a" || exit 1
    "$SICC" -O2 -c -o "$OUT/$n.x.o" "$t" && gcc -no-pie -o "$OUT/$n.x" "$OUT/$n.x.o" -lm || exit 1
    gcc -O2 -w -c -o "$OUT/$n.g.o" "$t" && gcc -o "$OUT/$n.g" "$OUT/$n.g.o" -lm || exit 1
    "$QEMU" "$OUT/$n.a" > "$OUT/$n.out.a"; "$OUT/$n.x" > "$OUT/$n.out.x"
    cmp -s "$OUT/$n.out.a" "$OUT/$n.out.x" || echo "$n: different output"
    printf "%-8s %9d %9d %9d   %9d %9d %9d\n" "$n" "$(text "$OUT/$n.a.o")" "$(text "$OUT/$n.x.o")" "$(text "$OUT/$n.g.o")" \
        "$(best "$QEMU" "$OUT/$n.a")" "$(best "$OUT/$n.x")" "$(best "$OUT/$n.g")"
done
