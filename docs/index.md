# SIEOS documentation

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

Start with the [README](../README.md) (what SIEOS is, how to run it), then:

| Page | What it covers |
|---|---|
| [status.md](status.md) | what works on each architecture, what is next, history |
| [kernel.md](kernel.md) | the guided tour: boot, messages, every server, crashes, on-demand servers, CPUs and time |
| [arch.md](arch.md) | the portable core and its architectures: the interface, x86-64, AArch64 (QEMU `virt`) |
| [raspberrypi.md](raspberrypi.md) | the Raspberry Pi 4 and 5: card images, serial console, what to check on real boards |
| [gpio.md](gpio.md) | the Raspberry Pi's pins (Pi 4; Pi 5 through RP1): the GPIO server, the C library (`gpio.h`), `/bin/gpio`, patterns, the self-check |
| [usb.md](usb.md) | a PC from a USB key: the UEFI loader, the key image, writing it safely, the key as SIEOS's disk |
| [install.md](install.md) | installing on a PC's NVMe drive: free space or a whole drive, the Install window, the boot menu entry, making free space on the Lenovo's Debian drive (LUKS + LVM), removing SIEOS |
| [usb-stack.md](usb-stack.md) | the USB stack: xHCI, hubs, USB disks (SCSI), keyboards and mice, hotplug, the Pi glue still to check |
| [power.md](power.md) | ACPI: power off, restart, suspend to RAM, battery, AC, lid, buttons (`acpid`), what to check on the laptop |
| [touchpad.md](touchpad.md) | laptop touchpads and touch screens: HID over I2C, the ACPI (AML) reader, the LPSS I2C controller, gestures, what to check on the laptop |
| [ice-lake.md](ice-lake.md) | the second laptop (an Ice Lake 2-in-1): sound in HDA mode, touch screen, power without S3, NVMe; what is left (DSP microphones, sensor hub, camera, s2idle) and what to check |
| [siefs.md](siefs.md) | SieFS, the file system: disk layout, commits, checksums, attributes, volumes, snapshots, encryption |
| [accounts.md](accounts.md) | users, logins, the first start, Argon2id, account files |
| [desktop.md](desktop.md) | the desktop: windows with title tabs, the Deskbar, workspaces, the sia strip, Files, Settings, Git, apps, the assistant |
| [audio.md](audio.md) | sound: Intel HD Audio playing and recording, the audio server, `play`, `rec`, `vol`, the test |
| [bluetooth.md](bluetooth.md) | Bluetooth: btd (HCI, L2CAP, SMP), the AX210 over USB and the Pis' UART, keyboards and mice (classic HID, HID over GATT), `bt`, Settings |
| [net.md](net.md) | the network: drivers, TCP/IP, DHCP, DNS, HTTP, TLS 1.3/1.2, `fetch` |
| [ssh.md](ssh.md) | SSH: `ssh`, `ssh-keygen`, `sshd`; the transport, Ed25519, how a remote shell gets its terminal, tests |
| [sia.md](sia.md) | the assistant: siad, backends (local, OpenAI-compatible), memory |
| [llm.md](llm.md) | the language-model engine: GGUF, number formats, AVX2 and NEON kernels, speeds |
| [ai-plan.md](ai-plan.md) | the AI plan: the 4 GB budget, CPU / NVIDIA / online backends |
| [gpu-plan.md](gpu-plan.md) | the GPU plan: sia on the laptop's RTX 5060 (GSP firmware, nvgpu, PTX kernels) and Intel iGPU (execlists, OpenCL kernels), the Pis' VideoCore (the HVS display; V3D if measured worth it) |
| [python.md](python.md) | `/bin/python`: MicroPython on SIEOS, its modules, the port, what is missing, its test |
| [python-plan.md](python-plan.md) | the Python plan: MicroPython first, then the POSIX layer and CPython |
| [webkit-plan.md](webkit-plan.md) | the browser plan: WPE WebKit, musl and a Linux personality (the shared POSIX layer), the kernel's lazy memory and futexes, phases, sizes |
| [cc.md](cc.md) | sicc, the C compiler: x86-64 and AArch64, optimizer, assembler, linker, tests |
| [make.md](make.md) | `/bin/make` on SIEOS: what it supports, the demo projects, `sh -c`, tests |
| [git.md](git.md) | `/bin/git`: commands, git:// and smart HTTP(S), the repository format, SHA-1 and zlib, the Git window, tests |
