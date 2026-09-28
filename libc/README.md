# SIEOS C library (musl 1.2.5, ABI v2)

- `musl-1.2.5.tar.gz`: the pristine musl release
  (sha256 a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4).
- `libc-test-7b95dfa5.tar.gz`: a snapshot of musl's libc-test, commit
  7b95dfa5f5d5ca4d949221e0228ccc290bacc14e.
- `sieos-port.py`: assembles the build tree from the tarball, `port/` and the ABI
  headers (`abi/include/sieos`), and applies the checked text patches.
- `port/`: files overlaid on musl:
  - `arch/sieos64`: the architecture;
  - `src/*/sieos64`: per-architecture replacements;
  - `src/sieos`: the emulation layer for Linux-only calls.
- `tests/`: libc-test configuration and `runall`, the runner used on SIEOS.

    make libc            # -> build/sysroot (headers, crt files, libc.a)
    gcc -specs=build/sysroot/lib/musl-gcc.specs -static -o prog prog.c
    make libc-test-img   # -> build/libc-test.img (tests in /opt/libc-test)

See docs/abi-v2.md, "Milestone 7", for the design and the test results.
