# The second laptop: an Ice Lake 2-in-1

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

The user's second laptop is an Ice Lake-LP machine, most likely a Dell XPS
13 2-in-1 7390 (the exact model is not known). This page says, device by
device, what SIEOS does on it, what to look for in the boot log, and what
is left with what it would take. Nothing here has run on the machine yet:
QEMU has none of these devices, so the code was checked on the host
(`make hdmi-test`, `bt-test`, `aml-test`) and the QEMU paths it shares
(`make test`, `gui-test`, `power-test`, `audio-test`, `nvme-test`).

| Device (lspci) | What SIEOS does | Where |
|---|---|---|
| 00:1f.3 Smart Sound audio, 8086:34c8 | HD Audio mode (not the DSP): speakers, headphones, headset microphone | `user/audiod`, [audio.md](audio.md) |
| 00:15.0, 15.2, 15.3, 19.0 Serial IO I2C, 8086:34e8/34ea/34eb/34c5 | the touchpad and the touch screen (HID over I2C) | `user/i2chid`, [touchpad.md](touchpad.md) |
| 00:1e.0 Serial IO UART, 8086:34a8 | nothing (no device behind it needs one: Bluetooth is USB there) | |
| 01:00.0 SK hynix BC501 NVMe | the disk server's NVMe driver; long transfers cut at the drive's MDTS | `user/vblk/nvme.c`, [usb.md](usb.md#the-pcs-nvme-drives) |
| ACPI: battery, AC, lid, power button | acpid, through the embedded controller; Intel's HID event filter and virtual buttons | `user/acpid`, [power.md](power.md) |
| ACPI: sleep | S3 when the firmware has `\_S3`; without it (Modern Standby only) `suspend` says "not supported" | same |
| 00:12.0 Integrated Sensor Hub, 8086:34fc | not done (rotation, lid angle): see below | |
| 00:05.0 Image Signal Processor (IPU4p) | not done (a MIPI camera): see below | |
| 00:02.0 Iris Plus graphics (display version 11) | the firmware's screen (GOP); igfx drives display version 12 only | [desktop.md](desktop.md#how-it-works) |

## Sound

Ice Lake's controller is the same HD Audio as Raptor Lake's, with
Skylake's quirks (Linux: `AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE`). What
changed in audiod for it:

- **The chipset test.** audiod took any Intel ID in 0x2600-0x3AFF for an
  old ICH (no snooping, clock gating untouched): Ice Lake's 0x34c8 (and
  Gemini Lake's 0x3198, Ice Lake-N's 0x38c8) fell in. ICHs are now only
  0x26xx-0x2Fxx and 0x3Axx.
- **The DSP left out of the way.** The firmware reports class 0401 (DSP
  on) when the microphones are wired to the DSP; the HDA registers still
  work, as Linux's `snd_hda_intel` uses them with `dsp_driver=1`. If the
  processing pipe was left on (`PPCTL.GPROCEN`, the capability list at
  `LLCH`), audiod turns it off, so streams go straight to the link.
- **The link clock.** Ice Lake on, the controller has a multi-link
  capability; a firmware that left link 0 at 6 MHz (`LCTL.SCF` = 0) has it
  moved to 24 MHz, as Linux's `intel_init_lctl` does (off, SCF, on).
- **The display codec.** Intel's HDMI codec for Ice Lake (8086:280f) has
  its vendor widget at node 2 (audiod used 8 below Tiger Lake's 0x2812;
  Gemini Lake and Cannon Lake use 11): "all pins and converters" now
  reaches it. But a monitor's sink needs its ELD, which the display driver
  writes, and igfx does not drive Ice Lake's display: no sound on a monitor
  until it does (and the XPS's monitor ports are Type-C/Thunderbolt only).
- **The Realtek codec.** Dell's XPS of that age have an ALC289 or ALC299
  (Dell's "ALC3271"), of Realtek's ALC225 family. As Linux's
  `patch_realtek.c` does: the headphone amplifier at high power and the
  EAPD coefficient (`alc225_init`, `alc_fill_eapd_coef`); Dell's firmware
  leaves the headset microphone's pin (25, 0x19) "not connected", so it is
  made a headset microphone (`DELL4_MIC_NO_PRESENCE`, 0x01a1913c); the
  combo jack's mode is set through coefficients when what is plugged in
  changes: unplugged, or a headset whose microphone ring is CTIA (iPhone)
  or OMTP wired, told apart by measuring (`alc_determine_headset_type`:
  0.9 s, once per plug). Speakers and EAPD: the generic walk, as before
  (EAPD on every output pin that has it; these Dells need no GPIO).
- **Recording from the headset.** With headphones in, recording looks
  first for a jack's microphone (on any ADC: on the ALC225 family the
  jack's microphones are on ADC 8, through mixer 0x23; ADC 7 only reaches
  the digital-microphone pins 0x12/0x13), else the built-in one.

Internal microphones: on this machine they are most likely digital
microphones wired to the PCH and read by the DSP (that is why the firmware
says "audio device"; Linux picks SOF for Ice Lake when the NHLT table
lists a DMIC). Not done. What it would take:

1. The DSP's firmware, `sof-icl.ri` (Intel-signed, from `sof-bin`; about
   300 KB, its licence allows redistribution): loaded through the HDA's
   host DMA into the DSP after powering its cores (`ADSPCS`), the ROM's
   handshake (`HIPC`), the code loader stream.
2. A topology (`sof-hda-generic-2ch.tplg` or `-4ch`): the DMIC capture
   pipeline (DAI, buffers, volume, host component), parsed and sent as IPC3
   messages (`comp_new`, `pipe_new`, `comp_connect`, `pipe_complete`).
3. The DMIC's configuration blob from the ACPI `NHLT` table (clock, PDM
   decimation), set on the DAI.
4. A capture stream through the DSP's host DMA, which `rec` then reads like
   today's input stream.

About 2,500-4,000 lines in SIEOS's style, a new server or a mode of
audiod; about 6-10 agent hours of writing, and the real bottleneck: each
try needs the user to boot the laptop (a DSP that does not boot says
little), probably several rounds.

## Touchpad and touch screen

The Serial IO I2C controllers need no ID list: i2chid takes whatever
controller the ACPI tables name, checks the DesignWare core (`44570140`)
and drives it. For Ice Lake:

- **The clock.** Linux gives Ice Lake's LPSS I2C 133 MHz
  (`bxt_i2c_info`, like Tiger Lake to Raptor Lake; not 100 MHz). i2chid
  now takes the board's own SCL counts from the controller's `FMCN`
  (400 kHz) or `SSCN` (100 kHz) method, as Linux does; without them, its
  counts for 216 MHz give 250 kHz at 133 MHz, within spec.
- **Two devices.** A 2-in-1 has a touchpad and a touch screen
  (`PNP0C50`/`ACPI0C50` both): i2chid now drives up to two, each with its
  controller, GPIO interrupt (or polling) and reports. Two on one
  controller: only the first.
- **The touch screen** reaches the desktop as an absolute pointer
  (`INJ_ABS`, as a USB tablet, which the console and Atlas already take):
  its first finger's place, the left button while touching (tap = click,
  drag = drag). It is switched to its multi-touch mode (input mode 2) when
  it has the feature. Nothing to add in Atlas. Not done: several fingers
  (pinch, two-finger scroll on the screen), the pen, rotation (the touch
  coordinates are the panel's: right only while the screen is landscape).

## Power

- **S3 or not.** Ice Lake laptops are often "Modern Standby" only: the
  FADT's `LOW_POWER_S0_IDLE` flag, and no `\_S3`. acpid logs which; with
  no `\_S3`, `suspend` (and the lid's "sleep", which Settings then does
  not offer, nor the Deskbar's Sleep) answers "not supported" at once,
  before `_PTS` or anything else is touched. `make power-test` checks it
  (QEMU with `PIIX4_PM.disable_s3=1`).
- **Buttons.** Modern Standby laptops often send the power button through
  Intel's HID event filter (`INT33D5`: Notify 0xCE) or virtual buttons
  (`INT33D6`: 0xC0); acpid enables them (`HDSM(1)`, `BTNL`; `VBDL`) and
  powers off on either, like the fixed button. Their other events are
  logged: 0xCC tablet mode, 0xCD laptop mode (folding the 2-in-1).
- **Battery, AC, lid**: the embedded controller's path, unchanged.

s2idle (future work): with no S3, sleeping means suspend-to-idle: every
device in D3 (PCI power management, the drivers stopped), the LPS0
device's `_DSM` (`PNP0D80`, Intel's and Microsoft's UUIDs: display off,
entry, then exit, display on), the CPUs in their deepest C-state (MWAIT
C10) with the timer stopped, woken by the SCI (the EC, the power button,
the lid). The PMC's SLP_S0 residency counter says whether the chipset
really slept. About 500-900 lines (kernel idle path, acpid, the drivers'
D3); 2-4 agent hours, and hardware rounds with the user to reach real
SLP_S0 residency (the bottleneck).

## The disk

The BC501 (1c5c:1327) has no entry in Linux's NVMe quirk table (SK
hynix's are the PC400 and BC901, write-zeroes, which vblk does not use,
and the P31, bogus namespace IDs). One fix all the same: vblk refused a
drive whose MDTS (largest transfer) was below 128 KiB; it now cuts each
request at the drive's MDTS (`vblk: NVMe: at most N KiB per command` in
the log). `make nvme-test` runs QEMU's drive with MDTS 3 (32 KiB) to keep
that path tested.

## Not done

- **Integrated Sensor Hub** (accelerometers: rotation in tablet mode; the
  lid angle; ambient light). What it takes: the ISH's IPC (doorbells,
  `ISH_COMM`, the firmware already loaded by the BIOS on Ice Lake), the
  ISHTP bus (HBM: version, client enumeration, connect, flow control), the
  ISHTP HID client (report descriptors per sensor, get/set feature
  reports), then the HID sensor usages (accelerometer 3D, its power and
  report interval properties) and a rotation decision for Atlas, which
  would also have to rotate its picture and the touch coordinates. Linux:
  `intel-ish-ipc` + `ishtp` + `ishtp-hid` + `hid-sensor-hub` + the
  accelerometer driver, several thousand lines; in SIEOS's style about
  1,500-2,000 lines, well above this round's 600: left documented. The
  tablet/laptop mode itself is already known (the virtual-button events).
- **The camera** (00:05.0, IPU4p): on the XPS 13 2-in-1 7390 the camera is
  a MIPI sensor on Intel's IPU, not USB video (the other laptop's UVC
  camera works through the USB server). Linux itself has no mainline
  driver for Ice Lake's IPU4 (Intel's was never upstreamed; the IPU3/IPU6
  ones do not cover it), and the IPU needs Intel's signed firmware and a
  closed image-processing library to give a usable picture. Not feasible
  here. If `lsusb` on the machine shows a camera, it is UVC and works.
- **The display**: Iris Plus is display version 11, which igfx does not
  set up (combo PHYs A/B, Type-C ports through the TC subsystem and the
  Thunderbolt controller): the firmware's screen stays, at the firmware's
  mode. Hence no brightness control from Settings and no monitor sound.
- The thermal and power controller, ME, SPI, LPC, DRAM controller: nothing
  to do (skipped, as on the other laptop).

## On the laptop: what to look for

Boot from the USB key, then read the log (serial console or log viewer):

1. Sound (`audiod:` lines, when something first plays, or `vol`):
   `HDA 8086:34c8 at 00:1f.3, DSP mode (audio device class)` (or `HDA
   mode`), perhaps `the DSP's processing pipe was on: off` and `link clock
   from 6 MHz to 24 MHz`, then `codecs on the link: 5` (bit 0 the Realtek,
   bit 2 the display codec; `1` if the display codec is not powered),
   `Realtek ALC299 on a Dell: pin 25 is the headset's microphone` (or
   ALC289), `codec 0 (10ec:0299): speaker or line out pin .., headphone pin
   33, DAC ..; ADC .. <- pin ..`. `play` a WAV: the speakers. Plug a
   headset in, play again: `headset jack: CTIA (iPhone) wiring, its
   microphone on` (or OMTP), the sound in the headset, the speakers quiet;
   `rec x.wav 5` records the headset's microphone (`ADC 8 <- pin 25` is
   not logged again; `play x.wav` to hear it).
2. Input (`i2chid:` lines): two `aml: \_SB_.PCI0.I2Cn.xxxx NAME: I2C
   address 0x.. at 400000 Hz on ...` lines; per device `controller ...:
   PCI 00:15.x, id 8086:34e8` (34ea, 34eb, 34c5), `DesignWare type
   44570140`, `SCL high .., low .., SDA hold ..: the firmware's FMCN`
   (or `ours`), `HID vvvv:pppp ...`, then `touch screen: report ..,
   1 buttons, absolute; ...` for the screen and `mouse: ...; touchpad:
   report .., 5 fingers, mode report ..` for the touchpad, and `NAME ready
   (touch screen), interrupts` / `NAME ready (touchpad), interrupts`. A
   touch on the screen moves the pointer there and clicks.
3. Power (`acpid:` lines): `\_S3: sleep type ...` or `\_S3: none: no S3`;
   `FADT: low-power S0 idle (Modern Standby), no S3: suspend is not
   available ...`; `Intel HID events: \_SB_....`, `Intel virtual buttons:
   ...`; battery, AC, lid as on the other laptop. Fold the screen back:
   `Intel buttons: event cc: tablet mode` (or `Intel HID: ...`). `suspend`:
   `suspend: not supported` at once if there is no S3. The power button:
   `power button: power off`.
4. Disk (`vblk:` lines): `NVMe 01:00.0: BC501 NVMe SK hynix ...`, perhaps
   `NVMe: at most N KiB per command`, the namespace, and the SieFS
   partition if one was made on it.
