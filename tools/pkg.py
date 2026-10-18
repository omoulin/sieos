#!/usr/bin/env python3
"""SIEOS's add-on packages on the host (docs/packages.md): make one, show
one, install one into a SieFS image (make usb PKGS=..., newdisk, sdcard;
the tests' disks). On SIEOS, /bin/pkg (user/pkg) reads the same format.

A package is a text manifest, then the files' data:
    SIEOS-PKG 1
    name web
    version 20261018
    size 92345678                   (the bytes it installs)
    dir /usr/libexec/wpe-webkit-2.0 (made if missing, in this order)
    file 755 90015184 SHA256 /bin/web
    link /bin/web /usr/libexec/wpe-webkit-2.0/WPEWebProcess
    end
Then each file's data in the manifest's order, in pieces of 1 MiB (the last
one shorter): a 4-byte little-endian header, the piece's stored length,
bit 31 set when it is stored as is, else a zlib stream of the piece.
SHA256 is the hex SHA-256 of the file's content. An installed package is
recorded in /etc/pkg/NAME: its manifest, with only the "dir" lines of the
directories it made (so that removing it is exact).

Usage:
  pkg.py build OUT NAME VERSION [--dir D]... [--file SRC DST MODE]... [--link TARGET DST]...
  pkg.py show PKG
  pkg.py install SIEFS-TOOL IMAGE PKG...

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import hashlib, os, struct, subprocess, sys, tempfile, zlib

PIECE = 1 << 20
MAGIC = "SIEOS-PKG 1"


def build(out, name, version, dirs, files, links):
    lines = [MAGIC, f"name {name}", f"version {version}",
             f"size {sum(os.path.getsize(s) for s, _, _ in files)}"]
    lines += [f"dir {d}" for d in dirs]
    for src, dst, mode in files:
        h = hashlib.sha256(open(src, "rb").read()).hexdigest()
        lines.append(f"file {mode} {os.path.getsize(src)} {h} {dst}")
    lines += [f"link {t} {d}" for t, d in links]
    tmp = out + ".part"
    with open(tmp, "wb") as f:
        f.write(("\n".join(lines) + "\nend\n").encode())
        for src, _, _ in files:
            with open(src, "rb") as s:
                stored = 0                       # (4 pieces in a row that do not shrink: the rest is stored as is)
                while piece := s.read(PIECE):
                    z = zlib.compress(piece, 6) if stored < 4 else piece
                    if len(z) < len(piece) * 15 // 16:
                        f.write(struct.pack("<I", len(z)) + z); stored = 0
                    else:
                        f.write(struct.pack("<I", len(piece) | 1 << 31) + piece); stored += 1
    os.replace(tmp, out)


def read_manifest(f):
    """-> (the manifest's lines, without 'end'), the file positioned at the data."""
    lines = []
    while True:
        ln = f.readline().decode().rstrip("\n")
        if not ln and not lines:
            sys.exit("pkg: empty file")
        if ln == "end":
            break
        lines.append(ln)
        if len(lines) == 1 and ln != MAGIC:
            sys.exit("pkg: not a SIEOS package")
    return lines


def field(lines, key):
    return next((ln.split(" ", 1)[1] for ln in lines if ln.startswith(key + " ")), "")


def size(path):
    """The bytes a package installs (mkusb.py sizes its image with it)."""
    with open(path, "rb") as f:
        return int(field(read_manifest(f), "size") or 0)


def extract(f, nbytes, out):
    """One file's pieces from f into out -> its SHA-256 (hex)."""
    h = hashlib.sha256()
    while nbytes > 0:
        n, = struct.unpack("<I", f.read(4))
        d = f.read(n & 0x7FFFFFFF)
        if not n >> 31:
            d = zlib.decompress(d)
        h.update(d); out.write(d)
        nbytes -= len(d)
    return h.hexdigest()


def install(tool, image, path):
    def siefs(*a, check=True):
        return subprocess.run([tool, image, *a], check=check, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
    with open(path, "rb") as f, tempfile.TemporaryDirectory() as tmp:
        lines = read_manifest(f)
        name = field(lines, "name")
        record = []
        for ln in lines:
            w = ln.split(" ")
            if w[0] == "dir":
                if not siefs("stat", w[1], check=False):
                    siefs("mkdir", w[1]); record.append(ln)
            elif w[0] == "file":
                mode, nbytes, sha, dst = w[1], int(w[2]), w[3], w[4]
                t = os.path.join(tmp, "f")
                with open(t, "wb") as o:
                    if extract(f, nbytes, o) != sha:
                        sys.exit(f"pkg: {path}: {dst}: bad SHA-256")
                siefs("put", t, dst); siefs("chmod", mode, dst); record.append(ln)
            elif w[0] == "link":
                siefs("rm", w[2], check=False); siefs("symlink", w[1], w[2]); record.append(ln)
            else:
                record.append(ln)
        rec = os.path.join(tmp, "record")
        open(rec, "w").write("\n".join(record) + "\nend\n")
        siefs("mkdir", "/etc/pkg", check=False)
        siefs("put", rec, "/etc/pkg/" + name)
    print(f"pkg: {name} {field(lines, 'version')} installed on {image}")


def main():
    a = sys.argv[1:]
    if a and a[0] == "build" and len(a) >= 4:
        dirs, files, links, i = [], [], [], 4
        while i < len(a):
            if a[i] == "--dir": dirs.append(a[i + 1]); i += 2
            elif a[i] == "--file": files.append((a[i + 1], a[i + 2], a[i + 3])); i += 4
            elif a[i] == "--link": links.append((a[i + 1], a[i + 2])); i += 3
            else: sys.exit(f"pkg: unknown option {a[i]}")
        build(a[1], a[2], a[3], dirs, files, links)
    elif a and a[0] == "show" and len(a) == 2:
        with open(a[1], "rb") as f:
            print("\n".join(read_manifest(f)))
    elif a and a[0] == "install" and len(a) >= 4:
        for p in a[3:]:
            install(a[1], a[2], p)
    else:
        sys.exit(__doc__.split("Usage:")[1].split("Part of")[0].rstrip())


if __name__ == "__main__":
    main()
