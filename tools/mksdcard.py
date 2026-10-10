#!/usr/bin/env python3
"""Build an SD card image for a Raspberry Pi 4 or 5 (make ARCH=arm64 PI=4|5 sdcard).

The card holds two partitions (an MBR partition table):
  1. FAT32 (type 0x0C), the boot partition the Pi's firmware reads:
     config.txt (written here, per board), kernel8.img (SIEOS's kernel),
     sieos.mod (the boot modules, as the "initramfs"), and the firmware's own
     files, copied from FIRMWARE if given: they are not part of SIEOS and are
     not redistributed here (docs/raspberrypi.md says where to get them).
  2. SieFS (type 0x5E): SIEOS's disk, a copy of a SieFS image.

The FAT32 writer is our own: 512-byte sectors and clusters, long file names,
one level of folders (the firmware's overlays/). Usage:
  mksdcard.py OUT --pi 4|5 --kernel K --initrd M --siefs S [--firmware DIR] [--fat MiB] [--pow2]

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import argparse, os, struct, sys

SECTOR = 512

CONFIG = {
    "4": """# SIEOS on a Raspberry Pi 4 (written by tools/mksdcard.py)
arm_64bit=1
kernel=kernel8.img
# the boot modules at their own address, 32 MiB: not "followkernel" (right
# after the file: the kernel's .bss, cleared at start, is not in the file)
initramfs sieos.mod 0x02000000
# the PL011 serial port (UART0) on GPIO 14 (TX) and 15 (RX), 115200 bit/s:
# SIEOS's console. (Bluetooth would otherwise use it.)
enable_uart=1
dtoverlay=disable-bt
# the frame buffer SIEOS asks the firmware for: 1920 x 1080
disable_overscan=1
hdmi_force_hotplug=1
""",
    "5": """# SIEOS on a Raspberry Pi 5 (written by tools/mksdcard.py)
arm_64bit=1
kernel=kernel8.img
# the boot modules at their own address, 32 MiB: not "followkernel" (right
# after the file: the kernel's .bss, cleared at start, is not in the file)
initramfs sieos.mod 0x02000000
# the debug serial port (the 3-pin "UART" connector), 115200 bit/s: SIEOS's console
enable_uart=1
# the frame buffer the firmware sets up and describes for SIEOS
framebuffer_width=1920
framebuffer_height=1080
hdmi_force_hotplug=1
""",
}

# Firmware files the boards need on the boot partition (copied from --firmware).
FIRMWARE = {
    "4": ["start4.elf", "fixup4.dat", "bcm2711-rpi-4-b.dtb", "overlays/disable-bt.dtbo"],
    "5": ["bcm2712-rpi-5-b.dtb"],
}


class Fat32:
    """A FAT32 file system in memory (one sector per cluster), written at the end."""

    def __init__(self, sectors, label):
        self.reserved, self.nfats = 32, 2
        clusters = sectors - self.reserved
        self.fat_sectors = (clusters * 4 + SECTOR - 1) // SECTOR + 1
        self.clusters = sectors - self.reserved - self.nfats * self.fat_sectors
        if self.clusters < 65525:
            sys.exit("mksdcard: the FAT partition is too small for FAT32 (use --fat 64 or more)")
        self.sectors, self.label = sectors, label
        self.fat = [0x0FFFFFF8, 0x0FFFFFFF]       # entries 0 and 1 are reserved
        self.data = {}                            # cluster -> 512 bytes
        self.root = self.newdir()                 # cluster 2

    def alloc(self, n):
        first = len(self.fat)
        if first + n > self.clusters + 2:
            sys.exit("mksdcard: the FAT partition is full")
        for i in range(n):
            self.fat.append(first + i + 1 if i < n - 1 else 0x0FFFFFFF)
        return first

    def newdir(self):
        c = self.alloc(1)
        self.data[c] = bytearray(SECTOR)
        self.dirs = getattr(self, "dirs", {})
        self.dirs[c] = [c]                        # the directory's clusters
        return c

    def short_name(self, name, taken):
        base, _, ext = name.upper().rpartition(".") if "." in name else (name.upper(), "", "")
        clean = lambda s: "".join(ch for ch in s if ch.isalnum() or ch in "_-~")
        base, ext = clean(base), clean(ext)[:3]
        if len(base) <= 8 and (base + "." + ext if ext else base) == name.upper() and (base, ext) not in taken:
            return base.ljust(8).encode() + ext.ljust(3).encode(), False
        for i in range(1, 100):
            s = (base[: 8 - len(str(i)) - 1] + "~" + str(i)).ljust(8)
            if (s.strip(), ext) not in taken:
                return s.encode() + ext.ljust(3).encode(), True
        sys.exit("mksdcard: too many similar names")

    def add_entries(self, dircl, entries):
        """Append 32-byte entries to directory dircl (growing it as needed)."""
        for e in entries:
            for c in self.dirs[dircl]:
                buf = self.data[c]
                for off in range(0, SECTOR, 32):
                    if buf[off] == 0:
                        buf[off : off + 32] = e
                        break
                else:
                    continue
                break
            else:                                 # every cluster full: one more
                last = self.dirs[dircl][-1]
                c = self.alloc(1)
                self.fat[last] = c
                self.data[c] = bytearray(SECTOR)
                self.dirs[dircl].append(c)
                self.data[c][0:32] = e

    def entry(self, dircl, name, attr, cluster, size):
        taken = set()
        for c in self.dirs[dircl]:
            for off in range(0, SECTOR, 32):
                e = self.data[c][off : off + 32]
                if e[0] not in (0, 0xE5) and e[11] != 0x0F:
                    taken.add((e[0:8].decode().strip(), e[8:11].decode().strip()))
        sn, need_lfn = self.short_name(name, taken)
        out = []
        if need_lfn:                              # long-name entries, last part first
            csum = 0
            for b in sn:
                csum = (((csum & 1) << 7) + (csum >> 1) + b) & 0xFF
            u = name.encode("utf-16-le") + b"\0\0"
            parts = [u[i : i + 26] for i in range(0, len(u), 26)]
            for i in reversed(range(len(parts))):
                p = parts[i].ljust(26, b"\xff")
                seq = (i + 1) | (0x40 if i == len(parts) - 1 else 0)
                out.append(bytes([seq]) + p[0:10] + bytes([0x0F, 0, csum]) + p[10:22] + b"\0\0" + p[22:26])
        out.append(sn + bytes([attr]) + bytes(8) + struct.pack("<H", cluster >> 16) + bytes(4)
                   + struct.pack("<HI", cluster & 0xFFFF, size))
        self.add_entries(dircl, out)

    def add_file(self, path, content):
        dircl = self.root
        parts = path.split("/")
        for d in parts[:-1]:                      # (one level: overlays/)
            dircl = self.subdir(dircl, d)
        n = max(1, (len(content) + SECTOR - 1) // SECTOR)
        c = self.alloc(n) if content else 0
        for i in range(n if content else 0):
            self.data[c + i] = content[i * SECTOR : (i + 1) * SECTOR].ljust(SECTOR, b"\0")
        self.entry(dircl, parts[-1], 0x20, c, len(content))

    def subdir(self, parent, name):
        self.subdirs = getattr(self, "subdirs", {})
        if (parent, name) in self.subdirs:
            return self.subdirs[(parent, name)]
        c = self.newdir()
        dot = lambda n, cl: n.ljust(11).encode() + bytes([0x10]) + bytes(8) + struct.pack("<H", cl >> 16) + bytes(4) + struct.pack("<HI", cl & 0xFFFF, 0)
        self.data[c][0:32] = dot(".", c)
        self.data[c][32:64] = dot("..", 0 if parent == self.root else parent)
        self.entry(parent, name, 0x10, c, 0)
        self.subdirs[(parent, name)] = c
        return c

    def write(self, f, offset):
        free = self.clusters + 2 - len(self.fat)
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x58\x90"
        bs[3:11] = b"SIEOS   "
        struct.pack_into("<HBHBHHBHHHII", bs, 11, SECTOR, 1, self.reserved, self.nfats, 0, 0, 0xF8, 0, 63, 255,
                         offset // SECTOR, self.sectors)
        struct.pack_into("<IHHIHH", bs, 36, self.fat_sectors, 0, 0, self.root, 1, 6)
        bs[64], bs[66] = 0x80, 0x29
        struct.pack_into("<I", bs, 67, 0x5E1E0500)
        bs[71:82] = self.label.ljust(11).encode()
        bs[82:90] = b"FAT32   "
        bs[510:512] = b"\x55\xAA"
        fsinfo = bytearray(SECTOR)
        struct.pack_into("<I", fsinfo, 0, 0x41615252)
        struct.pack_into("<III", fsinfo, 484, 0x61417272, free, len(self.fat))
        fsinfo[510:512] = b"\x55\xAA"
        for s, b in ((0, bs), (1, fsinfo), (6, bs), (7, fsinfo)):
            f.seek(offset + s * SECTOR); f.write(b)
        fat = bytearray(self.fat_sectors * SECTOR)
        for i, v in enumerate(self.fat):
            struct.pack_into("<I", fat, i * 4, v)
        for k in range(self.nfats):
            f.seek(offset + (self.reserved + k * self.fat_sectors) * SECTOR); f.write(fat)
        data0 = offset + (self.reserved + self.nfats * self.fat_sectors) * SECTOR
        for c, b in self.data.items():
            f.seek(data0 + (c - 2) * SECTOR); f.write(b)
        # the volume label, as the root's first entry would be the norm: kept in the boot sector only


def main():
    a = argparse.ArgumentParser()
    a.add_argument("out"); a.add_argument("--pi", choices=["4", "5"], required=True)
    a.add_argument("--kernel", required=True); a.add_argument("--initrd", required=True)
    a.add_argument("--siefs", required=True); a.add_argument("--firmware")
    a.add_argument("--fat", type=int, default=256, help="the boot partition's size, MiB")
    a.add_argument("--pow2", action="store_true", help="round the card to a power of two (QEMU wants it)")
    o = a.parse_args()

    fat_start = 2048                                     # 1 MiB: the usual alignment
    fat_sectors = o.fat * 2048
    sie_start = fat_start + fat_sectors
    sie_bytes = os.path.getsize(o.siefs)
    total = (sie_start * SECTOR + sie_bytes + (1 << 20) - 1) & ~((1 << 20) - 1)
    if o.pow2:
        p = 1
        while p < total: p <<= 1
        total = p

    fs = Fat32(fat_sectors, "SIEOS-BOOT")
    fs.add_file("config.txt", CONFIG[o.pi].encode())
    fs.add_file("kernel8.img", open(o.kernel, "rb").read())
    fs.add_file("sieos.mod", open(o.initrd, "rb").read())
    missing = []
    for name in FIRMWARE[o.pi]:
        src = os.path.join(o.firmware, name) if o.firmware else None
        if src and os.path.isfile(src):
            fs.add_file(name, open(src, "rb").read())
        else:
            missing.append(name)
    fs.add_file("README.txt", ("SIEOS boot partition for a Raspberry Pi %s.\r\n"
                               "The Raspberry Pi firmware files must be added here: see docs/raspberrypi.md.\r\n"
                               % o.pi).encode())

    with open(o.out, "wb") as f:
        f.truncate(total)
        mbr = bytearray(SECTOR)
        def part(i, typ, start, count):
            struct.pack_into("<B3sB3sII", mbr, 446 + 16 * i, 0, b"\xFE\xFF\xFF", typ, b"\xFE\xFF\xFF", start, count)
        part(0, 0x0C, fat_start, fat_sectors)
        part(1, 0x5E, sie_start, sie_bytes // SECTOR)
        mbr[510:512] = b"\x55\xAA"
        f.seek(0); f.write(mbr)
        fs.write(f, fat_start * SECTOR)
        with open(o.siefs, "rb") as s:                   # copied; zero chunks left as holes
            pos = 0
            while True:
                chunk = s.read(1 << 20)
                if not chunk: break
                if chunk.count(0) != len(chunk):
                    f.seek(sie_start * SECTOR + pos); f.write(chunk)
                pos += len(chunk)
    print("mksdcard: %s: %d MiB, boot partition %d MiB (FAT32), SieFS %d MiB" %
          (o.out, total >> 20, o.fat, sie_bytes >> 20))
    if missing:
        print("mksdcard: firmware files still to copy onto the boot partition: " + ", ".join(missing))
        print("          (FIRMWARE=dir, or by hand: docs/raspberrypi.md)")


if __name__ == "__main__":
    main()
