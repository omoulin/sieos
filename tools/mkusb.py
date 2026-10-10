#!/usr/bin/env python3
"""Build a USB key image for PCs with UEFI firmware (make usb).

The key holds a GPT partition table and two partitions:
  1. the EFI System Partition (FAT32), which the firmware reads:
       EFI/BOOT/BOOTX64.EFI   SIEOS's own UEFI loader (boot/uefi)
       SIEOS/KERNEL.BIN       the kernel
       SIEOS/*.ELF            the boot modules (init, con, vblk, fs)
       SIEOS/BOOT.CFG         "module=..." lines (init first), "screen=WxH"
  2. SIEOS's disk: a SieFS file system, read and written by SIEOS like any
     disk. It is found by its partition type (SIEFS_TYPE below); its name
     is "SIEOS".

The image is sparse: only what is written takes space. Usage:
  mkusb.py OUT --size BYTES --efi BOOTX64.EFI --kernel K --module M... \
           --mkfs MKFS --siefs-tool SIEFS --rootfs DIR [--put SRC DST]... [--esp MiB] [--screen WxH]

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
"""
import argparse, os, struct, subprocess, sys, tempfile, uuid, zlib

sys.path.insert(0, os.path.dirname(__file__))
from mksdcard import Fat32                       # our FAT32 writer (shared with the Pi's cards)

SECTOR = 512
ESP_TYPE = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")    # the UEFI standard's
SIEFS_TYPE = uuid.UUID("5E1E0500-51EF-4153-9E05-534945465331")  # ours: "a SieFS file system"
ENTRIES, ENTRY_SIZE = 128, 128


def gpt(total, parts, disk_guid):
    """The primary and backup GPT (header + entries), for a disk of `total` sectors."""
    ent = bytearray(ENTRIES * ENTRY_SIZE)
    for i, (ptype, first, last, name) in enumerate(parts):
        struct.pack_into("<16s16sQQQ72s", ent, i * ENTRY_SIZE, ptype.bytes_le, uuid.uuid4().bytes_le,
                         first, last, 0, name.encode("utf-16-le"))
    ent_sectors = len(ent) // SECTOR
    def header(me, other, entries_lba):
        h = bytearray(SECTOR)
        struct.pack_into("<8sIIIIQQQQ16sQIII", h, 0, b"EFI PART", 0x10000, 92, 0, 0, me, other,
                         2 + ent_sectors, total - 2 - ent_sectors, disk_guid.bytes_le, entries_lba,
                         ENTRIES, ENTRY_SIZE, zlib.crc32(ent))
        struct.pack_into("<I", h, 16, zlib.crc32(h[:92]))
        return h
    return (header(1, total - 1, 2), ent, header(total - 1, 1, total - 1 - ent_sectors), total - 1 - ent_sectors)


def copy_sparse(src, dst, offset):
    """Copy file src into dst at offset, only its data (holes stay holes)."""
    with open(src, "rb") as f:
        fd, size, pos = f.fileno(), os.fstat(f.fileno()).st_size, 0
        while pos < size:
            try:
                pos = os.lseek(fd, pos, os.SEEK_DATA)
                end = os.lseek(fd, pos, os.SEEK_HOLE)
            except OSError:                       # (no more data)
                break
            f.seek(pos)
            while pos < end:
                chunk = f.read(min(1 << 20, end - pos))
                if chunk.strip(b"\0"):
                    dst.seek(offset + pos); dst.write(chunk)
                pos += len(chunk)


def main():
    a = argparse.ArgumentParser()
    a.add_argument("out"); a.add_argument("--size", required=True, help="bytes, or with K, M, G")
    a.add_argument("--efi", required=True); a.add_argument("--kernel", required=True)
    a.add_argument("--module", action="append", required=True)
    a.add_argument("--mkfs", required=True); a.add_argument("--siefs-tool", required=True)
    a.add_argument("--rootfs", required=True); a.add_argument("--put", nargs=2, action="append", default=[])
    a.add_argument("--esp", type=int, default=64, help="the EFI partition's size, MiB")
    a.add_argument("--screen", default="1920x1080")
    o = a.parse_args()
    o.size = int(o.size[:-1]) << {"K": 10, "M": 20, "G": 30}[o.size[-1].upper()] if o.size[-1].isalpha() else int(o.size)

    total = o.size // SECTOR
    esp_first = 2048                                   # 1 MiB: the usual alignment
    esp_last = esp_first + o.esp * 2048 - 1
    fs_first = esp_last + 1                            # (1 MiB-aligned too)
    fs_last = (total - 34) // 8 * 8 - 1                # before the backup GPT, whole 4 KiB blocks
    if fs_last - fs_first < 64 * 2048:
        sys.exit("mkusb: the key is too small (USBDISK)")

    # The SieFS partition's file system, made at its exact size, then the files.
    with tempfile.TemporaryDirectory() as tmp:
        fsimg = os.path.join(tmp, "siefs.img")
        subprocess.run([o.mkfs, "-s", str((fs_last - fs_first + 1) * SECTOR), "-L", "SIEOS", "-d", o.rootfs, fsimg],
                       check=True, stdout=subprocess.DEVNULL)
        for src, dst in o.put:
            subprocess.run([o.siefs_tool, fsimg, "mkdir", os.path.dirname(dst)], stderr=subprocess.DEVNULL)
            subprocess.run([o.siefs_tool, fsimg, "put", src, dst], check=True)

        # The EFI partition.
        fat = Fat32(esp_last - esp_first + 1, "SIEOS-BOOT")
        fat.add_file("EFI/BOOT/BOOTX64.EFI", open(o.efi, "rb").read())
        fat.add_file("SIEOS/KERNEL.BIN", open(o.kernel, "rb").read())
        cfg = "# SIEOS's UEFI loader (boot/uefi): the boot modules, init first; the screen size\n"
        for m in o.module:
            name = os.path.basename(m)
            fat.add_file("SIEOS/" + name.upper(), open(m, "rb").read())
            cfg += f"module={name}\n"
        cfg += f"screen={o.screen}\n"
        fat.add_file("SIEOS/BOOT.CFG", cfg.encode())

        with open(o.out, "wb") as f:
            f.truncate(total * SECTOR)
            mbr = bytearray(SECTOR)                    # the "protective" MBR: one partition over all
            struct.pack_into("<B3sB3sII", mbr, 446, 0, b"\0\x02\0", 0xEE, b"\xff\xff\xff", 1, min(total - 1, 0xFFFFFFFF))
            mbr[510:512] = b"\x55\xAA"
            f.seek(0); f.write(mbr)
            h1, ent, h2, ent2_lba = gpt(total, [(ESP_TYPE, esp_first, esp_last, "EFI system"),
                                                (SIEFS_TYPE, fs_first, fs_last, "SIEOS")], uuid.uuid4())
            f.seek(SECTOR); f.write(h1)
            f.seek(2 * SECTOR); f.write(ent)
            f.seek(ent2_lba * SECTOR); f.write(ent)
            f.seek((total - 1) * SECTOR); f.write(h2)
            fat.write(f, esp_first * SECTOR)
            copy_sparse(fsimg, f, fs_first * SECTOR)
    print(f"{o.out}: {o.size >> 20} MiB key: EFI partition {o.esp} MiB, SieFS partition "
          f"{(fs_last - fs_first + 1) * SECTOR >> 20} MiB at {fs_first * SECTOR >> 20} MiB")


if __name__ == "__main__":
    main()
