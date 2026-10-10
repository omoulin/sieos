#!/bin/sh
# The sicc tests for AArch64: each test is compiled by sicc for AArch64, linked
# by sicc with a small test C library (a64/), and run under QEMU's user-mode
# emulator. The reference is the host compiler's program, built with
# SIEOS's AArch64 types (long double = double, char unsigned) and the same
# printf (a64/fmt.c), so both outputs must be identical.
# Usage: run_a64.sh SICC QEMU [sicc options...]
# Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
SICC=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); QEMU=$2; shift 2; OPTS="$*"; D=$(cd "$(dirname "$0")" && pwd); OUT=${OUT:-/tmp/sicc-a64-tests}
A="$SICC --target=aarch64"; RT="$OUT/rt"; mkdir -p "$RT"; pass=0; fail=0
REF="-w -O1 -fno-builtin -mlong-double-64 -funsigned-char"
# the test C library, built by sicc (always -O2: only the tests vary)
$A -c -o "$RT/crt0.o" "$D/a64/crt0.S" && $A -O2 -c -o "$RT/libc.o" "$D/a64/libc.c" &&
    $A -O2 -c -o "$RT/fmt.o" "$D/a64/fmt.c" && $A -O2 -c -o "$RT/rt.o" "$D/../../cc/rt/rt.c" &&
    gcc -O2 -fno-builtin -mlong-double-64 -c -o "$RT/fmt_host.o" "$D/a64/fmt.c" && gcc -O2 -c -o "$RT/host.o" "$D/a64/host.c" || exit 1
LIB="$RT/libc.o $RT/fmt.o $RT/rt.o"
link() { out=$1; shift; $A -nostdlib -T "$D/a64/prog.ld" -o "$out" "$RT/crt0.o" "$@" $LIB && chmod +x "$out"; }
check() {   # name, reference program, AArch64 program
    "$2" > "$OUT/$1.want" 2>&1; echo "exit $?" >> "$OUT/$1.want"
    "$QEMU" "$3" > "$OUT/$1.got" 2>&1; echo "exit $?" >> "$OUT/$1.got"
    if cmp -s "$OUT/$1.want" "$OUT/$1.got"; then pass=$((pass+1)); else echo "FAIL (output): $1"; diff "$OUT/$1.want" "$OUT/$1.got" | head -10; fail=$((fail+1)); fi
}
for t in "$D"/*.c "$D"/a64/t_*.c; do
    [ -f "$t" ] || continue; n=$(basename "$t" .c)
    case $n in *_gcc|asm|intrin|abi|tls) continue;; esac   # x86-only, or pairs with host objects (a64/ has their versions)
    fl=$(sed -n '1s/^\/\* cflags: \(.*\) \*\/$/\1/p' "$t")
    $A $OPTS $fl -c -o "$OUT/$n.o" "$t" && link "$OUT/$n.bin" "$OUT/$n.o" || { echo "FAIL (build): $n"; fail=$((fail+1)); continue; }
    if [ -f "${t%.c}.expect" ]; then                 # AArch64 assembly: the expected output, written by hand
        printf '#!/bin/sh\ncat "%s"; exit 0\n' "${t%.c}.expect" > "$OUT/$n.ref"; chmod +x "$OUT/$n.ref"
        "$QEMU" "$OUT/$n.bin" > "$OUT/$n.got" 2>&1; echo "exit $?" >> "$OUT/$n.got"
        if cmp -s "${t%.c}.expect" "$OUT/$n.got"; then pass=$((pass+1)); else echo "FAIL (output): $n"; diff "${t%.c}.expect" "$OUT/$n.got" | head -10; fail=$((fail+1)); fi
        continue
    fi
    gcc $REF $fl -o "$OUT/$n.ref" "$t" "$RT/fmt_host.o" "$RT/host.o" -lm -latomic 2>/dev/null || { echo "gcc failed: $n"; fail=$((fail+1)); continue; }
    check "$n" "$OUT/$n.ref" "$OUT/$n.bin"
done
# several sources in one run (-c), merged by sicc's linker (-r)
M="$D/multi"; gcc $REF -o "$OUT/multi.ref" "$M/main.c" "$M/other.c" "$RT/fmt_host.o" "$RT/host.o" &&
    (cd "$OUT" && $A $OPTS -c "$M/main.c" "$M/other.c" && $A -r -o multi.o main.o other.o) && link "$OUT/multi.bin" "$OUT/multi.o" &&
    check multi "$OUT/multi.ref" "$OUT/multi.bin" || { echo "FAIL: multi"; fail=$((fail+1)); }
echo "sicc AArch64 tests${OPTS:+ ($OPTS)}: $pass passed, $fail failed"
[ $fail = 0 ]
