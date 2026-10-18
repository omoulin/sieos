#!/usr/bin/env python3
"""Size budgets (make budget; docs/budgets.md): every item of tools/budget.txt
measured in the build directory and checked against its limit for this
architecture. An item whose file is not built is shown as "-" and skipped;
a program in the base system's /bin with no line in the table is an error
(a new program needs a budget). Usage:
  budget.py B ARCH [--quiet] [--only ITEM...]
--quiet: print only what fails (and one line otherwise), as the build does.

Measures:
  elf   text+data of B/ITEM.elf (kernel: B/kernel.elf): the bytes of its
        sections that are loaded and stored in the file (not .bss)
  tree  the bytes of every file under B/rootfs (the base system's files)
  img   the real data of an image (its allocated blocks; the file is sparse)
  file  the size of a file in B (an add-on package)

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import os, struct, sys

UNITS = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30}


def num(s):
    return int(float(s[:-1]) * UNITS[s[-1]]) if s[-1] in UNITS else int(s)


def human(n):
    return f"{n / (1 << 20):.2f} MiB" if n >= 1 << 20 else f"{n / 1024:.1f} KiB"


def elf_textdata(path):
    """Sum of the SHF_ALLOC sections that are not SHT_NOBITS (what `size` calls text + data)."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"\x7fELF" or d[4] != 2:
        raise ValueError(f"{path}: not a 64-bit ELF file")
    shoff, = struct.unpack_from("<Q", d, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", d, 0x3A)
    total = 0
    for i in range(shnum):
        _, stype, flags, _, _, size = struct.unpack_from("<IIQQQQ", d, shoff + i * shentsize)
        if flags & 2 and stype != 8:          # SHF_ALLOC, not SHT_NOBITS
            total += size
    return total


def tree_bytes(path):
    return sum(os.lstat(os.path.join(d, f)).st_size for d, _, fs in os.walk(path) for f in fs)


def main():
    a = sys.argv[1:]
    quiet = "--quiet" in a
    only = a[a.index("--only") + 1:] if "--only" in a else None
    b, arch = a[0], a[1]
    table = os.path.join(os.path.dirname(os.path.abspath(__file__)), "budget.txt")
    col = {"x86_64": 2, "arm64": 3}[arch]
    rows, named = [], set()
    for ln in open(table):
        w = ln.split("#")[0].split()
        if not w:
            continue
        item, measure, lim = w[0], w[1], w[col]
        named.add(item)
        if lim == "-" or (only and item not in only):
            continue
        if measure == "elf":
            path = os.path.join(b, item + ".elf")
            size = elf_textdata(path) if os.path.isfile(path) else None
        elif measure == "tree":
            path = os.path.join(b, w[4] if len(w) > 4 else "rootfs")
            size = tree_bytes(path) if os.path.isdir(path) else None
        elif measure in ("img", "file"):
            path = os.path.join(b, w[4])
            size = None if not os.path.isfile(path) else os.stat(path).st_blocks * 512 if measure == "img" else os.path.getsize(path)
        else:
            sys.exit(f"budget: {table}: unknown measure '{measure}' for {item}")
        rows.append((item, measure, size, num(lim), lim))

    # every program in the base system's /bin has a budget
    missing = []
    bindir = os.path.join(b, "rootfs", "bin")
    if not only and os.path.isdir(bindir):
        missing = sorted(p for p in os.listdir(bindir)
                         if not os.path.islink(os.path.join(bindir, p)) and p not in named)

    over = [r for r in rows if r[2] is not None and r[2] > r[3]]
    if not quiet:
        print(f"{'item':<14} {'measure':<7} {'size':>12} {'limit':>12}  use")
        for item, measure, size, limit, _ in rows:
            s = "-" if size is None else human(size)
            pct = "" if size is None else f"{100 * size // limit:3d}%"
            print(f"{item:<14} {measure:<7} {s:>12} {human(limit):>12}  {pct}{'  OVER' if size and size > limit else ''}")
    for item, measure, size, limit, lim in over:
        print(f"budget: {item} ({arch}, {measure}) is {size} bytes ({human(size)}), over its limit {lim} ({limit} bytes).\n"
              f"        Make it smaller, or raise its limit in tools/budget.txt in the same commit as the reason.",
              file=sys.stderr)
    for p in missing:
        print(f"budget: /bin/{p} has no budget: add a line for it to tools/budget.txt", file=sys.stderr)
    if over or missing:
        sys.exit(1)
    if quiet and not only:
        print(f"budget: {sum(r[2] is not None for r in rows)} items within their limits ({arch})")


if __name__ == "__main__":
    main()
