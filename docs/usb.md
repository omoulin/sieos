# SIEOS from a USB key (PCs with UEFI firmware)

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

`make usb` builds `build/sieos-usb.img`. Written to a USB key, it starts
SIEOS on a PC, and SIEOS then uses **the key as its hard disk**: what you
create and change is written to the key and is still there next time.
Nothing on the PC's own disks is touched.

```
  USB key (GPT partition table)
  ├── 1. EFI System Partition (FAT32, 64 MiB): what the firmware reads
  │      EFI/BOOT/BOOTX64.EFI   SIEOS's own UEFI loader (boot/uefi, 11 KB)
  │      SIEOS/KERNEL.BIN       the kernel
  │      SIEOS/INIT.ELF ...     the boot modules (init, con, usb, vblk, fs)
  │      SIEOS/BOOT.CFG         module= lines (init first), screen=WxH, root=GUID
  └── 2. SieFS (the rest of the key): SIEOS's disk, read and written
         type 5E1E0500-51EF-4153-9E05-534945465331, found by its own
         GUID (BOOT.CFG's root=); /boot holds the boot files, and the
         snapshot "factory" the system as built (what the installer copies)
```

To put SIEOS on the PC's own drive, beside its other systems (or on a
whole drive), see [install.md](install.md): the desktop's Install window,
started from this key.

## Make the image

```sh
make usb                      # build/sieos-usb.img, 7 GiB, so it fits a key sold as "8 GB" (sparse: it takes little room)
make usb USBDISK=16G          # the key's size (at most your key's: a key sold as "8 GB" is 7.5 GiB)
make usb USBMODEL=1           # with the assistant's model in /models (1 GB)
make usb USBSCREEN=2560x1600  # the screen size the loader asks for first
make run-usb                  # try it in QEMU first (UEFI firmware, the image as a USB stick)
make usb-test                 # the automatic test (below)
```

The image holds a fresh SieFS: **making it again starts from an empty
disk**; on the key itself your files stay, since you write the image once.

## Write it to a key

**This erases the whole key.** Choose the key, never one of the computer's
own disks: on the target laptop, its two NVMe drives hold your current
system and data.

1. Plug the key in, then find its name and check its size and model:
   - Debian/Ubuntu and other Unix-like systems: `lsblk -o NAME,SIZE,MODEL,TRAN`;
     the key is the one with `TRAN` = `usb` (for example `/dev/sdb`, never
     an `nvme...` name).
   - Windows: use a writer tool that lists only removable drives
     (balenaEtcher, or Rufus in "DD image" mode).
   - macOS: `diskutil list`, the "external, physical" disk (`/dev/diskN`).
2. Write it (Unix-like systems; replace `sdX` with the key):
   ```sh
   sudo dd if=build/sieos-usb.img of=/dev/sdX bs=4M conv=fsync status=progress
   ```
   (macOS: `of=/dev/rdiskN bs=4m`; first `diskutil unmountDisk /dev/diskN`.)
   The image is sparse, but `dd` writes all of it (the key's full size).
3. Eject the key properly before removing it.

## Start the PC from the key

1. **Secure Boot must be off** for now: SIEOS's loader is not signed. In the
   firmware's setup (often F2 or Del at power-on), Security > Secure Boot:
   Disabled. Turn it back on to start your usual system if it needs it.
2. Plug the key in, power on, and open the **boot menu** (often F12, F11,
   F8 or Esc depending on the maker), then choose the USB key ("UEFI: ...").
3. What you see: "SIEOS loader" on the firmware's screen, then the desktop's
   welcome screen at the screen's own resolution: the first start creates
   the accounts on the screen ([accounts.md](accounts.md)).
4. Power off from the desktop or with `poweroff` (root): changes are written to
   the key at once then (otherwise at most 1 s after they are made).

## How it works

- **The loader** (`boot/uefi/loader.c`, compiled by sicc with `-fpic`,
  linked twice and turned into a PE file by `tools/elf2efi.py`): reads its
  partition, loads the kernel at 1 MiB and the modules in one block below
  4 GiB (with room after them for the kernel's page bitmap), sets the screen
  mode, finds the ACPI tables, leaves the firmware (ExitBootServices) and
  enters the kernel's 64-bit entry with a `sieos_boot_t` (`include/mk/boot.h`).
- **The kernel** (`kernel/arch/x86_64`): `boot.S` has a second entry,
  `_start64`, found by the word `SIE6` after the multiboot header; it builds
  the same page tables as the multiboot path. `platform.c` turns the
  firmware's memory map into free RAM; the ACPI pointer comes from the
  loader (no BIOS areas under UEFI). Without the old PC timer chip (often
  switched off on recent laptops), the time base comes from the CPU
  (CPUID). The frame buffer is mapped write-combining (the PAT).
- **The screen**: the desktop asks the kernel for the firmware's frame buffer
  (`SYS_SCREEN`) and lays out to its size (tested at 1280 x 800,
  1920 x 1080 and 2560 x 1600).
- **The disk**: `BOOT.CFG`'s `root=GUID` (the SieFS partition's unique
  GUID, random for each image) is passed by the loader to the kernel as a
  boot option, and by the kernel to `vblk` (`SYS_INFO`), which takes that
  partition on whichever drive it is (on a real key through the USB
  storage driver, `user/usb`), never another drive's SieFS: with SIEOS
  also installed on the PC's NVMe drive, the key still uses its own.

## Tests

`make usb-test` makes a 512 MiB key image and boots it in QEMU with UEFI
firmware (OVMF): the first start on the firmware's screen, accounts created
there, a file written; a second boot logs in with the same accounts; then the
key's SieFS partition is checked on the host (`fsck.siefs`, the file read
back). Measured (KVM): the kernel starts 0.64 s after power-on (the firmware's
part), the welcome screen appears at 1.24 s.

By default QEMU attaches the image as a USB stick (its xHCI controller and
USB storage): the firmware boots from it, then SIEOS's USB driver (`user/usb`)
serves it and `vblk` takes its SieFS partition, exactly as on a real PC.
`USBBUS=virtio` attaches it as a plain disk instead; the test passes both ways
(over USB: the kernel at 0.87 s, the welcome screen at 1.48 s).

## The PC's NVMe drives

`vblk` also drives NVMe SSDs (`user/vblk/nvme.c`; the target laptop's
Unionmemory and Samsung PM9A1 drives, behind PCIe root ports: the PCI scan
follows the bridges). At boot, when there is no virtio disk, it lists each
controller and namespace on the console and log, then takes the first
namespace with a **SieFS partition** (GPT type above, or MBR `0x5E`) as
SIEOS's disk, before any USB key (with `root=GUID`, the partition with that
GUID only); a drive without one is only read (its partition table), never
written, except by the installer ([install.md](install.md): `DISK_RAW_*`,
root only, while SIEOS's own disk is not an NVMe drive). In `make nvme-test`:

```
vblk: NVMe 00:06.0: QEMU NVMe Ctrl
vblk: NVMe namespace 1: 64 MiB, 4096-byte sectors
vblk: NVMe 00:07.0: QEMU NVMe Ctrl
vblk: NVMe namespace 1: 258 MiB, 512-byte sectors
vblk: NVMe disk, 256 MiB (SieFS partition)
```

How: controller reset and enable (admin queues of two entries), Identify
controller and namespace, one I/O queue pair; read, write (128 KiB per
command: two PRP entries, or a PRP list built once, the buffer never moves)
and flush. One command at a time, as the file server sends them, and
**polled** (a short spin, then yielding): the drive answers in
microseconds, and its interrupts would need ACPI's routing or MSI-X, which
SIEOS does not have yet. No copies: the data arrives by IPC in the buffer
the drive reads. Sectors of 512 bytes or 4 KiB; partitions on 4 KiB
boundaries. `vblk` grew by 2.4 KB of code (x86-64; arm64 3.6 KB).

`make nvme-test` (both architectures; QEMU's `-device nvme`): a blank drive
with 4 KiB sectors (listed, skipped), then a GPT drive whose SieFS
partition starts at 1 MiB: first start, `bench`, a file written, power off,
then `fsck.siefs` and the file read back on the host. Measured (`bench 64`,
x86-64 KVM): writes 376-396 MiB/s, reads 557-629 MiB/s (virtio: 217 / 213);
arm64 (emulated, `bench 16`): 152-166 / 252 MiB/s.

A drive whose MDTS (its largest transfer) is below 128 KiB has each
request cut into commands of that size (`vblk: NVMe: at most N KiB per
command`); `make nvme-test` gives QEMU's SieFS drive an MDTS of 3 (32 KiB)
so that this path is the one tested (its `bench` then: about 67 MiB/s
both ways, QEMU's cost per command; without the limit 365 / 600 MiB/s).
The Ice Lake laptop's SK hynix BC501 needs no quirk ([ice-lake.md](ice-lake.md#the-disk)).

Untested on the real drives: the controllers' reset timing, their MDTS and
LBA formats as read, a SieFS partition on one (the installer makes it:
[install.md](install.md), tested in QEMU).

## The laptop's camera and keyboard light

The USB server also drives the Lenovo's camera (port `camera`: the Camera
window, [desktop.md](desktop.md#camera)) and its keyboard light (port
`kblight`: Settings and `/bin/kblight`); how, and what is tested without
the laptop, in [usb-stack.md](usb-stack.md#cameras).

## Limits

- Secure Boot: not supported yet (the loader is not signed).
- x86-64 PCs only (the Raspberry Pis boot from their own firmware: see
  [raspberrypi.md](raspberrypi.md)).
- The kernel and the modules must fit below 4 GiB, and the memory at 1 MiB
  must be free (it is on the firmwares we know of).
- Power off, restart and sleep on real hardware go through the firmware's
  ACPI tables ([power.md](power.md)); untested on the laptop yet.
