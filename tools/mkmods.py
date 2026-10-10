#!/usr/bin/env python3
"""mkmods.py - Pack SIEOS's boot modules into one archive, for machines whose
boot loader passes a single "initrd" (arm64: QEMU's -initrd, a Raspberry Pi's
"initramfs" line). The kernel (kernel/arch/arm64/platform.c) reads it in place.

Format (little-endian), our own:
  0   "SIEOSMOD"                      magic
  8   u32 count, u32 0
  16  count entries of 64 bytes: name[48] (0-terminated: what the kernel shows
      as the module's command line), u64 offset, u64 size
  then each module, at a 4096-byte boundary (page-aligned, like multiboot's)

Usage: mkmods.py OUT FILE... (the first file is init, the supervisor)

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
"""
import os, struct, sys

out, files = sys.argv[1], sys.argv[2:]
off = (16 + 64 * len(files) + 4095) & ~4095
head, body = bytearray(b"SIEOSMOD" + struct.pack("<II", len(files), 0)), bytearray()
for f in files:
    data = open(f, "rb").read()
    name = os.path.basename(f).encode()
    if len(name) > 47: sys.exit(f"mkmods: name too long: {f}")
    head += name.ljust(48, b"\0") + struct.pack("<QQ", off + len(body), len(data))
    body += data + b"\0" * (-len(data) % 4096)
open(out, "wb").write(bytes(head.ljust(off, b"\0")) + bytes(body))
