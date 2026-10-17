# Laptop touchpads and touch screens (I2C HID)

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

A laptop's touchpad is rarely USB or PS/2: it is a "HID over I2C" device
on one of the chipset's I2C controllers, described only by the firmware's
ACPI tables. The driver, `/bin/i2chid` (`user/i2chid`, x86-64), started
by init at boot, finds it there, drives the controller and feeds the
console like a USB mouse (`CON_INJECT`). A 2-in-1's touch screen is the
same kind of device: i2chid drives up to two (each on its own
controller), the screen as an absolute pointer like a USB tablet
([ice-lake.md](ice-lake.md#touchpad-and-touch-screen)). With no such
device it ends at once and is not started again.

| File | What it does |
|---|---|
| `user/lib/aml.c` | a small interpreter of the ACPI tables (AML), shared with `acpid` ([power.md](power.md)): finds the `PNP0C50` devices and evaluates what they need |
| `user/i2chid/i2chid.c` | the LPSS I2C controller (DesignWare), HID over I2C, the GPIO interrupt |
| `user/lib/hidrep.c` | report descriptors and reports: mice, tablets, precision touchpads (shared with `user/usb/hid.c`) |
| `kernel/arch/x86_64/smp.c` | `SYS_ACPI`: the FADT, DSDT and SSDTs, one after the other (drivers only) |

## How it works

- **The tables.** Firmware rarely writes a touchpad as constants: Intel's
  reference code (Lenovo's Legion included) picks the model, its address
  and the controller in `_INI`, `_CRS` and `_DSM`, from variables in
  memory. So `aml.c` is a small interpreter: one pass declares every named
  object (a flat table, children found by a hash), then for each device
  whose `_HID`/`_CID` is `PNP0C50` (or `ACPI0C50`): its controller's
  `_INI`, its own statements, `_STA`, `_INI`, `_HID`, `_CRS`
  (`I2cSerialBus`: address, speed, controller; `GpioInt`: pin) and `_DSM`
  (function 1: the HID descriptor's register). It runs integers, strings,
  buffers, packages, method calls, `If`/`While` (so `Switch`), arithmetic,
  buffer fields, `ConcatenateResTemplate`, and fields of memory regions
  (read through `map_phys`; AML's writes stay in the reader, never reach
  memory). We answer `_OSI` like Windows, as Linux does (`OSYS` = 2015).
  What it cannot run is logged with its offset in the tables.
- **The controller.** The `_CRS`'s controller path (`\_SB.PC00.I2C5`), its
  `_ADR` (`0x00190001`) is the PCI device (00:19.1). Powered to D0 (PMCSR),
  LPSS reset released (BAR0 + 0x204), the DesignWare core checked
  (`0x44570140`), master at the bus's speed: the board's SCL counts from
  the controller's `FMCN` (fast) or `SSCN` (standard) method, as Linux
  takes them, else ours (right for a 216 MHz clock: 400 kHz there, 250 kHz
  at the 133 MHz of Ice Lake to Raptor Lake), transfers through the FIFOs
  with restart and stop, waiting the bytes' time, then the stop.
- **HID over I2C.** The 30-byte HID descriptor (at `_DSM`'s register, else
  1 or 0x20), SET_POWER on, RESET, the report descriptor. A precision
  touchpad (digitizer "touch pad" with fingers and an input-mode feature)
  is switched to its own mode (SET_REPORT feature, mode 3); otherwise it
  stays a mouse (it starts as one). A touch screen (digitizer "touch
  screen" collection, unless a mouse collection comes first) gets mode 2;
  its first finger is then the pointer's place, the left button down while
  it touches.
- **Gestures** (`hidrep.c`): one finger moves the pointer (relative, about
  2500 counts across the pad, then the console's speed), two fingers
  scroll (natural direction), a touch shorter than 0.2 s that hardly moved
  is a click (two fingers: right), pressing the pad clicks (with two
  fingers down: right); contacts the pad calls palms are ignored.
- **Interrupts.** The pin is a pad of Intel's GPIO controller; the
  reference firmware's `GINF`/`GADR` say where its registers are. The pad
  is taken from ACPI (host ownership), set level (inverted if active low),
  its `GPI_IE` bit on; the controller's line (its `_CRS`: 14 on Raptor
  Lake, level, active low: `IRQ_LOW`) brings each report. A watchdog checks
  once a second that no report waited unseen; without the pin, or if
  interrupts do not come, the driver reads every 8 ms.

## Tests

QEMU has no I2C touchpad on PCs, so:

- `make aml-test`: the reader on `tests/aml/laptop.aml` (made by `iasl`
  from `laptop.asl`: the Intel/Lenovo pattern, constants, a device absent),
  with the firmware's variables set three ways.
- During development, the reader also ran on real tables from
  [linux-hardware.org](https://github.com/linuxhw/ACPI) dumps (decompiled
  there, recompiled with `iasl`): Lenovo Legion Pro 7 16IRX8H (the target
  laptop's platform: `\_SB.PC00.I2C5.TPD0`, `ELAN06FA`, address 0x15,
  400 kHz, GpioInt 8, HID descriptor 1, controller `_ADR` 0x190001,
  the same as Linux shows on the target), Lenovo LOQ 15IRH8, Legion 7
  16IAX7, Acer Nitro AN515-58 (`ELAN0504` at 0x15), HP Victus 16,
  Dell XPS 15 9500, ASUS Zenbook UX5401; and QEMU's own tables at every
  boot (`i2chid: ... no I2C touchpad`).
- `hidrep.c`'s touchpad part was checked on the host with the target's
  real report descriptor (ELAN 04F3:327E: a mouse report 1, touchpad report
  0x54 with 5 fingers, mode feature 3).

## On the laptop: what to look for

Boot, then read the log (`i2chid:` lines on the serial console, or the
log viewer):

1. `ACPI: N bytes ...` then `aml: \_SB_.PC00.I2C5.TPD0 ELAN06FA: I2C
   address 0x15 at 400000 Hz on \_SB.PC00.I2C5, GpioInt 8 ...`. Any
   `aml: ... at 0x...` line is something the reader could not run.
2. `controller ...: PCI 00:19.1, id 8086:7a7d`, `DesignWare type 44570140`.
3. `HID 04f3:327e ...`, then `mouse: report 1 ...; touchpad: report 84,
   5 fingers, mode report 3`.
4. `GPIO pin 8: group .. pad .. DW0 .. DW1 ..; interrupt line 14`, then
   `ELAN06FA ready, interrupts`; if the pointer does not move, look for
   `GPIO interrupts do not arrive` (it then reads every 8 ms).

Untested on hardware: everything past the tables (the controller's clock,
the GPIO registers' offsets, the input-mode switch).
