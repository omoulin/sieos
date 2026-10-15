/*
 * wifi.h - What the Wi-Fi server (user/wifi) is made of: the SDIO host
 * (sdio.c: an SDHCI controller driven as an SDIO host, CMD52/CMD53) and the
 * FullMAC driver (wifi.c: the CYW43455 on the Raspberry Pi 4 and 5).
 *
 * The chip is reached over SDIO: function 1 is a window onto the chip's
 * backplane (its cores and RAM), function 2 carries data and control frames.
 * sdio.c brings the controller and the card up and offers byte and block
 * reads and writes to a function; wifi.c downloads the firmware, starts the
 * chip's ARM core, speaks the BCDC/SDPCM protocol, scans, joins and plumbs
 * data frames into netd as a second interface (port "wifi").
 *
 * UNTESTED ON HARDWARE: QEMU emulates neither the SDIO controller's card
 * side nor the chip; written from the controllers' register layout, the
 * device trees, and Linux's brcmfmac and Plan 9's ether4330. What to check
 * on a real board is in docs/raspberrypi.md.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "mk.h"

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

/* The device tree (wifi.c loads it once), a printf into a buffer, and a
 * short busy wait: shared with sdio.c. */
const void *wifi_fdt(void);
void wifi_fmt(char *out, size_t cap, const char *fmt, ...);

/* ---- The driver (wifi.c): see user/wifi/wifi.c. */
