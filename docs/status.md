# Status

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

What works today, on each architecture, and how SIEOS got here. Last
checked 2026-10-10: every test suite below passes, zero build warnings.

## What works

| Area | x86-64 (QEMU, KVM) | AArch64: QEMU `virt` | Raspberry Pi 4 | Raspberry Pi 5 |
|---|---|---|---|---|
| Kernel: SMP, tickless, threads, quotas, FP/vector state | yes | yes (emulated) | QEMU `raspi4b`: yes | card image only, untested |
| SieFS disk | virtio, NVMe, USB key | virtio (MMIO), NVMe (PCIe) | SD card | SD card (untested) |
| Users, login, permissions | yes | yes | yes (serial) | untested |
| Supervisor, on-demand servers | yes | yes | yes | untested |
| Desktop | yes | yes (ramfb) | yes; USB keyboard and mouse (PCIe start-up untested) | untested |
| Network, TLS | yes: virtio (legacy, 1.0), e1000, e1000e; RTL8168h written, untested on the laptop | yes: virtio (MMIO, PCIe) | no driver yet | no driver yet |
| SSH client and server | yes | yes | needs the network | needs the network |
| Bluetooth (keyboards, mice, headphones, file transfer) | AX210 over USB: written, untested (`run-bt`); stack: `bt-test` | no controller | CYW43455 on the mini UART: untested | CYW43455 on the BCM2712 UART: untested |
| Sound (Intel HDA) | yes: intel-hda, ich9-intel-hda | no driver (no HDA there) | no | no |
| Assistant, local model | AVX2/VNNI | NEON (+ `sdot` if present) | NEON | NEON + `sdot` |
| Assistant, remote (OpenAI-compatible) | yes | yes | needs the network | needs the network |
| Built by sicc | yes (`cc-sieos`) | always | always | always |

Test suites: `test`, `crash-test`, `gui-test`, `gui-test-real`,
`kernel-test`, `power-test` (x86-64), `sig-test`, `posix-test`, `cxx-test`, `webdeps-test`, `sia-test`, `demand-test`, `net-test`, `net-nic-test`, `ssh-test`, `make-test`, `py-test`, `git-test`, `mail-test`, `nvme-test` (both
architectures), `audio-test` (x86-64), `pi4-test`, `cc-test`, `cc-bootstrap`, `cc-sieos`,
`cc-a64-test`, `cc-a64-bootstrap`, `siefs-test`, `crypto-test`, `bt-test`, `aml-test`, `camera-test`, `hdmi-test`, `igfx-test`,
`net-crypto-test`, `llm-test`, `llm-neon-test`. Real boards: see the
checklist in [raspberrypi.md](raspberrypi.md).

## Next

- The assistant's memory: history across sessions and restarts, with
  compaction ([sia.md](sia.md)).
- The target laptop: its drivers are written and wait for a boot from
  the USB key: the Intel display (full modeset), NVMe (passes in QEMU), the
  Realtek network card, the AX210 Wi-Fi ([net.md](net.md)), HD Audio
  (passes in QEMU, [audio.md](audio.md)), the touchpad
  ([touchpad.md](touchpad.md)), the camera and the keyboard light
  ([usb-stack.md](usb-stack.md#cameras)), Bluetooth
  ([bluetooth.md](bluetooth.md)), ACPI power: off, restart, sleep, battery,
  lid ([power.md](power.md)); then NVIDIA compute
  ([gpu-plan.md](gpu-plan.md)).
- The second laptop (an Ice Lake 2-in-1, [ice-lake.md](ice-lake.md)): its
  sound, touch screen, power and disk are written and wait for a boot;
  then, if wanted, the DSP's microphones (SOF), the sensor hub (rotation),
  s2idle and Ice Lake's display in igfx.
- A web browser: WPE WebKit on a musl-based POSIX layer, phase by phase
  ([webkit-plan.md](webkit-plan.md)).
- The Pis: USB input; running on the real boards (Ethernet and Wi-Fi are
  written but untested there, [raspberrypi.md](raspberrypi.md)).

## History

- **2026-10-09.** Microkernel, console and shell; SieFS (specification,
  core, host tools, crash tests); virtio disk and file server; users and
  Argon2id logins; the supervisor (restarts in ~1 ms); SMP and tickless
  time; the Atlas desktop; sicc, our C compiler, building all of SIEOS.
- **Night of 2026-10-09.** sicc: all of C11, optimizer, vectors; the
  language-model engine; floating point, TLS and quotas in the kernel;
  the sia assistant (local and remote); the network stack and TLS 1.3;
  the assistant panel and desktop improvements.
- **2026-10-10.** Atlas: island menus, apps, Driftwood, items on several
  islands; servers started on demand and stopped when idle; the graphical
  first start; the kernel split into a portable core and architectures;
  sicc for AArch64 (with NEON); SIEOS on AArch64 (QEMU `virt`), the
  Raspberry Pi 4 (QEMU) and card images for the Pi 4 and 5; NEON kernels
  for the engine; processor features reported by the kernel.
- **2026-10-10, later.** The map desktop (Atlas) replaced by **Stage**
  ([desktop.md](desktop.md)): projects (one per file, the Inbox),
  at most two windows, a shelf of cards, the Lens; island attributes
  migrated on first sight.
- **2026-10-12.** Stage replaced by a desktop laid out as BeOS's was, in
  SIEOS's colours ([desktop.md](desktop.md)): windows with title tabs
  (move, resize, zoom, minimize), the Deskbar (menu, apps, clock), 4
  workspaces, the sia strip, a Files window; Settings (screen size, mouse,
  sia's model, local or online).
- **2026-10-12, later.** sicc keeps NEON vectors in registers (the engine's
  kernels 1.9× faster under emulation, up to 5×; a dot-product loop from
  214 instructions to 15) and runs on SIEOS itself: `/bin/cc`, with SIEOS's
  headers and library in `/lib/sicc` ([cc.md](cc.md)); a program starts in
  its parent's current folder.
- **2026-10-15.** `/bin/make`: POSIX make with GNU make's common additions
  (pattern and suffix rules, `:=` `?=` `+=`, functions, `include`, `-n -k
  -s -B -C -f`), recipes run by `sh -c`; `/src/hello-make` and SIEOS's own
  hello, sh and make built on SIEOS by make and `/bin/cc` ([make.md](make.md));
  the shell gets `-c`, `cp`, `touch`, `rm -f`; file times never go back.
- **2026-10-15.** SieFS: several volumes (a home can be one) and
  snapshots, shared block by block with their volume, deleted ZFS-style
  through dead lists; the file server takes one of every volume each hour
  (kept a day) and lets the oldest go when the disk is full; `snap`, and
  the Snapshots button in Files ([siefs.md](siefs.md#snapshots)).
- **2026-10-15, later.** Encrypted homes: each user's home is a volume
  sealed with ChaCha20-Poly1305, its key wrapped by the user's password
  (Argon2id); it opens at login and gets a new wrapping when the password
  changes ([siefs.md](siefs.md#encryption), [accounts.md](accounts.md)).
- **2026-10-16.** SSH ([ssh.md](ssh.md)): `/bin/ssh`, `ssh-keygen` and
  `sshd`, a compact SSH-2 on SIEOS's own crypto (curve25519-sha256,
  ssh-ed25519, chacha20-poly1305@openssh.com, strict key exchange),
  interoperable with OpenSSH both ways; Ed25519 signatures (RFC 8032);
  the console's raw mode (`CON_RAW`); `make ssh-test`.
- **2026-10-16.** Python: `/bin/python` is MicroPython 1.29.0 (fetched at
  build time, checksum checked) with a SIEOS port: the prompt, scripts and
  `-c`, files and `os`, `socket` and `ssl` (TLS 1.3), `hashlib`, json, re,
  time, math and MicroPython's other modules, on both architectures (built
  by sicc on AArch64); in the desktop's menu; `make py-test`
  ([python.md](python.md)). sicc: `#include MACRO` inside an `#if` group,
  and static flexible array members with initializers (pointers included).
- **2026-10-16.** A driver for the target laptop's wired card, Realtek's
  RTL8168h/8111h (`user/vnet/rtl8168.c`, after Linux's `r8169`, polled;
  +3.3 KB to vnet); untested (QEMU has no model of it): vnet logs the chip's
  XID, the MAC, the PHY's id and the link ([net.md](net.md)).
- **2026-10-16.** The network on QEMU's other cards: Intel's e1000 and
  e1000e (the default cards of x86-64's `pc` and `q35`), and virtio 1.0 on
  PCI, also for the disk (arm64's `virt` with a plain `-nic user`); one PCI
  library for every driver (`user/lib/pci.c`). The everyday network
  commands as names of `/bin/fetch`: `ping`, `host`/`nslookup`,
  `ifconfig`, `netstat`, `traceroute`, `nc`, `wget`, `curl`
  ([net.md](net.md#programs)); `make net-nic-test`.
- **2026-10-16.** git ([git.md](git.md)): `/bin/git`, written for SIEOS
  in git's own formats (loose objects, packs and .idx v2 with deltas, thin
  packs completed, refs, index v2): init, clone, status, add, rm, reset,
  commit, log, show, diff, branch, checkout/switch, fetch, pull
  (fast-forward, or a merge when no file changed on both sides), push,
  remote, config; over git:// and smart HTTP(S) with credentials. SHA-1
  and zlib (inflate, a small deflate) in `lib/`. Host git fsck accepts what
  SIEOS pushes and the repositories it makes (`make git-test`). The Git
  window on the desktop drives `/bin/git` ([desktop.md](desktop.md#git)).
- **2026-10-16.** Mail grows to what `../mailbox` (the Linux client in
  Atlas's look) does ([desktop.md](desktop.md#mail)): several accounts
  (`[account]` sections in `~/.mail`; a one-account file reads as
  before), each with its folders, unread counts (STATUS) and connection,
  a From choice when writing, an account editor (pick, new, remove);
  self-signed certificates accepted per account, their SHA-256 shown and
  pinned (`tls_open`: that certificate only, the key still proven; a
  changed one refused); favorite folders at the top; the newest 300
  messages; attachments saved (`~/Downloads`), files attached (picked in
  Files; multipart/mixed, base64), forwarded with the message; Reply all;
  Delete into the Trash (SPECIAL-USE `\Trash` or its usual names), for
  good from it; a copy of what is sent in Sent (APPEND); `mailto:` links
  (the sia strip, `mail mailto:`); Ctrl+N/R/Shift+R/L, F5, Ctrl+Enter (con
  now tells Ctrl+Enter and Ctrl+Shift+letters apart). atlas.elf +8.8 KB
  (the window: 31 KB of code), mail.elf +13 KB. `make mail-test` covers it
  all against two host servers.
- **2026-10-16.** Mail ([desktop.md](desktop.md#mail)): a window of the
  desktop (folders, messages, reading, writing, replying, forwarding,
  deleting, the account) and `/bin/mail`, which does the talking: IMAP or
  POP3 to read, SMTP to send (AUTH PLAIN/LOGIN), always over TLS (at once
  or after STARTTLS/STLS); MIME text, quoted-printable, base64 and encoded
  words decoded. The window runs `mail -S` as the user on a console of its
  own, so the desktop links no TLS; `make mail-test` against a mail server
  on the host (`tools/mailserver.py`).
- **2026-10-16.** TLS 1.2 (RFC 5246) next to 1.3 in `user/lib/tls.c`
  ([net.md](net.md#the-library-sockets-http-tls)), for the servers that
  know no better (many mail providers, older sites): one ClientHello offers
  both, 1.3 preferred; in 1.2 only ECDHE (X25519, P-256, P-384) with
  ChaCha20-Poly1305, AES-128-GCM or AES-256-GCM, the ServerKeyExchange
  signature checked (RSA PKCS #1 v1.5 / PSS, ECDSA), the extended master
  secret, the 1.3 downgrade sentinel checked; no CBC, no static RSA, no
  renegotiation or resumption. AES-256-GCM in `lib/aead.c`, P-384 key
  exchange in `lib/pubkey.c` (also for 1.3's HelloRetryRequest). The same
  API: fetch, curl, wget, http.c, sia, mail, git and python all gain it.
  `tls.c` itself is now tested on the host against `openssl s_server` (27
  cases, `make net-crypto-test`); `net-test` fetches from 1.2-only servers,
  `mail-test` uses one. tls.o: +5 KB of code (12.9 KB).
- **2026-10-16.** Signals ([kernel.md](kernel.md#signals-ctrl-c)): Ctrl-C
  stops the running program, on the console, in the desktop's terminals
  and over SSH, and the shell gives a new prompt. `SYS_KILL` takes a signal
  and a process group, `SYS_SIGNAL` sets a function (or ignores), and
  `SYS_SIGRETURN` ends it; a caught signal wakes its thread from any wait
  with `-EINTR`, its registers (vector ones too) kept in the kernel
  meanwhile; reply tokens count calls, so an interrupted call's late answer
  goes nowhere. Each shell job is a process group the terminal knows
  (`CON_FG`); a closed window or SSH connection sends `SIGHUP`; `kill -INT
  /-TERM/-KILL/-HUP`. Python raises `KeyboardInterrupt`. The kernel's code
  grew by 1.6 KB (x86-64), the IPC round trip by 1% at most; `make sig-test`.
- **2026-10-16.** Sound ([audio.md](audio.md)): `/bin/audiod` drives Intel
  HD Audio (QEMU's intel-hda and ich9-intel-hda; Intel PCHs up to Raptor
  Lake: TCSEL, snooping, clock gating as Linux sets them): CORB/RIRB, the
  codec's widgets walked from pins to converters, one output and one input
  stream (48 kHz, 16-bit, stereo) on DMA rings that clients' frames reach
  by IPC with no copy of the server's own; volume and mute on the DACs, a
  plugged headphone silences the speaker. `play`, `rec`, `vol`; a volume
  choice in Settings; `make audio-test` checks the tone QEMU plays into a
  WAV file. audiod.o: 6.4 KB of code. Not yet run on the laptop.
- **2026-10-16, later.** HDMI/DP sound ([audio.md](audio.md#hdmi-and-displayport)):
  igfx turns display audio on for a monitor whose EDID has sound (ELD
  built from it into the HDMI codec, the transcoder's audio, HDMI mode with
  an AVI infoframe instead of DVI); audiod finds the sinks by the codec's
  "ELD valid" (Intel's codec opened by its vendor verb; DP-MST devices per
  pin), on up to two controllers (an NVIDIA GPU's HDMI function too,
  enabled if hidden: useful only when the firmware lit a screen on it).
  `vol out [N]`, a Settings "Output" row. `make hdmi-test`: the laptop's
  real codec dumps replayed, ELDs from two real EDIDs. audiod.o 10.5 KB
  (+4.1), igfx.o +1.4 KB. Not tried with a monitor yet.
- **2026-10-16.** NVMe ([usb.md](usb.md#the-pcs-nvme-drives)): the disk
  server drives NVMe SSDs (reset, Identify, one I/O queue pair; read,
  write, flush with PRP lists; polled), on both architectures; it lists
  every controller and namespace and takes the first SieFS partition found
  on one, before a USB key, never writing a drive without one. `bench` on
  QEMU's NVMe (KVM): 376-396 MiB/s written, 557-629 MiB/s read (virtio:
  217 / 213). vblk: +2.4 KB of code (x86-64), +3.6 KB (arm64); `make nvme-test`.
- **2026-10-16.** The laptop's Intel display set up by SIEOS itself
  (`user/atlas/igfx.c`, [desktop.md](desktop.md#how-it-works)): Raptor
  Lake-S UHD Graphics (display version 12), the eDP panel and up to two DP
  or HDMI monitors showing the same picture; the VBT from the ACPI
  OpRegion, EDID over DP AUX and GMBUS, CDCLK raised through pcode when a
  mode needs it, DPLLs, combo PHY voltage swing, DP link training, pipe
  scalers (any size up to the firmware's), the panel's power sequence and
  PWM backlight (a Brightness choice in Settings). Everything it does is
  logged to `/etc/igfx.log`; anything it cannot do leaves the firmware's
  screen. Not yet tried on the laptop; QEMU's paths unchanged (`gui-test`,
  `usb-test`). atlas: +17.6 KB of code (igfx.o 17.1 KB, built `-Os`).
- **2026-10-16, later.** Laptop touchpads ([touchpad.md](touchpad.md)):
  `/bin/i2chid` finds an I2C HID device in the ACPI tables (`SYS_ACPI`: the
  DSDT and SSDTs; a small AML interpreter, since Intel's reference firmware
  computes the touchpad's address, controller and pin in methods), drives
  the LPSS DesignWare I2C controller, speaks HID over I2C, switches a
  precision touchpad to its finger mode (one finger moves, two scroll, tap
  or press clicks), its reports interrupt-driven through the GPIO
  controller (`IRQ_LOW`). The report parser is shared with USB mice
  (`user/lib/hidrep.c`). Checked on the host against real tables (the
  target's platform: `ELAN06FA` at 0x15 on 00:19.1, pin 8, as Linux sees it)
  and the target's real report descriptor; `make aml-test`. Untested on the
  laptop itself. Sizes: i2chid 36 KB of code (the AML reader 13.5 KB,
  `-Os`), the kernel +0.5 KB, the USB server +2 KB (touchpad gestures).
- **2026-10-16, later.** Bluetooth ([bluetooth.md](bluetooth.md)): `btd`,
  one server for the whole host stack (HCI with command credits and ACL
  flow control, L2CAP channels, classic SSP answered for the controller,
  LE SMP Secure Connections and legacy pairing, bonds in
  `/etc/bluetooth.keys`), keyboards and mice by classic HID (boot protocol)
  and HID over GATT into the same `hidrep.c` as USB (which now reads
  keyboards too); the laptop's AX210 through a USB pipe in the usb server
  (`btusb.c`) with Intel's TLV firmware boot, the Pis' CYW43455 on a UART
  (Pi 4: the mini UART, since the PL011 is the console) with Broadcom's
  patch; `/bin/bt`; Settings' Bluetooth section. P-256 key exchange made
  constant time; AES moved to `lib/aes.c`. `make bt-test` (scripted HCI
  traces, 84 checks), SMP's functions in `crypto-test`, `make run-bt` (the
  host's adapter in QEMU, run by hand). Untested on hardware. Sizes
  (x86-64, text + data): btd 82 KB (its own code 38 KB, crypto 21 KB),
  bt 15 KB, the USB server +3 KB, Atlas +4 KB, wifid -5 KB on x86-64 (AES apart),
  i2chid +2 KB (keyboards in hidrep).
- **2026-10-17.** Bluetooth stage 2 ([bluetooth.md](bluetooth.md)):
  SDP (our A2DP source and Object Push records; a client), headphones and
  speakers (A2DP source: AVDTP as initiator and acceptor, a fixed-point SBC
  encoder at 48 kHz joint stereo, bitpool 53 or the sink's highest, RTP
  packets paced by the media clock, software volume, Suspend when idle;
  audiod's Bluetooth output, fed by one IPC copy to btd's port `btaudio`),
  file transfer (RFCOMM with credit flow control, OBEX Object Push both
  ways; `bt send`, received files in the desktop user's `~/Downloads` with
  a note in Atlas); `bt connect`/`disconnect`, Settings' Connect. Fixed:
  Atlas's ATLAS_CAM had ATLAS_BT's number (camera frames were taken for
  Bluetooth ticks). `make bt-test` 279 checks (SBC frames decoded by a test
  decoder and libsbc: 58 dB SNR). Untested on hardware. Sizes (x86-64,
  text): btd +21.7 KB (sdp 4.0, a2dp 4.3, sbc 2.8, rfcomm 2.8, obex 4.4,
  btd.c +2.3, hci +1.1), bt +1.2 KB, audiod +1.0 KB, Atlas +0.7 KB; btd's
  RAM +58 KB (the 32 KB sound ring, 8 KB OBEX packet, 8 KB request buffer).
- **2026-10-17, later.** Bluetooth remote control and a prompt for files
  ([bluetooth.md](bluetooth.md#remote-control-avrcp)): AVRCP 1.4 over AVCTP
  (PSM 0x17, two SDP records: target and controller). A headset's buttons
  (play, pause, stop, next, previous, volume) go to audiod as media keys
  (`AUDIO_KEY`, AV/C's own operation ids): pause holds the writer and
  suspends the stream, next/previous/stop reach `play` (now several
  files); absolute volume both ways (SetAbsoluteVolume, the headset's
  changes as SIEOS's volume), the encoder then at full scale. Files pushed
  by a phone wait for the user: Atlas shows Accept / Decline under the
  strip (`bt accept` / `bt decline` as root), declined or 30 s unanswered:
  "forbidden". btd's tick thread now also wakes for what goes to audiod
  and Atlas. `make bt-test` 342 checks. Untested on hardware. Sizes
  (x86-64, text): btd +3.4 KB (avrcp 1.9, obex +0.5, btd.c +0.6, sdp
  +0.2), audiod +0.6 KB, play +0.3 KB, bt +0.2 KB, Atlas +0.4 KB.
- **2026-10-16, later.** The laptop's camera and keyboard light, in the
  USB server ([usb-stack.md](usb-stack.md#cameras)). xHCI isochronous IN
  transfers (40 queued, an interrupt per 8); USB video class: the YUY2
  format at 640 x 480, probe and commit, the smallest alternate setting
  that fits, frames assembled from payload headers, converted to RGB on
  request; port `camera`, the camera powered only while open. The Camera
  window in Atlas (live picture, snapshots to `~/Pictures` as .ppm, a .ppm
  from Files shown). The Legion's ITE 8258 keyboard light (OpenRGB's
  protocol: feature report 7): off, low, high and a colour, from Settings
  or `/bin/kblight`, kept in `/etc/kblight` and restored at start; nothing
  sent unless the device and its report descriptor match exactly. QEMU has
  no camera: `make camera-test` checks the code on the host with the
  laptop's real descriptors. Untested on the laptop itself. Sizes: the USB
  server +11.2 KB of code (uvc.o 6.0 KB, kbl.o 2.4 KB, the file calls 2.5 KB, the isochronous core 0.3 KB), atlas +6.4 KB (cam.o 4.4 KB),
  `/bin/kblight` 13 KB.
- **2026-10-16, night.** ACPI power ([power.md](power.md)): power off and
  restart through the FADT (sleep type from `\_S5`, the reset register;
  QEMU's hard-coded ports gone), suspend to RAM (S3: the other CPUs parked,
  CPU 0 back through the trampoline, the TSC carried on, the CPUs started
  again; PCI headers and interrupt links restored; init restarts the device
  drivers), and `/bin/acpid`: the SCI, GPEs, the embedded controller's
  queries, Notify, battery (`_BIX`/`_BST`), AC, lid, the power button (a
  clean power off). The AML interpreter moved to `user/lib` and gained I/O,
  PCI and EC fields with writes, packages as arrays, more opcodes. The
  desktop: the battery in the Deskbar, Sleep in its menu, the lid (Settings:
  screen off or sleep), the display set up again after waking. `battery`,
  `suspend`; `make power-test`. Checked on the target platform's real tables
  on the host; untested on the laptop. Sizes: kernel.bin +3.3 KB, aml.o
  13.5 -> 17.6 KB, acpid 41 KB of code (acpid.o 10 KB), i2chid +4.4 KB (the interpreter), atlas +2.8 KB, sh +0.9 KB.
- **2026-10-17.** Waking from sleep by the lid ([power.md](power.md),
  Waking): acpid finds what can wake the machine from S3 (the lid's, the
  power button's and the controller's `_PRW`: a GPE of GPE0, S3 or
  deeper; `aml_wake()` in the interpreter), arms them before `_PTS(3)`
  (`_DSW(1, 3, 0)`, else `_PSW(1)`), leaves only those GPEs and the power
  button on (status cleared), and logs why it woke (`awake (PM1 status
  ..., GPE ..)`); after `_WAK(3)` it disarms them, turns its GPEs back on,
  runs the controller's pending queries and reads `_LID`: the desktop sees
  the lid open and the wake (a wake now always reaches it, even when
  nothing else changed) and turns the panel on. No loop: a sleep asked
  within 1 s of waking is not done, and the desktop does not sleep on a
  lid found closed in the wake's own status. The Legion's lid, power
  button and controller have no `_PRW` (Linux's `/proc/acpi/wakeup`
  agrees): its controller wakes the machine itself, told by `_PTS(3)`.
  `suspend N` (N < 3600 s): woken by the CMOS clock's alarm (RTC_EN), as
  `make power-test` now does in QEMU (no `_PRW` there); `make aml-test`
  checks the laptop's `_PRW` patterns (the Legion's `GPRW`, `_DSW`/`_PSW`,
  a GPE block, S1 only). Sizes: acpid.o +1.1 KB of code, aml.o +0.2 KB,
  sh +0.15 KB, atlas unchanged.
- **2026-10-17.** Monitor hot-plug on the laptop's Intel display
  ([desktop.md](desktop.md#how-it-works)): a DP or HDMI monitor plugged in
  after the start is lit as at the start (EDID, mode, the next free pipe,
  sound if it has speakers); one unplugged has its pipe, port, DPLL and
  sound turned off; the panel is never touched but for its DBUF share,
  which follows i915's table as pipes come and go (each pipe moved at its
  next frame once its blocks are free). Atlas reads the PCH's hot-plug pins
  (SDEISR, TC1-TC4) twice a second from a thread of its own, 200 ms of
  debounce; igfx's modeset code reused (a monitor's probe, the DBUF share
  now functions). audiod: a stream starting on a monitor that is gone goes
  back to the speakers (`vol out` already asked again). `make igfx-test`:
  a fake display engine on the host. Not yet tried on the laptop. Sizes:
  atlas.elf +1.3 KB of code (igfx.o +1.1 KB, atlas.o +0.25 KB), +1.4 KB
  of bss (five outputs instead of three); audiod.o +54 bytes.
- **2026-10-17, later.** The second laptop, an Ice Lake 2-in-1 (likely a
  Dell XPS 13 7390; [ice-lake.md](ice-lake.md)). Sound on its Smart Sound
  controller (8086:34c8) in HDA mode: Ice Lake taken for a PCH, not an old
  ICH (snooping, clock gating); the DSP's processing pipe off if left on,
  the multi-link clock from 6 to 24 MHz (as Linux's `intel_init_lctl`);
  Intel display codecs' vendor widget right from Ice Lake on (node 2);
  Realtek's ALC225 family (ALC289/299 on Dells): headphone amplifier,
  Dell's headset-microphone pin, the combo jack's mode measured (CTIA or
  OMTP); recording prefers a plugged headset's microphone on any ADC. The
  touch screen: i2chid drives two devices, the board's SCL counts from
  `FMCN`/`SSCN`, a touch screen's first finger as an absolute pointer
  (input mode 2; also for USB and Bluetooth through `hidrep.c`). acpid:
  no `\_S3` ("Modern Standby"): `suspend` says "not supported" at once
  (`power-test` boots once without S3), the FADT's low-power S0 flag
  logged; Intel's HID event filter and virtual buttons (power button,
  tablet/laptop mode logged). vblk: drives with an MDTS below 128 KiB
  (requests cut; `nvme-test` runs at MDTS 32 KiB). Not done, with what it
  would take: the DSP's digital microphones (SOF), the sensor hub, the
  IPU4 camera, s2idle, Ice Lake's display. `hdmi-test`: a Dell ALC299
  case; `bt-test`: a touch screen descriptor. Untested on the machine.
  Sizes (x86-64, text): audiod.o +2.9 KB, i2chid.o +1.8 KB, hidrep.o
  +0.3 KB, acpid.o +0.9 KB, nvme.o +0.25 KB (arm64 +0.3 KB), sh.o +14 B.
