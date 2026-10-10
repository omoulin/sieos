# SIEOS on the Raspberry Pi 4 and 5

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only

SIEOS runs on the Raspberry Pi 4 and 5 with the same kernel, servers and
programs as on QEMU's `virt` machine, all built by sicc. This page says what
works, how to try it on QEMU's emulated Pi 4 and on real boards, and what
must still be checked on real hardware (nothing here could be run on a
real board yet).

## What works (QEMU's `raspi4b`, tested)

| Part | How | Status |
|---|---|---|
| Boot | the firmware loads `kernel8.img`, `sieos.mod` (the boot modules) and the device tree | tested (QEMU does the firmware's part) |
| CPUs | Pi 4: the firmware's spin table; Pi 5: PSCI | Pi 4: 4 CPUs, tested |
| Interrupts, time | GIC-400 (GICv2), the generic timer, tickless | tested |
| Console | PL011 UART, 115200 bit/s | tested |
| Disk | SD card: SDHCI driver in the disk server (`user/vblk/sd.c`): ADMA2 DMA (64- or 32-bit descriptors), interrupts, High Speed, UHS-I (SDR50, DDR50, SDR104 with tuning) at 1.8 V when the board allows it; SieFS partition (MBR type `0x5E`) | PIO, interrupts and High Speed tested on `raspi4b`; ADMA2, interrupts and High Speed tested on QEMU's `virt` (a PCI SD controller); UHS-I and the 1.8 V switch: **untested** (QEMU has no regulators): checks 24-29 |
| Screen | Pi 4: frame buffer from the firmware (mailbox); Pi 5: the firmware's `simple-framebuffer` | Pi 4: tested |
| Keyboard, mouse | USB: **not supported yet** | the screen shows a notice; log in on the serial console |
| Network | Pi 4: GENET Ethernet (`user/vnet/genet.c`); Pi 5: RP1's GEM over PCIe (`user/vnet/gem.c`, polled); DHCP again when a cable is plugged in | **written, untested on hardware** (QEMU has neither): checks 16-23 below |
| Assistant | the local model with NEON kernels; `sdot` on the Pi 5 (the kernel reports it), plain NEON on the Pi 4 | emulated: tested; estimates in [llm.md](llm.md) |
| Power off, restart | Pi 4: the power manager's watchdog (halt partition); Pi 5: PSCI | Pi 4: tested |
| Tests | `make ARCH=arm64 pi4-test`: first start, logins, permissions, files, second boot, crash recovery | pass |

Without a keyboard or mouse, the desktop shows "This screen works, but
there is no keyboard or mouse yet" and the first start (choosing the
passwords) happens on the serial console. Everything else (files, users,
the assistant with a local model, `svc`, `sia`...) works there.

## Try it on QEMU's Pi 4

```sh
make ARCH=arm64 run-pi4      # QEMU raspi4b: screen window + serial console in this terminal
make ARCH=arm64 pi4-test     # the boot and crash tests on raspi4b
make ARCH=arm64 pi4-sdbench  # the SD card's speed there (bench on a fresh card)
make ARCH=arm64 sdhci-bench  # the same on QEMU's virt with a PCI SD controller: ADMA2 and interrupts
```

The first time, the Pi 4's device tree (`bcm2711-rpi-4-b.dtb`) is fetched
from the Raspberry Pi firmware repository into `.hosttools/rpi/` (it is the
firmware's file, not part of SIEOS, and is not put in the repository).
QEMU emulates the processors on your PC: expect it to be slow.

## Try it on a real Pi 4 or Pi 5

You need: the board, a micro-SD card (2 GB or more; **everything on it is
erased**), its power supply, an HDMI screen (optional), and a **USB to
3.3 V serial adapter** (TTL level: never connect RS-232 levels or 5 V).

### 1. Get the Raspberry Pi firmware files

SIEOS does not include them (they are the Raspberry Pi's, under their own
licence). Download them from the official repository,
https://github.com/raspberrypi/firmware, folder `boot/`:

| Board | Files |
|---|---|
| Pi 4 | `start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb`, `overlays/disable-bt.dtbo` |
| Pi 5 | `bcm2712-rpi-5-b.dtb` (the Pi 5 keeps the rest of its firmware in its own EEPROM: update it first with the official tools if it is old) |

Put them in a folder, keeping `overlays/` as a sub-folder, for example
`~/rpi-fw/`. One way, for the Pi 4:

```sh
mkdir -p ~/rpi-fw/overlays && cd ~/rpi-fw
B=https://raw.githubusercontent.com/raspberrypi/firmware/master/boot
for f in start4.elf fixup4.dat bcm2711-rpi-4-b.dtb overlays/disable-bt.dtbo; do curl -fLo $f $B/$f; done
```

### 2. Build the card image

```sh
make ARCH=arm64 PI=4 sdcard FIRMWARE=~/rpi-fw     # build-arm64/sieos-pi4.img
make ARCH=arm64 PI=5 sdcard FIRMWARE=~/rpi-fw     # build-arm64/sieos-pi5.img
```

`SDDISK=8G` makes SIEOS's partition bigger (default 2G; the image is sparse).
The image holds two partitions:

1. **FAT32, 256 MiB** (the firmware reads it): `config.txt` (written for the
   board: see `tools/mksdcard.py`), `kernel8.img` (SIEOS's kernel),
   `sieos.mod` (the boot modules, loaded at 32 MiB: `initramfs sieos.mod
   0x02000000`), the firmware files, and `README.txt`.
2. **SieFS** (type `0x5E`): SIEOS's disk, made from `rootfs/` and the programs.

Without `FIRMWARE=`, the image is made anyway and lists the missing files:
copy them onto the card's first partition by hand after writing it.

The assistant's model is not on the card by default (1 GB): add
`SDMODEL=1` (and a big enough `SDDISK`, e.g. `SDDISK=4G`) to copy
`models/$(MODEL)` into `/models` on the card's SieFS partition.

### 3. Write it to the card

Find the card's device name with `lsblk` (the whole card, for example
`/dev/sdX`, not a partition), then:

```sh
sudo dd if=build-arm64/sieos-pi4.img of=/dev/sdX bs=4M conv=fsync status=progress
```

Check the device name twice: `dd` overwrites whatever it is given.

### 4. Connect the serial console

| Board | Where | Wiring |
|---|---|---|
| Pi 4 | GPIO header: pin 6 GND, pin 8 GPIO14 (TX), pin 10 GPIO15 (RX) | adapter RX to pin 8, adapter TX to pin 10, GND to pin 6 |
| Pi 5 | the 3-pin "UART" connector between the HDMI ports | the official Debug Probe cable, or an adapter wired to it |

On the PC: `screen /dev/ttyUSB0 115200` (or `picocom -b 115200 /dev/ttyUSB0`).

### 5. Boot

Insert the card and power on. On the serial console you should see:

```
SIEOS microkernel
mk: generic timer 54.0 MHz, GICv2 with ... lines
mk: 4 CPUs running
vblk: brcm,bcm2711-emmc2 at fe340000: SD card (high capacity), 4-bit, DDR50 at 50000 kHz (1.8 V), ADMA2 64-bit, interrupts; caps ... ...
vblk: SD card disk, ... MiB (SieFS partition)
atlas: screen: Raspberry Pi firmware          (Pi 5: firmware frame buffer)
atlas: no keyboard or pointer: no desktop (the serial console is the way in)
Welcome to SIEOS. This is the first start, and there is no screen: ...
```

Choose root's password and create your user on the serial console, then
log in there. `poweroff` (as root) stops the board.

## Reading the boot steps without a serial cable

The firmware shows its rainbow screen, then starts the kernel. From its very
first instruction, the kernel asks the firmware for a frame buffer and paints
**one block per boot step reached**, left to right, each block with **as many
white dots as its number** (4 dots per row). Twelve dark marks under the
row show where all twelve would go. The desktop replaces this screen once
it starts; if it never does, the blocks stay. A photo of the screen says
where the boot stopped.

| Block | Step reached | If it stops after this one |
|---|---|---|
| 1 | the kernel runs (entered from the firmware, at EL2) | dropping to EL1 failed |
| 2 | at EL1 | clearing the kernel's memory (.bss) failed |
| 3 | .bss cleared | building the first page tables failed |
| 4 | page tables built | turning the MMU on failed |
| 5 | MMU and caches on (still at the physical address) | the jump to the kernel's high address failed |
| 6 | at the kernel's high address | entering C there failed |
| 7 | C code at the high address | reading the device tree, memory or boot modules failed |
| 8 | memory, firmware areas and boot modules read | the kernel's set-up (memory, CPU) failed |
| 9 | interrupt controller (GIC-400) | the timer |
| 10 | timer | starting the other 3 CPUs (the firmware's spin table) |
| 11 | other CPUs started (or given up after 2 s each) | starting init, the first program |
| 12 | a program runs (init's first system call) | the servers or the desktop: see the serial log |

No block at all (the rainbow stays): the kernel never ran, or the firmware's
mailbox did not answer. Check that the card's boot partition holds
`kernel8.img`, `sieos.mod` and `config.txt` (with `arm_64bit=1`), and the
green ACT LED: a repeating blink pattern is the firmware's own error code.

**The red band** (bottom third): something went wrong before any program ran.

- Top left: **dots = the last step reached** (the same count as its block).
- Top right: **dots = what happened**: 1 = an exception (a fault in the
  kernel), 2 = the boot modules are missing, damaged, or overlap the
  kernel's memory, 3 = the kernel stopped on purpose (a "panic").
- Then three rows of squares, read left to right, **white = 1**, dark = 0,
  in groups of 4 (one hexadecimal digit per group):
  1. the exception class (6 bits: `100101` = a data abort in the kernel,
     `100001` = an instruction abort, `000000` = unknown);
  2. the low 24 bits of the address of the faulting instruction;
  3. (yellow) the low 24 bits of the address it touched.

**Photographing it**: the whole screen, straight on, with the blocks and the
red band readable; one photo is enough. Send it as it is.

**Testing it in QEMU**: `make ARCH=arm64 B=build-diagtest DIAG_TEST=n` builds
a kernel that faults on purpose right after step *n* (`n` = 1 to 12), then
run it on QEMU's `raspi4b` (see `make ARCH=arm64 run-pi4` for the command,
with `-kernel build-diagtest/kernel.bin`).

## What must be checked on a real Pi 4 and Pi 5

These could not be tested here (QEMU has no Pi 5, and its Pi 4 differs from
the real one in the places below). Please report what you see on the
serial console for each.

**Both boards**

1. The kernel starts at all: the first line `SIEOS microkernel` appears
   (without a serial cable: the boot-step blocks on the screen, above).
   (The firmware loads `kernel8.img` and passes the device tree and
   `sieos.mod`, loaded at 32 MiB: `initramfs sieos.mod 0x02000000` in
   `config.txt`. Not `followkernel`: the firmware puts the file right after
   the kernel's file, and the kernel's .bss, which is not in the file and is
   cleared first thing, would wipe the start of it; the kernel now checks
   for that overlap and shows it as kind 2 in the red band.)
2. RAM: the free memory reported matches the board (the firmware fills in
   the device tree's memory; on a 4/8 GB Pi 4 the kernel maps all of it).
3. The generic timer frequency (54 MHz expected) and that `sleep 1` lasts a second.
4. The colours on the screen: blue background, cyan title. If red and blue
   are swapped, the pixel order differs from QEMU's (`user/atlas/screen.c`).
5. Power off and restart (`poweroff`, `reboot` as root).

**Pi 4**

6. The serial console needs `enable_uart=1` and `dtoverlay=disable-bt`
   (written in `config.txt`; the overlay file must be in `overlays/`):
   SIEOS drives the PL011 (UART0), which Bluetooth otherwise uses.
7. The spin-table start of the 3 other CPUs (`4 CPUs running`), with the
   firmware's own start-up code (QEMU imitates it).
8. The SD card on **EMMC2** (QEMU attached it to the other controller).
   EMMC2 cannot detect the card ("broken-cd"): the driver tries it anyway.
   Its base clock comes from the controller or the firmware (mailbox,
   clock 12). Check that `vblk` reports the card and that `bench` reads
   and writes; the speed checks are 24-29 below.
9. The mailbox frame buffer at 1920 x 1080 (`hdmi_force_hotplug=1` makes
   the firmware set it up even without a screen at boot).
10. The firmware's mailbox buffer is in the first GiB (`DMA_LOW`), as it requires.

**Pi 5**

11. The debug UART (PL011 at `0x107d001000`, "serial10") with `enable_uart=1`.
12. PSCI start of the other cores (firmware's ARM trusted firmware).
13. The GIC-400 at `0x107fff9000` and the timer interrupts.
14. The SD card on `brcm,bcm2712-sdhci` (the slot at `0x1000fff000`); the
    second one (Wi-Fi) fails identification and is skipped. Its regulators
    (1.8 V signalling: always-on GPIO 3; card power: GPIO 4) are driven by
    SIEOS for UHS-I: checks 24-29.
15. The firmware's frame buffer: it must add a `simple-framebuffer` node to
    the device tree (it does when no display driver overlay is configured;
    `framebuffer_width`/`height` ask for 1920 x 1080). If the screen stays
    black, `atlas: no screen found` on the console says so.

**Ethernet (both boards; untested)**: plug a cable into a network with a
DHCP server, then use the network (e.g. `ping 10.0.0.1` or `fetch
http://example.com`): `vnet` and `netd` start on demand.

16. `vnet: card xx:xx:xx:xx:xx:xx, GENET v5 (Raspberry Pi 4), PHY 600d:84a2 at 1, interrupt 157`
    (Pi 5: `GEM r... (Raspberry Pi 5, RP1), PHY ... at 1, polled`). The MAC
    should be the one printed on the board's label / by other systems; the
    PHY identifier should be `600d:84a2` (BCM54213PE). `ffff:ffff` or
    `0000:0000` means MDIO does not reach the PHY (wrong address, or on the
    Pi 5 RP1's Ethernet clock or the PHY's reset line is off: report it).
17. Within a few seconds: `vnet: link up, 1000 Mb/s full duplex` (or 100,
    on a slower switch), then `netd: link up` and
    `netd: 192.168.x.y/24, gateway ..., DNS ...` (the DHCP lease).
18. `ping` to the gateway answers (round trip printed). On the Pi 5 the
    first answer may take up to ~10 ms longer (polling).
19. `fetch http://example.com` prints the page; `fetch https://example.com`
    too (the certificate dates cannot be checked: no clock chip).
20. Pull the cable: `vnet: link down` and `netd: link down`; plug it back:
    `link up` and a new DHCP lease.
21. Speed: `fetch -o /tmp/x http://<a machine on your network>/<a big file>`
    (several MB) and the time it takes.
22. Pi 4: if the link comes up but nothing is received or every frame is
    corrupt, the RGMII clock delays are wrong (`phy-mode` in the device
    tree; `phy_delays` in `user/vnet/vnet.c`). If frames arrive but are
    garbled only on the Pi (not the switch), report it: the cache handling
    (`dma_sync`) is the first suspect.
23. Pi 5: whether the firmware leaves RP1's Ethernet clocked and its PHY
    out of reset (if MDIO reads `ffff:ffff`, it does not; SIEOS does not
    drive RP1's clock or GPIO blocks yet).

**SD card speed (both boards)**

The line `vblk: ...: SD card (...), 4-bit, MODE at N kHz (1.8 V), ADMA2 ..., interrupts; caps A B`
says what was chosen. `caps A B` are the controller's capability registers (useful if
something is off).

24. **DMA and interrupts**: the line says `ADMA2 64-bit` (or `32-bit`) and
    `interrupts`. If it says `PIO`, the controller's capabilities (bit 19 of
    `A`) offer no ADMA2. A line `vblk: SD: an interrupt was lost, polling
    from now on` means the interrupt did not reach SIEOS: the disk still
    works, polled.
25. **The mode**: with a UHS-I card ("U1"/"U3"/"V30" on the label), expect
    `SDR104 at 100000 kHz (1.8 V)` or `SDR50` on the Pi 5 (its device tree
    allows SDR104, SDR50, DDR50), and `DDR50 at 50000 kHz (1.8 V)` or
    `SDR50` on the Pi 4 (what its controller's capabilities offer: `B`'s
    bits 0-2). An older card: `High Speed at 50000 kHz`. The Pi 4's EMMC2
    base clock may limit SDR104 to 100 MHz: that is expected.
26. **The 1.8 V switch**: Pi 4: through the firmware's GPIO expander, line
    4 (`VDD_SD_IO_SEL`, mailbox "set GPIO state" for line 132, read back
    with "get GPIO state"); card power: line 6 (`SD_PWR_ON`). Pi 5: the
    always-on GPIO block at `0x107d517c00`, line 3 (`SD_IOVDD_SEL`), card
    power line 4 (`SD_PWR_ON`). Both read from the device tree's
    `vqmmc-supply` and `vmmc-supply`. If you see `vblk: SD: the 1.8 V switch
    failed; the card is used at 3.3 V`, the regulator did not switch (or
    the card did not answer); the card is powered off and on and used at
    High Speed. Please report that line with the `caps` line.
27. **Tuning** (SDR104): if it fails, the driver uses the next slower mode
    (SDR50, then DDR50, then High Speed); no message unless reads fail.
28. **Errors under load**: `vblk: SD errors (...): SDR104 -> SDR50` means
    two transfers failed in a row and the driver slowed down; it should not
    happen with a good card. `bench 64` exercises it.
29. **Speed**: `bench 64` as root. Expected, depending on the card: High
    Speed ~18-22 MB/s; DDR50 ~35-45 MB/s; SDR50 ~40-50 MB/s; SDR104 ~60-90
    MB/s on the Pi 5 (the Pi 4's 100 MHz limit: ~45 MB/s). Writes are
    usually slower than reads (the card itself). Before this driver (PIO,
    25 MHz): at most ~12 MB/s.

Measured in emulation (`bench 16`; QEMU's SD card model moves data a byte at
a time, so these compare the driver's paths, not real cards): PIO 3.8-3.9
MiB/s on `raspi4b` and on the PCI controller; ADMA2 with interrupts 5.4-5.6
MiB/s (64- and 32-bit descriptors, standard and high-capacity cards); every
card checked clean by `fsck.siefs` afterwards.

## How the device memory is handled (DMA and caches)

The Pis' devices do not see the processors' caches (QEMU's do), so:

- **SD card**: the controller moves the data itself (ADMA2) from or into
  the disk server's 128 KiB buffer, following a descriptor table. Before a
  transfer the table and the buffer are written back (`dma_sync`); after a
  read the buffer's lines are dropped again, so the processor reads what
  the card wrote. Both are allocated in the first GiB when the machine has
  RAM there (the Pi 4's EMMC2 sees no more), and their addresses go through
  the device tree's `dma-ranges` (`fdt_bus_addr`). A controller without
  ADMA2 (the Pi 4's older one, which QEMU's `raspi4b` uses) moves the words
  by the processor ("PIO"), as before.
- **The firmware's mailbox**: its buffer is in DMA memory in the first GiB;
  before the firmware reads it, its cache lines are written to memory, and
  after it answered they are dropped (`dma_sync`), so the answer is read
  from memory.
- **DMA memory in general**: the kernel writes newly zeroed DMA pages out of
  the caches before giving them to a driver (`arch_dma_clean`), and drivers
  call `dma_sync(buffer, size)` around a device's transfers (the kernel
  lets programs clean and invalidate the data cache: `SCTLR_EL1.UCI`). A
  device that sees the caches (the device tree's `dma-coherent`) needs
  none of it; `fdt_dma_coherent()` tells which.
- **Bus addresses**: a device may see RAM at another address than the
  processor (the Pi 4's firmware: RAM at `0xC0000000`): `fdt_bus_addr()`
  translates through the device tree's `dma-ranges`.
- **Frame buffers** are mapped "normal, uncached" (`MAP_WC`): writes are
  gathered, much faster than strictly ordered device memory.

## Not yet supported on the Pis

- USB (Pi 4: the VL805 chip behind PCIe; Pi 5: the RP1 chip behind PCIe):
  keyboard, mouse, USB drives.
- Wi-Fi, Bluetooth. (Ethernet: written, untested on hardware, see above.)
- RP1's interrupts (MSI-X): the Pi 5's Ethernet is polled.
- eMMC (Compute Modules): the card identification is the SD one; eMMC's
  (CMD1, HS200) is not written. The Pi 4's Wi-Fi SDIO.
- A real-time clock (the time shown is from boot).
