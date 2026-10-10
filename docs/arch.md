# Processors: the portable kernel and its architectures

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only

SIEOS runs on x86-64 and on 64-bit ARM (AArch64: QEMU's `virt` machine
today, the Raspberry Pi 4 and 5 next), both maintained side by side. The
arm64 system is built entirely by sicc (compiler, assembler, linker). The
kernel is split in two:

```
kernel/                      the portable core: identical on every processor
  main.c      kernel_main (boot sequence), the kernel lock, cpus[]
  task.c      processes, threads, scheduler, tickless timer decisions, ELF loading
  ipc.c       ports, messages, interrupts as messages
  syscall.c   the system calls (arguments through SC_* from arch_defs.h)
  mem.c       page allocator, kernel objects, quotas, DMA quarantine, vm_copy
  random.c    the random pool (sources from the architecture)
  kprintf.c   the log (bytes through arch_putc)
  kernel.h    processes, threads, cpu_t, the core's functions
  arch.h      THE INTERFACE: what every architecture provides
kernel/arch/x86_64/          one processor and its platform
  arch.mk     objects, compiler flags, linker script (for the Makefile)
  arch_defs.h layout, regs_t, arch_cpu_t, VM_* flags, inline helpers
  boot.S ap.S entry.S linker.ld   boot, other CPUs, entry/exit, layout
  cpu.c       GDT/TSS/IDT, SYSCALL, trap(), new thread frames, TLS
  smp.c       ACPI MADT, local and I/O APICs, TSC time, timer, IPIs, shootdowns
  fpu.c       x87/SSE/AVX state (lazy first use, XSAVE)
  mmu.c       4-level page tables, direct map, kmap
  platform.c  multiboot -> boot_info_t, COM1 log, power, RDSEED/RDRAND
  x86.h       what these files share among themselves
kernel/arch/arm64/           the same interface, for AArch64 (see below)
  arch.mk     objects, the tools (sicc --target=aarch64), what is not built
  arch_defs.h layout, regs_t, arch_cpu_t, descriptor bits, inline helpers
  boot.S      image header, EL2 -> EL1, first page tables, MMU on, other CPUs' entry
  entry.S     exception vectors, switch_to, FP/SIMD save/load
  linker.ld   the kernel at 0xFFFFFFFF80000000 (physically wherever it was loaded)
  cpu.c       per-CPU setup, new thread frames, system calls and faults
  smp.c       GICv2, generic timer (tickless), PSCI / spin table, kicks
  fpu.c       lazy FP/SIMD (CPACR_EL1)
  mmu.c       4-level tables, direct map, kmap, broadcast TLB invalidation
  platform.c  device tree -> boot_info_t, PL011 log, PSCI power, RNDR
  arm64.h     what these files share among themselves
lib/fdt.c                    the device tree reader, shared by the kernel and drivers
```

`make ARCH=x86_64` (the default) or `make ARCH=arm64` builds the kernel from `kernel/*.c` plus
`kernel/arch/$(ARCH)/`; everything else (servers, programs, SieFS) is the
same C for every processor.

## The interface (kernel/arch.h, arch_defs.h)

| Need | Provided by the architecture |
|---|---|
| Boot | its boot code fills `boot_info_t` (RAM ranges, boot-loader data to free later, reserved ranges, kernel image, already-mapped RAM, boot modules) and calls `kernel_main` |
| Memory layout | `P2V`/`V2P` (direct map), `USER_TOP`, `STACK_TOP`, `MMIO_BASE`, `PAGE` |
| Page tables | `vm_new`, `vm_map` (flags `VM_W VM_U VM_NX VM_UC VM_DEV VM_DMA`), `arch_vm_unmap`, `arch_vm_free` (each leaf to the core, which frees or quarantines), `arch_uaddr`, `arch_mem_init` (map all RAM, switch to `kernel_as`), `kmap`, `arch_as_load`, `tlb_shootdown`/`tlb_ack` |
| Threads | `regs_t` + `SC_NR/SC_ARGn/SC_RET`, `switch_to`, `arch_thread_start`, `arch_set_kstack`, `arch_set_tls`/`arch_tls_switch` |
| Traps | the architecture's handler decodes the event and calls core functions: `irq_raise`, `core_tick`, `proc_exit` (faults), `syscall`, `fpu_first_use`, `kernel_exit` on the way out |
| Interrupt controller | `irq_route`, `irq_set_level`, `irq_mask`, `irq_unmask`, `arch_kick` (wake/reschedule another CPU), `halt_others` |
| Time | `ticks()` (fast counter), `ns_to_ticks`, `now_ns`, `arch_timer_set` (one-shot deadline; `NEVER` = off) |
| CPUs | `cpu_init` (cpus[0]), `smp_init`, `smp_start`, `this_cpu()`, `arch_cpu_t` first in `cpu_t` |
| Floating point | `fpu_first_use`, `fpu_switch`, `fpu_free` |
| Platform | `arch_putc` (log), `arch_power`, `arch_hw_random_init`/`arch_hw_random`, `arch_phys_forbidden`, `arch_phys_screen`, `arch_dma_clean`, `arch_fdt`, `arch_screen` (the frame buffer a UEFI loader left, `SYS_SCREEN`; x86-64) |
| Processor features | `arch_hwcap`: what programs may use, in `SYS_INFO`'s `hwcap` (`HWCAP_AVX2`, `HWCAP_FMA`, `HWCAP_AVXVNNI` on x86-64; `HWCAP_DOTPROD`, `HWCAP_LSE` on AArch64, from `ID_AA64ISAR0_EL1`, which programs cannot read) |
| Spinning, idle | `arch_relax`, `arch_wait_irq`, `arch_halt_forever`, `arch_lock_wait` |
| Programs | `ELF_MACHINE` (62 on x86-64, 183 on AArch64) |

The split cost nothing measurable: IPC round trip 432/434/479 ns (no FP /
SSE / AVX) before and after; kernel.bin 41,816 -> 42,200 bytes (+0.9%:
a few functions no longer inlined across files); boot to the first prompt
0.08-0.12 s, unchanged; every test suite passes.

## AArch64 (arm64): QEMU `virt`

**Status.** SIEOS boots on QEMU's `virt` machine (GICv2, Cortex-A72, up to
8 CPUs) to the serial login, with the same servers and programs as on
x86-64: init, con, vblk, fs (SieFS), auth, login, sh, vnet, netd, sia/siad
(the local model, with NEON kernels; `sdot` when the processor has it),
started on demand like on x86-64. The desktop works too (`make ARCH=arm64
run`: QEMU's ramfb, virtio keyboard and tablet; the graphical first start).
Every test suite passes (`test`, `crash-test`, `kernel-test`,
`demand-test`, `sia-test`, `net-test`, `gui-test`), with x86-64 unchanged.

```
make ARCH=arm64 run                # build everything with sicc, boot it in QEMU (a window: the desktop)
make ARCH=arm64 run-nox            # the same, this terminal only (no screen)
make ARCH=arm64 sia-test CPU64=max # CPU64: cortex-a72 (default, the Pi 4's) or max (with sdot, like the Pi 5)
make ARCH=arm64 test SMP=4         # likewise for test, crash-test, kernel-test, demand-test, sia-test, net-test
```

`ARCH=arm64` builds into `build-arm64/` (or `B=...`) and needs
`qemu-system-aarch64`; sicc itself is built first, by the host compiler, as
a host tool. What QEMU runs (the Makefile's line):

```
qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a72 -smp 4 -m 4G \
    -device ramfb -device virtio-keyboard-device -device virtio-tablet-device \
    -accel tcg,thread=multi -no-reboot -nographic \
    -kernel build-arm64/kernel.bin -initrd build-arm64/modules.img \
    -drive file=build-arm64/disk.img,if=none,id=d0,format=raw -device virtio-blk-device,drive=d0 \
    -netdev user,id=n0 -device virtio-net-device,netdev=n0
```

The processor is emulated (no hardware acceleration on a PC), so
everything is several times slower than on x86-64: the tests scale their
waits by `SIEOS_SLOW` (4 for arm64).

**Boot.** `kernel.bin` is a flat image with the standard AArch64 image
header, so QEMU and boot loaders (the Pi firmware too) load it anywhere in
RAM (2 MiB aligned) and jump to it with the device tree's address in x0.
`boot.S` drops from EL2 to EL1 if needed, builds the first tables (the
kernel's pages at 0xFFFFFFFF80000000, the 1 GiB of RAM around the kernel
and the device tree in the direct map, an identity map for the switch),
turns the MMU on, and calls `arm64_main`. That function reads the device
tree (`lib/fdt.c`): RAM (`/memory`), reserved ranges (`/memreserve/`,
`/reserved-memory`), the boot modules (`/chosen` initrd), PSCI, the console
UART, and fills `boot_info_t` for `kernel_main`, like multiboot on x86-64.
The boot modules (init, con, vblk, fs) are one archive, `modules.img`
(`tools/mkmods.py`, "SIEOSMOD"), passed as the initrd.

**Memory.** 4 KiB pages, 4 levels, 48-bit addresses, the same layout as on
x86-64: TTBR1 holds the kernel's half (direct map at 0xFFFF800000000000,
device registers at 0xFFFFFF0000000000, the image at the top), TTBR0 each
process's. One address space id (0) for everyone: a switch invalidates its
non-global entries (`tlbi aside1`). Unmapping broadcasts in hardware
(`tlbi vale1is`), so no inter-processor interrupt is needed. Memory types
through MAIR: normal write-back, device (nGnRnE) for `VM_UC`. `VM_DEV` and
`VM_DMA` live in the descriptors' software bits.

**Exceptions.** One vector table; `svc #0` (number x8, arguments x0-x5,
result x0); faults kill the process with the syndrome and address in the
log. Interrupts: GICv2 (the Pi 4/5's GIC-400 is one), software interrupts
1 (kick) and 2 (halt), the virtual timer (PPI 27) as the one-shot tickless
deadline, devices as shared lines (SPIs) delivered as messages exactly as
on x86-64 (a driver's `irq_bind` number is the SPI number from the device
tree). FP/SIMD: lazy, saved per thread on first use (q0-q31, FPCR, FPSR).

**Other CPUs.** PSCI `CPU_ON` (QEMU, Pi 5), or a spin table (Pi 4
firmware), as the device tree says.

**Drivers.** They find their device in the device tree, which the kernel
hands out (`sys_fdt`): con on the PL011 UART (interrupt-driven input), vblk
and vnet over virtio's MMIO transport (`user/lib/virtio.c`, shared with the
PCI transport of x86-64), the clock from the PL031. Port I/O exists only on
x86-64.

**Programs.** TLS variant 1 (TPIDR_EL0 points 16 bytes before the block),
`svc` system-call stubs and crt0 per processor in `user/lib`; the language
model engine (`llm/`) uses its NEON kernels, and the dot product (`sdot`)
when the kernel reports it (`HWCAP_DOTPROD`, [llm.md](llm.md)).

**Measurements** (QEMU `virt`, 4 emulated Cortex-A72 on a 28-thread PC,
so only indicative):

| | arm64 (emulated) | x86-64 (KVM) |
|---|---|---|
| kernel.bin | 57,368 bytes | 42,904 bytes |
| boot to the first-start prompt | 0.11 s | 0.08-0.12 s |
| memory used after boot, logged in | 3.3 MiB | |
| RAM free of 1 GiB after boot | 1022 MiB | |
| IPC round trip | 4.8 µs (5.5 µs with FP/SIMD) | 433 ns (432 x87/SSE, 479 AVX) |
| sia, SmolLM2-135M Q8_0 | 3.6 tok/s plain C, 4.1 NEON (4 threads, emulated: NEON is slow under emulation) | 186 tok/s (AVX2) |
| desktop session | 14 MiB, 8 MiB of it QEMU's framebuffer picture; a full frame ~10 ms | 3.6-3.9 MiB; 1-4 ms |

**Limitations (now).** DMA buffers are cacheable, with explicit cache
maintenance where a device is not coherent (the Pi: `dma_sync`,
[raspberrypi.md](raspberrypi.md)); QEMU is coherent, so only real boards
can catch a mistake there. Halting
other CPUs uses an ordinary interrupt (GICv2 has no NMI). The device tree
and initrd must lie in the kernel's 1 GiB at boot (QEMU and the Pi firmware
put them there). TLS alignment up to 16 bytes. One address space id.

## The Raspberry Pi 4 and 5

Status, instructions and what to check on real boards: [raspberrypi.md](raspberrypi.md).
In short (QEMU's `raspi4b`, `make ARCH=arm64 pi4-test`: boot and crash tests pass):

- **Boot**: the firmware loads `kernel8.img` (= `kernel.bin`, an Image) and
  `sieos.mod` (`initramfs ... followkernel`); `tools/mksdcard.py` writes the
  card (FAT32 boot partition + SieFS partition, MBR type `0x5E`).
- **CPUs**: the Pi 4 firmware's spin table (tested, 4 CPUs); PSCI on the Pi 5.
- **Console**: the first PL011 when the device tree's `stdout-path` names
  the mini UART (Pi 4, with `dtoverlay=disable-bt`); the Pi 5's debug UART.
- **Disk**: SDHCI in the disk server (`user/vblk/sd.c`, PIO, 25 MHz),
  the controllers tried in order until one holds an SD card.
- **Screen**: `user/atlas/screen.c`: firmware mailbox (Pi 4), the
  firmware's `simple-framebuffer` (Pi 5), `ramfb` (QEMU virt); frame
  buffers mapped write-combining (`MAP_WC`, MAIR attribute 2).
- **Input**: virtio-input on QEMU virt (`con`); USB on the Pis is next.
  Without input the desktop shows a notice and the serial console takes
  the first start (`CON_HASINPUT`).
- **Power**: PSCI, else the power manager's watchdog (Pi 4).
- **DMA and caches**: DMA pages cleaned after zeroing (`arch_dma_clean`),
  `dma_sync()` for drivers (EL0 cache maintenance, `SCTLR_EL1.UCI`/`UCT`),
  `DMA_LOW` for the firmware's first GiB, `fdt_bus_addr()` for `dma-ranges`.
- **Next**: USB (VL805 on the Pi 4, RP1 on the Pi 5), Ethernet, SD DMA and
  high-speed modes, NEON kernels for the language model.
