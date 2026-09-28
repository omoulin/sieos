#!/usr/bin/env python3
"""
build.py NAME DL SRCDIR DESTDIR - cross-build a port for x86_64-pc-sieos.

Extracts DL/<tarball> under SRCDIR, teaches its config.sub files sieos,
configures with --host=x86_64-pc-sieos (the cross compiler must be on
PATH), builds, and installs into DESTDIR.  The ports:
  GNU coreutils, sed, grep, diffutils, findutils, gawk, make: /usr/gnu
  (as on Solaris 11; the SIEOS programs keep /bin), make also /usr/bin
"""
import glob
import os
import re
import shutil
import subprocess
import sys

TARGET = 'x86_64-pc-sieos'
GNU = ['--prefix=/usr/gnu', '--disable-nls']
# configure results that would need to run a target program
CROSS_CACHE = ['gl_cv_func_getcwd_path_max=yes', 'gl_cv_func_realpath_works=yes',
               'gl_cv_func_working_mkstemp=yes', 'ac_cv_func_malloc_0_nonnull=yes',
               'ac_cv_func_realloc_0_nonnull=yes', 'gl_cv_func_malloc_0_nonnull=yes']
PORTS = {
    'coreutils': ('coreutils-9.5.tar.xz', GNU + ['--without-selinux', '--disable-libcap', '--without-openssl',
                                                 '--disable-acl', '--disable-xattr', '--without-gmp',
                                                 '--enable-no-install-program=stdbuf']),
    'sed': ('sed-4.9.tar.xz', GNU + ['--without-selinux', '--disable-acl']),
    'grep': ('grep-3.11.tar.xz', GNU + ['--disable-perl-regexp']),
    'diffutils': ('diffutils-3.10.tar.xz', GNU),
    'findutils': ('findutils-4.10.0.tar.xz', GNU + ['--without-selinux']),
    'gawk': ('gawk-5.3.1.tar.xz', GNU + ['--without-readline', '--without-mpfr', '--disable-extensions']),
    'make': ('make-4.4.1.tar.gz', GNU + ['--without-guile']),
}

def teach_config_sub(tree):
    for path in glob.glob(os.path.join(tree, '**', 'config.sub'), recursive=True):
        text = open(path).read()
        if 'sieos' in text:
            continue
        new, n = re.subn(r'\| fuchsia\* ', '| fuchsia* | sieos* ', text, count=1)
        if not n:
            new, n = re.subn(r'\t\| fuchsia\* \\\n', '\t| fuchsia* \\\n\t| sieos* \\\n', text, count=1)
        if not n:
            sys.exit('%s: no place for sieos' % path)
        open(path, 'w').write(new)

def run(cmd, cwd, log):
    with open(log, 'a') as f:
        f.write('+ %s\n' % ' '.join(cmd))
        f.flush()
        r = subprocess.run(cmd, cwd=cwd, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode:
        sys.exit('%s failed (see %s)' % (' '.join(cmd[:2]), log))

def main():
    name, dl, srcdir, dest = sys.argv[1:5]
    tarball, conf = PORTS[name]
    tree = os.path.join(srcdir, re.sub(r'\.tar\.\w+$', '', tarball))
    shutil.rmtree(tree, ignore_errors=True)
    subprocess.run(['tar', 'xf', os.path.join(dl, tarball), '-C', srcdir], check=True)
    teach_config_sub(tree)
    log = os.path.join(srcdir, name + '.log')
    open(log, 'w').close()
    # gnu17: GCC 15 defaults to C23, where the K&R declarations in older bundled
    # getopt/fnmatch copies no longer compile
    run(['./configure', '--host=' + TARGET, 'CFLAGS=-O2 -std=gnu17'] + conf + CROSS_CACHE, tree, log)
    run(['make', '-j%d' % os.cpu_count()], tree, log)
    run(['make', 'install', 'DESTDIR=' + os.path.abspath(dest)], tree, log)

if __name__ == '__main__':
    main()
