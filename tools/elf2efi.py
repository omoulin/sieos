#!/usr/bin/env python3
"""elf2efi.py - Turn SIEOS's UEFI loader into the file UEFI firmware runs:
a PE32+ "EFI application".

The loader is linked twice by sicc, at two addresses (boot/uefi/efi.ld). Every
place where the two copies differ by exactly the distance between them is a
64-bit address in the code or data: those become the PE "base relocations",
so the firmware can load the loader anywhere. Any other difference means code
that is not position-independent (compile with -fpic): refused.

Usage: elf2efi.py A.elf B.elf out.efi   (A linked at the lower address)

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
"""
import struct, sys

def elf(path):
    """The loadable sections of an ELF64 file: {name: (address, bytes, size)}, and its entry."""
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 2:
        sys.exit(f"{path}: not an ELF64 file")
    entry, shoff = struct.unpack_from("<Q", d, 24)[0], struct.unpack_from("<Q", d, 40)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 58)
    sh = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
    strtab = sh[shstrndx]
    name = lambda off: d[strtab[4] + off:d.index(b"\0", strtab[4] + off)].decode()
    out = {}
    for s in sh:
        n = name(s[0])
        if n in (".text", ".data"):
            size = s[5]
            data = d[s[4]:s[4] + size] if s[1] != 8 else b"\0" * size   # (8: no bits, a .bss)
            out[n] = (s[3], data, size)
    return out, entry

def main():
    a, ea = elf(sys.argv[1])
    b, eb = elf(sys.argv[2])
    delta = b[".text"][0] - a[".text"][0]
    base = a[".text"][0] - 0x1000                       # (efi.ld: BASE + 0x1000)
    relocs = []
    for n in (".text", ".data"):
        if n not in a:
            continue
        (va, da, size), (_, db, _) = a[n], b[n]
        i = 0
        while i < size:
            if da[i] == db[i]:
                i += 1
                continue
            if i + 8 <= size and struct.unpack_from("<Q", db, i)[0] - struct.unpack_from("<Q", da, i)[0] == delta:
                relocs.append(va - base + i)
                i += 8
                continue
            sys.exit(f"{n}+{i:#x}: an address that is not 64 bits (compile with -fpic)")
    # The .reloc section: per 4 KiB page, its RVA, its size, then 16-bit entries (type 10: 64-bit).
    pages = {}
    for r in relocs:
        pages.setdefault(r & ~0xFFF, []).append(0xA000 | (r & 0xFFF))
    reloc = b""
    for p in sorted(pages):
        e = pages[p] + ([0] if len(pages[p]) % 2 else [])       # (blocks are 4-byte aligned)
        reloc += struct.pack("<II", p, 8 + 2 * len(e)) + struct.pack(f"<{len(e)}H", *e)
    if not reloc:
        reloc = struct.pack("<II", 0, 8)                         # (an empty block keeps the section)

    FA, SA = 0x200, 0x1000                              # file and memory alignments
    up = lambda v, al: (v + al - 1) // al * al
    text = a[".text"]
    data = a.get(".data", (text[0] + up(text[2], SA), b"", 0))
    secs = [(b".text", text[0] - base, text[1], text[2], 0x60000020),        # code, execute, read
            (b".data", data[0] - base, data[1], data[2], 0xC0000040),        # data, read, write
            (b".reloc", up(data[0] - base + data[2], SA), reloc, len(reloc), 0x42000040)]  # discardable
    hdr_size = up(64 + 4 + 20 + 240 + 40 * len(secs), FA)
    raw, off = [], hdr_size
    for s in secs:
        raw.append(off)
        off += up(len(s[2]), FA)
    image_size = up(secs[-1][1] + secs[-1][3], SA)
    dos = b"MZ" + b"\0" * 58 + struct.pack("<I", 64)
    coff = struct.pack("<HHIIIHH", 0x8664, len(secs), 0, 0, 0, 240, 0x0022)   # x86-64; executable, large addresses
    opt = struct.pack("<HBBIIIII", 0x20B, 0, 0, up(text[2], FA), up(data[2] + len(reloc), FA), 0,
                      ea - base, text[0] - base)
    opt += struct.pack("<QIIHHHHHHIIIIHHQQQQII", base, SA, FA, 0, 0, 0, 0, 0, 0, 0, image_size, hdr_size, 0,
                       10, 0x0140, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)  # EFI application; relocatable, NX
    dirs = [(0, 0)] * 16
    dirs[5] = (secs[2][1], len(reloc))                  # the base relocation table
    opt += b"".join(struct.pack("<II", *x) for x in dirs)
    sh = b"".join(struct.pack("<8sIIIIIIHHI", n, vsize, rva, up(len(d), FA), raw[i] if d else 0, 0, 0, 0, 0, fl)
                  for i, (n, rva, d, vsize, fl) in enumerate(secs))
    out = (dos + b"PE\0\0" + coff + opt + sh).ljust(hdr_size, b"\0")
    for s in secs:
        out += s[2].ljust(up(len(s[2]), FA), b"\0")
    open(sys.argv[3], "wb").write(out)
    print(f"{sys.argv[3]}: {len(out)} bytes, {len(relocs)} relocations")

main()
