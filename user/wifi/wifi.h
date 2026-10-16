/*
 * wifi.h - What the Wi-Fi server (user/wifi) is made of: the server itself
 * (wifid.c: the port "wifi", netd's second interface, the chosen network)
 * and one chip driver per architecture:
 *   bcm.c  the Raspberry Pis' CYW43455 (arm64), FullMAC, on SDIO (sdio.c:
 *          an SDHCI controller driven as an SDIO host, CMD52/CMD53);
 *   iwl.c  Intel's Wi-Fi 6E AX210 (x86-64), on PCIe: the firmware scans,
 *          picks rates and encrypts; the host joins and runs WPA2's
 *          handshake (wpa.c).
 * Both are UNTESTED ON HARDWARE (QEMU has neither chip): what to check is in
 * docs/raspberrypi.md and docs/net.md.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "mk.h"

/* ---- The chip driver (bcm.c or iwl.c), called by wifid.c's main thread. */
int  chip_up(void);                     /* 0 up (wifi_desc, wifi_mac set); 1 no such chip here; -1 failed */
int  chip_scan(wifi_net_t *list);       /* the networks in range (WIFI_MAXNET at most; a few seconds) */
void chip_join(const char *ssid, const char *pass);   /* start joining ("": open); wifi_link() says when up */
void chip_leave(void);
long chip_send(uint8_t *frames, size_t len);          /* NIC_SEND's layout -> 0 or -error */

/* ---- wifid.c, for the driver. */
extern volatile int wifi_state;         /* WIFI_IDLE, WIFI_JOINING, WIFI_UP, WIFI_FAILED ... */
extern char wifi_why[63], wifi_desc[64];   /* the last failure, in words; the chip and its firmware */
extern uint8_t wifi_mac[6], wifi_bssid[6];
extern int wifi_chan, wifi_rssi;
void wifi_link(int up);                 /* the link came up or went down: the state, netd told */
void wifi_frames(const void *batch, size_t n);   /* frames received, NET_FRAMES' layout, to netd */

/* ---- The SDIO host (sdio.c). All return 0, or -1 on a bus error. */
/* Find the Wi-Fi SDIO controller (the one with a "brcm,bcm4329-fmac" child
 * in the device tree), power the chip on, set the pins, and bring the card
 * up: CMD5 (SDIO reset and voltage), CMD3 (an address), CMD7 (select),
 * 4-bit, high speed, function 1 enabled. Fills `what` (the controller). */
int sdio_up(char *what, size_t cap);
/* One byte to/from a function's register (CMD52). rd returns the byte. */
int  sdio_rb(int fn, uint32_t addr);
int  sdio_wb(int fn, uint32_t addr, int val);
/* A block or byte transfer to/from a function (CMD53): `incr` advances the
 * chip address, else it is a FIFO. len may exceed the block size: split. */
int  sdio_rw(int fn, int write, uint32_t addr, void *buf, int len, int incr);
/* Enable function 2 (data frames) and wait until it is ready. */
int  sdio_enable_f2(void);
/* The card's interrupt: wait for it (the chip has a frame), or 1 if one came
 * while we were away. sdio_irq_ack re-arms it. 0 on a short sleep (polled). */
void sdio_irq_wait(void);
void sdio_irq_ack(void);

/* The device tree (bcm.c loads it once) and a printf into a buffer:
 * shared with sdio.c. */
const void *wifi_fdt(void);
void wifi_fmt(char *out, size_t cap, const char *fmt, ...);

