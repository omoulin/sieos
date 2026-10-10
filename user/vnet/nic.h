/*
 * nic.h - What vnet (the network card server, vnet.c) asks of a card
 * driver. One program serves "nic0" whatever the card is; it tries each
 * backend in turn and keeps the first that finds its card:
 *   virtio  (vnet.c)   QEMU, on both architectures;
 *   genet   (genet.c)  the Raspberry Pi 4's own Ethernet (BCM2711 GENET v5);
 *   gem     (gem.c)    the Raspberry Pi 5's Ethernet, in its RP1 chip.
 *
 * Receiving runs in vnet's second thread: rx_wait sleeps until frames may
 * have come (an interrupt, or a short sleep for a card polled), then rx_take
 * packs what arrived into one batch for the network server ("net",
 * NET_FRAMES): each frame a 16-bit length, then the frame.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "mk.h"

typedef struct {
    const char *name;
    /* Find the card and get it ready to receive; fill info's MAC and MTU,
     * describe the card in what (one line for the log). 0 if found. */
    int (*setup)(nic_info_t *info, char *what, size_t cap);
    /* Where NIC_SEND's frames land (NET_MAX bytes of DMA memory): the
     * message's data goes there directly, so sending copies nothing. */
    uint8_t *(*txbuf)(void);
    /* Send the frames now in txbuf (len bytes, NIC_SEND's layout). */
    long (*send)(size_t len);
    /* Receiving thread: wait, then take what arrived (at most cap bytes;
     * the rest stays for the next call). */
    void (*rx_wait)(void);
    size_t (*rx_take)(uint8_t *batch, size_t cap);
    /* The cable: 1 up, 0 down; NULL for cards without one (virtio). */
    int (*link)(void);
    /* No more DMA into our memory (before vnet ends). */
    void (*stop)(void);
} nic_t;

extern const nic_t nic_virtio, nic_genet, nic_gem;

/* printf into a buffer (vnet.c). */
void nic_fmt(char *out, size_t cap, const char *fmt, ...);

/* Shared by the Raspberry Pi backends (vnet.c): the device tree, loaded
 * once; a short busy wait; the MAC the firmware gives (mailbox), the
 * fallback when the device tree has none. */
const void *nic_fdt(void);
void nic_udelay(uint64_t us);
int nic_fw_mac(uint8_t mac[6]);
/* The standard registers of an Ethernet PHY (IEEE 802.3 clause 22), and
 * the speed and duplex both sides agreed on, from them. */
enum { PHY_BMCR = 0, PHY_BMSR = 1, PHY_ID1 = 2, PHY_ID2 = 3, PHY_ADV = 4, PHY_LPA = 5,
       PHY_CTRL1000 = 9, PHY_STAT1000 = 10 };
typedef struct { int (*rd)(int reg); void (*wr)(int reg, int v); } mdio_t;
/* Start auto-negotiation (10/100/1000, full and half duplex). */
void phy_start(const mdio_t *m);
/* The RGMII clock delays, which the PHY (the Pis' BCM54213PE) adds as the
 * MAC's device tree node says ("phy-mode": "rgmii-id" both, "rgmii-rxid"
 * receive only, "rgmii-txid" transmit only, "rgmii" none). */
void phy_delays(const mdio_t *m, const void *fdt, int mac_node);
/* 1 if the link is up, with *mbps and *full set; 0 if down. */
int phy_link(const mdio_t *m, int *mbps, int *full);
