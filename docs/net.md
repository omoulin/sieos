# The network

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

SIEOS's network is three programs and a library, all written for SIEOS:

```
  fetch, sia (remote.c) ...      user/lib: net.c (sockets), http.c (HTTP/1.1),
        │                                  tls.c (TLS 1.3), x509.c (certificates)
        │ NET_* messages, port "net"
        ▼
  netd  (/bin/netd)             IPv4, ARP, ICMP, UDP, TCP, DHCP client, DNS resolver
        │ NIC_SEND              ▲ NET_FRAMES / NET_LINK (received frames, the link;
        ▼                       │                        both carry the interface)
  vnet  (port "nic0")           the cable, and the clock chip      ┐  one interface
  wifid (port "wifi")           the Wi-Fi chip (the Raspberry Pis) ┘  used at a time
        │                       ▲
        ▼
  virtio-net (QEMU), GENET (Pi 4), RP1's GEM (Pi 5); the CYW43455 on SDIO (both Pis)
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
| `vnet.c` | virtio-net (QEMU, both architectures) | interrupt | tested |
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

The virtio backend: the legacy virtio interface, like the disk driver: a receive queue with 64
buffers of 2 KiB and a transmit queue, in DMA memory. A second thread waits
for the card's interrupt (a message), collects every frame received, and
sends them to netd in **one** message. Frames to send arrive from netd in
one message too, straight into the DMA memory the card reads (no copy of
our own); the reply waits until the card has taken them.

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
- **Owners**: every socket belongs to the process that opened it. Every 2 s
  netd checks that owners are alive; a dead owner's connections are reset.
- **Restart**: if netd dies, init restarts it (DHCP again); programs' calls
  in progress return an error (`-EPIPE`), never hang. Resolving names,
  pings and `NET_INFO` are simply sent again.

The protocol is in `include/mk/proto.h` (`NET_*`, `NIC_*`).

## Wi-Fi: wifid (the Raspberry Pis)

`wifid` (`user/wifi`, arm64 only, started at boot on the Pis) drives the
**CYW43455** on both the Pi 4 and the Pi 5, over SDIO, as a second network
interface (port `wifi`). Open and WPA2-PSK networks. **Untested on
hardware**: QEMU emulates neither the SDIO controller's card side nor the
chip; it was written from the controllers' register layout, the device
trees and Linux's `brcmfmac` and Plan 9's `ether4330`. What to check on a
real board, and what the log lines mean, is in
[raspberrypi.md](raspberrypi.md).

```
  wifi (/bin/wifi), Settings ──WIFI_*──► wifid ──SDIO──► the CYW43455
                      netd ◄──NET_FRAMES / NET_LINK──┘
```

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
- **The driver** (`wifid.c`): over SDIO function 1 it reaches the chip's
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

## The library: sockets, HTTP, TLS

- `net.c`: `net_resolve`, `net_connect`, `net_listen`/`net_accept`,
  `net_send`/`net_recv`, `net_udp`/`net_sendto`/`net_recvfrom`, `net_ping`,
  `net_info`.
- `http.c`: HTTP/1.1, `http://` and `https://`; Content-Length, chunked, or
  until close; up to 5 redirects for GET; `http_line` reads a body line by
  line (Server-Sent Events).
- `tls.c`: **TLS 1.3 only** (RFC 8446). Ciphers: ChaCha20-Poly1305
  (preferred) and AES-128-GCM, with SHA-256. Key exchange: X25519, or P-256
  when the server asks (HelloRetryRequest). The server's
  CertificateVerify: ECDSA P-256/P-384, RSA-PSS. Post-handshake key updates;
  session tickets are ignored (no resumption).
- `x509.c`: certificates in DER; the chain is followed to one of the
  trusted roots in `/etc/ssl/roots`, every signature checked (RSA PKCS #1
  v1.5, ECDSA P-256/P-384), validity dates checked against the clock chip's
  date, the name checked against the subject alternative names (`*.` for one
  label; IP addresses too), intermediate certificates must be authorities.
- The crypto (`lib/`): `sha2.c` (SHA-256/384/512, HMAC, HKDF), `aead.c`
  (Poly1305, ChaCha20-Poly1305, AES-128, GCM), `x25519.c`, `pubkey.c`
  (Montgomery arithmetic, P-256/P-384 ECDSA and key exchange, RSA). All plain
  integer code.

**The trusted roots**: `rootfs/etc/ssl/roots` is Mozilla's list of root
certificates as published by the curl project
(<https://curl.se/ca/cacert.pem>, "Certificate data from Mozilla as of: Fri
Sep 25 03:12:01 2026 GMT", sha256 `a41b5d35...00505`, 121 roots, fetched
2026-10-09), converted to DER by `tools/netcerts.py`. To update:
`curl -o cacert.pem https://curl.se/ca/cacert.pem && python3 tools/netcerts.py cacert.pem rootfs/etc/ssl/roots`.

## Programs

- `fetch [-v] [-k] [-o FILE] URL`: a URL's body (on the screen or into a
  file); `-v` status, headers and cipher; `-k` no certificate check (tests).
  Also `fetch -i` (address, counters, date), `fetch -p HOST [N]` (ping),
  `fetch -r HOST` (DNS), `fetch -l PORT` (answer one HTTP request: a test).
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

`make run` adds `-netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-modern=on,addr=5`:
QEMU's user network gives 10.0.2.15, the gateway 10.0.2.2 (which is also the
development machine), DNS 10.0.2.3. The card sits at PCI slot 5 so that its
interrupt line (10) is not the disk's (11): the kernel gives each interrupt
line to one driver only (see Limits).

## Tests and measurements

`make net-crypto-test`: 134 checks: FIPS 180-4, RFC 4231, 5869, 8439, 7748
and GCM test vectors, and 54 signatures made by the host's openssl (ECDSA
P-256/P-384 with SHA-256/384, RSA 2048/3072/4096 PKCS #1 v1.5 and PSS with
SHA-256/384/512), each also damaged and refused.

`make net-test` (SMP=1/4/8): DHCP, ping, DNS, HTTP from a server on the
development machine, a 24 MiB download checked byte for byte (sha256) on the
host afterwards, a listening socket reached from the host, HTTPS to
example.com and huggingface.co, TLS 1.3 servers (openssl s_server) with a
test CA (RSA-PSS, forced AES-128-GCM, forced P-256 with HelloRetryRequest,
ECDSA P-384) and certificates that must be refused (expired, another name,
unknown issuer); sia against a mock OpenAI-compatible server (tools/netmock.py)
over HTTP and HTTPS, a wrong key, a conversation; netd restarted during a
transfer.

| | |
|---|---|
| Download (HTTP, QEMU user network, KVM) | 65-78 MB/s |
| HTTPS (ChaCha20-Poly1305), crypto alone on the host | 400 MB/s (AES-128-GCM: 31 MB/s) |
| Signature checks (host) | ECDSA ~2.2 ms, RSA ~1.7 ms |
| Memory | vnet ~420-440 KiB (192 KiB of it DMA buffers; the device tree is dropped after setup), netd ~300 KiB plus 320 KiB per TCP connection |
| Code | netd 41 KB, vnet 20 KB (x86-64) / 44 KB (arm64, with the two Pi backends), fetch 78 KB (with TLS) |

## Limits

- IPv4 only; no fragments; no selective acknowledgements; no keepalives;
  one route (local network or gateway).
- TLS 1.3 only (servers that only speak TLS 1.2 are refused), no client
  certificates, no session resumption. Certificate names: subject
  alternative names only (no fallback to the common name), no revocation
  checks. AES uses a table, which in theory leaks timing to a program sharing
  the cache: ChaCha20 is preferred.
- The date comes from the clock chip read when vnet starts (no NTP). The
  Raspberry Pis have no clock chip: the date is unknown there, and TLS then
  skips the certificates' validity dates (everything else is checked).
- Raspberry Pi Ethernet and Wi-Fi are untested on hardware (see
  [raspberrypi.md](raspberrypi.md)); the Pi 5's Ethernet is polled.
- Wi-Fi: open and WPA2-PSK only (no WPA3, no enterprise, no WEP); 2.4 and
  5 GHz as the firmware's regulatory blob allows; one network at a time; no
  access-point mode. The host runs no supplicant: the chip's firmware does
  the handshake. A scan blocks the caller (and the desktop) for a few
  seconds.
- The card sits in PCI slot 5 in QEMU (line 10, apart from the disk's); the
  kernel now lets up to 4 drivers share a line, so this is no longer needed,
  only kept.
- netd handles one message at a time; a connection's buffers are allocated
  when it opens (320 KiB) and freed when it ends.
