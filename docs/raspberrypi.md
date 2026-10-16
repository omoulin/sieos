# SIEOS on the Raspberry Pi 4 and 5

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

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
| Ethernet | Pi 4: GENET (`user/vnet/genet.c`); Pi 5: RP1's GEM over PCIe (`user/vnet/gem.c`, polled); DHCP again when a cable is plugged in | **written, untested on hardware** (QEMU has neither): checks 16-23 below |
| Wi-Fi | the CYW43455 on SDIO (`user/wifi`, both boards): open and WPA2-PSK, a second network interface; Settings > Network, or `/bin/wifi` | **written, untested on hardware** (QEMU has no SDIO card or chip): checks 33-40 below |
| GPIO | the 40-pin header's pins through `/bin/gpiod` ([gpio.md](gpio.md)): Pi 4: the BCM2711 GPIO block; Pi 5: RP1's GPIO over PCIe (bank 0: `IO_BANK0`, `SYS_RIO0`, `PADS_BANK0`) | simulated pins tested (`make gpio-test`); both boards: **untested on hardware**: `gpio selftest`, checks 30-32 |
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

**The Wi-Fi chip's firmware** (for the CYW43455) is fetched once and copied
onto the card's SieFS partition, into `/lib/firmware/brcm/`
(`brcmfmac43455-sdio.bin`, `.clm_blob` and the nvram `.txt`), by `make
sdcard` itself. It is the Raspberry Pi's, under its own licence, from the
RPi-Distro `firmware-nonfree` repository at a pinned commit
([net.md](net.md) lists the exact files and URLs); it is not part of SIEOS.
If the fetch failed (no network at build time), Ethernet still works but
Wi-Fi reports the firmware missing; re-run `make sdcard` with a network.

The assistant's model is not on the card by default: add `SDMODEL=1` to
copy `models/$(PIMODEL)` into `/models` on the card's SieFS partition.
A Pi's card carries **SmolLM2-360M** (8-bit, 386 MB), not the PC's 1.7B
model (1 GB): the Pi 4 reads memory at 4-5 GB/s, so the 1.7B model would
write 2-4 tokens a second, the 360M one 6-9 (the 135M one is faster but
answers poorly; estimates in [llm.md](llm.md)). sia takes the first model
in `/models` when the PC's default is not there; `PIMODEL=` picks another
file of `models/`, and *Settings* can point sia at an online model
instead ([desktop.md](desktop.md#settings)).

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

**A 4-wire USB serial cable on the Pi 4** (the common USB-to-TTL kind:
black, white, green and red wires; 3.3 V logic). With the Pi off, find
pins 6, 8 and 10: the 3rd, 4th and 5th pins of the header's outer row (the
board's edge), counting from the SD card's end:

```
 outer row (board edge):  2   4  [6]  [8]  [10]  12 ...
                          5V  5V  GND  TXD  RXD
 inner row:               1   3   5    7    9   11 ...
```

| Cable wire | Usual colour | Pi pin |
|---|---|---|
| GND | black | pin 6 (GND) |
| RX (into the cable) | white | pin 8 (GPIO14, the Pi's TX) |
| TX (out of the cable) | green | pin 10 (GPIO15, the Pi's RX) |
| 5 V | red | not connected (the Pi has its own supply) |

Colours vary: go by the labels (GND, TXD, RXD, VCC) when the adapter has
them. Its logic must be 3.3 V: 5 V signals can damage the Pi's GPIO. The
card's `config.txt` already turns the port on (`enable_uart=1`,
`dtoverlay=disable-bt`).

On the PC (Debian): `sudo apt install picocom`, and
`sudo usermod -aG dialout $USER` (then log in again) to use the port
without sudo. Then `picocom -b 115200 /dev/ttyUSB0` (or
`screen /dev/ttyUSB0 115200`); Ctrl-A Ctrl-X leaves picocom. `dmesg | tail`
names the port if it is not `ttyUSB0`. Nothing at all: swap the white and
green wires (harmless); garbage: check the speed, 115200.

### 5. Boot

Insert the card and power on. On the serial console you should see:

```
SIEOS microkernel
mk: generic timer 54.0 MHz, GICv2 with ... lines
mk: 4 CPUs running
vblk: brcm,bcm2711-emmc2 at fe340000: SD card (high capacity), 4-bit, DDR50 at 50000 kHz (1.8 V), ADMA2 64-bit, interrupts; caps ... ...
vblk: SD card disk, ... MiB (SieFS partition)
usb: Pi 4 PCIe link up (status ...)            (Pi 4: SIEOS starts the PCIe bridge itself)
usb: xHCI VL805 xHCI (Raspberry Pi 4), ... ports, interrupts
atlas: screen: Raspberry Pi firmware          (Pi 5: firmware frame buffer)
wifi: CYW43455 (chip 4345 rev ...), ... KiB RAM  (the Wi-Fi chip; then "ready, MAC ...")
usb: port ...: ... keyboard/mouse             (one line per keyboard or mouse plugged in)
```

The desktop starts with or without a keyboard and mouse; one plugged in
later works at once. Without any at boot, the first start (root's
password, your user) is offered on the serial console, and the screen
takes it over as soon as a keyboard or mouse is plugged in. `poweroff`
(as root) stops the board.

## The console on a GFX HAT, without a serial cable

With a [Pimoroni GFX HAT](https://shop.pimoroni.com/products/gfx-hat) on
the Pi 4's header, its 128 x 64 screen shows the console's log: what the
serial wire would show (the kernel's messages, the servers', the text
console's prompts and output), 8 lines of 21 characters, long lines
wrapped. It follows the log as it grows; the six touch buttons go back
in it:

| Button | |
|---|---|
| up / down | a line back / forward (held: repeats, faster after 2 s) |
| minus / plus | a page back / forward |
| back | the newest lines again |
| select | the contrast, on the last row: minus / plus (or down / up) change it, back or 4 s untouched keeps it (`/etc/gfxhat.conf`); select again turns the backlight off / on |

These screens differ from one to the next: set wrong, the text reads only
when looked at from the side. Set it once with select, minus and plus.

Scrolled back, a bar on the right says where, and new lines do not move
what is shown. The backlight is phosphor green, like the desktop; a
touched button lights. The last 32 KiB of the log are kept (about 1000
screen lines), from the very start of the boot.

`/bin/gfxhat` does it: init starts it once the disk is up (the lines from
before are in the log too). It drives the HAT itself: the screen (an
ST7567) on SPI0, the backlight (SN3218) and the buttons (CAP1166) on
I2C1, setting the header's pins for them. Without a HAT (or on a machine
that is not a Pi) it ends at once and is not started again: `svc` shows
it stopped. The serial console keeps working alongside it. The text part
is tested on the development machine (`make gfxhat-test`); the HAT itself
only on a real board: if the picture is upside down or mirrored, or the
backlight is not green, say so (the screen's orientation and the LEDs'
colour order follow Pimoroni's own library).

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

**The log under the blocks**: what the serial console shows (the kernel's
lines and the programs': `vblk: ...`, `init: cannot start atlas ...`) is
written in small text below the blocks until the desktop takes the screen.
If all twelve blocks are there but no desktop appears, photograph it: its
last lines say which server stopped or failed.

**The red band** (bottom third): something went wrong before any program ran.

- Top left: **dots = the last step reached** (the same count as its block).
- Top right: **dots = what happened**: 1 = an exception (a fault in the
  kernel), 2 = the boot modules are missing, damaged, or overlap the
  kernel's memory, 3 = the kernel stopped on purpose (a "panic"), 4 = init,
  the first program, faulted before its first system call (the rows below
  are then its fault: `100100` = a data abort, `100000` = an instruction
  abort, `000000` = an undefined instruction).
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
   SIEOS drives the PL011 (UART0), which Bluetooth otherwise uses;
   Bluetooth takes the mini UART instead, with `core_freq_min=500` (its
   clock is the VPU's): checks in [bluetooth.md](bluetooth.md).
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
    `0000:0000` means MDIO does not reach the PHY. Just before it, a line
    says more: `vnet: GENET at fe580000: MAC from the device tree,
    rgmii-rxid, RGMII control ..., DMA below 4 GiB` (Pi 4), or `vnet: GEM at
    1f00100000: MAC from the device tree, rgmii-id, design ..., RP1 Ethernet
    clock ..., our memory at +1000000000` (Pi 5). On the Pi 5, if you see
    `vnet: RP1's PCIe link is down` or `... does not answer ...`, the
    firmware reset RP1: check `pciex4_reset=0` is in `config.txt`. `vnet:
    RP1's Ethernet clock was off ...` and `vnet: PHY reset ...` are normal
    (SIEOS turning them on). If no PHY answers at the device tree's address,
    `vnet: ... no PHY answers on MDIO ...` (and it tries every address):
    report the whole block.
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
23. Pi 5: SIEOS now turns RP1's Ethernet clock on and pulses the PHY's
    reset line itself. If MDIO still reads `ffff:ffff`, report the `vnet:
    GEM ...` block and the two lines before it: the clock source or the
    reset GPIO may be wrong. The Pi 5's Ethernet is polled (no MSI-X yet),
    so latency is a little higher under load.

**Wi-Fi (both boards; untested)**: the firmware files must be on the card
(`make sdcard` puts them there, see above). Settings > Network, or
`/bin/wifi`: `wifi scan`, `wifi join "<name>" <passphrase>` (as root; an
open network: no passphrase), `wifi` for the state, `wifi forget`.

33. At boot, `wifid` starts. It prints the controller and the chip:
    `wifi: mmcnr@7e300000 at ..., SDIO 4-bit high speed 50000 kHz, card
    interrupt` then `wifi: CYW43455 (chip 4345 rev ...), ... KiB RAM at
    198000`. `wifi: no Wi-Fi SDIO controller ...` means the device-tree
    node was not found; `wifi: the chip did not answer CMD5` means the
    SDIO card did not come up (pins or WL_ON); report which.
34. `wifi: firmware loaded (... bytes, nvram ... bytes)` then `wifi:
    ready, MAC ...`. `wifi: the firmware is missing ...` means the files
    are not in `/lib/firmware/brcm/` (rebuild the card with a network).
    `wifi: the chip's HT clock did not come up` means the firmware did not
    start: report the lines around it.
35. `wifi scan`: a list of networks with their signal (dBm) and security
    (open / WPA2). An empty list where you expect networks: report it.
36. `wifi join "<name>" <passphrase>` (WPA2), then `wifi` shows
    `state: connected` within a few seconds, and the console shows `wifi:
    link up`, `netd: using wifi (Wi-Fi)` and a DHCP lease. A wrong
    passphrase: it stays `joining` then `failed`; report what `wifi` says.
37. With the cable unplugged, the network goes over Wi-Fi: `ping`, `fetch
    http://example.com`, `fetch https://example.com` (no clock chip, so the
    certificate dates are not checked). Plug a cable in: netd switches to it
    (`netd: using nic0`); unplug it: back to Wi-Fi.
38. The passphrase is kept in `/etc/wifi.conf` (root only, mode 600); after
    `poweroff` and a fresh boot, the Pi rejoins the network on its own.
39. Speed over Wi-Fi: `fetch -o /tmp/x http://<a machine>/<a big file>` and
    the time. (The SDIO data path is PIO, a word at a time: expect modest
    throughput; report it so a DMA path can be judged worth writing.)
40. If a scan or a join hangs, or frames are lost, report the `wifi:` lines:
    the whole SDIO/BCDC path is untested. The desktop's Settings blocks for
    the few seconds a scan takes; that is expected.

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

**GPIO (both boards; nothing wired to the 40-pin header)**

Take any HAT and jumper wire off the header first: the check drives the
free pins high and low. Then, logged in on the serial console:

30. `gpio` lists the pins: the first line names the board,
    `Raspberry Pi 4 (BCM2711): 28 pins` or `Raspberry Pi 5 (RP1): 28 pins`
    (not `simulated`). On the Pi 5, a console line `gpiod: RP1: the PCIe
    link is down (status ...)` means the firmware reset the link to RP1:
    check that `config.txt` has `pciex4_reset=0` (the card `make sdcard`
    writes has it), and report the status value.
31. `gpio selftest`: a row per pin, then `selftest: PASS: 22 pins checked,
    0 failed, 6 skipped` (fewer checked, more skipped, if the firmware gave
    some pins a function: they say `used by a device`). Every checked pin
    should say `ok` in each column and `PASS`. Run it twice: the second
    run must pass too, and `gpio` must list the pins the same before and
    after (the `kept` column).
32. If it fails, report the whole table, the output of `gpio` before it,
    and the console's `gpiod:` lines. What a failure points to: `out 1`
    and `out 0` failing on every pin: the levels do not reach the pads or
    are not read back (Pi 5: RP1's `SYS_RIO` function, its `OE`/`OUT`
    registers or the pads' input enable); only `pull-up` or only
    `pull-down` failing: that pin's pad pull (or something is wired to
    it); a pin that should be free shown `busy`: the firmware gave it a
    function (Pi 5: its `CTRL` register's function is not `0x1f`); the
    console stopping during the check (Pi 5): a read through PCIe did not
    come back.

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

- USB on the Pi 5 (the RP1 chip behind PCIe). On the Pi 4 the PCIe bridge
  and the VL805 USB chip are started by SIEOS (as Linux's pcie-brcmstb
  does): written, untested on a board yet.
- Bluetooth: written, untested on the boards ([bluetooth.md](bluetooth.md)). (Ethernet and Wi-Fi: written, untested on hardware, see above.)
- RP1's interrupts (MSI-X): the Pi 5's Ethernet is polled.
- eMMC (Compute Modules): the card identification is the SD one; eMMC's
  (CMD1, HS200) is not written.
- Wi-Fi is open and WPA2-PSK only (no WPA3/enterprise/WEP), one network at
  a time, and its SDIO data path is PIO (no DMA): modest throughput.
- A real-time clock (the time shown is from boot).
