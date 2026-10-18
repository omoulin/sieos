# The second laptop: a Microsoft Surface Pro 7

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

The user's second machine is a Surface Pro 7: Ice Lake-LP (Core
i5-1035G4 or i7-1065G7, "10th gen", 10 nm), a 12.3" 2736 x 1824 panel
(3:2, 267 dpi), a detachable Type Cover. (This page first assumed a Dell
XPS 13 7390 2-in-1; the Ice Lake facts below hold for both, the Dell
handling stays in the code, keyed to Dell's subsystem id.) It says, device
by device, what SIEOS does on the Surface, what to look for in the boot
log, and what is left with what it would take. Nothing here has run on
the machine yet: QEMU has none of these devices, so the code was checked
on the host against pretend devices (`make hid-test`, `ssam-test`,
`ipts-test`, `hdmi-test`, `aml-test`) and against the Surface Pro 7's own
ACPI tables (linux-surface's `acpidumps`, see [ACPI](#acpi-what-the-tables-say)),
and the QEMU paths it shares still pass (`make test`, `gui-test`,
`power-test`, `audio-test`).

A Surface is not built like other laptops: no embedded controller in
the ACPI sense, no battery or charger device in its tables. Microsoft's
own controller, the **Surface Aggregator Module** (SAM), owns the
battery and the charger and speaks over a UART; the keyboard is a USB
device that comes and goes with the cover; the touch screen is run by
Intel's Management Engine (IPTS), not a HID device; the buttons are bare
GPIO pins. Linux needs the linux-surface project's drivers for most of
it; SIEOS's are below.

| Device | What SIEOS does | Where |
|---|---|---|
| Type Cover (USB 045e:09c0 and kin, on the xHCI at 00:14.0) | keyboard, precision touchpad (fingers, two-finger scroll, tap, click), volume and media keys | `user/usb/hid.c`, `user/lib/hidrep.c` |
| Power and volume buttons (ACPI `MSHW0040`: three GPIO pads) | power off; volume up and down to the audio server | `user/acpid` |
| Surface Aggregator (`MSHW0084`, UART 00:1e.0, 8086:34a8) | battery and charger: percent, time left, charging; their events | `user/acpid/ssam.c` |
| Lid (ACPI `LID0`, a GPIO pad, GPE 0x4D) | open / closed to the desktop (screen off) | `user/acpid` |
| Touch screen (IPTS: iTouch MEI 00:16.4, 8086:34e4) | one finger as an absolute pointer (tap = click, drag); no pen, no multi-touch | `user/ipts` |
| 00:1f.3 Smart Sound audio, 8086:34c8, HDA mode | speakers, headphones, the built-in microphones (Realtek ALC274) | `user/audiod`, [audio.md](audio.md) |
| 00:02.0 Iris Plus graphics (display version 11) | igfx: full modeset of the eDP panel, the USB-C port's DP alternate mode | [desktop.md](desktop.md#how-it-works) |
| 00:14.3 Wi-Fi AX201 (CNVi), Bluetooth 8087:0026 on USB | written (22000 family, legacy Intel boot) | [net.md](net.md), [bluetooth.md](bluetooth.md) |
| 01:00.0 NVMe (Toshiba/Kioxia BG4, SK hynix BC501 or Samsung PM991: by model) | vblk's NVMe driver; long transfers cut at the drive's MDTS | `user/vblk/nvme.c` |
| ACPI: sleep | no `\_S3` ("Modern Standby" only): `suspend` says "not supported" | `user/acpid`, [power.md](power.md) |
| 00:15.x / 00:19.x Serial IO I2C | nothing to drive: no HID device on them (the cameras' sensors and a power monitor are there) | (i2chid ends at once) |
| 00:12.0 Integrated Sensor Hub | not done (rotation, ambient light) | |
| 00:05.0 IPU4 and three MIPI cameras (OV5693, OV8865, OV7251) | not done (not feasible) | |

## The Type Cover

The cover is a USB device on the chipset's xHCI (Linux's dmesg on a
Pro 7: `usb 3-8: New USB device found, idVendor=045e, idProduct=09c0`,
hid-multitouch on interface 0). Every collection is on that one
interface, which calls itself a boot keyboard: a keyboard (report 1, ten
keys), a mouse (2), consumer keys (3: volume, media), a precision
touchpad (4: five fingers, Windows' PTP), its configuration (input mode,
selective reporting) and Microsoft's own collections (the backlight, the
folded/tablet switch).

What was wrong: the USB server put any boot keyboard in the **boot
protocol** (8-byte keyboard reports, nothing else), so the touchpad would
never have spoken; and a keyboard described only by its report
descriptor was refused unless it also had a pointer. Now:

- The report descriptor is read for every HID interface (up to 2 KiB:
  the cover's is about 1.1 KiB). A boot keyboard that is only a keyboard
  keeps the boot protocol; one that also has a pointer, a touchpad or
  consumer keys gets the **report protocol** and is read entirely
  through its descriptor (`hidrep.c`: one layout for all its reports).
- The touchpad is switched to its fingers as Linux's hid-multitouch
  does: `SET_REPORT` (feature) on its input-mode report (value 3), then
  the "surface switch" and "button switch" report (both on). Until then
  (or if it refuses) it is a plain mouse through report 2.
- Consumer keys (the top row with Fn as the cover sets it): volume
  up/down (`AUDIO_KEY`, as a headset's buttons), mute (on/off), play/pause,
  next, previous, stop go to the audio server. Brightness keys: not yet
  (they would go to the desktop's backlight).
- Insert (usage 0x49) added to the key table.

`make hid-test` runs the USB driver and hidrep on the host with a
descriptor of the same Microsoft firmware family (the Surface Book 2's
keyboard base, from hid-tools' tests; the Pro 7 cover's own descriptor
was not available): the report protocol chosen, both `SET_REPORT`s, keys,
volume keys, the mouse report, finger moves, a tap, the pad's click.

After the first boot on the machine (2026-10-18): the pointer was "very
sluggish". Two causes, both fixed:

- **No interrupt.** The firmware leaves the xHCI's interrupt line unset
  (`usb: no interrupt line known: reading the controller's events by
  polling`), so every report waited for the next 8 ms look. The kernel now
  has MSI (`SYS_IRQ_MSI`: one of 16 vectors, the address and data for the
  device, x2APIC ids up to 255); the USB server uses the controller's MSI
  (else MSI-X, else its line): `usb: xHCI 8086:34ed at 00:14.0, 18 ports,
  interrupts: MSI`.
- **A fixed scale.** The pad's movement was a fixed 2500 counts per pad
  width. It is now by millimetres (the descriptor's physical size, logged:
  `usb: 045e:09c0 touchpad: X 0-..., Y 0-..., NN x NN mm`) with
  acceleration (libinput's flat-then-linear: 8 counts per mm up to 25
  mm/s, up to 32 at 175 mm/s): slow moves precise, the 2736-pixel screen
  in one quick stroke. The desktop's mouse speed still multiplies it.

Not done: the cover's keyboard backlight (a feature report linux-surface
sets off for suspend), the folded-back switch (Microsoft's report 0x22:
0x22 normal, 0x33 folded; would tell the desktop it is a tablet).

## The buttons

`MSHW0040` (`\_SB.MSBT`) has three `GpioInt`s in its `_CRS`: power (pin
35), volume up (246), volume down (247), active low (Linux:
`soc_button_array`, which takes the Pro 7 when the device's `_DSM` gives
a nonzero OEM platform revision; the Pro 4's `surfacepro3_button` sends
ACPI notifications instead: acpid also takes its 0xC6, power). The FADT
says there is no fixed power button.

acpid finds each pin's pad through the firmware's own GPIO tables
(`aml_gpio` in `user/lib/aml.c`: `\_SB.GINF`/`GADR`; Ice Lake's packages
have the group's first pin at index 6, Tiger Lake's on at 8; i2chid now
uses the same function) and reads the pads' RX state every 50 ms: the
power button powers off (as `poweroff`), the volume buttons send
`AUDIO_KEY` (held: again every 150 ms after 450 ms). Interrupts instead
of reading (the GPIO controller's line, as i2chid does) would save the
20 wake-ups a second; not needed for a first boot.

## Battery and charger: the Surface Aggregator

`MSHW0084` (`\_SB.SSH`): `UartSerialBusV2` on `\_SB.PCI0.UA00` (PCI
00:1e.0, Intel's LPSS UART: a DesignWare 16550 with 32-bit registers),
RTS/CTS, its speed written into the template by the device's `_INI` from
a firmware variable (`PSBR`; 3,000,000 bit/s in the fallback branch).
`user/acpid/ssam.c` (about 230 lines, after Linux's
`drivers/platform/surface/aggregator`):

- **The UART.** Powered to D0, LPSS out of reset, its clock: LPSS's 120
  MHz (Ice Lake: Linux's `spt_uart_info`) through the private fractional
  divider (register 0x200: M, N, update) set to 16 x the speed, divisor 1;
  8N1, FIFOs, automatic RTS/CTS. Read every 50 ms (1 ms while a request
  waits): with RTS/CTS, SAM holds what it sends while our FIFO is half
  full, so nothing is lost without an interrupt.
- **The Surface Serial Hub protocol.** Frames: `AA 55`, type, length,
  sequence number, CRC-16-CCITT; data frames ACKed (or NAKed, then sent
  again), ours sent again after 1 s without an ACK (3 tries), a frame
  SAM sends twice used once; commands: category, target, instance,
  request id, command id; an answer matched by its request id within 3 s.
- **At start**, as Linux: the firmware version, "D0 entry", "display on",
  then the battery's events enabled (category 2, request id 2).
- **The battery and charger** (category 2, SAM, instance 1): `_STA`
  (0x01), `_BIX` (0x02), `_BST` (0x03), `_PSR` (0x0d): the same values as
  ACPI's methods, so acpid's battery model (percent, time left or to
  full, charging) takes them unchanged and the Deskbar and `battery` show
  them. Events: 0x15 (capacities changed), 0x16 (state), 0x17 (charger):
  read again at once, and 5 s later (SAM's battery state lags behind the
  charger's; Linux's surface_battery waits the same).

`make ssam-test`: the protocol against a pretend SAM (answers as a Pro 7's
would; a lost frame, a NAK, noise, a bad CRC, an answer sent twice, an
event before the answer, silence).

Not done (Linux has them): the performance profile (SAM's thermal
category), SAM's wake GPIO (pin 170, for s2idle), "display off" / "D0
exit" before sleeping (no s2idle yet), the `_SAN` device's ACPI
operation region (the AML's own requests through SAM: a few writes at
start fail quietly).

## The lid

The lid is an ordinary ACPI lid on the Pro 7: `LID0._LID` reads a GPIO
pad (group 5 pad 13, `GGIV(0x0605000D)`); the pad raises GPE 0x4D, whose
`_L4D` inverts the pad's input (for the next change) and notifies the lid
(Linux's `surface_gpe` exists to keep that GPE on, also for waking).
acpid enables every GPE with a method, so it needed nothing new: closed,
the desktop turns the panel off (or would sleep, which needs s2idle).

## The touch screen (IPTS)

The touch controller is driven by Intel's Management Engine, which talks
to the host through a second MEI function, the "iTouch" one: PCI 00:16.4,
8086:34e4 (linux-surface adds that id to Linux's MEI driver; the normal
MEI is 00:16.0, 8086:34e0, not used). `/bin/ipts` (`user/ipts/ipts.c`,
about 330 lines, after Linux's `drivers/misc/mei` and linux-surface's
`drivers/hid/ipts`):

1. **MEI**: two rings of 32-bit slots (ours, the ME's) behind four
   registers; the reset handshake (`H_RST`, `ME_RDY`, `H_RDY`; out of
   D0i3 first if the firmware status says it can be there); messages of
   a header (ME client, host client, length, complete) and bytes, in
   pieces when longer than our ring. Polled.
2. **HBM**: version 2.0, the ME's clients (a bitmap), their properties,
   the IPTS client's UUID (3e8d0870-271a-4208-8eb5-9acb9402ae04),
   connect; flow control: we give a credit before each answer we want,
   the ME gives one before each command it takes.
3. **IPTS**: `GET_DEVICE_INFO` (the sensor, buffer sizes, interface
   "EDS" version), `SET_MODE`, `SET_MEM_WINDOW` (16 data and 16 feedback
   buffers, a doorbell, a work queue, in DMA memory), then
   `READY_FOR_DATA`: its answer means the ME filled the buffer the doorbell
   names; we read it, give it back (`FEEDBACK`; an empty one is answered
   "invalid parameters" and refilled anyway, as Linux finds), ask again.
   Every 4 ms while touched in the last 5 s, else every 50 ms.
4. **Single touch.** On the Pro 4 to 7 (EDS 1), the "event" mode is the
   ME's own single-touch HID report (id 0x40: tip, X and Y of 32767), read
   by hidrep.c as a touch screen: the console gets an absolute pointer,
   the left button while touching. Nothing to add in Atlas.

`make ipts-test`: the driver against a pretend ME (its rings with 64
slots, so the 324-byte `SET_MEM_WINDOW` goes in pieces; HBM; IPTS as a
Pro 7 answers; touches through all 16 buffers; no message without a
credit).

Not done, with what it would take:

- **The pen and several fingers** need the raw ("poll") mode, as Linux
  runs it with iptsd in user space: the ME then fills the buffers with
  the sensor's frames. (a) The pen: its reports in those frames (stylus
  v1/v2: position, pressure, tilt, buttons) are plain records: an
  absolute pointer with pressure, about 150-250 lines, 2-3 agent hours.
  (b) Fingers: heat maps (the sensor's capacitance grid, about 72 x 48 on
  this one), contacts found as blobs above a threshold, their centres,
  tracked from frame to frame, palms dropped by size: iptsd's simplest
  detector, about 400-600 lines, 4-6 agent hours. The real bottleneck for
  both: each try needs the user to boot the Surface (thresholds and the
  frame layout can only be checked on the sensor), probably several
  rounds.
- Newer Surfaces (EDS 2: Pro 8 on, the "IPTS HID" mode with the ME's own
  HID descriptor) are refused with a log line; their touch is mostly the
  same raw data.
- Rotation: the touch coordinates are the panel's landscape ones (right
  only while the screen is landscape; the sensor hub would say when not).

## Sound

The Pro 7's codec is a **Realtek ALC274** (Windows' hardware id on the
7+: `HDAUDIO\FUNC_01&VEN_10EC&DEV_0274&SUBSYS_10EC121E`: Realtek's own
subsystem id, not Microsoft's). Its NHLT table lists no digital
microphone (45 bytes, no endpoint), so the controller is in plain HDA
mode, Linux uses `snd_hda_intel` (no SOF), and the built-in microphones
are on the codec. Linux has no Surface Pro 7 fixup: the generic ALC294
family path. audiod now does the same for the ALC234/274/294:

- **Linux's `alc294_init`**: the EAPD coefficients (`alc_fill_eapd_coef`:
  0x6E = 0x0C25, 0x10 bit 15 off), the jack microphone's bias by verb
  (0x6B), and the headphone amplifier's depop once (pin muted and off,
  coefficient 0x58 started, waited for, 0x6F back to automatic).
- Then the generic walk: the speaker pin (or line out), the headphone pin
  (plugging headphones in silences the speakers: Linux users of the Pro 7
  report exactly that), the built-in microphone through an ADC, a jack
  microphone preferred while headphones are in.
- The Dell handling (ALC225 family's combo jack modes, the headset pin
  rewritten on Dells only, by subsystem id) does not touch this codec.

`make hdmi-test` checks it (a real Realtek graph renamed ALC274): the
coefficients, the depop, none of the Dell's verbs, speaker 0x14,
headphones 0x21, built-in microphone 0x12. Users report a hiss from the
Pro 7's speakers under Linux: if SIEOS has it too, the speaker amplifier's
gain is the place to look.

What changed earlier for Ice Lake's HDA controller (any Ice Lake laptop):

- **The chipset test.** audiod took any Intel ID in 0x2600-0x3AFF for an
  old ICH (no snooping, clock gating untouched): Ice Lake's 0x34c8 fell
  in. ICHs are now only 0x26xx-0x2Fxx and 0x3Axx.
- **The DSP left out of the way.** If the firmware left the processing
  pipe on (`PPCTL.GPROCEN`), audiod turns it off, so streams go straight
  to the link.
- **The link clock.** A firmware that left link 0 at 6 MHz has it moved
  to 24 MHz, as Linux's `intel_init_lctl` does.
- **The display codec.** Intel's HDMI codec for Ice Lake (8086:280f) has
  its vendor widget at node 2: "all pins and converters" reaches it.
  Sound on a monitor (the Pro 7's USB-C port in DP alternate mode) is
  untested.

## Power

- **No S3.** The FADT has `LOW_POWER_S0_IDLE` and no fixed power button
  (flags 0x20C4F5), and there is no `\_S3`: `suspend` answers "not
  supported" at once; the desktop offers no Sleep. s2idle (future work)
  would also tell SAM ("display off", "D0 exit") and arm its wake pin.
- **Power button**: see [the buttons](#the-buttons). **Battery, AC**: SAM.
  **Lid**: GPE 0x4D.
- Ice Lake laptops with Intel's HID event filter (`INT33D5`) or virtual
  buttons (`INT33D6`) have them enabled by acpid; the Pro 7 has neither.

s2idle: every device in D3, the LPS0 device's `_DSM` (`PNP0D80`, present
on the Pro 7), the CPUs in their deepest C-state with the timer stopped,
woken by the SCI, the power button's pad, SAM's pin. About 500-900
lines; 2-4 agent hours, and hardware rounds with the user to reach real
SLP_S0 residency (the bottleneck).

## ACPI: what the tables say

linux-surface keeps the Pro 7's tables (`acpidumps/surface_pro_7`). The
interpreter (`aml-test`) ran on them with the firmware's variables set by
hand (`PCHS` = 2, the sideband base `SBRG` = 0xFD000000, `PSBR` =
3,000,000), loading 5,648 objects:

- `\_SB.LID0._LID` = 1 (pad high: open), 0 with the pad's RX bit set;
  `\_GPE._L4D` inverts the pad (`write memory 0xfd6d09a2 = 0x80`) and
  notifies the lid when it changed.
- `-r '\_SB.MSBT._CRS'`: pins 35 (group 1 pad 3, 0xfd6e06b0), 246 (group
  2 pad 22, 0xfd6a0760), 247 (group 2 pad 23, 0xfd6a0770).
- `\_SB.SSH._INI` then `-r '\_SB.SSH._CRS'`: UART 3,000,000 bit/s,
  RTS/CTS, on `\_SB.PCI0.UA00` (`_ADR` 0x001E0000); its wake pin 170.
- No `PNP0C09` (embedded controller), `PNP0C0A` (battery), `ACPI0003`
  (charger), `PNP0C50` (I2C HID), `INT33D5`/`INT33D6`.

The tables are not in SIEOS's tree (they are Microsoft's firmware);
`tests/aml` keeps its hand-written ones. To repeat: `aml-test -m
0x7BBA9D18=2 -m 0x7BBA9DA4=0x00FD0000 -v '\_SB.LID0._LID' -r
'\_SB.MSBT._CRS' dsdt.dat` (the DSDT out of `acpidump.out`).

## The disk

By model the Pro 7 has a Toshiba/Kioxia BG4, an SK hynix BC501 or a
Samsung PM991, all plain NVMe for vblk. One fix all the same (for the
BC501): vblk refused a drive whose MDTS (largest transfer) was below 128
KiB; it now cuts each request at the drive's MDTS (`vblk: NVMe: at most
N KiB per command`). `make nvme-test` runs QEMU's drive with MDTS 3
(32 KiB) to keep that path tested.

## Not done

- **Integrated Sensor Hub** (accelerometers: rotation; ambient light).
  What it takes: the ISH's IPC, the ISHTP bus (HBM-like: version, client
  enumeration, connect, flow control: close to ipts's MEI code), the
  ISHTP HID client, the HID sensor usages, and a rotation decision for
  Atlas, which would also rotate its picture and the touch coordinates.
  About 1,500-2,000 lines.
- **The cameras**: three MIPI sensors on Intel's IPU4 (front OV5693, rear
  OV8865, IR OV7251). Linux has no mainline IPU4 driver, and the IPU
  needs Intel's signed firmware and a closed image library. Not feasible
  here.
- The pen, multi-touch, the cover's backlight and tablet switch,
  brightness keys, s2idle: above.

## On the Surface: what to look for

Boot from the USB key (the Pro 7 boots USB with volume-down held while
pressing power), then read the log (the log viewer, or the screen
console):

1. **Type Cover** (`usb:` lines): `usb: xHCI 8086:34ed at 00:14.0, 18
   ports, interrupts: MSI` (`polled` there: no MSI, the old sluggishness),
   `usb: port N: 045e:09c0 Surface Type
   Cover, ... speed: keyboard/mouse` (another product id is fine: the
   covers of the Pro 4 to 7 fit) and before it `usb: 045e:09c0
   interface 0: keyboard, touchpad (fingers), mouse, volume keys, its
   reports` and `usb: 045e:09c0 touchpad: X 0-..., Y 0-..., NN x NN mm`
   (about 100 x 55 mm; `size not given: assumed` means 10 cm was taken).
   Type; move a finger (the pointer moves; quick strokes go further), two fingers
   (scroll), tap (click), press the pad (click). `touchpad (as a mouse)`
   instead of `(fingers)`: the input mode was refused (the pointer still
   moves, no tap or scroll). The volume keys of the top row: the volume
   changes (`vol` shows it).
2. **Buttons** (`acpid:` lines): `Surface buttons: \_SB_.MSBT`, then
   `Surface buttons: power: pin 35, group 1 pad 3, DW0 ........ (up)`,
   the same for `volume up` (246) and `volume down` (247). Press volume
   up: `acpid: volume up`. A line saying `pressed` at start, or a button
   that does nothing: the pin's polarity or pad is wrong (the DW0 value
   says which). The power button: `power button (Surface)`, `power button:
   power off`.
3. **Battery** (`acpid:` lines): `Surface Aggregator: \_SB_.SSH_`, `SAM:
   UART \_SB.PCI0.UA00, PCI 00:1e.0 (8086:34a8), 3000000 bit/s, RTS/CTS
   (clock M/N 2/5)`, `SAM: firmware N.N.N`, `battery, AC adapter: the
   Surface Aggregator's`, then `battery NN%, charging: ...` and `AC on,
   battery in, lid open`. Unplug the charger: a new status line within a
   second (and again 5 s later). `battery` in a shell shows the same.
   Trouble: `SAM: no ACK` (the UART's speed or clock: the M/N in the log,
   the speed `assumed`?), `SAM: no answer`, `battery events not on:
   polled` (then it changes only every 30 s).
4. **Lid**: `lid: \_SB_.LID0`, `GPE 4d: _L4D`; close the cover over the
   screen: `AC on, battery in, lid closed`, the panel goes off; open: on.
5. **Touch screen** (`ipts:` lines): `MEI 00:16.4 (8086:34e4): registers
   ..., firmware status ..., H_CSR ..., ME_CSR ...`, `HBM version 2.0
   (the ME's up to ...)`, `ME clients: N; IPTS's: address A (messages up
   to M bytes)`, `sensor 045e:....,... EDS 1.x, interface 1`, `event mode
   (one finger), 16 buffers of N bytes`. Touch the screen: `touch at X, Y
   (of 32767)` for the first three touches, the pointer there, a click.
   If the pointer lands mirrored or rotated, the X/Y values in the log
   say how. Trouble: `MEI: the ME is not ready`, `HBM: no answer`,
   `IPTS: connect refused`, `no answer to N`.
6. **Sound** (`audiod:` lines, when something first plays): `HDA
   8086:34c8 at 00:1f.3, HDA mode, ...` (`DSP mode` would mean the
   firmware routes microphones to the DSP after all), `codecs on the link: 5` (the Realtek
   and the display codec), `Realtek ALC274 (subsystem 10ec....)`, `codec
   0 (10ec:0274): speaker or line out pin .., headphone pin 33, DAC ..;
   ADC .. <- pin ..`. `play` a WAV: the speakers; with headphones in: the
   headphones, the speakers quiet. `rec x.wav 5` records the built-in
   microphones.
7. **Power**: `\_S3: none: no S3`, `FADT: SCI 9, PM1 events 1800/0,
   control 1804, GPE0 1860 (128), SMI b2, flags 20c4f5`, `FADT: low-power
   S0 idle (Modern Standby), no S3: ...`; `suspend`: `suspend: not
   supported` at once.
8. **Disk** (`vblk:` lines): the NVMe drive's model, perhaps `NVMe: at
   most N KiB per command`.

## If starting the other CPUs goes wrong

Add a line `options=cpus=1` to `\SIEOS\BOOT.CFG` on the key's EFI partition
(FAT: editable from any computer): the kernel then runs on the first CPU
alone. `cpus=N` starts at most N.
