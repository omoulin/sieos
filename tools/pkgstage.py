#!/usr/bin/env python3
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
"""pkgstage.py DIR PACKAGE.spkg ... - install packages on the build host into
DIR, the root of a file system that SIEOS mounts on /usr/pkg (a USB drive's
package partition, see rootfs/etc/rc), as pkg would: the files of usr/pkg/X
in DIR/X, the database (MANIFEST, FILES of each package) in DIR/.pkgdb, which
rc puts at /var/lib/pkg."""
import gzip
import os
import sys
import tarfile

PREFIX = 'usr/pkg'
KEYS = ('name', 'version', 'summary', 'depends', 'size')


def manifest(text):
    rec = {}
    for line in text.splitlines():
        if not line.strip():
            break
        k, _, v = line.partition(':')
        rec[k.strip()] = v.strip()
    return rec


def stage(dest, path):
    with gzip.open(path, 'rb') as z, tarfile.open(fileobj=z, mode='r|') as tar:
        rec, files = None, []
        for m in tar:
            name = m.name[2:] if m.name.startswith('./') else m.name
            name = name.rstrip('/')
            if rec is None:
                if name != '+MANIFEST':
                    sys.exit('%s: not a SIEOS package (no +MANIFEST)' % path)
                rec = manifest(tar.extractfile(m).read().decode())
                if not rec.get('name') or not rec.get('version'):
                    sys.exit('%s: a manifest without name or version' % path)
                continue
            parts = name.split('/')
            if (name != PREFIX and not name.startswith(PREFIX + '/')) or '..' in parts:
                sys.exit('%s: %s: outside %s' % (path, name, PREFIX))
            rel = name[len(PREFIX) + 1:]
            out = os.path.join(dest, rel) if rel else dest
            if m.isdir():
                os.makedirs(out, exist_ok=True)
                os.chmod(out, (m.mode & 0o7777) | 0o700)
                files.append('d ' + name)
            elif m.issym():
                os.makedirs(os.path.dirname(out), exist_ok=True)
                if os.path.lexists(out):
                    os.unlink(out)
                os.symlink(m.linkname, out)
                files.append('l ' + name)
            elif m.isfile():
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with tar.extractfile(m) as src, open(out, 'wb') as dst:
                    while True:
                        b = src.read(1 << 20)
                        if not b:
                            break
                        dst.write(b)
                os.chmod(out, m.mode & 0o7777)
                files.append('f ' + name)
            else:
                sys.exit('%s: %s: only files, directories and symbolic links' % (path, name))
    db = os.path.join(dest, '.pkgdb', rec['name'])
    os.makedirs(db, exist_ok=True)
    with open(os.path.join(db, 'MANIFEST'), 'w') as f:
        f.write(''.join('%s: %s\n' % (k, rec.get(k, '0' if k == 'size' else '')) for k in KEYS))
    with open(os.path.join(db, 'FILES'), 'w') as f:
        f.write(''.join(l + '\n' for l in files))
    print('%s %s staged' % (rec['name'], rec['version']))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    os.umask(0o022)                                 # (as pkg, run by root, makes them)
    dest = sys.argv[1]
    os.makedirs(os.path.join(dest, '.pkgdb'), exist_ok=True)
    for p in sys.argv[2:]:
        stage(dest, p)


if __name__ == '__main__':
    main()
