# The network

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

SIEOS's network is three programs and a library, all written for SIEOS:

```
  fetch, ping, nc, sia ...       user/lib: net.c (sockets), http.c (HTTP/1.1),
        │                                  tls.c (TLS 1.3/1.2), x509.c (certificates)
        │ NET_* messages, port "net"
        ▼
  netd  (/bin/netd)             IPv4, ARP, ICMP, UDP, TCP, DHCP client, DNS resolver
        │ NIC_SEND              ▲ NET_FRAMES / NET_LINK (received frames, the link;
        ▼                       │                        both carry the interface)
  vnet  (port "nic0")           the cable, and the clock chip      ┐  one interface
  wifid (port "wifi")           the Wi-Fi chip (the Raspberry Pis) ┘  used at a time
        │                       ▲
        ▼
  virtio-net, Intel e1000/e1000e (QEMU), Realtek RTL8168h (the laptop),
  GENET (Pi 4), RP1's GEM (Pi 5);
  the CYW43455 on SDIO (both Pis)
```

`init` starts and supervises `vnet`, `wifid` (on the Pis) and `netd` like
the other servers. netd has two interfaces and uses one at a time: the
cable (`nic0`) while its link is up, else Wi-Fi (`wifi`) while it is
associated, else the cable anyway (virtio has no link to watch). Switching
interface is a new network, so netd forgets its ARP cache and asks DHCP
again. `NET_FRAMES` and `NET_LINK` carry which interface they are from;
`nic_info_t` carries each interface's link state.

## The driver: vnet

One program serves "nic0" whatever the card: it tries its backends in turn
(`user/vnet/nic.h`) and keeps the first that finds a card.

| Backend | Card | Receiving | Status |
|---|---|---|---|
| `vnet.c` | virtio-net (QEMU, both architectures): legacy or virtio 1.0, on PCI or (arm64) MMIO | interrupt | tested |
| `e1000.c` | x86-64: Intel 82540EM (QEMU's `e1000`, the default card of the `pc` machine), 82545EM, 82574L (`e1000e`, the default of `q35`) | interrupt | tested (QEMU) |
| `rtl8168.c` | x86-64: Realtek RTL8168h/8111h (PCI 10ec:8168, revision 0x15: the target laptop's) | polled, as `gem.c` (the laptop's interrupt routing is in ACPI, which SIEOS does not read) | **untested on hardware** (QEMU has no RTL8168) |
| `genet.c` | Raspberry Pi 4: BCM2711 GENET v5, PHY BCM54213PE | interrupt (status bit 13) | **untested on hardware** (QEMU has no GENET) |
| `gem.c` | Raspberry Pi 5: RP1's Cadence GEM over PCIe, PHY BCM54213PE | polled: 0.2 ms after traffic, up to 10 ms when quiet (RP1's interrupts need MSI-X, not routed yet) | **untested on hardware** (no Pi 5 emulator) |

The Pi backends find the controller, the PHY's MDIO address (the MAC
node's `phy-handle`, else address 1, else whichever address answers), the
MAC address (the device tree's `local-mac-address`, else the firmware's
mailbox) and the RGMII clock delays (`phy-mode`) in the device tree; they
start auto-negotiation and a third thread checks the cable once a second.
When the link comes up or goes down, vnet tells netd (`NET_LINK`); netd
asks DHCP again on "up", so plugging a cable in later works. The MAC
follows the speed and duplex the PHY agreed on (10/100/1000).

The two backends were reviewed against Linux's `bcmgenet`, `macb_main` and
`clk-rp1` and the boards' device trees (none could be run here). GENET: the
MAC reset is pulsed (bit 1 of the flush register; the old code pulsed bit
0, only the receive buffer), the transmit FIFO and the hardware filters are
flushed, and the RGMII delay mode follows `phy-mode` exactly (`rgmii-rxid`
on the Pi 4). GEM: SIEOS leaves RP1 as the firmware set it up
(`pciex4_reset=0` in `config.txt`), checks RP1's PCIe link and the
controller's id before touching it, turns RP1's Ethernet clock on if the
firmware left it off (125 MHz from `pll_sys_sec`), pulses the PHY's reset
line (RP1 GPIO 32, from `phy-reset-gpios`) and parks the controller's
unused queues on a "used" descriptor. New log lines (below) say where the
MAC came from, the RGMII mode, RP1's clock and the design registers, so a
real-board report is diagnosable.

Caches: the Pis' devices do not see the CPU's caches. GENET keeps its
descriptors in its own registers, so only frame buffers need `dma_sync`.
GEM's descriptors are in our memory, four per cache line: receive
descriptors are given back a whole line at a time and a batch is sent
only once the previous one is done, so the CPU never writes a line the
controller may be writing. Short frames are padded to 60 bytes in a small
buffer of their own (the minimum Ethernet frame). Both accept all frames
("promiscuous"): netd drops what is not for us, which avoids programming
the address filters.

The virtio backend: a receive queue with 64 buffers of 2 KiB and a transmit
queue, in DMA memory, through `user/lib/virtio.c`, which hides the three
ways to reach a virtio device (the disk driver uses the same): the legacy
PCI interface (an I/O port range: QEMU's `virtio-net-pci` on a PCI bus, or
with `disable-modern=on`), legacy MMIO (arm64's `virtio-net-device`), and
**virtio 1.0 on PCI** ("modern": registers in memory, in pieces the
device's PCI capabilities point to; QEMU's `virtio-net-pci` on PCIe, behind
a root port, with `disable-legacy=on`, and on arm64's `virt`). x86-64 takes
the legacy interface when the device has both. All three take the same
queue layout; a modern card's frame header is 12 bytes instead of 10. A second thread waits
for the card's interrupt (a message), collects every frame received, and
sends them to netd in **one** message. Frames to send arrive from netd in
one message too, straight into the DMA memory the card reads (no copy of
our own); the reply waits until the card has taken them.

The e1000 backend (x86-64): Intel's gigabit cards as QEMU emulates them,
all with the same registers and "legacy" descriptors: a ring of 64 receive
buffers of 2 KiB and a ring of 256 transmit descriptors in DMA memory. The
card is reset (its MAC address comes back from its EEPROM into the first
receive-address register), filters what it receives (our address and
broadcasts), strips the CRC and pads short frames itself; an interrupt
(frames received, the ring running low, an overrun) wakes the receiving
thread, and the line is acknowledged once every frame is taken, as with
virtio. Frames to send are one descriptor each, at most 255 at a time; the
reply waits for the last one's "done" bit. The cable: `STATUS`, read once a
second like the Pis' PHYs.

The RTL8168h backend (x86-64), after Linux's `r8169` (`rtl_hw_init_8168g`,
`rtl_hw_start_8168h_1`) without what only tunes: the PHY's firmware patch
(`rtl8168h-2.fw`, which Linux also runs without) and analog settings, the
PCIe PHY's settings, EEE and power saving. The chip is named by the XID in
`TxConfig` (`(TxConfig >> 20) & 0xFCF`, 0x540/0x541 for the 8168h); any
other 10ec:8168 is logged and left alone. Setup: ASPM off in the card's
PCIe Link Control (as Linux does: L1 hangs these chips); the chip taken
back from its out-of-band firmware (receive path gated, FIFOs drained, "now
is OOB" off, MAC OCP 0xE8DE); reset; the MAC from the eFuse (ERI 0xE0/0xE4,
else the ID registers), written back to the ID registers; Linux's FIFO
sizes; the packet filter reset; rings of 64 receive buffers of 2 KiB and
256 transmit descriptors (16 bytes: "own" bit, ring end, first/last
segment, length; address); our address and broadcasts accepted. The PHY is
inside the chip, on the OCP bus (standard register r at 0xA400 + 2r):
auto-negotiation with `phy_start` (nic.h). The cable: `PHYstatus`, once a
second; vnet logs the link's speed and duplex when it changes. Polled: a
card without `rx_wait` is polled by vnet's receiving thread itself (0.2 ms
after traffic, up to 10 ms when quiet; `gem.c` too).

**PCI** (`user/lib/pci.c`, shared with the USB server and the disk's SD
controllers): x86-64 reads configuration space through the kernel and
walks every bus behind the bridges, taking the addresses and interrupt
lines the firmware gave. On arm64's `virt` there is no firmware: the
"pci-host-ecam-generic" bridge's configuration space is in memory (ECAM,
bus 0), each BAR gets an address in the bridge's memory window (device d's
BAR i at d × 16 MiB + i MiB), and the interrupt comes from the bridge's
`interrupt-map`, read from the device tree once.

vnet also reads the CMOS real-time clock once at start (ports 0x70/0x71)
and passes the date to netd: TLS needs today's date to check certificates,
and SIEOS has no separate clock driver yet.

## The server: netd

One thread, one message at a time (no locks): a client's request, a batch of
received frames, or a tick of its timer thread. Calls that must wait keep the
client's reply token and answer later. The timer thread asks netd how long to
sleep; when nothing is due, netd keeps that question unanswered, so an idle
network costs no CPU.

- **DHCP** (RFC 2131): discover, offer, request, acknowledge; renewed at half
  the lease. Connections and DNS queries made before the address is known wait.
- **ARP**: a cache of 32 addresses; packets for an unknown address wait for
  the answer (3 tries).
- **IPv4**: checksums, DF set on everything we send, fragments refused
  (the TCP MSS keeps us under the MTU).
- **ICMP**: answers pings; `NET_PING` sends ours.
- **UDP**: bound sockets, a queue of 64 datagrams each.
- **DNS** (RFC 1035): A records over UDP, CNAMEs followed by the server,
  a cache of 32 names (their TTL, 30 s to 1 h), a retry every second.
- **TCP** (RFC 9293): handshake, a 64 KiB send buffer (data stays until
  acknowledged, for retransmission) and a 256 KiB receive buffer with window
  scaling (RFC 7323); retransmission timer from the measured round trip
  (RFC 6298, Karn's rule); slow start, congestion avoidance and fast
  retransmit (RFC 5681); out-of-order segments kept (32 per connection);
  zero-window probes; FIN, RST, TIME_WAIT (10 s). Listening sockets with a
  backlog; ports below 1024 only for root.
- **Readiness** (`NET_POLL`): what a socket is ready for (data, room, a
  connection to accept, an error); if not for what the caller waits for
  (reading, writing, either), netd notes it (`SYS_NOTE`) once it is:
  libposix's `poll` ([webkit-plan.md](webkit-plan.md)).
- **Owners**: every socket belongs to the process that opened it. Every 2 s
  netd checks that owners are alive; a dead owner's connections are reset.
- **Restart**: if netd dies, init restarts it (DHCP again); programs' calls
  in progress return an error (`-EPIPE`), never hang. Resolving names,
  pings and `NET_INFO` are simply sent again.

The protocol is in `include/mk/proto.h` (`NET_*`, `NIC_*`).

## Wi-Fi: wifid

`wifid` (`user/wifi`, started at boot) is the Wi-Fi server: it serves the
port `wifi` as a second network interface for netd, keeps the chosen
network and joins it again at every start (`wifid.c`, one thread, the same
shape on both architectures). One chip driver per architecture does the
radio: `bcm.c` the Raspberry Pis' CYW43455 (below), `iwl.c` a PC's Intel
AX210 (further down). Open and WPA2-PSK networks, one at a time. On a
machine with neither chip, `wifid` ends at once and init does not start it
again. **Both drivers are untested on hardware** (QEMU has neither chip).

```
  wifi (/bin/wifi), Settings ──WIFI_*──► wifid ──► bcm.c (SDIO)  the CYW43455
                      netd ◄──NET_FRAMES / NET_LINK─┴─► iwl.c (PCIe) the AX210
```

### The Raspberry Pis' CYW43455 (`bcm.c`, arm64)

A FullMAC chip over SDIO: its own firmware scans, joins and runs the WPA2
handshake. Written from the controllers' register layout, the device trees
and Linux's `brcmfmac` and Plan 9's `ether4330`. What to check on a real
board, and what the log lines mean, is in [raspberrypi.md](raspberrypi.md).

- **The SDIO host** (`sdio.c`): an SDHCI controller (the same register
  layout as the disk's `sd.c`) driven as an SDIO host — CMD5 brings the
  card up, CMD52 reads or writes one register, CMD53 moves a block or a
  byte stream to a function; 4-bit, High Speed, the card's interrupt. It
  finds the Wi-Fi controller by its `brcm,bcm4329-fmac` child (the Pi 4's
  legacy "emmc" `mmcnr@7e300000`, the Pi 5's "sdio2" `mmc@1001100000`),
  sets the pins (Pi 4: GPIO 34-39 to ALT3; Pi 5: GPIO 30-35 to the "sd2"
  function through the BCM2712 pin controller) and powers the chip (WL_ON:
  the Pi 4's firmware GPIO expander by the mailbox, the Pi 5's always-on
  GPIO block). No 1.8 V switch and no card detection: the chip is fixed.
- **The driver** (`bcm.c`): over SDIO function 1 it reaches the chip's
  backplane (a 32 KiB sliding window onto its cores and RAM): it scans the
  cores, resets the ARM and the 802.11 core, downloads the firmware and the
  nvram into chip RAM, takes the ARM core out of reset, waits for the chip's
  "HT" clock and enables function 2 (frames). Then the **BCDC/SDPCM**
  protocol carries control commands (iovars) and data frames. Scanning is
  the firmware's `escan` (results arrive as events); joining sets the
  security (`wsec` AES, `wpa_auth` WPA2-PSK, `sup_wpa` so the **firmware's
  own supplicant** does the 4-way handshake — no handshake code on the
  host) and the passphrase (`WLC_SET_WSEC_PMK`, as a passphrase, not a
  pre-computed key), then the `join` iovar; link up/down come as events.
  Data frames go to netd as the Wi-Fi interface, DHCP and the rest the same
  as on the cable.
- **The chosen network** is kept in `/etc/wifi.conf` (root only:
  `ssid=`, `passphrase=`) and rejoined at every start.
- The SDIO bus is shared by two threads (the receiver, blocked on the
  card's interrupt, and the one serving requests); one lock serializes a
  chip transaction. Frames read while waiting for a command's reply are
  still delivered.

**The chip's firmware** is the Raspberry Pi's, not part of SIEOS (it has
its own licence), so it is not in the repository. `make ARCH=arm64 PI=4|5
sdcard` fetches it once (like the device trees) and copies it onto the
card's SieFS partition, into `/lib/firmware/brcm/`:

| On the card | From (RPi-Distro `firmware-nonfree`, commit `c91cd28`) |
|---|---|
| `brcmfmac43455-sdio.bin` | `debian/config/brcm80211/cypress/cyfmac43455-sdio-standard.bin` |
| `brcmfmac43455-sdio.clm_blob` | `debian/config/brcm80211/cypress/cyfmac43455-sdio.clm_blob` |
| `brcmfmac43455-sdio.txt` (nvram) | `debian/config/brcm80211/brcm/brcmfmac43455-sdio.txt` |

The exact URLs are pinned in the `Makefile`
(<https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/c91cd2804cf7463aab913e7247c176049f16bbd6/>...).
The **"standard"** Cypress build is used, not "minimal": it has the internal
supplicant (`idsup`/`sup_wpa`) that WPA2 needs. Version 7.45.265 (2023).

### The laptop's Intel AX210 (`iwl.c`, x86-64)

`iwl.c` drives Intel's **Wi-Fi 6E AX210** ("Typhoon Peak", PCI `8086:2725`),
on PCIe. Unlike the Pi's FullMAC chip, the AX210's firmware is "softMAC":
the firmware scans, picks rates and does CCMP in hardware with keys we give
it, but the **host** does the MLME (authentication, association) and the
WPA2 4-way handshake. Written from Linux's `iwlwifi` (the `pcie/` gen2
transport with the v2 context info and "peripheral scratch", `fw/` for the
firmware file and the PNVM, and the `mvm/` commands as Linux sends them to
this firmware — the "MLD" ones this firmware wants). Station mode, one
network, 2.4 and 5 GHz, HT at 20 MHz; no power save, no aggregation
(block-ack requests are declined), WPA2-PSK and open only (no WPA3). The AP
is enumerated on **PCI bus `0x12` behind a root port**; `lib/pci.c`'s x86-64
scan already walks every bus behind the bridges, so nothing there changed.

```
  Settings/wifi ─WIFI_*─► wifid ─► iwl.c ──TFD rings / RX buffers (PCIe DMA)──► AX210
          netd ◄─NET_FRAMES / NET_LINK─┘        firmware: scan, rates, CCMP
```

- **Bring-up**: take the device from the platform (and the management
  engine) — `prepare`, a software reset, the "APM" clocks — then build, in
  DMA memory, the **context info** and **peripheral scratch** that point the
  device at the firmware's sections, the PNVM, the RX queue and the command
  queue; load the image loader, kick the ROM and wait for the firmware's
  `ALIVE`. The firmware's `.ucode` (TLV sections: CPU1, CPU2, paging) and
  the matching `.pnvm` (the section for our SKU, from `ALIVE`, and hardware)
  come from `linux-firmware`. Then the firmware's own init (`NVM_ACCESS`),
  and the MAC address is read from the device's `MAC_STRAP`/`MAC_OTP`
  registers.
- **Transport, no interrupts**: PCI interrupt lines need ACPI routing and
  MSI needs the kernel, so one thread **polls** the RX queue (128 buffers of
  4 KiB, a free ring and a completion ring; the device writes how many it
  filled) — 50 µs after traffic, up to 4 ms when quiet, like the Pi 5's
  Ethernet. TX queues are 128 "TFDs" (256 bytes, buffer pointers): the
  command queue, and two the firmware allocates for the AP's station
  (management, TID 15; data, TID 0). Data frames go into the TX slot with
  **no copy of the payload** (netd's bytes and the SNAP header straight in).
- **Joining** (`iwl.c`'s manager thread): a passive scan (beacons arrive as
  received frames), then — for the strongest match — a PHY context, a link,
  the AP as a station, Open System authentication and an association request
  (built from the beacon's rates, HT and WMM elements), all as 802.11
  management frames we send and receive. For WPA2 the **4-way handshake runs
  on the host** (`wpa.c`): EAPOL-Key frames carried as data frames, the
  pairwise key from the passphrase (PBKDF2) and the nonces, the group key
  unwrapped; the pairwise (CCMP) and group keys are then installed in the
  chip (`SEC_KEY`), and from there the firmware encrypts and decrypts. Link
  loss (missed beacons, a deauth) is logged and the network rejoined.
- **The handshake crypto** (`wpa.c`, shared-nothing with the Pi path since
  that chip does its own): `lib/sha1.c` gained **HMAC-SHA-1** and
  **PBKDF2**, `lib/aead.c` gained **AES key unwrap** (RFC 3394; its inverse
  cipher). All host-testable — see the tests below.

**The firmware** is Intel's, not part of SIEOS (its own licence). `make usb`
fetches it once (pinned in the `Makefile`, `linux-firmware` commit
`38c1a70`) and copies it onto the USB key, into `/lib/firmware/`:
`iwlwifi-ty-a0-gf-a0-89.ucode` and `iwlwifi-ty-a0-gf-a0.pnvm` (API version
89). If they are missing, `wifid` says so and the radio stays off.

## The library: sockets, HTTP, TLS

- `net.c`: `net_resolve`, `net_connect`, `net_listen`/`net_accept`,
  `net_send`/`net_recv`, `net_udp`/`net_sendto`/`net_recvfrom`, `net_ping`,
  `net_info`.
- `http.c`: HTTP/1.1, `http://` and `https://`; Content-Length, chunked, or
  until close; up to 5 redirects for GET; `http_line` reads a body line by
  line (Server-Sent Events).
- `tls.c`: **TLS 1.3** (RFC 8446), preferred, and **TLS 1.2** (RFC 5246)
  for servers that know no better (many mail providers, older sites). One
  ClientHello offers both (supported_versions 1.3, 1.2). The same API either
  way; `tls_cipher` says which, e.g. `TLS 1.3 TLS_CHACHA20_POLY1305_SHA256`
  or `TLS 1.2 ECDHE-RSA-AES128-GCM-SHA256` (`fetch -v`, `curl -v`).
  ALPN: `http/1.1`, or `h2` first with `TLS_H2` (`tls_alpn` says which
  the server chose); `TLS_CHAIN` keeps the server's chain for the caller to
  judge (`tls_certs`; GIO's TLS for WebKit, `user/web/giotls.c`). Each
  connection has its own buffers: several threads may handshake at once.
  - 1.3: ChaCha20-Poly1305 (preferred) and AES-128-GCM, with SHA-256. Key
    exchange: X25519, or P-256/P-384 when the server asks
    (HelloRetryRequest). The server's CertificateVerify: ECDSA P-256/P-384,
    RSA-PSS. Post-handshake key updates; session tickets are ignored.
  - 1.2: only suites with forward secrecy and authenticated encryption:
    ECDHE-ECDSA and ECDHE-RSA with ChaCha20-Poly1305, AES-128-GCM-SHA256 or
    AES-256-GCM-SHA384 (no static RSA key exchange, no CBC, no SHA-1 MACs).
    ECDHE over X25519, P-256 or P-384; the ServerKeyExchange signature
    checked: RSA PKCS #1 v1.5 or RSA-PSS, ECDSA P-256/P-384, SHA-256/384/512.
    Keys from the PRF (HMAC-SHA-256 or -384) with the extended master secret
    (RFC 7627) whenever the server offers it (OpenSSL, Go, Windows and the
    big providers all do; without it the old derivation, which is safe here
    as there is no resumption nor renegotiation). Records: GCM's nonce is the
    4-byte implicit IV + 8 bytes sent (the sequence number); ChaCha20's the
    IV xor the sequence number (RFC 7905). An empty renegotiation_info (RFC
    5746); a HelloRequest gets a no_renegotiation warning; warning alerts
    are ignored; no resumption; a certificate request is answered with an
    empty Certificate.
  - Downgrade protection (RFC 8446 4.1.3): a 1.3 server made to answer 1.2
    (someone in the middle took 1.3 out of our hello) marks its random with
    `DOWNGRD`; tls.c refuses that ("downgrade to TLS 1.2 refused").
- `x509.c`: certificates in DER; the chain is followed to one of the
  trusted roots in `/etc/ssl/roots`, every signature checked (RSA PKCS #1
  v1.5, ECDSA P-256/P-384), validity dates checked against the clock chip's
  date, the name checked against the subject alternative names (`*.` for one
  label; IP addresses too), intermediate certificates must be authorities.
- The crypto (`lib/`): `sha2.c` (SHA-256/384/512, HMAC, HKDF), `aead.c`
  (Poly1305, ChaCha20-Poly1305, AES-128/256, GCM), `x25519.c`, `pubkey.c`
  (Montgomery arithmetic, P-256/P-384 ECDSA and key exchange, RSA). All plain
  integer code. TLS 1.2's PRF is in `tls.c` (over `sha2.c`'s HMAC).

**The trusted roots**: `rootfs/etc/ssl/roots` is Mozilla's list of root
certificates as published by the curl project
(<https://curl.se/ca/cacert.pem>, "Certificate data from Mozilla as of: Fri
Sep 25 03:12:01 2026 GMT", sha256 `a41b5d35...00505`, 121 roots, fetched
2026-10-09), converted to DER by `tools/netcerts.py`. To update:
`curl -o cacert.pem https://curl.se/ca/cacert.pem && python3 tools/netcerts.py cacert.pem rootfs/etc/ssl/roots`.

## Programs

- `fetch [-v] [-k] [-o FILE] URL`: a URL's body (on the screen or into a
  file); `-v` status, headers and cipher; `-k` no certificate check (tests).
  `fetch -l PORT` answers one HTTP request (a test).
- **The everyday commands** are other names of `/bin/fetch` (symbolic links,
  `NETLINKS` in the Makefile): the name it was started by says which. One
  program holds the network library once (65 KB of code on x86-64, TLS
  included); the nine commands added 2.5 KB to it, where nine programs
  would each carry the library (12 KB of code for even `hello`; 60 KB for
  `curl` and `wget`, with TLS).

  | Command | |
  |---|---|
  | `curl [-v] [-k] [-o FILE] URL` | the same as `fetch` |
  | `wget [-k] [-O FILE] URL` | into a file named as the URL's last part (else `index.html`) |
  | `ping HOST [-c COUNT]` | ICMP echo, one a second (4 by default), then how many came back |
  | `host NAME`, `nslookup NAME` | the address of a name (DNS) |
  | `ifconfig` | the interface in use (`nic0` the cable, `wifi`), its link, MAC and MTU, address/mask, gateway, DNS, the counters, the date |
  | `netstat` | every socket: TCP or UDP, our port, the other side, TCP's state, the owner's pid (`NET_SOCKETS`) |
  | `traceroute HOST` | pings with a growing time to live (1-30): each router on the way answers "time exceeded" |
  | `nc [-u] HOST PORT` | connect (TCP, or UDP): each line typed goes there, what comes back is shown; Ctrl-D ends |
  | `nc -l [-u] PORT` | the same, waiting for one connection (UDP: answers the last sender) |

  The older `fetch -i`, `fetch -p HOST [N]` and `fetch -r HOST` still work
  (`ifconfig`, `ping`, `host`).
- `ssh [-p PORT] [-i KEY] [USER@]HOST [COMMAND]`, `ssh-keygen` and the
  server `sshd` (port 22, off until `/etc/ssh/sshd_config` exists):
  SSH-2 with OpenSSH's default algorithms, on the same sockets and crypto
  ([ssh.md](ssh.md)).
- `mail` (`user/mail`): IMAP, POP3 and SMTP over TLS, for the desktop's
  Mail window and the command line ([desktop.md](desktop.md#mail)). It
  uses the sockets and `tls_connect` on a socket it opened itself, so it
  can switch to TLS after STARTTLS (IMAP, SMTP) or STLS (POP3).
- **sia's remote backend** (`user/sia/remote.c`): an OpenAI-compatible API.
  `sia config backend=remote`, `sia config url=https://api.example.com/v1`
  (or `http://10.0.2.2:8000/v1` for a server on the development machine),
  `sia config api_model=NAME`, `sia config api_key=KEY` (kept in
  `/etc/sia.key`, root only). It POSTs the whole conversation to
  `{url}/chat/completions` with `"stream": true` and turns the Server-Sent
  Events (`data: {"choices":[{"delta":{"content":...}}]}` ... `data: [DONE]`)
  into sia's pieces of answer. A server that answers all at once, or with an
  error (`{"error":{"message":...}}`), is shown too.

## In QEMU

`make run` adds `-netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-modern=on,addr=5`,
but any card QEMU gives by default works too: a plain `-nic user` is an
e1000 on x86-64's `pc` machine, an e1000e on `q35`, a virtio 1.0 card on
PCIe on arm64's `virt`. With any of them
QEMU's user network gives 10.0.2.15, the gateway 10.0.2.2 (which is also the
development machine), DNS 10.0.2.3. The card sits at PCI slot 5 so that its
interrupt line (10) is not the disk's (11): the kernel gives each interrupt
line to one driver only (see Limits).

## Tests and measurements

`make net-crypto-test`: 160 checks: FIPS 180-4, RFC 4231, 5869, 8439, 7748
and GCM test vectors (AES-128 and AES-256), P-256 and P-384 key exchange,
and 54 signatures made by the host's openssl (ECDSA P-256/P-384 with
SHA-256/384, RSA 2048/3072/4096 PKCS #1 v1.5 and PSS with SHA-256/384/512),
each also damaged and refused. It also checks the AX210 Wi-Fi's handshake
crypto (`user/wifi/wpa.c` and the new `lib/` pieces): HMAC-SHA-1 and PBKDF2
(RFC 2202, 6070), the IEEE 802.11 passphrase vector, AES key unwrap (RFC
3394), and a whole 4-way and group handshake against EAPOL frames an access
point makes apart (`tests/net/wpagen.py`: hashlib and openssl's AES), each
answer compared byte for byte and damaged or replayed frames refused. Then
`tls.c` and `x509.c` themselves, built
on the host (`tests/net/tlsclient.c`, `tests/net/tlsmk.h` standing in for
`mk.h`), against `openssl s_server` with a test CA (`tests/net/tls.py`, 27
cases): TLS 1.2 with each suite, RSA and ECDSA P-256/P-384 certificates,
X25519/P-256/P-384, PKCS #1 v1.5 (SHA-256, SHA-512), RSA-PSS and ECDSA with
another hash than the curve's, without the extended master secret, a
certificate request, the server's preference; TLS 1.3 (both ciphers,
HelloRetryRequest to P-256 and P-384, a certificate request, preferred over
1.2 when the server has both); and what must fail: a downgrade by a man in
the middle (the sentinel), a damaged ServerKeyExchange signature, a damaged
record, CBC-only and static-RSA-only servers, an unknown issuer.

`make net-test` (SMP=1/4/8): DHCP, ping, DNS, HTTP from a server on the
development machine, a 24 MiB download checked byte for byte (sha256) on the
host afterwards, a listening socket reached from the host, HTTPS to
example.com and huggingface.co, TLS 1.3 servers (openssl s_server) with a
test CA (RSA-PSS, forced AES-128-GCM, forced P-256 with HelloRetryRequest,
ECDSA P-384), TLS 1.2-only servers (RSA: ChaCha20; ECDSA P-384:
AES-256-GCM-SHA384) and certificates that must be refused (expired, another
name, unknown issuer); the commands (`ifconfig`, `ping -c`, `host`, `nslookup`,
`traceroute`, `wget`, `curl -o`, `netstat` showing a listening socket, `nc`
to a host's TCP and UDP servers and `nc -l` / `nc -l -u` reached from the
host, both ways); sia against a mock OpenAI-compatible server (tools/netmock.py)
over HTTP and HTTPS, a wrong key, a conversation; netd restarted during a
transfer.

`make net-nic-test`: the essentials (the card found, DHCP, ping, DNS,
HTTP and a 24 MiB download, HTTPS to example.com and to a test-CA server)
on each other card (`NICS_x86_64`, `NICS_arm64` in the Makefile): x86-64
`pc` + e1000, `q35` + e1000e, `pc` and `q35` + virtio 1.0 without the
legacy interface; arm64 `virt` + virtio 1.0 on PCIe. `tools/nettest.py`
takes `NET_NIC=CARD` (QEMU's `-device`), `NET_MACHINE=q35` and
`NET_QUICK=1` for that.

| | |
|---|---|
| Download (HTTP, QEMU user network, KVM) | 65-110 MB/s; by card (2026-10-16): e1000 110, virtio 1.0 87-106, e1000e 33 MB/s; arm64 (emulated) 27 MB/s |
| HTTPS (ChaCha20-Poly1305), crypto alone on the host | 400 MB/s (AES-128-GCM: 31 MB/s, AES-256-GCM: 25 MB/s) |
| Signature checks (host) | ECDSA ~2.2 ms, RSA ~1.7 ms |
| Memory | vnet ~420-440 KiB (192 KiB of it DMA buffers; the device tree is dropped after setup), netd ~300 KiB plus 320 KiB per TCP connection |
| Code (text, 2026-10-16) | netd 36 KB, vnet 24 KB (x86-64, with e1000 and RTL8168h: +3.3 KB) / 47 KB (arm64, with the two Pi backends), fetch 69 KB (with TLS 1.3 and 1.2 and the commands; TLS 1.2 added 5 KB to tls.o, 12.9 KB in all); wifid 52 KB on x86-64 (iwl.c 21 KB, wpa.c 2.2 KB) / 44 KB on arm64 (bcm.c 12 KB, sdio.c 6.5 KB) |

## Limits

- IPv4 only; no fragments; no selective acknowledgements; no keepalives;
  one route (local network or gateway).
- TLS 1.3 and 1.2 only (1.1 and older are refused); in 1.2, only ECDHE with
  AEAD ciphers (servers offering only CBC or static RSA are refused). No
  client certificates, no session resumption, no renegotiation. Certificate names: subject
  alternative names only (no fallback to the common name), no revocation
  checks. AES uses a table, which in theory leaks timing to a program sharing
  the cache: ChaCha20 is preferred.
- The date comes from the clock chip read when vnet starts (no NTP). The
  Raspberry Pis have no clock chip: the date is unknown there, and TLS then
  skips the certificates' validity dates (everything else is checked).
- Raspberry Pi Ethernet and Wi-Fi are untested on hardware (see
  [raspberrypi.md](raspberrypi.md)); the Pi 5's Ethernet is polled. The
  AX210 Wi-Fi is untested too (QEMU has no AX210): what to check on the
  laptop is above.
- Wi-Fi: open and WPA2-PSK only (no WPA3, no enterprise, no WEP); 2.4 and
  5 GHz as the firmware's regulatory blob allows; one network at a time; no
  access-point mode. On the Pis the chip's firmware runs the handshake; on
  the AX210 the host does (`wpa.c`). The AX210 is station mode only, HT at
  20 MHz, no power save and no aggregation, and it polls its queues (no PCI
  interrupt routed yet), so throughput is modest. A scan blocks the caller
  (and the desktop) for a few seconds.
- The card sits in PCI slot 5 in QEMU (line 10, apart from the disk's); the
  kernel now lets up to 4 drivers share a line, so this is no longer needed,
  only kept.
- Cards: e1000 and the RTL8168h (untested on hardware) only on x86-64 (arm64 has virtio, the Pis their own); the
  e1000e is slower in QEMU (33 MB/s). On arm64's `virt`, virtio on PCI is
  virtio 1.0 only (a legacy-only card, `disable-modern=on`, is not found:
  its I/O ports would need the bridge's I/O window), on bus 0, with INTx
  interrupts (no MSI); x86-64 also uses INTx.
- `traceroute`: QEMU's user network does not count the time to live, so
  every host is one hop away there; the "time exceeded" answers of real
  routers are untested.
- `nc` works a line at a time (the console gives typed lines); when the
  other side closes, it ends at the next Enter (a console read under way
  cannot be taken back); no `nc` in a pipe (the shell has no pipes).
- netd handles one message at a time; a connection's buffers are allocated
  when it opens (320 KiB) and freed when it ends.
