# Bluetooth

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

One server, `btd` (`user/btd/`), drives the controller and holds the whole
host stack: HCI, L2CAP, pairing (classic SSP, LE SMP), keyboards and mice
(classic HID, HID over GATT), headphones and speakers (A2DP: AVDTP, SBC;
the sound from `audiod`; AVRCP: their buttons, absolute volume), file
transfer (OBEX Object Push over RFCOMM, both ways, received files only
if the user accepts), with SDP for what each side offers. Keyboard and mouse
reports go to the console through the same code as USB ones
(`user/lib/hidrep.c`). Settings has a Bluetooth section; `/bin/bt` does
the same from a shell.

## The controllers

| Machine | Controller | Transport | Firmware |
|---|---|---|---|
| the laptop | Intel AX210 (USB 8087:0032, full speed) | the usb server's pipe, port `bthci` (`user/usb/btusb.c`): commands on the control endpoint, events on interrupt IN, ACL on bulk | `ibt-0041-0041.sfi` + `.ddc` from linux-firmware, in `/lib/firmware` (`make usb`) |
| the second laptop (Ice Lake) | Intel AX201, the CNVi Wi-Fi's companion (USB 8087:0026; the AX200's 8087:0029 the same way) | the same pipe, on the chipset's xHCI (00:14.0) | `ibt-19-0-4.sfi` + `.ddc` (every `ibt-19-*` name in linux-firmware is this file), `make usb` |
| Raspberry Pi 4 | CYW43455 | the **mini UART** on GPIO 30-33 (ALT5, RTS/CTS), H4; BT_ON: firmware GPIO expander line 0 | `BCM4345C0.hcd` (RPi-Distro bluez-firmware), `/lib/firmware/brcm` (`make sdcard`) |
| Raspberry Pi 5 | CYW43455 | the BCM2712 UART at `0x107d50c000` (16550, auto RTS/CTS, 96 MHz) on GPIO 24-27; BT_ON: GPIO 29 | the same `.hcd` |

The firmware files are fetched once at pinned commits and checked against
`tools/bt-firmware.sha256`.

- **Intel** (as Linux's `btintel.c`, "TLV" boot): Intel Read Version
  (`0xFC05 0xFF`) gives CNVi/CNVr (the file name), the image type and the
  secure-boot engine; in the bootloader the file's CSS header, key and
  signature (RSA or ECDSA, by the engine) then its command stream in
  4-byte-aligned pieces go as Secure Send (`0xFC09`, on the bulk endpoint);
  the "download done" vendor event; Intel Reset (`0xFC01`) with the boot
  address found in the file; the "booted" vendor event; the DDC settings
  (`0xFC8B`).
- **Intel, legacy boot** (the AX201, AX200, 9560: hardware variants
  0x11-0x14, as `btintel_bootloader_setup`): the same Read Version answers
  with a fixed 10-byte record (`hw_platform` 0x37) instead of TLVs: hardware
  variant, revision, firmware variant (0x06 bootloader, 0x23 operational) and
  revision, build. The file is `ibt-<variant>-<revision>-<fw revision>.sfi`
  in decimal (the AX201: `ibt-19-0-4`, which linux-firmware also installs
  under every other `ibt-19-*` name; btd falls back to it). It has the RSA
  header (128-byte CSS, 256-byte key, 256-byte signature at 388), the
  command stream from byte 644; then the same download-done event, Intel
  Reset, booted event and DDC. (Linux also reads the boot parameters,
  `0xFC0D`, to log them: skipped.)
- **Pi 4: the mini UART, not the PL011.** Linux puts Bluetooth on the PL011
  (UART0), but SIEOS's serial console is the PL011 on GPIO 14/15
  (`disable-bt`); GPIO 30-33 also offer the mini UART with flow control
  (Linux's `miniuart-bt` layout). Its clock is the VPU's, so `config.txt`
  pins it (`core_freq_min=500`); the speed is the device tree's `max-speed`
  for that node (230400).
- **Broadcom** (as `btbcm.c`): reset, version, the line raised to its top
  speed (`0xFC18`), Download Minidriver (`0xFC2E`), each `.hcd` record, 250 ms
  (the patched firmware restarts at 115200), reset, top speed again.

Then for all: address, buffer sizes (classic and LE), every event, Secure
Simple Pairing, LE host, extended inquiry, a laptop's class (with the
"capturing", "object transfer" and "audio" service bits), the name
"SIEOS", Secure Connections. Page scan stays on so bonded classic devices
can come back.

## The stack (`user/btd`)

| File | What |
|---|---|
| `btd.c` | the server: port `bt`, the transports, firmware, set-up, `/etc/bluetooth.keys` (root only, 0600) |
| `uart.c` | the Pis' UART and the chip's power |
| `hci.c` | commands (credit-counted, queued), events, ACL fragmented and flow-controlled (Number Of Completed Packets), devices, inquiry and LE scan, connections, classic pairing |
| `l2cap.c` | basic-mode channels, signalling (connect, configure the MTU, disconnect, info, echo), LE parameter updates |
| `smp.c` | LE pairing as central: Secure Connections (just works, numeric comparison, passkey shown) and legacy (just works, passkey) for old devices |
| `input.c` | classic HID (PSM 0x11/0x13, boot protocol) and HID over GATT (a minimal ATT client) |
| `sdp.c` | SDP server (our two records) and client (one query at a time) |
| `a2dp.c` | A2DP source: AVDTP signalling, the media channel, pacing |
| `sbc.c` | the SBC encoder (fixed point) |
| `rfcomm.c` | RFCOMM: one multiplexer, one data channel, credit flow control |
| `obex.c` | OBEX Object Push, sending and receiving (asking first) |
| `avrcp.c` | AVRCP over AVCTP: a headset's buttons, absolute volume |
| `lib/btcrypt.c` | SMP's f4, f5, f6, g2, c1, s1 on AES-CMAC/AES-128, P-256 in SMP's byte order |

**Pairing.** Our IO capability is display + yes/no.
- Classic: the controller does SSP's arithmetic (P-256, f1-f3, g); btd
  answers its questions: no link key yet, our IO capability, the 6 digits
  to compare (just works with a device with no display nor buttons), or the
  passkey to type on a keyboard; the link key is kept, encryption turned on,
  the HID channels opened.
- LE: SMP with LE Secure Connections (P-256 from `lib/pubkey.c`, now
  constant time: masked reductions and a Montgomery ladder), else legacy
  pairing; the LTK (with EDIV/Rand for legacy) is kept.
- Bonded devices: classic ones page us (accepted, we become central, the
  link key proves them); LE ones are in the controller's accept list with
  one pending LE Create Connection, encrypted with the kept LTK on connect.

**Keyboards and mice.** Classic HID asks for the boot protocol (report 1:
keyboard, report 2: mouse with a wheel byte), which needs no SDP. HOGP:
Find By Type Value (the HID service), Find Information (every attribute's
type: report values, the report map, client configurations, report
references), the report map (Read Blob for the rest), each report
reference, notifications on; each notification is a report (its id put
first when the map has ids). `hidrep.c` now also reads keyboards (boot
layout or a keyboard collection in the map), shared with USB.

### The L2CAP channel API (for stage 2)

```c
typedef struct { void (*open)(l2ch_t *); void (*recv)(l2ch_t *, const uint8_t *, int); void (*close)(l2ch_t *); } l2ops_t;
void    l2_listen(uint16_t psm, const l2ops_t *ops);            /* incoming, bonded devices only */
l2ch_t *l2_open(conn_t *c, uint16_t psm, const l2ops_t *ops);   /* outgoing */
int     l2_send(l2ch_t *h, const void *p, int n);               /* one frame, <= h->rmtu */
void    l2_close(l2ch_t *h);
```

A channel is open once both directions are configured (our MTU 1017);
`h->priv` is the user's. SDP (PSM 1), AVDTP (0x19) and RFCOMM (3) use
these calls (AVCTP: 0x17); all run under btd's one lock (the receiver thread, client
requests, the 1 s tick and the sound threads take it). Outgoing frames are
fragmented and wait for controller buffers in `hci.c`. When a link drops,
all its channels are marked closed before their users hear of it, so
nobody sends on the dead link.

When a classic link is encrypted, `hci.c` starts what the device is for:
a file waiting to be sent to it (OBEX), A2DP for an audio device (class
of device "audio"), HID for the rest that we connected (not phones nor
computers).

## SDP

The server answers Service Search, Service Attribute and Service Search
Attribute requests over four records: A2DP source (handle 0x10001: L2CAP
PSM 0x19, AVDTP 1.3, A2DP 1.3, "player"), Object Push (0x10002: RFCOMM
channel 9, OBEX, any format), AVRCP target (0x10003: 110C, category 1,
"player") and controller (0x10004: 110E and 110F, category 2,
"amplifier": the volume), both L2CAP PSM 0x17, AVCTP 1.3, AVRCP 1.4 (the
first version with absolute volume). An answer longer than the asker allows is
cut, with a 2-byte continuation (the offset). Patterns may use 16, 32 or
128-bit UUIDs (on the Bluetooth base).

The client asks one UUID, every attribute, follows continuations, then
walks the data elements once: a uint right after a UUID is that UUID's
value (0x0100: the L2CAP PSM, 0x0019: AVDTP's version, 0x0003: the RFCOMM
channel). Logged: `bt: SDP 110b on NAME: found, PSM 19, AVDTP 103, RFCOMM channel 0, 54 bytes`.

## Headphones and speakers (A2DP)

```
  play ──AUDIO_WRITE──► audiod ──AUDIO_WRITE (w[1]: the volume), from where it landed──► btd, port "btaudio": pcm_ring (32 KiB, 170 ms)
  btd's pump, when the media clock says ──► SBC ──► RTP ──► the L2CAP media channel ──► headphones
```

- **Connecting.** `bt connect ADDR` (or Settings' Connect, or pairing an
  audio device) pages the device; once encrypted: SDP for its audio sink
  (0x110B), then AVDTP on PSM 0x19: Discover, Get Capabilities of each free
  sink endpoint until one has SBC (AAC, aptX... ones are skipped), Set
  Configuration, Open, then the second L2CAP channel (media). A bonded
  sink that comes back by itself usually does that to us (we answer as
  acceptor: our one endpoint, SEID 1); if it has not after 3 s, we start.
- **The configuration.** SBC, 48 kHz, joint stereo, 16 blocks, 8
  subbands, loudness allocation, bitpool 53 or the sink's highest if
  lower (53: 119-byte frames, 357 kbit/s). Logged: `bt: NAME: A2DP: SBC 48 kHz
  joint stereo, 16 blocks, 8 subbands, loudness; bitpool 53 (it takes 2-53),
  119 bytes a frame` and `A2DP ready: 8 SBC frames a packet (MTU ...)`.
- **Sound.** When the media channel is up, btd tells audiod (AUDIO_OUT
  with AUDIO_BT), which then sends each write on to btd's port `btaudio`
  with the volume: one copy, from audiod's ring (where the client's frames
  landed) to btd's. The first sound sends Start; packets go as the media
  clock says (each packet's samples at 48 kHz after the last), from when
  streaming began, or from now after a gap of more than 100 ms. A write
  that finds the ring full waits for room. No sound for 5 s: Suspend. A
  packet the controller has no buffer for is dropped (the radio is
  behind) rather than delaying all that follows. Headphones gone: audiod
  goes back to the speakers.
- **Volume:** in software, in the encoder (gain = volume squared, mute 0);
  with absolute volume (AVRCP, below) the encoder runs at full scale and
  the headset scales (mute stays in the encoder).
- **The encoder** (`sbc.c`, 2.8 KB): the analysis filter as the spec
  writes it (the prototype filter's 41 values, Q20; the 8x16 cosine
  matrix, Q14; 64-bit sums), scale factors, joint stereo per subband when
  mid/side costs fewer bits, the loudness bit allocation, the frame and
  its CRC-8. About 1% of one core for 48 kHz stereo. Checked against
  libsbc: 63 dB SNR at bitpool 53 on two tones, 58 dB at bitpool 35.

## Remote control (AVRCP)

```
  headset's button ──PASS THROUGH──► btd ──AUDIO_KEY (the operation id)──► audiod ──► play: held / goes on / skips / ends
  SIEOS's volume  ──AUDIO_VOL──► audiod ──AUDIO_VOL──► btd ──SetAbsoluteVolume──► headset
  headset's volume ──CHANGED──► btd ──AUDIO_VOL──► audiod (echoed back: same value, nothing sent)
```

- **The channel.** AVCTP on L2CAP PSM 0x17, one at a time: the sink
  usually opens it after A2DP; if it has not 2 s after the media channel
  is up, we do. Single packets only (ours are all under 20 bytes).
- **Target (the buttons).** PASS THROUGH play, pause, stop, next,
  previous, volume up and down are accepted (others: not implemented);
  each press (not the release) goes to audiod as a media key, sent by
  btd's tick thread outside the lock and woken for it. The key codes are
  AV/C's own (`MK_PLAY` 0x44...), so nothing is translated. Pause and stop
  also suspend the stream at once: headsets that guess the state from the
  stream then send play on the next press. UNIT INFO and SUBUNIT INFO get
  a panel; GetCapabilities: the SIG's company id, no events (no playback
  status: we do not track a player); other vendor commands are rejected.
- **Controller (absolute volume).** When the channel opens we register
  for the volume-changed event. An INTERIM answer means the headset
  scales: SIEOS's volume (0-100%, as 0-127, rounded) goes to it by
  SetAbsoluteVolume, now if known and at each change, and the encoder
  stops scaling. A CHANGED notification (its own buttons) becomes SIEOS's
  volume (AUDIO_VOL to audiod), and we register again. A refusal leaves
  the volume in software. Logged: `bt: NAME: AVRCP connected`, `AVRCP:
  absolute volume` (or `no absolute volume (code)`), `AVRCP: key 44`.
- Size: `avrcp.c` 1.9 KB of code, the two SDP records 0.2 KB.

## File transfer (OBEX Object Push)

- **RFCOMM** (PSM 3): one multiplexer and one data channel at a time.
  Frames with their FCS (CRC-8 over address and control, and the length
  for all but UIH); PN asks credit-based flow control (7 credits to start;
  we give the peer more, up to 32, in our data frames or an empty one when
  it runs low); MSC sent and answered; RPN, RLS, Test, FCon/FCoff echoed;
  other control messages get NSC. Our frames are at most the L2CAP MTU
  less 6 bytes, or what PN agreed.
- **Sending:** `bt send ADDR FILE` reads the file (as its user) and gives
  it to btd in pieces (BT_SEND, then BT_DATA of up to 8000 bytes; -EAGAIN
  while the device is not ready). btd pages the device if needed, finds
  its Object Push channel by SDP (0x1105), opens RFCOMM, OBEX CONNECT,
  then PUTs: the first with the name (UTF-16) and length, each as big as
  the device takes (its CONNECT answer; ours: 8 KiB), each waiting for its
  "continue"; the last one final; DISCONNECT. 30 s without progress: given
  up. Logged: `bt: OBEX: sending NAME (N bytes) to DEV`, `connected,
  packets up to N bytes`, `NAME sent to DEV (N bytes)`.
- **Receiving:** only a bonded, encrypted device can open our channel 9
  and push. The first PUT that carries the file's bytes (or ends it) is
  held, unanswered, and the user is asked: Atlas shows "DEV wants to send
  NAME (N KB)." under the sia strip with **Accept** and **Decline**
  (ATLAS_NOTE with w[1] = 1; the answer: BT_ACCEPT), and the log says the
  same (`bt accept` / `bt decline` as root answer too, e.g. on a machine
  with no desktop session). Accepted: the held PUT is taken and the file
  goes to the desktop user's `~/Downloads` (Atlas tells btd who is logged
  in, BT_USER; nobody: root's), created as that user, `1-NAME`,
  `2-NAME`... if taken; names lose `/`, `\`, control characters and a
  leading dot; Atlas shows "Receiving NAME from DEV..." then "Received
  NAME (Downloads).". Declined, or no answer in 30 s: "Forbidden" (0xC3),
  nothing written, "NAME: not taken.". A transfer that breaks off is
  deleted. Logged: `bt: OBEX: DEV wants to send ...`, `NAME from DEV
  accepted` / `declined`, `no answer from the user`, `into PATH (N
  bytes)`, `NAME received from DEV (N bytes)`.

## Using it

```
bt                    the controller, its address, on/off
bt scan               10 s of inquiry and LE scanning, then the list
bt list
bt pair AA:BB:CC:DD:EE:FF   (root) shows the code: "Does the device show 123456? [y/n]"
                            or "Type 123456 on the device, then Enter."
bt forget AA:BB:CC:DD:EE:FF (root)
bt connect ADDR             a bonded device: headphones then play what audiod plays
bt disconnect ADDR
bt send ADDR FILE           to a bonded phone or computer
bt accept | bt decline      (root) the file a device offers
bt on | bt off              (root; kept)
```

Settings → Bluetooth: On/Off, Scan, the devices (found, paired, in use),
Pair (the code with Yes/No, or to type), Forget, and Connect / Disconnect
for a paired classic device (no password asked). Root's password is asked
for the others unless root is logged in. Connected headphones are an
output in Settings → Sound (and `vol out`), chosen when they connect.

## Tests

- `make crypto-test`: f4, f5, f6, g2, c1, s1 and two P-256 key exchanges
  against the spec's sample data. Classic SSP's f1/f2/f3/g are not in SIEOS:
  the controller computes them.
- `make bt-test`: the stack on the host with a fake transport (342 checks):
  classic scan, pairing (passkey), link key, encryption, both HID channels,
  a key and a mouse move into CON_INJECT, flow control, a bonded device's
  return; LE advertisement, SMP Secure Connections against a responder
  written with `btcrypt.c` (the LTK compared), HOGP discovery, a
  notification moving the pointer, MTU and parameter requests, the accept
  list reconnecting the device. Stage 2: `bt connect` to bonded
  headphones, SDP for their sink (the answer in two parts, a continuation),
  AVDTP Discover (an AAC endpoint skipped, one in use ignored), Get
  Capabilities, Set Configuration (bitpool 35, the sink's highest), Open,
  the media channel, audiod told; sound put in, Start, packets paced (one
  now, the next 18.7 ms later, catching up after a gap), the RTP headers,
  each SBC frame's header and CRC, the frames decoded by a float decoder in
  the test (58 dB SNR) and by the host's libsbc if there (dlopen; 58 dB),
  Suspend after 5 s; AVRCP: our control channel after 2 s, register for
  the volume, the interim answer, SetAbsoluteVolume (50% = 0x40, 100% =
  0x7F; the same value not sent twice), the headset's change (0x20) to
  audiod as 25% and registered again, UNIT INFO, GetCapabilities, a
  rejected PDU, another profile refused; the buttons (play pressed and
  released, next, volume up: three keys; record: not implemented), pause
  suspending the stream at once; a phone asking our SDP records (Object
  Push's channel in four cut answers, Service Search, one attribute, the
  two AVRCP records), pushing a file (RFCOMM: SABM, PN, MSC, credits;
  OBEX CONNECT, the user asked and the PUT held until accepted, two
  PUTs: the file's name and bytes checked; one declined, one not
  answered in 30 s: forbidden, no file), then us sending it 3000 bytes (SDP, RFCOMM with one
  credit to start, 4 PUTs of at most 1024 bytes in frames of at most 400,
  its name in UTF-16, every byte checked, DISCONNECT, DISC).
- `make run-bt` (not run by the tests): QEMU with this PC's Intel adapter
  passed through (`-device qemu-xhci -device usb-host,vendorid=0x8087,productid=0x0032`)
  and the firmware put on `build/disk.img`. Linux loses its Bluetooth while
  it runs and QEMU needs write access to the device node under
  `/dev/bus/usb` (e.g. run QEMU with sudo, or a udev rule).

## What to check on real hardware

Every step logs a `bt:` line on the serial console / kernel log: send them.
- Laptop: `bt: adapter: USB 8087:0032`, the Intel version lines (bootloader,
  CNVi/CNVr), `loading /lib/firmware/ibt-0041-0041.sfi ... boot address 100800`,
  `firmware loaded`, `booted`, `.ddc applied`, `ready: ... address ...`.
  Then `bt scan`, `bt pair` with a keyboard (passkey) and a mouse.
- Ice Lake laptop (AX201): `usb: xHCI 8086:34ed at 00:14.0 ...` (the
  chipset's controller, not the Thunderbolt one at 00:0d.0), `bt: adapter:
  USB 8087:0026`, `bt: Intel (legacy): variant 13, revision 0, firmware
  variant 06 (bootloader) revision 4, ...` (revision 0 or 32, firmware
  revision 0-4: any `ibt-19-*` ends on `ibt-19-0-4`), `loading
  /lib/firmware/ibt-19-0-4.sfi (801412 bytes), firmware 83-7.26, boot
  address 24800`, `firmware loaded in N ms`, `booted in N ms`, the version
  line again with `firmware variant 23 (operational firmware)`,
  `ibt-19-0-4.ddc applied`, `ready: Intel ibt-19-0-4, Intel, Bluetooth 5.x`.
- Pis: the UART line (`Pi 4 mini UART at ... Hz`), `Broadcom BCM4345C0`,
  `patch loaded`, `ready`. If nothing answers: the flow-control polarity
  (Pi 4: `CNTL` = 0xCF), BT_ON, and on the Pi 5 the pin functions.

- Headphones: `bt pair` then `bt connect` (or the headphones' own
  reconnect): the log should show the SDP line, `A2DP: N free sink
  endpoints`, the configuration and bitpool, `A2DP ready`, `streaming`
  when `play x.wav` starts; `vol out` lists them. Listen for gaps
  (pacing, packets dropped for want of controller buffers) and check the
  volume. If a sink refuses: the `refuses command N (error)` line.
- Remote control: with headphones connected, `bt: NAME: AVRCP connected`
  and `AVRCP: absolute volume` (or `no absolute volume`); `play a.wav
  b.wav`, then the headset's play/pause (the sound stops and comes back
  where it was), next/previous, stop; `AVRCP: key NN` for each. Change
  `vol 30` and listen (the headset should get quieter while the encoder is
  at full scale); press the headset's volume buttons: `vol` should follow.
- Files: pair a phone (`bt pair`), send a photo to "SIEOS" from the
  phone: Atlas asks (Accept / Decline); accept, look in `~/Downloads`;
  send again and decline (the phone should say refused), and once without
  answering (30 s); `bt send PHONE FILE` the other way. The `bt: RFCOMM`
  and `bt: OBEX` lines say how far it went.

## Limits

Classic HID uses the boot protocol (SDP could now give the report map:
not done; no consumer keys, a 3-button mouse with a wheel byte). A2DP:
SBC only, 48 kHz only (audiod's rate), one sink at a time, no delay
reporting; AVRCP: no playback status nor track events, no browsing, no
fragmented AVCTP packets; the tail of
a sound shorter than a packet (under 20 ms) is not sent. OBEX: one
transfer at a time, Object Push only (no FTP, no GOEP over L2CAP, no SRM),
no "Send by Bluetooth" in the Files window. No LE privacy (bonded LE devices that use
resolvable private addresses do not come back by themselves: pair again);
no SCO/isochronous; one adapter; HCI commands time out only at start.
