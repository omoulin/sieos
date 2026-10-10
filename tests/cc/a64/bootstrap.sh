#!/bin/sh
# sicc bootstraps on AArch64, under QEMU's user-mode emulator:
#   stage 2: sicc's sources (and the test C library) compiled for AArch64 by
#            SICC (on the development machine), linked by it;
#   stage 3: the same, compiled and linked by stage 2, running emulated.
# The two programs must be identical. Then stage 3 runs the AArch64 tests.
# Usage: bootstrap.sh SICC QEMU   (OUT: the work directory)
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
SICC=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); QEMU=$2; D=$(cd "$(dirname "$0")" && pwd); C=$(cd "$D/../../../cc" && pwd)
OUT=${OUT:-/tmp/sicc-a64-boot}; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
case $QEMU in */*) QEMU=$(cd "$(dirname "$QEMU")" && pwd)/$(basename "$QEMU");; esac
SRC="util lex pp parse ir opt ra emit emit_a64 asm link main"
export SICC_INCLUDE="$C/include"
stage() {   # stage number, the compiler (a command)
    S="$OUT/s$1"; rm -rf "$S"; mkdir -p "$S"
    for f in $SRC; do $2 -O2 -c -o "$S/$f.o" "$C/$f.c" || exit 1; done
    $2 -c -o "$S/crt0.o" "$D/crt0.S" && $2 -O2 -c -o "$S/libc.o" "$D/libc.c" && $2 -O2 -c -o "$S/fmt.o" "$D/fmt.c" &&
        $2 -O2 -c -o "$S/rt.o" "$C/rt/rt.c" || exit 1
    $2 -nostdlib -T "$D/prog.ld" -o "$OUT/sicc$1" "$S/crt0.o" $(for f in $SRC; do echo "$S/$f.o"; done) "$S/libc.o" "$S/fmt.o" "$S/rt.o" || exit 1
    chmod +x "$OUT/sicc$1"
}
stage 2 "$SICC --target=aarch64"
stage 3 "$QEMU $OUT/sicc2"
cmp "$OUT/sicc2" "$OUT/sicc3" && echo "AArch64 bootstrap: stage 2 and stage 3 are identical ($(wc -c < "$OUT/sicc3") bytes)"
# stage 3 (emulated, through a wrapper) runs the AArch64 tests
printf '#!/bin/sh\nexec %s %s "$@"\n' "$QEMU" "$OUT/sicc3" > "$OUT/sicc3-run"; chmod +x "$OUT/sicc3-run"
OUT="$OUT/tests" "$D/../run_a64.sh" "$OUT/sicc3-run" "$QEMU" -O2
