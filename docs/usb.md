# SIEOS from a USB key (PCs with UEFI firmware)

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only

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
  │      SIEOS/BOOT.CFG         module= lines (init first), screen=WxH
  └── 2. SieFS (the rest of the key): SIEOS's disk, read and written
         found by its partition type 5E1E0500-51EF-4153-9E05-534945465331
```

## Make the image

```sh
make usb                      # build/sieos-usb.img, 8 GiB (sparse: it takes little room)
make usb USBDISK=16G          # the key's size (at most your key's)
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
- **The disk**: `vblk` takes the GPT partition of type SieFS on the boot
  disk; on a real key it reaches it through the USB storage driver
  (`user/usb`).

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

## Limits

- Secure Boot: not supported yet (the loader is not signed).
- x86-64 PCs only (the Raspberry Pis boot from their own firmware: see
  [raspberrypi.md](raspberrypi.md)).
- The kernel and the modules must fit below 4 GiB, and the memory at 1 MiB
  must be free (it is on the firmwares we know of).
- Power off on real hardware: the ACPI sleep command needs the firmware's
  tables to be read (not done yet): on a real PC, `poweroff` stops SIEOS and
  writes the disk, then the machine is turned off with its power button.
