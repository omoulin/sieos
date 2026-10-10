# Status

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only

What works today, on each architecture, and how SIEOS got here. Last
checked 2026-10-10: every test suite below passes, zero build warnings.

## What works

| Area | x86-64 (QEMU, KVM) | AArch64: QEMU `virt` | Raspberry Pi 4 | Raspberry Pi 5 |
|---|---|---|---|---|
| Kernel: SMP, tickless, threads, quotas, FP/vector state | yes | yes (emulated) | QEMU `raspi4b`: yes | card image only, untested |
| SieFS disk | virtio | virtio (MMIO) | SD card | SD card (untested) |
| Users, login, permissions | yes | yes | yes (serial) | untested |
| Supervisor, on-demand servers | yes | yes | yes | untested |
| Desktop (Stage) | yes | yes (ramfb) | screen yes, no USB input yet | untested |
| Network, TLS | yes | yes | no driver yet | no driver yet |
| Assistant, local model | AVX2/VNNI | NEON (+ `sdot` if present) | NEON | NEON + `sdot` |
| Assistant, remote (OpenAI-compatible) | yes | yes | needs the network | needs the network |
| Built by sicc | yes (`cc-sieos`) | always | always | always |

Test suites: `test`, `crash-test`, `gui-test`, `gui-test-real`,
`kernel-test`, `sia-test`, `demand-test`, `net-test` (both
architectures), `pi4-test`, `cc-test`, `cc-bootstrap`, `cc-sieos`,
`cc-a64-test`, `cc-a64-bootstrap`, `siefs-test`, `crypto-test`,
`net-crypto-test`, `llm-test`, `llm-neon-test`. Real boards: see the
checklist in [raspberrypi.md](raspberrypi.md).

## Next

- The assistant's memory: history across sessions and restarts, with
  compaction ([sia.md](sia.md)).
- The target laptop: UEFI boot from USB into RAM, NVMe, USB input, the
  Realtek network card, then NVIDIA compute ([ai-plan.md](ai-plan.md)).
- The Pis: USB input, Ethernet, faster SD; running on the real boards.
- sicc on SIEOS itself (a C library), encryption, snapshots.

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
