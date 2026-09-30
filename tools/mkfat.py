#!/usr/bin/env python3
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
"""
mkfat.py IMAGE SIZE_KB DEST=SRC...

Create a FAT12/16 image (formatted by mkfs.fat) and copy files into it,
creating directories as needed.  Used to build the EFI system partition
image for the ISO without needing mtools.

Example: mkfat.py efi.img 4096 EFI/BOOT/BOOTX64.EFI=build/BOOTX64.EFI
"""
import os
import struct
import subprocess
import sys


def main():
    img, size_kb, specs = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    if os.path.exists(img):
        os.remove(img)
    subprocess.run(['mkfs.fat', '-C', '-n', 'SIEOS-EFI', img, str(size_kb)],
                   check=True, stdout=subprocess.DEVNULL)
    data = bytearray(open(img, 'rb').read())

    bps, spc, rsvd, nfats, nroot, tot16, _, fatsz = struct.unpack_from('<HBHBHHBH', data, 11)
    tot = tot16 or struct.unpack_from('<I', data, 32)[0]
    root_sec = rsvd + nfats * fatsz
    root_secs = (nroot * 32 + bps - 1) // bps
    data_sec = root_sec + root_secs
    nclusters = (tot - data_sec) // spc
    fat12 = nclusters < 4085
    csize = bps * spc
    next_free = [2]

    def set_fat(cl, val):
        for f in range(nfats):
            base = (rsvd + f * fatsz) * bps
            if fat12:
                off = base + cl * 3 // 2
                cur = struct.unpack_from('<H', data, off)[0]
                if cl & 1:
                    cur = (cur & 0x000F) | (val << 4)
                else:
                    cur = (cur & 0xF000) | (val & 0x0FFF)
                struct.pack_into('<H', data, off, cur & 0xFFFF)
            else:
                struct.pack_into('<H', data, base + cl * 2, val & 0xFFFF)

    eoc = 0xFFF if fat12 else 0xFFFF

    def alloc(nbytes):
        n = max(1, (nbytes + csize - 1) // csize)
        first = next_free[0]
        if first + n - 2 > nclusters:
            sys.exit('mkfat: image too small')
        for i in range(n):
            set_fat(first + i, first + i + 1 if i + 1 < n else eoc)
        next_free[0] += n
        return first

    def cl_off(cl):
        return (data_sec + (cl - 2) * spc) * bps

    def name83(name):
        base, _, ext = name.upper().partition('.')
        if not base or len(base) > 8 or len(ext) > 3 or '.' in ext:
            sys.exit(f'mkfat.py: {name}: not an 8.3 name (the image has no long file names)')
        return base.ljust(8).encode() + ext.ljust(3).encode()

    def dirent(name11, attr, cl, size):
        return struct.pack('<11sBBBHHHHHHHI', name11, attr, 0, 0, 0, 0, 0, 0, 0x21, 0, cl, size)

    # directory -> (offset of entry area, max entries, cluster)
    dirs = {'': (root_sec * bps, nroot, 0)}
    used = {'': 0}

    def add_entry(parent, entry):
        off, maxn, _ = dirs[parent]
        if used[parent] >= maxn:
            sys.exit('mkfat: directory full')
        data[off + used[parent] * 32: off + used[parent] * 32 + 32] = entry
        used[parent] += 1

    def mkdir(path):
        if path in dirs:
            return
        parent = os.path.dirname(path)
        mkdir(parent)
        cl = alloc(csize)
        off = cl_off(cl)
        dirs[path] = (off, csize // 32, cl)
        used[path] = 0
        add_entry(path, dirent(b'.          ', 0x10, cl, 0))
        add_entry(path, dirent(b'..         ', 0x10, dirs[parent][2], 0))
        add_entry(parent, dirent(name83(os.path.basename(path)), 0x10, cl, 0))

    for spec in specs:
        dest, src = spec.split('=', 1)
        content = open(src, 'rb').read()
        parent = os.path.dirname(dest)
        mkdir(parent)
        cl = alloc(len(content))
        off = cl_off(cl)
        data[off: off + len(content)] = content
        add_entry(parent, dirent(name83(os.path.basename(dest)), 0x20, cl, len(content)))

    open(img, 'wb').write(data)


if __name__ == '__main__':
    main()
