# SIEOS

A small, fast microkernel operating system written from scratch in C, for
**x86-64** and **AArch64** (QEMU, Raspberry Pi 4 and 5). It has its own file
system (SieFS), its own C compiler (sicc), a calm desktop (Stage), its
own network stack with TLS 1.3, and an AI assistant (sia) that runs a
language model on the machine or talks to an OpenAI-compatible service.
GPL-3.0. All the code is new.

| | x86-64 (QEMU, KVM) | AArch64 (QEMU `virt`, emulated) |
|---|---|---|
| Kernel | 43 KB | 57 KB |
| Boot to the first prompt | 0.08-0.11 s | 0.11 s |
| RAM in use after boot, logged in | 3.1 MiB | 3.3 MiB |
| Message round trip (IPC) | 0.43 µs | 4.8 µs (emulated) |
| Desktop session | 3.6-3.9 MiB | 14 MiB (8 MiB is QEMU's screen) |
| Assistant, SmolLM2-1.7B (4-bit) | 37 tok/s on 4 CPUs, 55 on 8 | NEON; Pi 5 estimate 5-9 tok/s |

Documentation map: [docs/index.md](docs/index.md). What works where:
[docs/status.md](docs/status.md).

## Quick start (x86-64)

Needs `gcc`, `make`, `python3` and `qemu-system-x86_64` (KVM is used when
`/dev/kvm` is writable).

```sh
make            # the kernel, the servers, the host tools, the disk image build/disk.img
make run        # QEMU: a window with the desktop, and this terminal (serial console)
make run SMP=8 MEM=8G    # 8 CPUs, 8 GiB (default: 4 CPUs, 4 GiB)
make run-nox    # no screen at all: this terminal only
```

**First start:** a new disk has no account. The window shows the welcome
screen: choose root's password, create your account (passwords of 8
characters or more), "Create accounts", and you are logged in. With no
screen (`make run-nox`) the first start happens on the terminal.
[docs/accounts.md](docs/accounts.md).

**Leaving:** log in as root and type `poweroff` (it writes everything to
the disk first). The QEMU window can be resized; the picture is scaled.

**Your files** live on `build/disk.img` (3 GiB, sparse): `make` and
`make clean` keep it; `make newdisk` makes a fresh one (yours is lost),
`make distclean` removes everything. From the host:
`build/host/siefs build/disk.img ls /` (also `cat`, `put`, `get`, `attr`,
`find`) and `build/host/fsck.siefs build/disk.img`.

The QEMU command `make run` uses (without the window options):

```sh
qemu-system-x86_64 -smp 4 -m 4G -accel kvm -cpu host -no-reboot \
    -kernel build/kernel.bin -initrd "build/init.elf,build/con.elf,build/vblk.elf,build/fs.elf" \
    -drive file=build/disk.img,if=none,id=d0,format=raw -device virtio-blk-pci,drive=d0,disable-modern=on \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-modern=on,addr=5 \
    -chardev stdio,id=s0,signal=off -serial chardev:s0
```

## A PC from a USB key (x86-64, UEFI)

```sh
make usb        # build/sieos-usb.img: write it to a USB key (it erases the key)
make run-usb    # try it in QEMU first, with UEFI firmware
make usb-test   # the automatic test: first start, files, second boot, the key checked
```

The PC starts SIEOS's own UEFI loader from the key (Secure Boot off), and
SIEOS uses the key as its disk: what you write stays on it. How to write the
key safely and start a PC from it: [docs/usb.md](docs/usb.md).
The USB stack itself (xHCI, hubs, disks, keyboards and mice, `make
usb-dev-test`): [docs/usb-stack.md](docs/usb-stack.md).

## Quick start (AArch64 and the Raspberry Pi)

Everything for AArch64 is built by sicc alone, into `build-arm64/`. The
emulators come from `.hosttools/` if they are not installed.

```sh
make ARCH=arm64 run         # QEMU virt: the desktop (ramfb, virtio keyboard and tablet)
make ARCH=arm64 run-nox     # the same, this terminal only
make ARCH=arm64 run-pi4     # QEMU's Raspberry Pi 4 (screen; no USB input yet: serial console)
make ARCH=arm64 PI=4 sdcard FIRMWARE=dir    # a card image for a real Pi 4 (PI=5: a Pi 5)
```

The processor is emulated on a PC, so it is several times slower.
Details: [docs/arch.md](docs/arch.md); the Pis, the card, the serial
console and what to check on a real board:
[docs/raspberrypi.md](docs/raspberrypi.md).

## How it works

A **microkernel**: the kernel only manages memory, threads on every CPU,
time, and **messages** between programs; it delivers hardware interrupts as
messages too. Everything else is an ordinary program, a **server**, that a
crash takes down alone: init starts it again in about a millisecond, and
its clients carry on by themselves.

```
 programs   sh, fetch, sia, svc, hello, bench ...           (any user)
               │ messages (the kernel copies them, stamped with the sender's identity)
 servers    init  con  vblk  fs  auth  login  atlas  netd  vnet  siad
               │
 kernel     portable core: memory, threads, scheduler, messages, quotas
            + one architecture: x86-64 or AArch64
               │
 hardware   CPUs, RAM, disk, screen, keyboard, network card, serial port
```

| Server | Role | Started |
|---|---|---|
| `init` | the supervisor: starts and restarts every server | by the kernel |
| `con` | the console: serial port, keyboard, mouse | at boot |
| `vblk` | the disk driver: virtio, or the Pi's SD card | at boot |
| `fs` | the file server (SieFS), checks permissions | at boot |
| `login`, `auth` | the text login; the accounts server (passwords) | at boot; auth on demand |
| `atlas` | the desktop | at boot |
| `vnet`, `netd` | the network card's driver; TCP/IP, DHCP, DNS | on demand |
| `siad` | the assistant | on demand |

On-demand servers start the first time a program asks for them and stop
after a quiet period (network 10 min, assistant 15, accounts 5). The full
tour, file by file, with the crash and on-demand mechanisms:
[docs/kernel.md](docs/kernel.md).

## What is inside

- **Kernel** ([docs/kernel.md](docs/kernel.md), [docs/arch.md](docs/arch.md)):
  SMP with one kernel lock, tickless time, threads, floating-point and
  vector state saved only for threads that use it, thread-local storage,
  memory quotas, processor features reported to programs, interrupts on any
  CPU (shared lines), pages zeroed on allocation, W^X.
- **SieFS** ([docs/siefs.md](docs/siefs.md)): copy-on-write B+tree, two
  superblocks, checksums in every pointer (a crash leaves the old or the
  new state, never a mix), small files inside the tree, attributes with an
  index (`find key=value`). Tested with 2,151 simulated power cuts.
- **Users** ([docs/accounts.md](docs/accounts.md)): Argon2id passwords,
  groups, permissions enforced by the file server with the identity the
  kernel stamps on every message; `passwd`, `su`, `useradd`, `userdel`.
- **Stage, the desktop** ([docs/desktop.md](docs/desktop.md)): one project
  at a time (a rail of projects, the Inbox for files with none), at most two
  windows on the stage (terminals as you, an editor, the assistant), the
  project's other things as cards on a shelf, and the Lens (Ctrl+Space) to
  find a file, start an app or ask sia. One process, no redraw when nothing
  happens.
- **Network** ([docs/net.md](docs/net.md)): TCP/IP, DHCP, DNS, HTTP, TLS 1.3
  with certificate checking; `fetch URL`.
- **sia, the assistant** ([docs/sia.md](docs/sia.md)): below.
- **sicc, the C compiler** ([docs/cc.md](docs/cc.md)): below.

## The assistant: sia

`sia QUESTION`, `sia` (a conversation), or Ctrl+Space then Tab on the desktop. It runs a
language model on this machine with our engine ([docs/llm.md](docs/llm.md):
GGUF models, AVX2/AVX-VNNI kernels on x86-64, NEON and `sdot` on AArch64),
or uses any OpenAI-compatible service:

```sh
sia config backend=remote                     # as root; backend=local to come back
sia config url=https://api.example.com/v1
sia config api_model=MODEL_NAME
sia config api_key=YOUR_KEY                   # kept in a root-only file
```

The default local model, SmolLM2-1.7B (4-bit, 1 GB, copied to `/models` by
`make model`), loads in 2-3 s and answers at 37 tokens/s on 4 CPUs, 55 on 8;
the assistant is limited to 4.5 GiB by the kernel and stops when unused.

sia remembers: conversations are saved per user (in `/var/sia`, private to
each user) and continue after a restart or a reboot (`sia list`,
`sia resume N`, `sia new`, `sia forget`). A long conversation is compacted:
the model replaces its older turns by short notes. It also keeps a few facts
about you, only with your OK (`sia memory`, `sia remember "..."`).
Details, configuration and the assistant's memory: [docs/sia.md](docs/sia.md).

## The compiler: sicc

One program (`cc/`) that preprocesses, compiles, assembles and links C11
(plus the GNU extensions SIEOS uses) for **x86-64 and AArch64**, with its
own optimizer, assemblers and linker. It builds all of SIEOS, compiles
itself (byte-identical stages, on both architectures), and compiles SIEOS
about 15 times faster than gcc. Details: [docs/cc.md](docs/cc.md).

## Make targets

| Target | What it does |
|---|---|
| `make` | build everything for `ARCH` (default x86_64) and the disk image |
| `make run` / `run-nox` | run in QEMU with a window / with this terminal only |
| `make run-pi4` | (ARCH=arm64) run on QEMU's Raspberry Pi 4 |
| `make sdcard PI=4\|5 FIRMWARE=dir` | (ARCH=arm64) a card image for a real Pi |
| `make usb` / `run-usb` / `usb-test` | a USB key image for UEFI PCs / it in QEMU (OVMF) / its test |
| `make model` | copy the assistant's model to the disk (`MODEL=` another file of `models/`) |
| `make newdisk` | a fresh disk (your files on it are lost) |
| `make clean` / `distclean` | remove the build (keeping your disk) / everything |
| `make test` | first start, logins, permissions, files, disk check, second boot, crash recovery |
| `make crash-test` | only the crash recovery: servers killed while they work |
| `make gui-test` | the desktop through QEMU's monitor: first start, projects (and the islands' migration), stage, shelf, Lens, apps, editor, assistant (a stand-in) |
| `make gui-test-real` | the same with the real assistant and the 1.7B model |
| `make kernel-test` | floating point and vector state, thread-local variables, quotas |
| `make demand-test` | servers started on demand, stopped when idle, calls racing a stop |
| `make sia-test` | the assistant: answers, two conversations, restart (`CPU64=max` on arm64: `sdot`) |
| `make sia-mem-test` | the assistant's memory ([docs/sia.md](docs/sia.md)) |
| `make net-test` / `net-crypto-test` | the network end to end / its crypto against official vectors |
| `make pi4-test` | (ARCH=arm64) the boot and crash test on QEMU's Pi 4 |
| `make pi4-sdbench`, `make sdhci-bench` | (ARCH=arm64) SD card speed: on QEMU's Pi 4, and on a PCI SD controller with DMA (`SDCAPS`, `SDBSIZE`, `SDMIB`) |
| `make siefs-tools` / `siefs-test` / `siefs-freestanding` | SieFS host tools / its tests / a freestanding build check |
| `make crypto-test` | BLAKE2b, ChaCha20, Argon2 against official vectors |
| `make cc` / `cc-test` / `cc-bootstrap` / `cc-bench` | sicc / its tests / self-compilation / speed vs gcc |
| `make cc-sieos` | all of SIEOS (x86-64) built by sicc, then the boot and crash tests |
| `make cc-a64-test` / `cc-a64-bootstrap` | sicc's AArch64 tests / self-compilation, emulated |
| `make llm` / `llm-test` / `llm-neon-test` / `llm-bench` / `llm-freestanding` | the engine's host tools / tests / NEON tests / speed / freestanding check |

Options: `ARCH=x86_64|arm64`, `SMP=`, `MEM=`, `DISK=`, `B=` (build
directory), `CPU64=cortex-a72|max` (the emulated ARM processor).

## Limits

- **Kernel:** one kernel lock (many clients of one server on many CPUs
  contend); the scheduler does not tell P-cores from E-cores yet.
- **Desktop:** text apps only (in terminals); up to 256 files, 23
  projects, 8 terminals and one open file at a time.
- **Files:** up to 1 s of changes can be lost if the file server crashes
  (`sync` reports it).
- **Network:** IPv4 only; TLS 1.3 only.
- **Raspberry Pi:** no USB, Ethernet or Wi-Fi drivers yet; SD card at
  25 MHz; real boards not yet tried.
- **The target laptop** (UEFI, NVMe, NVIDIA): planned
  ([docs/ai-plan.md](docs/ai-plan.md)).

## Roadmap

Done: microkernel, SieFS, disk, users, supervisor, on-demand servers,
the desktop, network and TLS, sia (local and remote), sicc (x86-64 and AArch64),
AArch64 port (QEMU, Raspberry Pi 4 in QEMU, card images). Next: the
assistant's memory, the laptop (UEFI, NVMe, USB, NVIDIA compute), the Pis
on real boards (USB, Ethernet), sicc running on SIEOS, encryption and
snapshots. Details: [docs/status.md](docs/status.md).

## License

(c) Olivier Moulin. SIEOS is free software under the GNU General
Public License, version 3 only (GPL-3.0-only): see [LICENSE](LICENSE).
