# Bluetooth

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

One server, `btd` (`user/btd/`), drives the controller and holds the whole
host stack: HCI, L2CAP, pairing (classic SSP, LE SMP), keyboards and mice
(classic HID, HID over GATT). Their reports go to the console through the
same code as USB ones (`user/lib/hidrep.c`). Settings has a Bluetooth
section; `/bin/bt` does the same from a shell. Stage 2 (A2DP headphones
through `audiod`, OBEX file push over RFCOMM) builds on the L2CAP channel
API below.

## The controllers

| Machine | Controller | Transport | Firmware |
|---|---|---|---|
| the laptop | Intel AX210 (USB 8087:0032, full speed) | the usb server's pipe, port `bthci` (`user/usb/btusb.c`): commands on the control endpoint, events on interrupt IN, ACL on bulk | `ibt-0041-0041.sfi` + `.ddc` from linux-firmware, in `/lib/firmware` (`make usb`) |
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
Simple Pairing, LE host, extended inquiry, a laptop's class, the name
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
`h->priv` is the user's. AVDTP (PSM 0x19: a signalling channel, then a
media channel; SBC frames from audiod) and RFCOMM (PSM 3, then OBEX) are
new files in `user/btd` using these calls; all run under btd's one lock
(the receiver thread, client requests and the 1 s tick take it). Outgoing
frames are fragmented and wait for controller buffers in `hci.c`.

## Using it

```
bt                    the controller, its address, on/off
bt scan               10 s of inquiry and LE scanning, then the list
bt list
bt pair AA:BB:CC:DD:EE:FF   (root) shows the code: "Does the device show 123456? [y/n]"
                            or "Type 123456 on the device, then Enter."
bt forget AA:BB:CC:DD:EE:FF (root)
bt on | bt off              (root; kept)
```

Settings → Bluetooth: On/Off, Scan, the devices (found, paired, in use),
Pair (the code with Yes/No, or to type), Forget. Root's password is asked
unless root is logged in.

## Tests

- `make crypto-test`: f4, f5, f6, g2, c1, s1 and two P-256 key exchanges
  against the spec's sample data. Classic SSP's f1/f2/f3/g are not in SIEOS:
  the controller computes them.
- `make bt-test`: the stack on the host with a fake transport (84 checks):
  classic scan, pairing (passkey), link key, encryption, both HID channels,
  a key and a mouse move into CON_INJECT, flow control, a bonded device's
  return; LE advertisement, SMP Secure Connections against a responder
  written with `btcrypt.c` (the LTK compared), HOGP discovery, a
  notification moving the pointer, MTU and parameter requests, the accept
  list reconnecting the device.
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
- Pis: the UART line (`Pi 4 mini UART at ... Hz`), `Broadcom BCM4345C0`,
  `patch loaded`, `ready`. If nothing answers: the flow-control polarity
  (Pi 4: `CNTL` = 0xCF), BT_ON, and on the Pi 5 the pin functions.

## Limits

No SDP (classic HID uses the boot protocol: no consumer keys, a 3-button
mouse with a wheel byte); no LE privacy (bonded LE devices that use
resolvable private addresses do not come back by themselves: pair again);
no SCO/isochronous; one adapter; HCI commands time out only at start.
