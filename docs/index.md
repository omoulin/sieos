# SIEOS documentation

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

Start with the [README](../README.md) (what SIEOS is, how to run it), then:

| Page | What it covers |
|---|---|
| [status.md](status.md) | what works on each architecture, what is next, history |
| [kernel.md](kernel.md) | the guided tour: boot, messages, every server, crashes, on-demand servers, CPUs and time |
| [arch.md](arch.md) | the portable core and its architectures: the interface, x86-64, AArch64 (QEMU `virt`) |
| [raspberrypi.md](raspberrypi.md) | the Raspberry Pi 4 and 5: card images, serial console, what to check on real boards |
| [usb.md](usb.md) | a PC from a USB key: the UEFI loader, the key image, writing it safely, the key as SIEOS's disk |
| [usb-stack.md](usb-stack.md) | the USB stack: xHCI, hubs, USB disks (SCSI), keyboards and mice, hotplug, the Pi glue still to check |
| [siefs.md](siefs.md) | SieFS, the file system: disk layout, commits, checksums, attributes, encryption plan |
| [accounts.md](accounts.md) | users, logins, the first start, Argon2id, account files |
| [desktop.md](desktop.md) | the desktop, Stage: projects, the stage, the shelf, the Lens, apps, the assistant |
| [net.md](net.md) | the network: drivers, TCP/IP, DHCP, DNS, HTTP, TLS 1.3, `fetch` |
| [sia.md](sia.md) | the assistant: siad, backends (local, OpenAI-compatible), memory |
| [llm.md](llm.md) | the language-model engine: GGUF, number formats, AVX2 and NEON kernels, speeds |
| [ai-plan.md](ai-plan.md) | the AI plan: the 4 GB budget, CPU / NVIDIA / online backends |
| [cc.md](cc.md) | sicc, the C compiler: x86-64 and AArch64, optimizer, assembler, linker, tests |
