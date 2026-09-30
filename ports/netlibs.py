#!/usr/bin/env python3
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
"""
netlibs.py NAME DL SRCDIR DESTDIR - cross-build a library NetSurf needs.

The libraries under the web browser: zlib, libpng, libjpeg (IJG), expat,
FreeType, Mbed TLS and curl, static (and position-independent, for shared
objects and PIEs alike), installed with prefix /usr into DESTDIR (a staging
root, not the SDK sysroot); their pkg-config files then name DESTDIR/usr.  Each finds the ones it builds on in DESTDIR,
so the Makefile orders them.  The cross compiler must be on PATH.
"""
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build import TARGET, CROSS_CACHE, teach_config_sub, run

CC = TARGET + '-gcc'
AR = TARGET + '-ar'
RANLIB = TARGET + '-ranlib'
CFLAGS = '-O2 -fPIC -std=gnu17'
STATIC = ['--disable-shared', '--enable-static']

LIBS = {
    'zlib':     'zlib-1.3.2.tar.xz',
    'libpng':   'libpng-1.6.58.tar.xz',
    'jpeg':     'jpegsrc.v9f.tar.gz',
    'expat':    'expat-2.8.5.tar.xz',
    'freetype': 'freetype-2.14.3.tar.xz',
    'mbedtls':  'mbedtls-3.6.7.tar.bz2',
    'curl':     'curl-8.22.0.tar.xz',
}

def autoconf(tree, log, dest, extra):
    usr = os.path.join(dest, 'usr')
    run(['./configure', '--host=' + TARGET, '--prefix=/usr', 'CFLAGS=' + CFLAGS,
         'CPPFLAGS=-I' + os.path.join(usr, 'include'), 'LDFLAGS=-L' + os.path.join(usr, 'lib'),
         'PKG_CONFIG=false'] + STATIC + extra + CROSS_CACHE, tree, log)
    run(['make', '-j%d' % os.cpu_count()], tree, log)
    run(['make', 'install', 'DESTDIR=' + dest], tree, log)

def zlib(tree, log, dest):
    # its own configure: the cross tools come from CHOST
    env = 'CHOST=%s CFLAGS="%s"' % (TARGET, CFLAGS)
    run(['sh', '-c', env + ' ./configure --static --prefix=/usr'], tree, log)
    run(['make', '-j%d' % os.cpu_count(), 'libz.a'], tree, log)
    run(['make', 'install', 'DESTDIR=' + dest], tree, log)

def mbedtls(tree, log, dest):
    # the libraries only (make lib), installed by hand: its install target
    # also builds the test programs, and takes DESTDIR as the prefix
    run(['make', '-j%d' % os.cpu_count(), '-C', 'library', 'CC=' + CC, 'AR=' + AR,
         'CFLAGS=' + CFLAGS, 'static'], tree, log)
    inc = os.path.join(dest, 'usr', 'include')
    lib = os.path.join(dest, 'usr', 'lib')
    os.makedirs(lib, exist_ok=True)
    for d in ['mbedtls', 'psa']:
        shutil.rmtree(os.path.join(inc, d), ignore_errors=True)
        shutil.copytree(os.path.join(tree, 'include', d), os.path.join(inc, d))
    for a in ['libmbedcrypto.a', 'libmbedx509.a', 'libmbedtls.a']:
        shutil.copy(os.path.join(tree, 'library', a), lib)

def curl(tree, log, dest):
    usr = os.path.join(dest, 'usr')
    autoconf(tree, log, dest, [
        '--with-mbedtls=' + usr, '--with-zlib=' + usr,
        '--with-ca-bundle=/etc/ssl/certs.pem', '--without-ca-path',
        '--enable-threaded-resolver', '--without-libpsl', '--without-brotli', '--without-zstd',
        '--without-nghttp2', '--without-libidn2', '--without-librtmp', '--without-libssh2',
        '--disable-ldap', '--disable-ldaps', '--disable-rtsp', '--disable-dict', '--disable-telnet',
        '--disable-tftp', '--disable-pop3', '--disable-imap', '--disable-smtp', '--disable-gopher',
        '--disable-mqtt', '--disable-smb', '--disable-manual', '--disable-docs',
        # the C library has eventfd() but the kernel has no eventfd2 (ENOSYS): curl's
        # wakeup channel would fail, and every transfer with it ("out of memory"); a pipe
        'ac_cv_func_eventfd=no'])

RECIPES = {
    'zlib': zlib,
    'libpng': lambda t, l, d: autoconf(t, l, d, ['--disable-tools']),
    'jpeg': lambda t, l, d: autoconf(t, l, d, []),
    'expat': lambda t, l, d: autoconf(t, l, d, ['--without-docbook', '--without-examples',
                                                '--without-tests', '--without-xmlwf']),
    # no zlib, bzip2, png, brotli or HarfBuzz: NetSurf gives FreeType plain TrueType files
    'freetype': lambda t, l, d: autoconf(t, l, d, ['--with-zlib=no', '--with-bzip2=no', '--with-png=no',
                                                   '--with-brotli=no', '--with-harfbuzz=no']),
    'mbedtls': mbedtls,
    'curl': curl,
}

def main():
    name, dl, srcdir, dest = sys.argv[1:5]
    dest = os.path.abspath(dest)
    tarball = LIBS[name]
    top = subprocess.run(['tar', 'tf', os.path.join(dl, tarball)], check=True,
                         capture_output=True, text=True).stdout.split('/', 1)[0]
    tree = os.path.join(srcdir, top)
    shutil.rmtree(tree, ignore_errors=True)
    subprocess.run(['tar', 'xf', os.path.join(dl, tarball), '-C', srcdir], check=True)
    teach_config_sub(tree)
    log = os.path.join(srcdir, name + '.log')
    open(log, 'w').close()
    RECIPES[name](tree, log, dest)
    usr = os.path.join(dest, 'usr')
    for d in ['lib/pkgconfig', 'share/pkgconfig']:
        pcdir = os.path.join(usr, d)
        for pc in os.listdir(pcdir) if os.path.isdir(pcdir) else []:
            path = os.path.join(pcdir, pc)
            text = open(path).read()
            # (FreeType's name every directory in full: exec_prefix, libdir, includedir)
            open(path, 'w').write(re.sub(r'^(\w+)=/usr(/|$)', lambda m: m.group(1) + '=' + usr + m.group(2),
                                         text, flags=re.M))

if __name__ == '__main__':
    main()
