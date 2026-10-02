#!/usr/bin/env python3
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
"""
build.py NAME DL SRCDIR DESTDIR - cross-build a port for x86_64-pc-sieos.

Extracts DL/<tarball> under SRCDIR, teaches its config.sub files sieos,
configures with --host=x86_64-pc-sieos (the cross compiler must be on
PATH), builds, and installs into DESTDIR.  The ports:
  GNU coreutils, sed, grep, diffutils, findutils, gawk, make, tar, gzip: /usr/gnu
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
               'ac_cv_func_realloc_0_nonnull=yes', 'gl_cv_func_malloc_0_nonnull=yes',
               # (else gnulib replaces fchownat(AT_SYMLINK_NOFOLLOW) by a fchdir dance that failed:
               # cp -a could not keep a symbolic link's owner)
               'gl_cv_func_fchownat_nofollow_works=yes', 'gl_cv_func_chown_ctime_works=yes']
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
    'tar': ('tar-1.35.tar.xz', GNU + ['--without-selinux', '--without-posix-acls', '--without-xattrs']),
    'gzip': ('gzip-1.13.tar.xz', GNU),
}

def teach_config_sub(tree):
    # (config.sub, and copies under other names: SQLite's autosetup-config.sub)
    for path in glob.glob(os.path.join(tree, '**', '*config.sub'), recursive=True):
        text = open(path).read()
        if 'sieos' in text:
            continue
        new, n = re.subn(r'\| fuchsia\* ', '| fuchsia* | sieos* ', text, count=1)
        if not n:
            new, n = re.subn(r'\t\| fuchsia\* \\\n', '\t| fuchsia* \\\n\t| sieos* \\\n', text, count=1)
        if not n:
            sys.exit('%s: no place for sieos' % path)
        open(path, 'w').write(new)

# libtool knows the systems whose shared libraries it can build; SIEOS's are
# Linux's kind (ELF, sonames, ld.so searching LD_LIBRARY_PATH and the path
# file), so the case patterns of libtool's code in configure scripts that
# name linux* get sieos* too (without it: static libraries only).
LINUX_CASE = re.compile(r'^(\s*)((?:[\w*.+-]+\s*\|\s*)*linux\*(?:\s*\|\s*[\w*.+-]+)*)\)(\s*)$')

def teach_libtool(tree):
    for path in glob.glob(os.path.join(tree, '**', 'configure'), recursive=True):
        if not os.path.isfile(path):
            continue
        lines = open(path, errors='surrogateescape').read().split('\n')
        if 'ltmain' not in '\n'.join(lines) or any('sieos*' in l for l in lines):
            continue
        lines = [LINUX_CASE.sub(lambda m: '%s%s | sieos*)%s' % (m.group(1), m.group(2), m.group(3)), l) for l in lines]
        open(path, 'w', errors='surrogateescape').write('\n'.join(lines))

def run(cmd, cwd, log):
    with open(log, 'a') as f:
        f.write('+ %s\n' % ' '.join(cmd))
        f.flush()
        r = subprocess.run(cmd, cwd=cwd, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode:
        sys.exit('%s failed (see %s)' % (' '.join(cmd[:2]), log))

def fix_shebangs(dest):
    """Scripts that name the build host's shell (#!/bin/bash) run with SIEOS's POSIX /bin/sh."""
    for root, _, files in os.walk(dest):
        for f in files:
            path = os.path.join(root, f)
            if os.path.islink(path) or not os.access(path, os.X_OK):
                continue
            with open(path, 'rb') as fh:
                head = fh.read(64)
            if not head.startswith(b'#!'):
                continue
            line = head.split(b'\n', 1)[0]
            if line.split()[0] in (b'#!/bin/bash', b'#!/usr/bin/bash', b'#!/usr/bin/sh'):
                data = open(path, 'rb').read()
                open(path, 'wb').write(b'#!/bin/sh' + data[len(line.split()[0]):])

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
    fix_shebangs(dest)

if __name__ == '__main__':
    main()
