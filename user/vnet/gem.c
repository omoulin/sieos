/*
 * gem.c - The Raspberry Pi 5's Ethernet: a Cadence "GEM" controller inside
 * the RP1 chip, which the BCM2712 reaches through PCIe (the firmware brings
 * the link up and opens a window: RP1's addresses 0xC0_4000_0000... appear
 * at 0x1F_0000_0000; RP1 sees our RAM at 0x10_0000_0000), and its PHY (a
 * BCM54213PE) on the controller's MDIO bus. UNTESTED ON HARDWARE: no Pi 5
 * emulator exists; written from the controller's register layout; what to
 * check on a real Pi 5 is in docs/raspberrypi.md.
 *
 * Unlike the Pi 4's GENET, the descriptors are in our memory, 16 bytes each
 * (64-bit addressing): address low (receive: bit 0 "used" = the controller
 * filled it, bit 1 "wrap" = the last one), status or control, address high.
 *
 * Polled: RP1's interrupts arrive as PCIe MSI-X messages, which SIEOS does
 * not route yet. rx_wait sleeps 0.2 ms after traffic, growing to 10 ms when
 * the line is quiet: up to ~10 ms more latency on the first frame of a
 * burst, ~100 short wake-ups a second when idle.
 *
 * Caches: whether the PCIe path sees the CPU's caches depends on the board;
 * we assume it does not. A cache line (64 bytes) holds four descriptors, so
 * we never write a line the controller may be writing: receive descriptors
 * go back four at a time (a whole line), and a batch is sent only once the
 * previous one is done.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "nic.h"
#include "mk/fdt.h"

/* Registers (byte offsets). */
#define NCR    0x000                    /* control: 2 receive on, 3 transmit on, 4 MDIO on, 9 start */
#define NCFGR  0x004                    /* configuration: 0 100 Mb/s, 1 full duplex, 4 all frames,
                                           10 gigabit, 17 drop the checksum, 18-20 MDC divider, 21-22 bus width */
#define NSR    0x008                    /* status: 2 MDIO idle */
#define USRIO  0x00C                    /* bit 0: RGMII */
#define DMACFG 0x010                    /* 0-4 burst, 8-9/10 buffer memory, 16-23 receive buffer /64, 30 64-bit */
#define RBQP   0x018
#define TBQP   0x01C
#define ISR    0x024
#define IDR    0x02C
#define MAN    0x034                    /* PHY maintenance (MDIO) */
#define SA1B   0x088
#define SA1T   0x08C
#define DCFG1  0x280
#define TBQPH  0x4C8
#define RBQPH  0x4D4

#define NRX 64                          /* a multiple of 4: one cache line = 4 descriptors */
#define NTX 64
#define BUFSZ 2048
#define RX_USED 1u
#define RX_WRAP 2u
#define TX_USED (1u << 31)
#define TX_WRAP (1u << 30)
#define TX_LAST (1u << 15)

typedef struct { uint32_t addr, ctl, addr_hi, pad; } desc_t;

static volatile uint8_t *g;
static volatile desc_t *rxd, *txd;       /* descriptor rings (DMA memory) */
static uint8_t *rxbuf, *txmem, *pad;
static uint64_t rxdbus, txdbus, rxbus, txbus, padbus;
static int64_t bus_off;                  /* RAM as RP1 sees it = physical + bus_off */
static int rx_next, tx_next, phy_addr = 1, cur_mbps, cur_full, idle_ms;

static uint32_t rd(uint32_t o)             { return mmio_r32(g + o); }
static void     wr(uint32_t o, uint32_t v) { mmio_w32(g + o, v); }

static int mdio(uint32_t v)
{
    wr(MAN, v);
    for (int k = 0; k < 1000; k++) {
        if (rd(NSR) & 4) return (int)(rd(MAN) & 0xFFFF);
        nic_udelay(1);
    }
    return -1;
}
/* Clause 22 frame: start 01, operation (10 read, 01 write), PHY, register, 10. */
static int  mrd(int reg)        { return mdio(1u << 30 | 2u << 28 | (uint32_t)phy_addr << 23 | (uint32_t)reg << 18 | 2u << 16); }
static void mwr(int reg, int v) { mdio(1u << 30 | 1u << 28 | (uint32_t)phy_addr << 23 | (uint32_t)reg << 18 | 2u << 16 | (uint16_t)v); }
static const mdio_t bus = { mrd, mwr };

static void rx_give(int i)               /* descriptor i back to the controller (wrap on the last) */
{
    rxd[i].addr_hi = (uint32_t)((rxbus + (uint64_t)i * BUFSZ) >> 32);
    rxd[i].ctl = 0;
    rxd[i].addr = (uint32_t)(rxbus + (uint64_t)i * BUFSZ) | (i == NRX - 1 ? RX_WRAP : 0);
}

static int setup(nic_info_t *info, char *what, size_t cap)
{
    const void *f = nic_fdt();
    int n = -1, len;
    uint64_t a, size, pa;
    if (!f || fdt_find(f, -1, "brcm,bcm2712-pcie") < 0) return -1;
    if ((n = fdt_find(f, -1, "raspberrypi,rp1-gem")) < 0 && (n = fdt_find(f, -1, "cdns,macb")) < 0) return -1;
    if (fdt_reg(f, n, 0, &a, &size)) return -1;
    if (a >= 0xC040000000ULL) a = a - 0xC040000000ULL + 0x1F00000000ULL;   /* RP1's view -> the PCIe window */
    g = map_phys(a, size ? size : 0x4000);
    if ((long)g < 0) return -1;
    uint64_t b;
    bus_off = !fdt_bus_addr(f, n, 0, &b) && b ? (int64_t)b : 0x1000000000LL;
    const uint8_t *m = fdt_prop(f, n, "local-mac-address", &len);
    if (m && len == 6 && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5])) memcpy(info->mac, m, 6);
    else if (nic_fw_mac(info->mac)) return -1;
    for (int p = -1; (p = fdt_find(f, p, "ethernet-phy-ieee802.3-c22")) >= 0; ) {
        const void *r = fdt_prop(f, p, "reg", &len);
        if (r && len == 4) { phy_addr = (int)fdt_be32(r); break; }
    }
    info->mtu = 1500;

    char *ring = dma_alloc(4096, &pa);           /* 64 + 64 descriptors */
    if ((long)ring < 0) return -1;
    rxd = (desc_t *)ring; txd = (desc_t *)(ring + 2048);
    rxdbus = pa + bus_off; txdbus = pa + 2048 + bus_off;
    if ((long)(rxbuf = dma_alloc(NRX * BUFSZ, &pa)) < 0) return -1;
    rxbus = pa + bus_off;
    if ((long)(txmem = dma_alloc(NET_MAX, &pa)) < 0) return -1;
    txbus = pa + bus_off;
    if ((long)(pad = dma_alloc(NTX * 64, &pa)) < 0) return -1;
    padbus = pa + bus_off;

    /* Quiet, then configure. */
    wr(NCR, 0);
    wr(IDR, 0xFFFFFFFF);
    rd(ISR);
    for (int i = 0; i < NRX; i++) rx_give(i);
    for (int i = 0; i < NTX; i++) txd[i] = (desc_t){ 0, TX_USED | (i == NTX - 1 ? TX_WRAP : 0), 0, 0 };
    dma_sync(ring, 4096);
    dma_sync(rxbuf, NRX * BUFSZ);
    rx_next = tx_next = 0;
    uint32_t dbw = rd(DCFG1) >> 25 & 7;           /* the bus width the chip was built with */
    dbw = dbw >= 4 ? 2 : dbw >= 2 ? 1 : 0;
    wr(NCFGR, 1u << 10 | 1u << 1 | 1u << 4 | 1u << 17 | 6u << 18 | dbw << 21);   /* gigabit full duplex,
                                                    all frames (netd filters), checksum dropped, MDC /128 */
    wr(USRIO, 1);                                /* RGMII */
    wr(DMACFG, 16u | 3u << 8 | 1u << 10 | (uint32_t)(BUFSZ / 64) << 16 | 1u << 30);
    wr(RBQP, (uint32_t)rxdbus); wr(RBQPH, (uint32_t)(rxdbus >> 32));
    wr(TBQP, (uint32_t)txdbus); wr(TBQPH, (uint32_t)(txdbus >> 32));
    wr(SA1B, (uint32_t)info->mac[3] << 24 | info->mac[2] << 16 | info->mac[1] << 8 | info->mac[0]);
    wr(SA1T, (uint32_t)info->mac[5] << 8 | info->mac[4]);
    wr(NCR, 1u << 4);                            /* MDIO on */
    int id1 = mrd(PHY_ID1), id2 = mrd(PHY_ID2);
    phy_delays(&bus, f, n);
    phy_start(&bus);
    wr(NCR, 1u << 2 | 1u << 3 | 1u << 4);        /* receive, transmit, MDIO on */
    nic_fmt(what, cap, "GEM r%x (Raspberry Pi 5, RP1), PHY %04x:%04x at %d, polled", rd(0xFC),
            id1 & 0xFFFF, id2 & 0xFFFF, phy_addr);
    return 0;
}

static void rx_wait(void)
{
    sys_sleep(idle_ms ? (uint64_t)idle_ms * 1000000UL : 200000UL);
    if (idle_ms < 10) idle_ms++;
}

static size_t rx_take(uint8_t *batch, size_t cap)
{
    size_t n = 0;
    for (;;) {
        volatile desc_t *d = &rxd[rx_next];
        dma_sync(d, sizeof *d);                  /* the controller's write, not an old line */
        if (!(d->addr & RX_USED)) break;
        uint32_t fl = d->ctl & 0x1FFF;
        int whole = (d->ctl & (3u << 14)) == (3u << 14);   /* start and end of frame */
        uint8_t *b = rxbuf + rx_next * BUFSZ;
        if (whole && fl >= 14 && fl <= 1536) {
            if (n + 2 + fl > cap) break;         /* full: the rest goes in the next batch */
            dma_sync(b, fl);
            batch[n] = (uint8_t)fl; batch[n + 1] = (uint8_t)(fl >> 8);
            memcpy(batch + n + 2, b, fl);
            n += 2 + fl;
        }
        if (rx_next % 4 == 3) {                  /* a whole line done: give its four back */
            for (int i = rx_next - 3; i <= rx_next; i++) rx_give(i);
            dma_sync(&rxd[rx_next - 3], 4 * sizeof *d);
        }
        rx_next = (rx_next + 1) % NRX;
    }
    if (n) idle_ms = 0;
    return n;
}

static uint8_t *txbuf(void) { return txmem; }

/* Write the descriptors of up to NTX frames, start, wait until all are sent. */
static long send(size_t len)
{
    dma_sync(txmem, len);
    size_t off = 0;
    while (off + 2 <= len) {
        int first = tx_next, k = 0;
        dma_sync(txd, NTX * sizeof *txd);        /* the controller's "used" marks, fresh */
        while (off + 2 <= len && k < NTX - 1) {  /* (one descriptor stays "used": the stop mark) */
            uint32_t fl = txmem[off] | txmem[off + 1] << 8;
            if (fl < 14 || fl > 1514 || off + 2 + fl > len) return -EINVAL;
            uint64_t at = txbus + off + 2;
            if (fl < 60) {                       /* pad short frames to the 60-byte minimum */
                uint8_t *p = pad + tx_next * 64;
                memcpy(p, txmem + off + 2, fl);
                memset(p + fl, 0, 60 - fl);
                dma_sync(p, 64);
                at = padbus + (uint64_t)tx_next * 64;
            }
            volatile desc_t *d = &txd[tx_next];
            d->addr = (uint32_t)at; d->addr_hi = (uint32_t)(at >> 32);
            d->ctl = (fl < 60 ? 60 : fl) | TX_LAST | (tx_next == NTX - 1 ? TX_WRAP : 0);   /* used = 0: ready */
            off += 2 + fl;
            tx_next = (tx_next + 1) % NTX;
            k++;
        }
        dma_sync(txd, NTX * sizeof *txd);
        wr(NCR, rd(NCR) | 1u << 9);              /* start transmitting */
        for (int i = first, left = k; left; ) {  /* wait: each sent descriptor turns "used" again */
            dma_sync(&txd[i], sizeof *txd);
            if (txd[i].ctl & TX_USED) { i = (i + 1) % NTX; left--; }
            else sys_yield();
        }
    }
    return 0;
}

static int link(void)
{
    int mbps = 0, full = 0, up = phy_link(&bus, &mbps, &full);
    if (up && (mbps != cur_mbps || full != cur_full)) {
        uint32_t c = rd(NCFGR) & ~(1u | 2u | 1u << 10);
        c |= (mbps == 1000 ? 1u << 10 : mbps == 100 ? 1u : 0) | (full ? 2u : 0);
        wr(NCFGR, c);
        printf("vnet: link up, %d Mb/s %s duplex\n", mbps, full ? "full" : "half");
        cur_mbps = mbps; cur_full = full;
    } else if (!up && cur_mbps) {
        printf("vnet: link down\n");
        cur_mbps = 0;
    }
    return up;
}

static void stop(void) { wr(NCR, 0); wr(IDR, 0xFFFFFFFF); }

const nic_t nic_gem = { "gem", setup, txbuf, send, rx_wait, rx_take, link, stop };
