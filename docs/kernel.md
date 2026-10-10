# The kernel and the servers, file by file

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

This is the guided tour: how SIEOS starts, how its pieces talk, and what
each file does. The architecture interface (x86-64, AArch64) is in
[arch.md](arch.md); each subsystem has its own page ([index.md](index.md)).

## The idea: a microkernel

The kernel (about 1,900 lines of portable core, plus 1,500 per processor
architecture; 43 KB on x86-64, 57 KB on AArch64) does only what nothing
else can do:

| The kernel does | Programs ("servers") do |
|---|---|
| memory, address spaces, processors | the console (`con`): screen, keyboard, serial |
| processes, threads, scheduling, time | the disk (`vblk`), files (`fs`), network (`vnet`, `netd`) |
| message passing (IPC) | users (`auth`, `login`), the desktop (`atlas`) |
| interrupts delivered as messages | the assistant (`siad`), the supervisor (`init`) |

Programs talk with **messages**. A server creates a named **port** and
waits on it. A client sends a message and sleeps until the reply. The
kernel copies the message **directly** from one program's memory into the
other's and stamps it with the sender's identity (pid, uid, gid, groups),
which servers can trust. A driver receives its device's interrupts as
messages on the same port, so one loop serves hardware and clients:

```
   sh                         kernel                        con
   printf("hi")
   ipc_call(console, WRITE) ->  copy "hi" into con's buffer -> ipc_recv returns
   (sleeps)                                                   prints on screen + serial
                            <-  copy the reply           <-   ipc_reply_recv(sh, 2)
   continues
   keyboard interrupt       ->  mask the line, wake con   ->  ipc_recv returns (token 0)
                                                              reads the key, irq_ack
```

A bug in a server kills only that server, and init starts it again.

## The boot, file by file

The kernel is a portable core (`kernel/*.c`) and one processor's part
(`kernel/arch/x86_64/` or `kernel/arch/arm64/`), joined by
`kernel/arch.h`. `make ARCH=x86_64|arm64` chooses (default `x86_64`).

1. **Entry** (`kernel/arch/*/boot.S`):
   - x86-64: QEMU (or GRUB) finds the *multiboot header*, loads the kernel
     at 1 MiB in 32-bit mode; `boot.S` builds the first page tables,
     switches to 64-bit mode, and `platform.c` turns the boot loader's
     information into a `boot_info_t`.
   - AArch64: QEMU or the Pi firmware loads the kernel `Image` and passes a
     *device tree*; `boot.S` drops from EL2 to EL1 if needed, turns the MMU
     on, and `platform.c` reads memory, CPUs and devices from the device
     tree (`fdt.c`).
2. **`kernel/main.c`**: `kernel_main` sets up memory and the first CPU,
   starts the first *boot module*, `init` (the supervisor), starts the
   other CPUs, and from then on only runs processes. The other boot
   modules (`con`, `vblk`, `fs`) stay in memory for init to start and
   restart. Also the kernel lock.
3. **`arch/*/cpu.c`, `entry.S`**: per CPU, the way into the kernel
   (x86: segments, TSS, `SYSCALL`; AArch64: the exception vector table,
   `svc #0`) and `trap()`, which handles every interrupt and exception.
4. **`arch/*/smp.c`** (and x86's `ap.S`): the other processors, the
   interrupt controllers (x86: I/O APIC and local APICs; AArch64: GIC-400),
   time and the tickless timer.
5. **Memory**: `kernel/mem.c` keeps a bitmap of free 4 KiB pages over all
   of RAM, a small allocator for kernel objects (it gives empty pages
   back), the DMA quarantine, quotas, and `vm_copy`, the checked copy
   between address spaces that IPC relies on. `arch/*/mmu.c`: the page
   tables (the *direct map* of all physical memory, each process's address
   space; AArch64 uses ASIDs so switching needs no TLB flush).
6. **`kernel/task.c`**: processes and threads (as many as memory allows),
   the round-robin scheduler shared by all CPUs (a waiting thread costs no
   CPU time; a CPU with nothing to do sleeps), the tickless timer's
   decisions, sleeping, ending and waiting for processes, and loading ELF
   programs (`SYS_SPAWN`).
7. **`kernel/ipc.c`**: ports, `call` / `recv` / `reply` (and `reply_recv`,
   the server's fast path), interrupts as messages (several drivers may
   share a line), and on-demand names. **The heart of the microkernel.**
8. **`kernel/syscall.c`**: the system call table. The contract with
   programs is `include/mk/abi.h`; the servers' message formats are
   `include/mk/proto.h`.

Then the servers, all ordinary programs:

9. **`user/con`**: the console: the serial port, and on x86 the VGA text
   screen and the PS/2 keyboard and mouse (QEMU's absolute pointer); on
   AArch64 the PL011 serial port and virtio keyboard and tablet. Line
   editing is `user/lib/tty.c`, shared with the desktop's terminals.
10. **`user/vblk`**: the disk driver: virtio (PCI on x86, MMIO on AArch64)
    or, on the Raspberry Pi, the SD card (`sd.c`). It serves the port
    "disk" (up to 128 KiB per message); data arrives by IPC straight into
    the buffer the device reads.
11. **`user/fs`**: the file server: the SieFS library ([siefs.md](siefs.md))
    on top of "disk", serving "fs". It checks permissions with the
    identity the kernel stamps on every message, keeps open-file handles,
    and commits changes 1 s after the first one (or on `sync`).
12. **`user/init`**: the supervisor, the only program the kernel starts.
    It starts every server from its table, starts each again when it ends,
    starts some only when needed and stops them when idle (below). Only
    init may give device rights. `svc` lists and controls the services.
13. **Users** ([accounts.md](accounts.md)): `user/auth` (accounts,
    Argon2id passwords), `user/login` (the text prompt), `user/acct`
    (`passwd`, `su`, `useradd`, `userdel`). The first accounts are created
    on the desktop's welcome screen (`user/atlas/setup.c`), or on the
    terminal when there is no screen.
14. **`user/sh`**: the shell (files, tags, users, processes; any other word
    runs `/bin/NAME`).
15. **`user/atlas`**: the desktop ([desktop.md](desktop.md)).
16. **The network** ([net.md](net.md)): `user/vnet` (the card's driver),
    `user/netd` (TCP/IP, DHCP, DNS), `user/fetch`.
17. **The assistant** ([sia.md](sia.md), [llm.md](llm.md)): `user/sia`.

`user/lib` is the library, an archive from which each program takes only
what it uses (system calls, `printf`, `malloc`, threads, files, network,
crypto).

## How a program reads a file

Every arrow is a message; the kernel copies the data once per arrow:

```
   sh: cat /etc/motd
     |  FS_OPEN "/etc/motd", FS_READ          (port "fs")
     v
   fs: walk the path, check the rights, SieFS finds the blocks
     |  DISK_READ block, count                (port "disk")
     v
   vblk: virtqueue request, waits for the device's interrupt (a message)
     v
   disk -> data -> vblk -> fs -> sh -> con (screen)
```

## When a server crashes

```
   kernel ──"your child ended" (a message)──► init ──spawn──► fs (new)
   client ──FS_READ──► (old fs dies) ── -EPIPE ──► library: port_lookup("fs")
          ◄── waits until the new fs registers ── reopens its files ── sends FS_READ again
```

- **Restart:** about 1 ms later (measured: fs 0.2-0.9 ms, vblk 0.2-0.9 ms,
  con 0.2-0.6 ms, auth 1-2 ms). A server failing within 1 s of starting
  waits 10, 20, 40, 80 ms ... and is given up after 5 such failures
  (`svc` shows "GIVEN UP"; `svc restart` tries again).
- **Clients carry on by themselves:** a call that fails because the server
  died (`-EPIPE`) or the port number is stale (`-ENOENT`) finds the port
  again by name and is resent, if repeating it is harmless (reads, writes
  at an offset, disk blocks, console text, most file operations; not a
  login, nor closing a file). Open files are reopened.
- **What survives:** `vblk`: everything. `con`: everything but the screen.
  `fs`: the disk's last commit, at most 1 s old (`sync` reports a loss
  once). `auth`: everything. `login`: the open session goes on.
- **Safety:** a dead driver's DMA buffers stay unused for 100 ms. init
  itself must not fail: the kernel stops with a clear message if it does.

## Servers started on demand, stopped when unused

```
   fetch ──lookup "net"──► kernel: no such port, but "net" is declared on demand
                             └──NOTE_WANT (a message)──► init ──spawn──► netd
   netd ──lookup "nic0"──► (the same) ─────────────────► init ──spawn──► vnet
   fetch wakes as soon as netd creates its port "net"

   unused for its idle time: init ──"may you stop?" (SVC_MAYSTOP)──► netd
   netd: nothing asked since, no connection open ──► yes, ends
   a call arriving meanwhile gets -ENOENT: the library resends it,
   which starts a new netd
```

- **Which ones:** the network (`netd`, `vnet`), the assistant (`siad`) and
  the accounts server (`auth`). `con`, `vblk`, `fs`, `login` and `atlas`
  start at boot.
- **Idle times:** network 10 min, assistant 15 min, accounts 5 min
  (`svc idle NAME SECONDS`; 0: never).
- **Measured:** after boot and a login, 3.1 MiB of RAM in use, instead of
  1 GiB when the assistant's 1.7B model was loaded at boot.

## Several processors, time, floating point

- **CPUs:** x86 finds them in the ACPI MADT and starts them with
  INIT/SIPI (`ap.S` trampoline); AArch64 reads the device tree and uses
  PSCI (QEMU, Pi 5) or the spin table (Pi 4). Each CPU has one page of its
  own data.
- **Tickless:** a CPU's timer (x86: TSC deadline or LAPIC; AArch64: the
  generic timer) is set only when something must happen: a time slice
  ending while others wait, or a sleeper's wake-up. 8 idle CPUs went from
  5.6% of a host core with a 100 Hz tick to under 0.1%.
- **Wake-ups:** a sleeping CPU is woken by an inter-processor interrupt;
  in IPC the receiver usually gets the sender's CPU at once ("handoff"),
  so a round trip stays on one CPU: about 0.43 µs on x86 (KVM).
- **One kernel lock**, as in seL4: a system call is a few hundred
  instructions, so it is rarely contended; servers run in parallel.
- **Interrupts:** each line can go to any CPU; it stays masked from the
  moment it fires until its drivers acknowledge it; up to 4 drivers may
  share a line.
- **TLB:** x86 flushes other CPUs by IPI; AArch64 uses broadcast `tlbi`.
- **Floating point and vectors** (`arch/*/fpu.c`): the kernel never uses
  them. A thread's first floating-point instruction traps once and gets a
  clean save area; from then on each switch saves and restores it
  (x86: XSAVEOPT/XRSTOR with AVX; AArch64: the FP/SIMD registers).
- **Processor features** (`SYS_INFO`'s `hwcap`, `arch_hwcap()`): the kernel
  tells programs what they may use: AVX2, FMA, AVX-VNNI on x86; the dot
  product (`sdot`, Pi 5) and LSE atomics on AArch64. The assistant picks
  its kernels from it.
- **Thread-local variables:** FS base (x86) or TPIDR_EL0 (AArch64), set
  per thread by the library.
- **Memory quotas:** a process may be limited in pages (heap, DMA,
  program); init gives the assistant 4.5 GiB.

## Choices made for speed and size

- **Fast system calls** (`SYSCALL`/`SYSRET`, `svc`), no message buffering
  (one copy, sender to receiver).
- **Small:** 4 KiB kernel stacks (deepest use measured: under 2 KiB,
  `make STACKCHECK=1`), one page per CPU, small hash tables.
- **Pages zeroed on allocation:** memory never leaks between programs.
- **W^X:** code is read-only, data never executable.

## Limits

- **CPUs:** up to 256 (xAPIC, GICv2); tested with 1 to 28 on x86, 4 on
  AArch64.
- **Memory:** all of it (tested with 6 GiB on x86).
- **Processes and threads:** as many as memory allows (sanity cap about a
  million threads).
- **Drivers** may map device memory only; DMA buffers come from the kernel.
- **One kernel lock:** many clients hammering one server on many CPUs
  contend (40 clients: 0.3 µs on 1 CPU, 1.4 µs on 8, 4 µs on 16).
- **Crashes:** up to 1 s of file changes can be lost if `fs` dies; `sync`
  reports it. If init fails, the system stops.
- **No P-core/E-core awareness** in the scheduler yet.
