# The USB stack

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only

One user-space server, `usb` (`user/usb/`), drives the xHCI controller,
enumerates every device (hubs included), serves each USB disk with the
`DISK_*` protocol and feeds USB keyboards and mice to `con`. init starts it
before `vblk`, so a USB key can be SIEOS's root disk. How the key image is
built and booted (UEFI loader, `make usb`) is in [usb.md](usb.md).

## Files

| File | What it does |
|---|---|
| `user/usb/pci.c` | finds the controller: x86 PCI scan (`SYS_PCI`), arm64 ECAM from the device tree (BARs, `interrupt-map`), Pi 4 (VL805 behind the BCM2711 PCIe), Pi 5 (RP1) |
| `user/usb/xhci.c` | the controller: rings, contexts, commands, event ring, interrupts or polling, transfers |
| `user/usb/usb.c` | enumeration, control transfers, Configure Endpoint, hubs, hotplug, the `usb` port (`USB_DISKS`, `USB_DISK`) |
| `user/usb/msc.c` | mass storage: Bulk-Only Transport + SCSI, one `usbdiskN` port per disk |
| `user/usb/hid.c` | boot keyboards, mice and tablets (report descriptor parsed) → `CON_INJECT` |
| `include/mk/gpt.h` | GPT layout and the SieFS partition type GUID (shared with `tools/mkusb.py`) |

## How it works

- **Controller.** Firmware hand-off (legacy support capability), reset, up
  to 64 slots, scratchpad pages, a 64-TRB command ring, a 256-TRB event ring
  with interrupt moderation (40 µs). Memory comes from `dma_alloc`; `DMA_LOW`
  only when the controller cannot address 64 bits or on the Pis. Addresses
  given to the controller are bus addresses (physical + the DT's offset).
- **Interrupts.** INTx (level), no MSI. The irq thread drains the event ring
  and wakes the waiting thread. A slow poll thread backs it up; if no
  interrupt has ever arrived after three timeouts the server switches to
  fast polling (0.5 ms) and says so.
- **Enumeration.** A port change wakes the enumeration thread: reset, Enable
  Slot, Address Device, the descriptors, Set Configuration, Configure
  Endpoint, then the class driver. Hubs (USB 2 and 3) get their own ports
  scanned; full/low-speed devices behind a high-speed hub use its TT.
- **Disks.** Each disk has its own thread serving `DISK_READ/WRITE/FLUSH`
  straight into a DMA buffer (`DISK_MAX`, 128 KiB per command). READ/WRITE
  10, or 16 above 2 TiB; FLUSH is SYNCHRONIZE CACHE. A stall or a failed
  status triggers reset recovery and one retry; a unit attention is retried.
- **Hotplug.** An unplugged disk answers errors. When it comes back (same
  vendor, product, serial and size) it gets its old `usbdiskN` name back,
  so a mounted file system continues.
- **Restart.** If `usb` crashes, init restarts it; `vblk` retries a failed
  call for up to 20 attempts, finds the port again and redoes the request.
- **Root disk.** `vblk` uses virtio first, then the SD card, then the first
  USB disk (waiting until enumeration settles). On that disk it takes the
  GPT partition of type `5E1E0500-51EF-4153-9E05-534945465331`, else an MBR
  partition of type `0x5E`, else the whole disk if it is raw SieFS.
- **Input.** Key presses become set-1 scancodes (with key repeat), pointers
  relative or absolute moves; `con` treats them like its own devices.

## Tests and measurements

`make usb-dev-test` (x86-64 with KVM; `ARCH=arm64` on QEMU `virt`, xHCI on
PCIe): the root disk is only a USB key; the first start is typed on a USB
keyboard; `bench`; `usb` killed during `fsloop` (no failed operation); a
second disk plugged, unplugged and plugged back; a clean `fsck` on the host;
a second boot. `make USBBUS=usb usb-test` boots the key through UEFI over
xHCI.

| | write | read |
|---|---|---|
| x86-64, KVM, QEMU usb-storage | 213–320 MiB/s | 391–428 MiB/s |
| arm64 `virt`, TCG | 100 MiB/s | 117 MiB/s |

USB root mounted 0.41 s after boot (x86-64); UEFI boot from the key: kernel
after 0.87 s.

## Raspberry Pi: untested

The Pi glue in `pci.c` is written from the hardware documentation but has
not run on a board (QEMU has no Pi PCIe). To verify:

- **Pi 4**: the PCIe link is up as left by the firmware (register 0x4068);
  the VL805 is loaded through the mailbox (tag 0x00030058); the controller's
  interrupt comes through `interrupt-map`; DMA works with the bus offset and
  low buffers; a key enumerates and `bench` runs.
- **Pi 5**: the RP1 xHCI window (0x1F00000000) is mapped by the firmware;
  the DWC3 switches to host mode; polling works (no interrupt yet); the bus
  offset 0x10_0000_0000 is right; only the first of the two controllers is used.

## Limits

No MSI; one controller; the memory of an unplugged device is not given back;
no isochronous transfers;
a USB disk is root only when there is no virtio or SD disk.
