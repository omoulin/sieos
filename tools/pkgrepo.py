#!/usr/bin/env python3
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
"""
pkgrepo.py key KEY PUB        make the signing key KEY (if absent), write its public half to PUB
pkgrepo.py index REPO KEY     write REPO/INDEX from REPO/*.spkg (the newest two versions of each),
                              and REPO/INDEX.sig, KEY's signature of it

The key is an ECDSA P-256 private key (PEM).  It stays outside the source
tree (by default ~/.config/sieos/pkg-signing-key.pem): whoever has it can
publish packages SIEOS systems carrying its public half (/etc/pkg/keys)
install.  PUB is that public half as pkg reads it: 04||X||Y in hexadecimal.
INDEX.sig is the DER ECDSA signature of INDEX's SHA-256, as pkg checks it.
The keys and signatures are made by the openssl command.
"""
import hashlib
import os
import re
import subprocess
import sys
import tarfile


def openssl(*args, **kw):
    return subprocess.run(['openssl'] + list(args), check=True, capture_output=True, **kw).stdout


def cmd_key(key, pub):
    if not os.path.exists(key):
        os.makedirs(os.path.dirname(key), exist_ok=True)
        pem = openssl('ecparam', '-name', 'prime256v1', '-genkey', '-noout')
        fd = os.open(key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'wb') as f:
            f.write(pem)
        print('pkgrepo: made the package signing key %s (keep it: packages signed with another key '
              'will not install on systems built with this one)' % key)
    der = openssl('ec', '-in', key, '-pubout', '-outform', 'DER')   # SubjectPublicKeyInfo: the point last
    raw = der[-65:]
    if raw[0] != 4:
        sys.exit('%s: not a P-256 key' % key)
    os.makedirs(os.path.dirname(os.path.abspath(pub)), exist_ok=True)
    open(pub, 'w').write(raw.hex() + '\n')


def natural(v):
    return [int(x) if x.isdigit() else x for x in re.split(r'(\d+)', v)]


def cmd_index(repo, key):
    pkgs = {}
    for f in sorted(os.listdir(repo)):
        if not f.endswith('.spkg'):
            continue
        path = os.path.join(repo, f)
        with tarfile.open(path, 'r:gz') as t:
            first = t.next()
            if first is None or first.name != '+MANIFEST':
                sys.exit('%s: no +MANIFEST first' % path)
            man = dict(l.split(': ', 1) for l in t.extractfile(first).read().decode().splitlines() if ': ' in l)
        man['file'] = f
        man['sha256'] = hashlib.sha256(open(path, 'rb').read()).hexdigest()
        pkgs.setdefault(man['name'], []).append(man)
    out = []
    for name in sorted(pkgs):
        for m in sorted(pkgs[name], key=lambda m: natural(m['version']))[-2:]:   # (the newest two)
            out.append('name: %s\nversion: %s\nsummary: %s\ndepends: %s\nsize: %s\nfile: %s\nsha256: %s\n' %
                       (m['name'], m['version'], m.get('summary', ''), m.get('depends', ''), m.get('size', '0'),
                        m['file'], m['sha256']))
    index = '\n'.join(out).encode()
    open(os.path.join(repo, 'INDEX'), 'wb').write(index)
    sig = openssl('dgst', '-sha256', '-sign', key, os.path.join(repo, 'INDEX'))
    open(os.path.join(repo, 'INDEX.sig'), 'wb').write(sig)
    print('%s/INDEX: %d package%s, signed' % (repo, len(pkgs), '' if len(pkgs) == 1 else 's'))


def main():
    if len(sys.argv) == 4 and sys.argv[1] == 'key':
        cmd_key(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == 'index':
        cmd_index(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
