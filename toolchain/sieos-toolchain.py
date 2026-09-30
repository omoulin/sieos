#!/usr/bin/env python3
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
"""
sieos-toolchain.py DL SRC - prepare binutils and GCC sources for x86_64-pc-sieos.

DL holds the release tarballs (checked against SHA256 below); SRC receives
the extracted trees with the SIEOS target added: config.sub, the binutils
target tables, gcc/config.gcc, gcc/config/sieos.h, libgcc/config.host and
libstdc++'s cross configuration (crossconfig.m4 and the generated
configure, so that no autoconf run is needed).  Every edit must match
exactly once.
"""
import hashlib, os, shutil, sys, tarfile

HERE = os.path.dirname(os.path.abspath(__file__))

TARBALLS = {
    'binutils-2.45.tar.xz': 'c50c0e7f9cb188980e2cc97e4537626b1672441815587f1eab69d2a1bfbef5d2',
    'gcc-15.2.0.tar.xz':    '438fd996826b0c82485a29da03a72d71d6e3541a83ec702df4271f6fe025d24e',
    'gmp-6.3.0.tar.xz':     'a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898',
    'mpfr-4.2.2.tar.xz':    'b67ba0383ef7e8a8563734e2e889ef5ec3c3b898a01d00fa0a6869ad81c6ce01',
    'mpc-1.3.1.tar.gz':     'ab642492f5cf882b74aa0cb730cd410a81edcdbec895183ce930e706c1c759b8',
}
BINUTILS, GCC = 'binutils-2.45', 'gcc-15.2.0'

def edit(path, old, new, count=1):
    text = open(path).read()
    n = text.count(old)
    if n != count:
        sys.exit('%s: expected %d match(es), found %d:\n%s' % (path, count, n, old))
    open(path, 'w').write(text.replace(old, new))

def config_sub(tree):
    p = os.path.join(tree, 'config.sub')
    text = open(p).read()
    if '| fuchsia* \\\n' in text:                      # newer config.sub (binutils)
        edit(p, '\t| fuchsia* \\\n', '\t| fuchsia* \\\n\t| sieos* \\\n')
    else:                                              # older (gcc)
        edit(p, '| fuchsia* |', '| fuchsia* | sieos* |')

def binutils(src):
    b = os.path.join(src, BINUTILS)
    config_sub(b)
    edit(os.path.join(b, 'bfd', 'config.bfd'),
         'x86_64-*-elf* | x86_64-*-rtems* | x86_64-*-fuchsia | x86_64-*-genode*)',
         'x86_64-*-elf* | x86_64-*-rtems* | x86_64-*-fuchsia | x86_64-*-genode* | x86_64-*-sieos*)')
    edit(os.path.join(b, 'ld', 'configure.tgt'),
         'x86_64-*-elf* | x86_64-*-rtems* | x86_64-*-fuchsia* | x86_64-*-genode*)',
         'x86_64-*-elf* | x86_64-*-rtems* | x86_64-*-fuchsia* | x86_64-*-genode* | x86_64-*-sieos*)')
    edit(os.path.join(b, 'gas', 'configure.tgt'),
         '  i386-*-fuchsia*)\t\t\tfmt=elf ;;\n',
         '  i386-*-fuchsia*)\t\t\tfmt=elf ;;\n  i386-*-sieos*)\t\t\tfmt=elf ;;\n')

def sieos_dip(g):
    edit(os.path.join(g, 'libgcc', 'unwind-dw2-fde-dip.c'),
         '    && defined(TARGET_DL_ITERATE_PHDR) \\\n    && defined(__linux__)\n',
         '    && (defined(__sieos__) || (defined(TARGET_DL_ITERATE_PHDR) \\\n    && defined(__linux__)))\n')
    edit(os.path.join(g, 'libgcc', 'crtstuff.c'),
         '    && defined(__sun__) && defined(__svr4__)\n#include <link.h>\n# define USE_PT_GNU_EH_FRAME\n#endif\n',
         '    && defined(__sun__) && defined(__svr4__)\n#include <link.h>\n# define USE_PT_GNU_EH_FRAME\n#endif\n'
         '#if defined(OBJECT_FORMAT_ELF) && defined(HAVE_LD_EH_FRAME_HDR) \\\n'
         '    && !defined(inhibit_libc) && !defined(CRTSTUFFT_O) && defined(__sieos__)\n'
         '#include <link.h>\n# define USE_PT_GNU_EH_FRAME\n#endif\n')

def gcc(src):
    g = os.path.join(src, GCC)
    config_sub(g)
    cg = os.path.join(g, 'gcc', 'config.gcc')
    edit(cg, '*-*-fuchsia*)\n  native_system_header_dir=/include\n',
         '''*-*-sieos*)
  gas=yes
  gnu_ld=yes
  default_use_cxa_atexit=yes
  use_gcc_stdint=wrap
  thread_file='posix'
  tmake_file="t-slibgcc"
  extra_options="$extra_options gnu-user.opt"
  ;;
*-*-fuchsia*)
  native_system_header_dir=/include
''')
    edit(cg, 'x86_64-*-fuchsia*)\n\ttmake_file="${tmake_file} i386/t-x86_64-elf"\n',
         '''x86_64-*-sieos*)
\ttm_file="${tm_file} i386/unix.h i386/att.h elfos.h glibc-stdint.h i386/i386elf.h i386/x86-64.h sieos.h"
\t;;
x86_64-*-fuchsia*)
\ttmake_file="${tmake_file} i386/t-x86_64-elf"
''')
    shutil.copy(os.path.join(HERE, 'sieos.h'), os.path.join(g, 'gcc', 'config', 'sieos.h'))
    edit(os.path.join(g, 'libgcc', 'config.host'), 'x86_64-*-fuchsia*)\n\ttmake_file="$tmake_file t-libgcc-pic"\n\t;;\n',
         '''x86_64-*-fuchsia*)
\ttmake_file="$tmake_file t-libgcc-pic"
\t;;
x86_64-*-sieos*)
\ttmake_file="$tmake_file i386/t-crtstuff t-crtstuff-pic t-libgcc-pic t-eh-dw2-dip"
\ttmake_file="$tmake_file t-slibgcc t-slibgcc-gld t-slibgcc-elf-ver"
\textra_parts="$extra_parts crtbegin.o crtbeginS.o crtbeginT.o crtend.o crtendS.o"
\t;;
''')
    # the unwinder finds FDEs through dl_iterate_phdr and PT_GNU_EH_FRAME,
    # as on Linux, so one copy of it sees every loaded object
    sieos_dip(g)
    # libstdc++: configure like the other libcs with link tests (crossconfig)
    pat = '*-linux* | *-uclinux* | *-gnu* | *-kfreebsd*-gnu | *-cygwin* | *-solaris*)'
    for f in ['crossconfig.m4', 'configure']:
        edit(os.path.join(g, 'libstdc++-v3', f), pat, pat[:-1] + ' | *-sieos*)')
    # libtool: shared libraries work as on Linux (ELF, sonames, pass_all);
    # the generated configure scripts are edited so no autoconf run is needed
    n = 0
    for root, dirs, files in os.walk(g):
        for f in files:
            if f != 'configure':
                continue
            path = os.path.join(root, f)
            text = open(path).read()
            new = text.replace('\nlinux* | k*bsd*-gnu | kopensolaris*-gnu | uclinuxfdpiceabi)\n  lt_cv_deplibs_check_method=pass_all',
                               '\nlinux* | k*bsd*-gnu | kopensolaris*-gnu | uclinuxfdpiceabi | sieos*)\n  lt_cv_deplibs_check_method=pass_all')
            new = new.replace('\nlinux* | k*bsd*-gnu | kopensolaris*-gnu | gnu* | uclinuxfdpiceabi)\n  version_type=linux',
                              '\nlinux* | k*bsd*-gnu | kopensolaris*-gnu | gnu* | uclinuxfdpiceabi | sieos*)\n  version_type=linux')
            if new != text:
                open(path, 'w').write(new)
                n += 1
    if n < 3:
        sys.exit('libtool patterns found in only %d configure scripts' % n)
    # in-tree math libraries for the host compiler
    for lib in ['gmp-6.3.0', 'mpfr-4.2.2', 'mpc-1.3.1']:
        os.symlink(os.path.join('..', lib), os.path.join(g, lib.split('-')[0]))

def main():
    dl, src = sys.argv[1], sys.argv[2]
    for name, want in TARBALLS.items():
        path = os.path.join(dl, name)
        h = hashlib.sha256(open(path, 'rb').read()).hexdigest()
        if h != want:
            sys.exit('%s: sha256 %s, expected %s' % (name, h, want))
    if os.path.exists(src):
        shutil.rmtree(src)
    os.makedirs(src)
    for name in TARBALLS:
        with tarfile.open(os.path.join(dl, name)) as t:
            t.extractall(src)
    binutils(src)
    gcc(src)

if __name__ == '__main__':
    main()
