#!/bin/sh
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
#
# build.sh TREE OUT - Rust for SIEOS: the target x86_64-unknown-linux-sieos.
#
# The pinned toolchain (rustup, in ~/.rustup and ~/.cargo) with its standard
# library's sources; the libc crate std uses, fetched and checked, then made
# SIEOS's by gen-libc.py (its constants, types and structures from SIEOS's C
# headers); std built from source for the target (-Zbuild-std, through the
# rust-src copy OUT/library, whose workspace takes OUT/libc); a sysroot,
# OUT/sysroot, holding it and the target's specification, for rustc used
# directly (Meson):
#     rustc --target x86_64-unknown-linux-sieos --sysroot OUT/sysroot ...
# (with RUSTC_BOOTSTRAP=1: custom targets are unstable); cargo through
# OUT/cargo-sieos.  A test program (hello: threads, files, processes, TCP) is
# built with it, OUT/hello.
set -e
TREE=$(cd "$1" && pwd)
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
RUST_VER=1.99.0
T=x86_64-unknown-linux-sieos
RUSTUP=$HOME/.cargo/bin/rustup
[ -x "$RUSTUP" ] || { echo "rust-sieos: rustup is needed (https://rustup.rs; see README)" >&2; exit 1; }
"$RUSTUP" toolchain install $RUST_VER --profile minimal -c rust-src -t x86_64-unknown-linux-musl > /dev/null 2>&1
CARGO="$HOME/.cargo/bin/cargo +$RUST_VER"
RUSTC="$HOME/.cargo/bin/rustc +$RUST_VER"
SRC=$($RUSTC --print sysroot)/lib/rustlib/src/rust/library
export RUSTC_BOOTSTRAP=1

# the libc crate std depends on, at its version
LIBC_VER=$(sed -n '/^name = "libc"$/{n;s/version = "\(.*\)"/\1/p}' "$SRC/Cargo.lock")
mkdir -p "$OUT/dl"
( cd "$OUT/dl" && "$TREE/tools/fetch.sh" https://static.crates.io/crates/libc/libc-$LIBC_VER.crate libc-$LIBC_VER.crate &&
  grep " libc-$LIBC_VER.crate\$" "$TREE/ports/SHA256SUMS" | sha256sum -c --quiet &&
  rm -rf libc-$LIBC_VER && tar xzf libc-$LIBC_VER.crate )

# its definitions for Linux with musl (rustdoc's JSON), compared with SIEOS's headers
( cd "$OUT/dl/libc-$LIBC_VER" && $CARGO rustdoc -q --target x86_64-unknown-linux-musl --target-dir "$OUT/docjson" \
      -- -Zunstable-options --output-format json )
python3 "$TREE/tools/rust-sieos/gen-libc.py" "$OUT/docjson/x86_64-unknown-linux-musl/doc/libc.json" \
    "$OUT/dl/libc-$LIBC_VER" "$TREE/build/sysroot" "$OUT/gen" "$OUT/libc"

# std's sources, taking that crate
rm -rf "$OUT/library" && cp -r "$SRC" "$OUT/library"
sed -i 's|^\[patch.crates-io\]|[patch.crates-io]\nlibc = { path = "../libc" }  # SIEOS: tools/rust-sieos/gen-libc.py|' \
    "$OUT/library/Cargo.toml"

# cargo for the target, std built from those sources
cat > "$OUT/cargo-sieos" <<EOF
#!/bin/sh
# cargo for x86_64-unknown-linux-sieos (tools/rust-sieos/build.sh)
export RUSTC_BOOTSTRAP=1 __CARGO_TESTS_ONLY_SRC_ROOT="$OUT/library"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_SIEOS_LINKER="$TREE/build/cross/bin/x86_64-pc-sieos-gcc"
exec $CARGO "\$@" -Zbuild-std=std,panic_abort,panic_unwind -Zjson-target-spec \\
    --target "$TREE/tools/rust-sieos/$T.json"
EOF
chmod 755 "$OUT/cargo-sieos"
# the sysroot: std's crates (as built for a crate depending on nothing else) and the target's specification
rm -rf "$OUT/std" && mkdir -p "$OUT/std/src" && echo 'fn main() {}' > "$OUT/std/src/main.rs"
printf '[package]\nname = "std-sieos"\nversion = "0.1.0"\nedition = "2021"\n' > "$OUT/std/Cargo.toml"
"$OUT/cargo-sieos" build -q --release --manifest-path "$OUT/std/Cargo.toml" --target-dir "$OUT/std/target"
L="$OUT/sysroot/lib/rustlib/$T/lib"
rm -rf "$OUT/sysroot" && mkdir -p "$L"
cp "$OUT"/std/target/$T/release/deps/*.rlib "$L/"
cp "$TREE/tools/rust-sieos/$T.json" "$OUT/sysroot/lib/rustlib/$T/target.json"

# the test program
rm -rf "$OUT/hello" && cp -r "$TREE/tools/rust-sieos/hello" "$OUT/hello"
"$OUT/cargo-sieos" build -q --release --manifest-path "$OUT/hello/Cargo.toml" --target-dir "$OUT/target"
cp "$OUT/target/$T/release/hello" "$OUT/hello-sieos"
echo "rust-sieos: $($RUSTC --version), std for $T in $OUT/sysroot"
