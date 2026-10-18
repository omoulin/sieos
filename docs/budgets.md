# Size budgets

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

SIEOS is meant to stay small, so its size is checked by the build, not by
good intentions. [tools/budget.txt](../tools/budget.txt) gives each part a
limit per architecture; every `make` (the `all` target, so every test too)
measures them with [tools/budget.py](../tools/budget.py) and fails when one
is over:

    budget: sh (x86_64, elf) is 31841 bytes (31.1 KiB), over its limit 30K (30720 bytes).
            Make it smaller, or raise its limit in tools/budget.txt in the same commit as the reason.

`make budget` (and `make ARCH=arm64 budget`) prints the whole table for
what was last built: each item's size, its limit and how much of it is used.
The check takes well under a second.

## What is measured

| Measure | Items | How |
|---|---|---|
| `elf` | the kernel, the boot modules (init, con, usb, vblk, fs), each program of the base system's `/bin` | text + data: the bytes of the sections that are loaded and stored in the file (`.bss` not counted) |
| `tree` | `rootfs`: the base system | the bytes of every file in `$(B)/rootfs`, what each new disk, key or card starts with |
| `img` | `usb-image` (x86-64), `sdcard-image` (arm64, Pi 4) | the real data of the base image (its allocated blocks; the file is sparse), checked by `make usb` and `make sdcard` |

A program in the base system's `/bin` with no line in the table is an
error too: a new program comes with its budget. The add-on packages
([packages.md](packages.md)) are not in the base system: their files are
checked (`file`), not counted in `rootfs` or the images.

## Changing a limit

The limits were set at the measured sizes + about 7%, so that growth is a
decision. When a change needs more room, raise the item's limit in
`tools/budget.txt` in the same commit, and say why in its message (what
grew, by how much, and why it is worth it). When something shrinks, lower
its limit, so the room is not quietly used again.

Builds that are not the default one skip the check: `NOBUDGET=1`, sicc's
build of x86-64 (`make cc-sieos`), `STACKCHECK=1` and `DIAG_TEST=n`.
