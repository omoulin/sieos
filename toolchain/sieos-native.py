#!/usr/bin/env python3
"""
sieos-native.py SRC - prepare the extracted toolchain sources (see
sieos-toolchain.py) for the native build: binutils and GCC hosted on SIEOS
(a Canadian cross: build = Linux, host = target = x86_64-pc-sieos).

The math libraries GCC builds in-tree for its host carry their own
config.sub, which must know sieos.  Idempotent.
"""
import os
import sys

SUBS = ['gmp-6.3.0/configfsf.sub', 'mpfr-4.2.2/config.sub', 'mpc-1.3.1/build-aux/config.sub']
OLD = '| onefs* | tirtos* | phoenix* | fuchsia* | redox* | bme* \\'
NEW = '| onefs* | tirtos* | phoenix* | fuchsia* | sieos* | redox* | bme* \\'

def main():
    src = sys.argv[1]
    for rel in SUBS:
        path = os.path.join(src, rel)
        text = open(path).read()
        if NEW in text:
            continue
        if text.count(OLD) != 1:
            sys.exit('%s: config.sub pattern not found' % path)
        open(path, 'w').write(text.replace(OLD, NEW))

if __name__ == '__main__':
    main()
