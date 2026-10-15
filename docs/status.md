# Status

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

What works today, on each architecture, and how SIEOS got here. Last
checked 2026-10-10: every test suite below passes, zero build warnings.

## What works

| Area | x86-64 (QEMU, KVM) | AArch64: QEMU `virt` | Raspberry Pi 4 | Raspberry Pi 5 |
|---|---|---|---|---|
| Kernel: SMP, tickless, threads, quotas, FP/vector state | yes | yes (emulated) | QEMU `raspi4b`: yes | card image only, untested |
| SieFS disk | virtio | virtio (MMIO) | SD card | SD card (untested) |
| Users, login, permissions | yes | yes | yes (serial) | untested |
| Supervisor, on-demand servers | yes | yes | yes | untested |
| Desktop | yes | yes (ramfb) | yes; USB keyboard and mouse (PCIe start-up untested) | untested |
| Network, TLS | yes | yes | no driver yet | no driver yet |
| Assistant, local model | AVX2/VNNI | NEON (+ `sdot` if present) | NEON | NEON + `sdot` |
| Assistant, remote (OpenAI-compatible) | yes | yes | needs the network | needs the network |
| Built by sicc | yes (`cc-sieos`) | always | always | always |

Test suites: `test`, `crash-test`, `gui-test`, `gui-test-real`,
`kernel-test`, `sia-test`, `demand-test`, `net-test`, `make-test` (both
architectures), `pi4-test`, `cc-test`, `cc-bootstrap`, `cc-sieos`,
`cc-a64-test`, `cc-a64-bootstrap`, `siefs-test`, `crypto-test`,
`net-crypto-test`, `llm-test`, `llm-neon-test`. Real boards: see the
checklist in [raspberrypi.md](raspberrypi.md).

## Next

- The assistant's memory: history across sessions and restarts, with
  compaction ([sia.md](sia.md)).
- The target laptop: UEFI boot from USB into RAM, NVMe, USB input, the
  Realtek network card, then NVIDIA compute ([ai-plan.md](ai-plan.md)).
- The Pis: USB input; running on the real boards (Ethernet and Wi-Fi are
  written but untested there, [raspberrypi.md](raspberrypi.md)).

## History

- **2026-10-09.** Microkernel, console and shell; SieFS (specification,
  core, host tools, crash tests); virtio disk and file server; users and
  Argon2id logins; the supervisor (restarts in ~1 ms); SMP and tickless
  time; the Atlas desktop; sicc, our C compiler, building all of SIEOS.
- **Night of 2026-10-09.** sicc: all of C11, optimizer, vectors; the
  language-model engine; floating point, TLS and quotas in the kernel;
  the sia assistant (local and remote); the network stack and TLS 1.3;
  the assistant panel and desktop improvements.
- **2026-10-10.** Atlas: island menus, apps, Driftwood, items on several
  islands; servers started on demand and stopped when idle; the graphical
  first start; the kernel split into a portable core and architectures;
  sicc for AArch64 (with NEON); SIEOS on AArch64 (QEMU `virt`), the
  Raspberry Pi 4 (QEMU) and card images for the Pi 4 and 5; NEON kernels
  for the engine; processor features reported by the kernel.
- **2026-10-10, later.** The map desktop (Atlas) replaced by **Stage**
  ([desktop.md](desktop.md)): projects (one per file, the Inbox),
  at most two windows, a shelf of cards, the Lens; island attributes
  migrated on first sight.
- **2026-10-12.** Stage replaced by a desktop laid out as BeOS's was, in
  SIEOS's colours ([desktop.md](desktop.md)): windows with title tabs
  (move, resize, zoom, minimize), the Deskbar (menu, apps, clock), 4
  workspaces, the sia strip, a Files window; Settings (screen size, mouse,
  sia's model, local or online).
- **2026-10-12, later.** sicc keeps NEON vectors in registers (the engine's
  kernels 1.9× faster under emulation, up to 5×; a dot-product loop from
  214 instructions to 15) and runs on SIEOS itself: `/bin/cc`, with SIEOS's
  headers and library in `/lib/sicc` ([cc.md](cc.md)); a program starts in
  its parent's current folder.
- **2026-10-15.** `/bin/make`: POSIX make with GNU make's common additions
  (pattern and suffix rules, `:=` `?=` `+=`, functions, `include`, `-n -k
  -s -B -C -f`), recipes run by `sh -c`; `/src/hello-make` and SIEOS's own
  hello, sh and make built on SIEOS by make and `/bin/cc` ([make.md](make.md));
  the shell gets `-c`, `cp`, `touch`, `rm -f`; file times never go back.
- **2026-10-15.** SieFS: several volumes (a home can be one) and
  snapshots, shared block by block with their volume, deleted ZFS-style
  through dead lists; the file server takes one of every volume each hour
  (kept a day) and lets the oldest go when the disk is full; `snap`, and
  the Snapshots button in Files ([siefs.md](siefs.md#snapshots)).
- **2026-10-15, later.** Encrypted homes: each user's home is a volume
  sealed with ChaCha20-Poly1305, its key wrapped by the user's password
  (Argon2id); it opens at login and gets a new wrapping when the password
  changes ([siefs.md](siefs.md#encryption), [accounts.md](accounts.md)).
