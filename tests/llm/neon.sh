#!/bin/sh
# neon.sh - The engine's AArch64 NEON kernels: tests/llm/neon_test.c, built
# by sicc for AArch64 with the AArch64 test C library (tests/cc/a64), run
# under QEMU's user-mode emulator (which has sdot: its CPU is "max").
# Usage: neon.sh SICC QEMU   (OUT: the work directory)
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
SICC=$1; QEMU=$2; OUT=${OUT:-/tmp/llm-neon}; D=$(cd "$(dirname "$0")/../.." && pwd); T=$D/tests/cc/a64
A="$SICC --target=aarch64 -O2"; mkdir -p "$OUT"
$A -c -o "$OUT/crt0.o" "$T/crt0.S" && $A -c -o "$OUT/libc.o" "$T/libc.c" && $A -c -o "$OUT/fmt.o" "$T/fmt.c" &&
$A -c -o "$OUT/rt.o" "$D/cc/rt/rt.c" || exit 1
for f in quant math neon; do $A -I"$D/include" -c -o "$OUT/$f.o" "$D/llm/$f.c" || exit 1; done
$A -march=armv8.2-a+dotprod -I"$D/include" -c -o "$OUT/neon_dot.o" "$D/llm/neon_dot.c" &&
$A -I"$D/include" -c -o "$OUT/neon_test.o" "$D/tests/llm/neon_test.c" &&
$SICC --target=aarch64 -nostdlib -T "$T/prog.ld" -o "$OUT/neon_test" "$OUT/crt0.o" "$OUT/neon_test.o" \
    "$OUT/quant.o" "$OUT/math.o" "$OUT/neon.o" "$OUT/neon_dot.o" "$OUT/libc.o" "$OUT/fmt.o" "$OUT/rt.o" || exit 1
chmod +x "$OUT/neon_test" && "$QEMU" -cpu max "$OUT/neon_test"
