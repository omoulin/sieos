#!/usr/bin/env python3
"""gptcheck.py - A disk image's partition table, checked from the host: a GPT
(both headers and their CRCs, the entries identical, the backup at the
disk's last sector) or an MBR. Used by the grow tests (tools/growtest.py,
tools/usbtest.py); by hand:

  gptcheck.py IMAGE            the table and its partitions; exit 1 if not valid

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import os, struct, sys, zlib

SIEFS_GPT = bytes.fromhex("00051e5eef5153419e05534945465331")     # (on-disk byte order)


def table(path, sector=512):
    """-> (kind, [(type, first, last)], problems): kind "gpt" or "mbr"; first/last
    in sectors, inclusive; type the GUID's 16 bytes, or the MBR's type byte."""
    size = os.path.getsize(path) // sector
    bad = []
    with open(path, "rb") as f:
        def rd(lba, n=1):
            f.seek(lba * sector); return f.read(n * sector)
        mbr = rd(0)
        if rd(1)[:8] != b"EFI PART":
            parts = []
            for i in range(4):
                t, lba, n = mbr[446 + 16 * i + 4], *struct.unpack_from("<II", mbr, 446 + 16 * i + 8)
                if t: parts.append((t, lba, lba + n - 1))
            if mbr[510:512] != b"\x55\xaa": bad.append("no MBR signature")
            if any(z >= size for _, _, z in parts): bad.append("a partition past the disk's end")
            return "mbr", parts, bad
        hdrs = []
        for lba in (1, size - 1):
            h = rd(lba)[:92]
            if h[:8] != b"EFI PART":
                bad.append(f"no GPT header at sector {lba}"); continue
            crc = struct.unpack_from("<I", h, 16)[0]
            if zlib.crc32(h[:16] + b"\0\0\0\0" + h[20:92]) != crc: bad.append(f"header {lba}: bad CRC")
            me, alt, fu, lu = struct.unpack_from("<QQQQ", h, 24)
            el, ne, es, ec = struct.unpack_from("<QIII", h, 72)
            ents = rd(el, ne * es // sector)
            if zlib.crc32(ents) != ec: bad.append(f"header {lba}: entries' CRC")
            if me != lba or alt != (size - 1 if lba == 1 else 1): bad.append(f"header {lba}: self {me}, alternate {alt}")
            if lu >= size - 1 - ne * es // sector or (lba != 1 and el != size - 1 - ne * es // sector):
                bad.append(f"header {lba}: last usable {lu}, entries at {el}")
            hdrs.append((h, ents))
        if len(hdrs) == 2 and (hdrs[0][1] != hdrs[1][1] or hdrs[0][0][56:72] != hdrs[1][0][56:72]):
            bad.append("the two copies differ")
        parts = []
        if hdrs:
            h, ents = hdrs[0]
            ne, es = struct.unpack_from("<II", h, 80)
            for i in range(ne):
                e = ents[i * es:(i + 1) * es]
                if e[:16] != bytes(16): parts.append((e[:16], *struct.unpack_from("<QQ", e, 32)))
        return "gpt", parts, bad


def siefs_part(path):
    """The SieFS partition: (first, last) sectors, or None."""
    kind, parts, _ = table(path)
    for t, a, z in parts:
        if t in (SIEFS_GPT, 0x5E): return a, z
    return None


def extract(path, out, first, last, sector=512):
    """Copy sectors first..last of path into out, sparse."""
    with open(path, "rb") as f, open(out, "wb") as o:
        f.seek(first * sector)
        left = (last + 1 - first) * sector
        while left:
            b = f.read(min(1 << 20, left))
            if b.strip(b"\0"): o.write(b)
            else: o.seek(len(b), 1)
            left -= len(b)
        o.truncate((last + 1 - first) * sector)


if __name__ == "__main__":
    kind, parts, bad = table(sys.argv[1])
    for t, a, z in parts:
        print(f"{kind}: {t.hex() if isinstance(t, bytes) else hex(t)}: sectors {a}..{z} ({(z + 1 - a) >> 11} MiB)")
    print("\n".join(bad) or f"{kind}: valid")
    sys.exit(1 if bad else 0)
