# SIEOS cross toolchain (x86_64-pc-sieos)

binutils 2.45 and GCC 15.2.0 (C, C++ with libstdc++) targeting SIEOS, with the
musl-based libc of `libc/` as the target root (`build/sysroot/usr`).

- `sieos-toolchain.py`: checks the SHA-256 of the release tarballs, extracts
  them and adds the `sieos` target (config.sub, binutils target tables,
  gcc/config.gcc, libgcc/config.host, libstdc++ cross configuration). No
  autoconf run is needed.
- `sieos.h`: GCC's target header (`gcc/config/sieos.h`): predefined macros,
  start files, static and static-PIE linking, and non-executable stacks.
- `tests/`: C and C++ programs run on SIEOS by `runall`.

    make toolchain        # download, prepare and build into build/cross
    make toolchain-test   # build the tests into build/toolchain-test
    build/cross/bin/x86_64-pc-sieos-g++ -O2 -o prog prog.cc

The tarballs come from ftp.gnu.org: binutils-2.45, gcc-15.2.0, gmp-6.3.0,
mpfr-4.2.2 and mpc-1.3.1 (GMP, MPFR and MPC are built in-tree for the host).
