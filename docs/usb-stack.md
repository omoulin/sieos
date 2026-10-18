# The USB stack

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

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
| `user/usb/hid.c` | boot keyboards, mice and tablets (report descriptor parsed by `user/lib/hidrep.c`, shared with I2C touchpads: [touchpad.md](touchpad.md)) → `CON_INJECT`; a keyboard with more on its interface (a Surface Type Cover's touchpad and volume keys) in the report protocol, its touchpad switched to fingers ([surface-pro-7.md](surface-pro-7.md#the-type-cover)) |
| `user/usb/uvc.c` | cameras (USB video class): YUY2 frames over isochronous transfers, port `camera` |
| `user/usb/kbl.c` | the Legion laptop's keyboard light (ITE 8258, 048d:c195), port `kblight` |
| `include/mk/gpt.h` | GPT layout and the SieFS partition type GUID (shared with `tools/mkusb.py`) |

## How it works

- **Controller.** Firmware hand-off (legacy support capability), reset, up
  to 64 slots, scratchpad pages, a 64-TRB command ring, a 256-TRB event ring
  with interrupt moderation (40 µs). Memory comes from `dma_alloc`; `DMA_LOW`
  only when the controller cannot address 64 bits or on the Pis. Addresses
  given to the controller are bus addresses (physical + the DT's offset).
- **Interrupts.** MSI when the controller has it (x86-64: `pci_msi`, the
  kernel's `SYS_IRQ_MSI`), else MSI-X, else its INTx line (level); with
  none (a laptop's firmware often leaves the line unset: the Surface Pro
  7's), polling every 8 ms. The log says which: `usb: xHCI ..., 18 ports,
  interrupts: MSI`. The irq thread drains the event ring and wakes the
  waiting thread. A slow poll thread backs it up; if no interrupt has ever
  arrived after three timeouts the server switches to fast polling (0.5 ms)
  and says so. `make usb-test` boots with MSI, then MSI-X.
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
  NVMe namespace with a SieFS partition ([usb.md](usb.md#the-pcs-nvme-drives)), then the first
  USB disk (waiting until enumeration settles). On that disk it takes the
  GPT partition of type `5E1E0500-51EF-4153-9E05-534945465331`, else an MBR
  partition of type `0x5E`, else the whole disk if it is raw SieFS.
- **Input.** Key presses become set-1 scancodes (with key repeat), pointers
  relative or absolute moves; `con` treats them like its own devices.

## Cameras

`uvc.c` takes a camera's VideoStreaming interface from the whole
configuration descriptor: the uncompressed **YUY2** format (no JPEG
decoder), 640 x 480 if offered, else the biggest size that does 15
frames a second or more; its frame rates; the alternate settings and
their isochronous endpoints. The laptop's camera (04f2:b83e) offers
640 x 480 YUY2 at 30 frames/s, alternate settings of 128 to 3060 bytes per
125 µs.

- **Port `camera`** (`CAM_OPEN`, `CAM_FRAME`, `CAM_CLOSE`, mk/proto.h):
  one client at a time; frames come converted to 0x00RRGGBB (integer
  BT.601, 0.7 ms a 640 x 480 frame on the host). The camera streams only
  while open: closed (or after 3 s without a request: a client that ended),
  it is put back to alternate setting 0, which powers its sensor down.
- **Opening:** VS_PROBE_CONTROL with the format, frame and the fastest
  rate; the camera answers how many bytes it sends per interval; the
  smallest alternate setting that carries them (else the next slower
  rate); VS_COMMIT_CONTROL; SET_INTERFACE; the endpoint added to the
  controller (`configure`; `ep_drop` removes it at close).
- **Isochronous transfers** (`xhci.c`): 40 "isoch" TRBs stay queued (5 ms
  at one per microframe), each into its own 4 KiB of DMA memory, started
  "as soon as possible" (SIA), an interrupt every 8th (BEI on the others:
  1000 interrupts a second). Each completion (`async_trb` names its TRB)
  hands the packet to the frame being filled and queues the TRB again;
  TRBs the controller skipped (a missed interval) are queued again too.
- **Frames:** each packet starts with a payload header: the frame bit
  (flips each frame), end of frame, error. A frame complete, without
  error and exactly w x h x 2 bytes becomes the ready one (three buffers:
  being filled, ready, being sent), else it is dropped.

## The keyboard light

The Legion 5 Gen 10's keyboard (ITE 8258, 048d:c195) takes 960-byte
feature reports with id 7 on its interface 0 (vendor page 0xFF89), as
OpenRGB found ("Lenovo Legion 5 Gen 10", `LenovoUSBController_Gen7_8`):
`0xCA` asks the active profile, `0xCB` sets a profile's lighting (one
group: static, the colour, the 24 zones), `0xCE` the brightness (0 off, 3
low, 9 high). `kbl.c` sends nothing unless the device is exactly
048d:c195 **and** its report descriptor holds that very feature report
(page, usage, id, 959 bytes); every command sent is logged with its first
bytes. Port `kblight` (`KBL_SET`, `KBL_GET`): the level (off, low, high)
and a colour; the setting is kept in `/etc/kblight` and given to the
keyboard again at each start. `/bin/kblight` and Settings use it.

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

QEMU has no USB camera (only `usb-host` passthrough): `make camera-test`
runs `uvc.c` and `kbl.c`'s check on the host, with the laptop's own
descriptors (`tests/camera`, read from Linux's sysfs): the format, frame,
rates and alternate settings found; frames assembled from synthetic
packets (an error bit, a lost packet, a frame without its end mark,
header-only packets, a buffer being sent left alone); YUY2 to RGB values;
the light's report found on interface 0 only. `gui-test` opens the
Camera window ("No camera.") and a .ppm in it.

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

MSI and MSI-X on x86-64 only (the Pis' and QEMU virt's controllers use their
lines); one controller (on a PC with several, the chipset's at 00:14.0 when
there is one: Intel laptops also have the CPU's Thunderbolt xHCI at
00:0d.0, e.g. Ice Lake's 8086:8a13, first in PCI order but serving only the
USB-C ports, while the built-in Bluetooth, camera and USB-A ports are on the
chipset's, e.g. 8086:34ed; neither needs a quirk beyond Linux's power
management ones); the memory of an unplugged device is not given back;
isochronous transfers only at high speed (and full speed: written, not
tried), up to 4 KiB per interval; one camera at a time;
a USB disk is root only when there is no virtio or SD disk.
