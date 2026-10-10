#!/bin/sh
# Compile each test with the host compiler and with sicc, run both, compare.
# Usage: run.sh SICC [mode]   mode "S": sicc emits assembly, the host assembles
# (only for checking code generation before sicc's own assembler exists).
# Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
LIBS="-lm -latomic"   # -latomic: the host compiler calls it for floating _Atomic compound assignment
SICC=$1; MODE=${2:-c}; D=$(dirname "$0"); OUT=${OUT:-/tmp/sicc-tests}
mkdir -p "$OUT"; pass=0; fail=0
for t in "$D"/*.c; do
    n=$(basename "$t" .c)
    case $n in *_gcc) continue;; esac          # X_gcc.c: compiled by the host, linked with X
    extra=""; [ -f "$D/${n}_gcc.c" ] && { gcc -c -O2 -o "$OUT/${n}_gcc.o" "$D/${n}_gcc.c" || exit 1; extra="$OUT/${n}_gcc.o"; }
    fl=$(sed -n '1s/^\/\* cflags: \(.*\) \*\/$/\1/p' "$t")   # first line "/* cflags: ... */": for both compilers
    gcc -w -O1 $fl -o "$OUT/$n.ref" "$t" $extra $LIBS 2>/dev/null || { echo "gcc failed: $n"; fail=$((fail+1)); continue; }
    if [ "$MODE" = S ]; then
        "$SICC" $fl -S -o "$OUT/$n.s" "$t" && gcc -no-pie -o "$OUT/$n.bin" "$OUT/$n.s" $extra $LIBS
    else
        "$SICC" $fl -c -o "$OUT/$n.o" "$t" && gcc -no-pie -o "$OUT/$n.bin" "$OUT/$n.o" $extra $LIBS
    fi || { echo "FAIL (build): $n"; fail=$((fail+1)); continue; }
    "$OUT/$n.ref" > "$OUT/$n.want" 2>&1; "$OUT/$n.bin" > "$OUT/$n.got" 2>&1
    if cmp -s "$OUT/$n.want" "$OUT/$n.got"; then pass=$((pass+1)); else echo "FAIL (output): $n"; diff "$OUT/$n.want" "$OUT/$n.got" | head -10; fail=$((fail+1)); fi
done
# Several sources in one run (-c), merged by sicc's linker (-r), linked by the host.
M=$(cd "$D/multi" && pwd); S=$(cd "$(dirname "$SICC")" && pwd)/$(basename "$SICC"); gcc -w -o "$OUT/multi.ref" "$M/main.c" "$M/other.c" && "$OUT/multi.ref" > "$OUT/multi.want"
if (cd "$OUT" && "$S" -c "$M/main.c" "$M/other.c" && "$S" -r -o multi.o main.o other.o) &&
   gcc -no-pie -o "$OUT/multi.bin" "$OUT/multi.o" && "$OUT/multi.bin" | cmp -s - "$OUT/multi.want"
then pass=$((pass+1)); else echo "FAIL: multi"; fail=$((fail+1)); fi
# Thread-local storage linked by sicc's own linker (no C library; it sets %fs itself),
# also with an object from the host compiler, whose initial-exec code gets relaxed.
X="$D/static"; ok=1
for o in -O0 -O2; do
    "$SICC" $o -ffreestanding -nostdlib -T "$X/tls.ld" -o "$OUT/tls_static" "$X/tls.c" "$X/tls_other.c" &&
        chmod +x "$OUT/tls_static" && "$OUT/tls_static" > /dev/null || ok=0
done
gcc -O2 -fno-pic -fno-stack-protector -ffreestanding -fcf-protection=none -c -o "$OUT/tls_host.o" "$X/tls.c" &&
    "$SICC" -c -o "$OUT/tls_other.o" "$X/tls_other.c" && "$SICC" --ld -T "$X/tls.ld" -o "$OUT/tls_mixed" "$OUT/tls_host.o" "$OUT/tls_other.o" &&
    chmod +x "$OUT/tls_mixed" && "$OUT/tls_mixed" > /dev/null || ok=0
if [ $ok = 1 ]; then pass=$((pass+1)); else echo "FAIL: tls_static"; fail=$((fail+1)); fi
# -g: the same code, plus line tables the host's tools read (also through sicc's linker)
ok=1
for t in "$D"/*.c; do
    "$SICC" -c -o "$OUT/nog.o" "$t" && "$SICC" -g -c -o "$OUT/g.o" "$t" &&
        objcopy -O binary -j .text "$OUT/nog.o" "$OUT/nog.bin" && objcopy -O binary -j .text "$OUT/g.o" "$OUT/g.bin" &&
        cmp -s "$OUT/nog.bin" "$OUT/g.bin" || { echo "  -g changes the code: $t"; ok=0; }
done
"$SICC" -g -ffreestanding -nostdlib -T "$X/tls.ld" -o "$OUT/tls_g" "$X/tls.c" "$X/tls_other.c" &&
    a=$(nm "$OUT/tls_g" | sed -n 's/ T _start$//p') && [ -n "$a" ] &&
    addr2line -e "$OUT/tls_g" "0x$a" < /dev/null | grep -q 'tls.c:[0-9]' || { echo "  no line for _start"; ok=0; }
if [ $ok = 1 ]; then pass=$((pass+1)); else echo "FAIL: debug"; fail=$((fail+1)); fi
echo "sicc tests: $pass passed, $fail failed"
[ $fail = 0 ]
