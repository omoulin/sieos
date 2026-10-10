/*
 * genet.c - The Raspberry Pi 4's Ethernet: the BCM2711's "GENET" v5
 * controller, and its PHY (a BCM54213PE, the chip on the cable side) reached
 * through the controller's MDIO bus. UNTESTED ON HARDWARE: QEMU emulates
 * no GENET; written from the controller's register layout; what to check
 * on a real Pi 4 is in docs/raspberrypi.md.
 *
 * The controller keeps its descriptors in its own registers (256 per
 * direction, 3 words each: length and status, address low, address high),
 * so only the frame buffers live in our memory. We use the "default" ring
 * (number 16) in each direction, 64 descriptors each:
 *
 *   Receiving: each descriptor points at a 2 KiB buffer; the controller
 *   fills them in order and advances its "producer" index; we copy frames
 *   out and advance our "consumer" index, which gives the buffers back.
 *   An interrupt (status bit 13, "receive buffer done") wakes us.
 *
 *   Sending: we write a descriptor per frame and advance the producer
 *   index; the controller advances its consumer index as frames leave.
 *
 * The Pi 4's devices do not see the CPU's caches: frame buffers are written
 * to memory before the controller reads them, and dropped from the caches
 * before we read what it wrote (dma_sync).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "nic.h"
#include "mk/fdt.h"

/* Registers (byte offsets from the controller's base). */
#define SYS_PORT_CTRL   0x0004          /* 3: an external PHY */
#define SYS_RBUF_FLUSH  0x0008
#define SYS_TBUF_FLUSH  0x000C
#define EXT_RGMII_OOB   0x008C          /* bit 6 RGMII mode, 5 "link from registers" off, 4 link */
#define IRQ_STAT        0x0200          /* interrupt controller 0 */
#define IRQ_CLEAR       0x0208
#define IRQ_MASK_STAT   0x020C
#define IRQ_MASK_SET    0x0210
#define IRQ_MASK_CLEAR  0x0214
#define RBUF_CTRL       0x0300          /* bit 0: 64-byte status before each frame; 1: 2-byte align */
#define RBUF_TBUF_SIZE  0x03B4
#define TBUF_CTRL       0x0600          /* bit 0: 64-byte status before each frame sent */
#define UMAC_CMD        0x0808
#define UMAC_MAC0       0x080C
#define UMAC_MAC1       0x0810
#define UMAC_MAX_FRAME  0x0814
#define UMAC_TX_FLUSH   0x0B34
#define UMAC_MIB_CTRL   0x0D80
#define UMAC_MDIO       0x0E14
#define RDMA            0x2000          /* the receive DMA block */
#define TDMA            0x4000          /* the transmit DMA block */
#define RING            (0xC00 + 16 * 0x40)    /* ring 16's registers, after the descriptors */
#define CTL             (0xC00 + 17 * 0x40)    /* the block's own registers */
enum { R_WPTR = 0x00, R_PROD = 0x08, R_CONS = 0x0C, R_BUFSZ = 0x10, R_START = 0x14, R_END = 0x1C,
       R_THRESH = 0x24, R_XON = 0x28, R_RPTR = 0x2C };            /* receive ring */
enum { T_RPTR = 0x00, T_CONS = 0x08, T_PROD = 0x0C, T_FLOW = 0x28, T_WPTR = 0x2C };  /* transmit */
enum { C_RING_CFG = 0x00, C_CTRL = 0x04, C_STATUS = 0x08, C_BURST = 0x0C };
/* UMAC_CMD bits. */
#define CMD_TX_EN   (1u << 0)
#define CMD_RX_EN   (1u << 1)
#define CMD_PROMISC (1u << 4)
#define CMD_HD      (1u << 10)
#define CMD_RESET   (1u << 13)
#define CMD_LOOP    (1u << 15)
/* Descriptor status (low 16 bits; the length is in the high 16). */
#define D_SOP   0x2000
#define D_EOP   0x4000
#define D_CRC   0x0040                  /* send: the controller appends the checksum */
#define D_QTAG  (0x3Fu << 7)
#define D_RXERR 0x001F                  /* receive: overrun, CRC, error, odd, too long */

#define NRX 64
#define NTX 64
#define BUFSZ 2048

static volatile uint8_t *g;             /* the registers */
static int irq, acked, phy_addr = 1, cur_mbps, cur_full;
static uint8_t *rxbuf, *txmem, *pad;    /* DMA memory */
static uint64_t rxbus, txbus, padbus;   /* ... as the controller sees it */
static uint16_t rx_cons, tx_prod;
static long irq_port;

static uint32_t rd(uint32_t o)             { return mmio_r32(g + o); }
static void     wr(uint32_t o, uint32_t v) { mmio_w32(g + o, v); }
static void desc(uint32_t blk, int i, uint64_t bus, uint32_t ls)
{
    wr(blk + i * 12 + 4, (uint32_t)bus);
    wr(blk + i * 12 + 8, (uint32_t)(bus >> 32));
    wr(blk + i * 12, ls);
}

/* ---- The PHY, over MDIO: start a transfer, wait until it is done. */
static int mdio(uint32_t cmd)
{
    wr(UMAC_MDIO, cmd | 1u << 29);
    for (int k = 0; k < 1000; k++) {
        uint32_t v = rd(UMAC_MDIO);
        if (!(v & 1u << 29)) return v & 1u << 28 ? -1 : (int)(v & 0xFFFF);   /* bit 28: read failed */
        nic_udelay(1);
    }
    return -1;
}
static int  mrd(int reg)        { return mdio(2u << 26 | (uint32_t)phy_addr << 21 | (uint32_t)reg << 16); }
static void mwr(int reg, int v) { mdio(1u << 26 | (uint32_t)phy_addr << 21 | (uint32_t)reg << 16 | (uint16_t)v); }
static const mdio_t bus = { mrd, mwr };

/* ---- Setup. */
static int dma_stop(uint32_t blk)
{
    wr(blk + CTL + C_CTRL, rd(blk + CTL + C_CTRL) & ~1u);
    for (int k = 0; k < 1000; k++) {
        if (rd(blk + CTL + C_STATUS) & 1) return 0;   /* "disabled" */
        nic_udelay(10);
    }
    return -1;
}

static int setup(nic_info_t *info, char *what, size_t cap)
{
    const void *f = nic_fdt();
    int n, len, spi, flags;
    uint64_t a, size;
    if (!f || (n = fdt_find(f, -1, "brcm,bcm2711-genet-v5")) < 0 || fdt_reg(f, n, 0, &a, &size)) return -1;
    if (fdt_spi(f, n, 0, &spi, &flags)) return -1;
    g = map_phys(a, size ? size : 0x10000);
    if ((long)g < 0) return -1;
    const uint8_t *m = fdt_prop(f, n, "local-mac-address", &len);   /* the firmware fills it in */
    if (m && len == 6 && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5])) memcpy(info->mac, m, 6);
    else if (nic_fw_mac(info->mac)) return -1;
    for (int p = -1; (p = fdt_find(f, p, "ethernet-phy-ieee802.3-c22")) >= 0; ) {   /* its MDIO address */
        const void *r = fdt_prop(f, p, "reg", &len);
        if (r && len == 4) { phy_addr = (int)fdt_be32(r); break; }
    }
    info->mtu = 1500;

    /* Memory: 64 receive buffers, the transmit area, small buffers for short frames. */
    uint64_t pa;
    rxbuf = dma_alloc(NRX * BUFSZ, &pa);
    if ((long)rxbuf < 0 || fdt_bus_addr(f, n, pa, &rxbus)) return -1;
    txmem = dma_alloc(NET_MAX, &pa);
    if ((long)txmem < 0 || fdt_bus_addr(f, n, pa, &txbus)) return -1;
    pad = dma_alloc(NTX * 64, &pa);
    if ((long)pad < 0 || fdt_bus_addr(f, n, pa, &padbus)) return -1;
    dma_sync(rxbuf, NRX * BUFSZ);                /* nothing of ours left in the caches */

    /* Quiet the DMA, reset the MAC (flush its buffers, pulse its reset). */
    if (dma_stop(RDMA) || dma_stop(TDMA)) return -1;
    wr(SYS_RBUF_FLUSH, 1); nic_udelay(10); wr(SYS_RBUF_FLUSH, 0); nic_udelay(10);
    wr(UMAC_CMD, 0);
    wr(UMAC_CMD, CMD_RESET | CMD_LOOP); nic_udelay(2); wr(UMAC_CMD, 0);
    wr(UMAC_MIB_CTRL, 7); wr(UMAC_MIB_CTRL, 0);  /* counters reset */
    wr(UMAC_MAX_FRAME, 1536);
    wr(RBUF_CTRL, rd(RBUF_CTRL) & ~3u);          /* frames start at offset 0, no status block */
    wr(TBUF_CTRL, rd(TBUF_CTRL) & ~1u);
    wr(RBUF_TBUF_SIZE, 1);
    wr(SYS_PORT_CTRL, 3);                        /* an external PHY, over RGMII */
    wr(EXT_RGMII_OOB, (rd(EXT_RGMII_OOB) & ~(1u << 5)) | 1u << 6);
    wr(UMAC_MAC0, (uint32_t)info->mac[0] << 24 | info->mac[1] << 16 | info->mac[2] << 8 | info->mac[3]);
    wr(UMAC_MAC1, (uint32_t)info->mac[4] << 8 | info->mac[5]);
    wr(IRQ_MASK_SET, 0xFFFFFFFF); wr(IRQ_CLEAR, 0xFFFFFFFF);

    /* The receive ring: descriptors 0-63, each a buffer. Pointers count words. */
    for (int i = 0; i < NRX; i++) desc(RDMA, i, rxbus + (uint64_t)i * BUFSZ, 0);
    wr(RDMA + RING + R_START, 0);      wr(RDMA + RING + R_START + 4, 0);
    wr(RDMA + RING + R_END, NRX * 3 - 1); wr(RDMA + RING + R_END + 4, 0);
    wr(RDMA + RING + R_RPTR, 0);       wr(RDMA + RING + R_RPTR + 4, 0);
    wr(RDMA + RING + R_WPTR, 0);       wr(RDMA + RING + R_WPTR + 4, 0);
    wr(RDMA + RING + R_PROD, 0);       wr(RDMA + RING + R_CONS, 0);
    wr(RDMA + RING + R_BUFSZ, (uint32_t)NRX << 16 | BUFSZ);
    wr(RDMA + RING + R_THRESH, 1);               /* an interrupt per frame (batched by us) */
    wr(RDMA + RING + R_XON, 5u << 16 | NRX >> 4);
    /* The transmit ring: descriptors 0-63, filled as frames come. */
    wr(TDMA + RING + R_START, 0);      wr(TDMA + RING + R_START + 4, 0);
    wr(TDMA + RING + R_END, NTX * 3 - 1); wr(TDMA + RING + R_END + 4, 0);
    wr(TDMA + RING + T_RPTR, 0);       wr(TDMA + RING + T_RPTR + 4, 0);
    wr(TDMA + RING + T_WPTR, 0);       wr(TDMA + RING + T_WPTR + 4, 0);
    wr(TDMA + RING + T_PROD, 0);       wr(TDMA + RING + T_CONS, 0);
    wr(TDMA + RING + R_BUFSZ, (uint32_t)NTX << 16 | BUFSZ);
    wr(TDMA + RING + T_FLOW, 0);
    rx_cons = tx_prod = 0;
    for (uint32_t blk = RDMA; blk <= TDMA; blk += TDMA - RDMA) {
        wr(blk + CTL + C_BURST, 8);
        wr(blk + CTL + C_RING_CFG, 1u << 16);    /* ring 16 on ... */
        wr(blk + CTL + C_CTRL, 1u | 1u << 17);   /* ... and the block */
    }

    /* The MAC on (gigabit full duplex until the PHY says otherwise); all
     * frames accepted (netd filters what is not for us: simpler and safer
     * than programming the address filter, and a switch sends little else). */
    wr(UMAC_CMD, CMD_TX_EN | CMD_RX_EN | 2u << 2 | CMD_PROMISC);
    irq = spi | ((flags & 4) ? IRQ_LEVEL : 0);
    irq_port = port_create(0);
    if (irq_bind(irq, irq_port)) return -1;
    wr(IRQ_MASK_CLEAR, 1u << 13);                /* receive buffer done */
    int id1 = mrd(PHY_ID1), id2 = mrd(PHY_ID2);
    phy_delays(&bus, f, n);
    phy_start(&bus);
    int major = (int)(rd(0) >> 24 & 15);          /* the revision register says 6 for version 5 */
    nic_fmt(what, cap, "GENET v%d (Raspberry Pi 4), PHY %04x:%04x at %d, interrupt %d", major == 6 ? 5 : major,
            id1 & 0xFFFF, id2 & 0xFFFF, phy_addr, spi);
    return 0;
}

/* ---- Receiving. */
static void rx_wait(void)
{
    msg_t m = { 0 };
    ipc_recv(irq_port, &m);
    uint32_t st = rd(IRQ_STAT) & ~rd(IRQ_MASK_STAT);
    wr(IRQ_CLEAR, st);
    acked = 0;
}

static size_t rx_take(uint8_t *batch, size_t cap)
{
    size_t n = 0;
    uint16_t prod = (uint16_t)rd(RDMA + RING + R_PROD);
    while (rx_cons != prod) {
        int i = rx_cons % NRX;
        uint32_t ls = rd(RDMA + i * 12), fl = ls >> 16;
        uint8_t *b = rxbuf + i * BUFSZ;
        if ((ls & (D_SOP | D_EOP)) == (D_SOP | D_EOP) && !(ls & D_RXERR) && fl >= 14 && fl <= BUFSZ) {
            if (n + 2 + fl > cap) break;         /* full: the rest goes in the next batch */
            dma_sync(b, fl);                     /* what the controller wrote, not old cache lines */
            batch[n] = (uint8_t)fl; batch[n + 1] = (uint8_t)(fl >> 8);
            memcpy(batch + n + 2, b, fl);
            n += 2 + fl;
        }
        rx_cons++;
        wr(RDMA + RING + R_CONS, rx_cons);       /* the buffer goes back to the controller */
    }
    if (!acked && rx_cons == (uint16_t)rd(RDMA + RING + R_PROD)) { irq_ack(irq & ~IRQ_LEVEL); acked = 1; }
    return n;
}

/* ---- Sending: one descriptor per frame; short frames are padded to 60
 * bytes in a small buffer of their own (the minimum Ethernet frame). */
static uint8_t *txbuf(void) { return txmem; }

static long send(size_t len)
{
    dma_sync(txmem, len);                        /* the frames, in memory for the controller */
    for (size_t off = 0; off + 2 <= len; ) {
        uint32_t fl = txmem[off] | txmem[off + 1] << 8;
        if (fl < 14 || fl > 1514 || off + 2 + fl > len) return -EINVAL;
        while ((uint16_t)(tx_prod - (uint16_t)rd(TDMA + RING + T_CONS)) >= NTX) sys_yield();   /* ring full */
        int i = tx_prod % NTX;
        uint64_t at = txbus + off + 2;
        if (fl < 60) {
            uint8_t *p = pad + i * 64;
            memcpy(p, txmem + off + 2, fl);
            memset(p + fl, 0, 60 - fl);
            dma_sync(p, 64);
            at = padbus + (uint64_t)i * 64;
            fl = 60;
        }
        desc(TDMA, i, at, fl << 16 | D_SOP | D_EOP | D_CRC | D_QTAG);
        tx_prod++;
        wr(TDMA + RING + T_PROD, tx_prod);
        off += 2 + (txmem[off] | txmem[off + 1] << 8);
    }
    while ((uint16_t)rd(TDMA + RING + T_CONS) != tx_prod) sys_yield();   /* all taken: txmem reusable */
    return 0;
}

/* ---- The cable: the MAC follows the speed and duplex the PHY agreed on. */
static int link(void)
{
    int mbps = 0, full = 0, up = phy_link(&bus, &mbps, &full);
    if (up && (mbps != cur_mbps || full != cur_full)) {
        uint32_t c = rd(UMAC_CMD) & ~(3u << 2 | CMD_HD);
        c |= (mbps == 1000 ? 2u : mbps == 100 ? 1u : 0u) << 2 | (full ? 0 : CMD_HD);
        wr(UMAC_CMD, c);
        wr(EXT_RGMII_OOB, (rd(EXT_RGMII_OOB) & ~(1u << 5)) | 1u << 4 | 1u << 6);   /* link on */
        printf("vnet: link up, %d Mb/s %s duplex\n", mbps, full ? "full" : "half");
        cur_mbps = mbps; cur_full = full;
    } else if (!up && cur_mbps) {
        wr(EXT_RGMII_OOB, rd(EXT_RGMII_OOB) & ~(1u << 4));
        printf("vnet: link down\n");
        cur_mbps = 0;
    }
    return up;
}

static void stop(void)
{
    wr(UMAC_CMD, rd(UMAC_CMD) & ~(CMD_TX_EN | CMD_RX_EN));
    dma_stop(RDMA);
    dma_stop(TDMA);
    wr(IRQ_MASK_SET, 0xFFFFFFFF);
}

const nic_t nic_genet = { "genet", setup, txbuf, send, rx_wait, rx_take, link, stop };
