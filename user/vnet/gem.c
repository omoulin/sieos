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
 * RP1 must still be set up by the firmware: config.txt says "pciex4_reset=0"
 * (tools/mksdcard.py), else the firmware resets RP1's PCIe link before
 * starting us and nothing answers at 0x1F_0000_0000. Then, as Linux does
 * (clk-rp1.c, macb_main.c), we make sure RP1's Ethernet clock runs (125
 * MHz from the PLL "sys_sec"), pulse the PHY's reset line (RP1's GPIO 32,
 * the device tree's "phy-reset-gpios"), and point the controller's other
 * queues at a descriptor marked "used", so they never move data.
 *
 * Polled: RP1's interrupts arrive as PCIe MSI-X messages, which SIEOS does
 * not route yet. vnet's receiver sleeps 0.2 ms after traffic, growing to
 * 10 ms when the line is quiet: up to ~10 ms more latency on the first
 * frame of a burst, ~100 short wake-ups a second when idle.
 *
 * Caches: whether the PCIe path sees the CPU's caches depends on the board;
 * we assume it does not. A cache line (64 bytes) holds four descriptors, so
 * we never write a line the controller may be writing: receive descriptors
 * go back four at a time (a whole line), and a batch is sent only once the
 * previous one is done.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
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
#define TSR    0x014
#define RSR    0x020
#define AMP    0x054                    /* AXI pipelines: 0-7 reads, 8-15 writes, 16 "AW to B" */
#define DCFG1  0x280
#define DCFG6  0x294                    /* bits 1-7: queues 1-7 exist; 23: 64-bit addresses */
#define TBQP1  0x440                    /* queues 1-7 (4 bytes each) */
#define RBQP1  0x480
#define TBQPH  0x4C8                    /* (the high half, for every queue) */
#define RBQPH  0x4D4
/* RP1's other blocks, as RP1 sees them (rp1.dtsi). */
#define RP1_BASE   0xC040000000ULL      /* RP1's peripherals ... */
#define RP1_WINDOW 0x1F00000000ULL      /* ... where the PCIe window shows them to us */
#define CLK_ETH_CTRL 0x18064            /* the clocks block: bit 11 on, 5-9 source (0: pll_sys_sec) */
#define CLK_ETH_DIV  0x18068
#define PLL_SYS_SEC  0x20014            /* bits 8-12 divider (1 GHz / 8 = 125 MHz), 16 held in reset */

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
static int rx_next, tx_next, phy_addr = 1, cur_mbps, cur_full;
static volatile uint32_t *rp1;           /* RP1's first MiB of registers (clocks, GPIO) */

static uint32_t rp1_rd(uint32_t o)             { return rp1[o / 4]; }
static void     rp1_wr(uint32_t o, uint32_t v) { rp1[o / 4] = v; }

/* The clock the controller sends frames with (RGMII, 125 MHz): the firmware
 * sets it up for a network boot, perhaps not otherwise. */
static void rp1_eth_clock(void)
{
    uint32_t ctrl = rp1_rd(CLK_ETH_CTRL), sec = rp1_rd(PLL_SYS_SEC);
    if (ctrl & 1u << 11) return;
    if (sec & 1u << 16 || (sec >> 8 & 31) != 8) {        /* sys_sec off, or not 125 MHz: divide by 8 */
        rp1_wr(PLL_SYS_SEC, (sec & ~(31u << 8)) | 8u << 8 | 1u << 16);   /* (changed while held in reset) */
        rp1_wr(PLL_SYS_SEC, (sec & ~(31u << 8 | 1u << 16)) | 8u << 8);
    }
    rp1_wr(CLK_ETH_DIV, 1);
    rp1_wr(CLK_ETH_CTRL, (ctrl & ~(31u << 5)) | 1u << 11);
    printf("vnet: RP1's Ethernet clock was off (control %x, sys_sec %x): turned on, 125 MHz\n", ctrl, sec);
}

/* The PHY's reset line: an RP1 GPIO held at its active level for
 * "phy-reset-duration" ms, then released (Linux's macb_mdio_reset). RP1's
 * GPIO, three banks (pins 0-27, 28-33, 34-53), each in three blocks 16 KiB
 * apart: the pins' control words (+0xd0000: status, control; function 5 =
 * driven by RIO), RIO (+0xe0000: OUT at 0, OE at 4; +0x2000 sets bits,
 * +0x3000 clears them), the pads (+0xf0000, 4 + 4 n: bit 7 output off). */
static void rp1_phy_reset(const void *f, int n)
{
    int len;
    const uint8_t *g = fdt_prop(f, n, "phy-reset-gpios", &len), *d;
    int c = g && len >= 12 ? fdt_phandle(f, fdt_be32(g)) : -1;
    if (c < 0 || !fdt_compatible(f, c, "raspberrypi,rp1-gpio")) return;
    uint32_t pin = fdt_be32(g + 4), act_low = fdt_be32(g + 8) & 1, ms = 10;
    if ((d = fdt_prop(f, n, "phy-reset-duration", &len)) && len == 4 && fdt_be32(d) <= 1000) ms = fdt_be32(d);
    if (pin >= 54) return;
    uint32_t b = pin < 28 ? 0 : pin < 34 ? 1 : 2, k = pin - (b == 0 ? 0 : b == 1 ? 28 : 34);
    uint32_t io = 0xd0000 + b * 0x4000 + 8 * k + 4, rio = 0xe0000 + b * 0x4000, pad = 0xf0000 + b * 0x4000 + 4 + 4 * k;
    uint32_t assert = act_low ? 0x3000 : 0x2000, release = act_low ? 0x2000 : 0x3000;
    rp1_wr(rio + assert, 1u << k);               /* OUT: reset ... */
    rp1_wr(rio + 0x2000 + 4, 1u << k);           /* OE: driven */
    rp1_wr(pad, (rp1_rd(pad) & ~0x80u) | 0x40);
    rp1_wr(io, (rp1_rd(io) & ~(0x1Fu | 0xFu << 12)) | 5);
    sys_sleep((uint64_t)ms * 1000000);
    rp1_wr(rio + release, 1u << k);              /* ... released */
    sys_sleep(10000000);                         /* (the PHY starts, then answers on MDIO) */
    printf("vnet: PHY reset (RP1 GPIO %u, %u ms)\n", pin, ms);
}

static uint32_t rd(uint32_t o)             { return mmio_r32(g + o); }
static void     wr(uint32_t o, uint32_t v) { mmio_w32(g + o, v); }

static int mdio(uint32_t v)
{
    for (int k = 0; k < 1000 && !(rd(NSR) & 4); k++) nic_udelay(1);   /* the bus idle first */
    wr(MAN, v);
    nic_udelay(1);
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
    for (int p = -1, d; (p = fdt_find(f, p, "brcm,bcm2712-pcie")) >= 0; ) {   /* RP1's link, as the firmware left it */
        int c = (d = 0, fdt_next(f, p, &d));     /* (its first child, "rp1") */
        uint64_t pa0, psz;
        volatile uint32_t *pc;
        if (c < 0 || d != 1 || memcmp(fdt_name(f, c), "rp1", 3) || fdt_reg(f, p, 0, &pa0, &psz) || (long)(pc = map_phys(pa0, 0x10000)) < 0)
            continue;
        uint32_t st = pc[0x4068 / 4];            /* bit 4 PHY link up, 5 data link active */
        if ((st & 0x30) != 0x30) {
            printf("vnet: RP1's PCIe link is down (status %x): config.txt needs pciex4_reset=0\n", st);
            return -1;
        }
    }
    if (fdt_reg(f, n, 0, &a, &size)) return -1;
    if (a >= RP1_BASE) a = a - RP1_BASE + RP1_WINDOW;   /* RP1's view -> the PCIe window */
    g = map_phys(a, size ? size : 0x4000);
    rp1 = map_phys(RP1_WINDOW, 0x100000);
    if ((long)g < 0 || (long)rp1 < 0) return -1;
    uint32_t mid = rd(0xFC), cfg6 = rd(DCFG6);
    if (mid == 0xFFFFFFFF || !mid) {                 /* (a PCIe read nobody answered) */
        printf("vnet: RP1's Ethernet does not answer at %lx (id %x): is pciex4_reset=0 in config.txt?\n",
               (unsigned long)a, mid);
        return -1;
    }
    uint64_t b;
    bus_off = !fdt_bus_addr(f, n, 0, &b) && b ? (int64_t)b : 0x1000000000LL;
    const uint8_t *m = fdt_prop(f, n, "local-mac-address", &len);
    const char *macfrom = "device tree";
    if (m && len == 6 && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5])) memcpy(info->mac, m, 6);
    else if (nic_fw_mac(info->mac)) return -1;
    else macfrom = "firmware";
    phy_addr = phy_addr_dt(f, n, 1);
    info->mtu = 1500;
    if (!(cfg6 & 1u << 23)) printf("vnet: GEM: no 64-bit addressing (design %x): it cannot reach our memory\n", cfg6);
    rp1_eth_clock();
    rp1_phy_reset(f, n);

    char *ring = dma_alloc(4096, &pa);           /* 64 + 64 descriptors, and one for the other queues */
    if ((long)ring < 0) return -1;
    rxd = (desc_t *)ring; txd = (desc_t *)(ring + 2048);
    rxdbus = pa + bus_off; txdbus = pa + 2048 + bus_off;
    volatile desc_t *stop = (desc_t *)(ring + 3072);   /* "used" to the receiver and the sender, wrapping */
    uint64_t stopbus = pa + 3072 + bus_off;
    *stop = (desc_t){ RX_USED | RX_WRAP, TX_USED | TX_WRAP, (uint32_t)(stopbus >> 32), 0 };
    if ((long)(rxbuf = dma_alloc(NRX * BUFSZ, &pa)) < 0) return -1;
    rxbus = pa + bus_off;
    if ((long)(txmem = dma_alloc(NET_MAX, &pa)) < 0) return -1;
    txbus = pa + bus_off;
    if ((long)(pad = dma_alloc(NTX * 64, &pa)) < 0) return -1;
    padbus = pa + bus_off;

    /* Quiet (statistics cleared, status flags cleared), then configure. */
    wr(NCR, 1u << 5);
    wr(TSR, 0xFFFFFFFF); wr(RSR, 0xFFFFFFFF);
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
    wr(AMP, (rd(AMP) & ~0x1FFFFu) | (fdt_prop(f, n, "cdns,use-aw2b-fill", 0) ? 1u << 16 : 0) | 8u << 8 | 8);
    wr(RBQP, (uint32_t)rxdbus); wr(RBQPH, (uint32_t)(rxdbus >> 32));
    wr(TBQP, (uint32_t)txdbus); wr(TBQPH, (uint32_t)(txdbus >> 32));
    for (int q = 1; q < 8; q++)                  /* the other queues: the "used" descriptor (one high half for all) */
        if (cfg6 & 1u << q) { wr(TBQP1 + 4 * (q - 1), (uint32_t)stopbus); wr(RBQP1 + 4 * (q - 1), (uint32_t)stopbus); }
    wr(SA1B, (uint32_t)info->mac[3] << 24 | info->mac[2] << 16 | info->mac[1] << 8 | info->mac[0]);
    wr(SA1T, (uint32_t)info->mac[5] << 8 | info->mac[4]);
    wr(NCR, 1u << 4);                            /* MDIO on */
    int id1, id2;
    if (phy_probe(&bus, &phy_addr, &id1, &id2))
        printf("vnet: GEM: no PHY answers on MDIO (ids %04x:%04x): RP1's Ethernet clock or the PHY's reset?\n",
               id1 & 0xFFFF, id2 & 0xFFFF);
    phy_delays(&bus, f, n);
    phy_start(&bus);
    wr(NCR, 1u << 2 | 1u << 3 | 1u << 4);        /* receive, transmit, MDIO on */
    const char *mode = fdt_prop(f, n, "phy-mode", &len);
    printf("vnet: GEM at %lx: MAC from the %s, %s, design %x, RP1 Ethernet clock %x, our memory at +%lx\n",
           (unsigned long)a, macfrom, mode ? mode : "rgmii-id", cfg6, rp1_rd(CLK_ETH_CTRL), (unsigned long)bus_off);
    nic_fmt(what, cap, "GEM r%x (Raspberry Pi 5, RP1), PHY %04x:%04x at %d, polled", mid,
            id1 & 0xFFFF, id2 & 0xFFFF, phy_addr);
    return 0;
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

const nic_t nic_gem = { "gem", setup, txbuf, send, 0, rx_take, link, stop };
