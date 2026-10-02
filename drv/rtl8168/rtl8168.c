/*
 * rtl8168.c - Realtek RTL8111/8168/8411 Gigabit Ethernet (PCI 10ec:8168,
 * 10ec:8161): every card found is an interface of its own.
 *
 * The chip's generation is its XID (TxConfig bits 30:20, masked 0x7cf), as
 * Realtek's chips report it.  The 8168g and later ones (8168g, gu, h, ep,
 * fp, M, 8411b) are handed over from the firmware's out-of-band mode (MCU
 * NOW_IS_OOB, the link list), reach their PHY through OCP registers
 * instead of PHYAR, and gate receive DMA (MISC RXDV_GATED_EN) until the
 * driver opens it.  Older ones (8168b to 8168f) take the classic sequence.
 * The PHY is powered up and autonegotiates 10/100/1000; the PHY's
 * parameter patches (the rtl_nic firmware files) are not applied, which the chips do
 * without.
 *
 * Descriptor rings of 256 entries (16 bytes: opts1 with OWN, ring end,
 * first and last fragment and the length; opts2; the buffer's address) in
 * physical memory reached through the direct map, 2 KiB buffers.  Received
 * frames are taken by net_poll (the network timer, 100 Hz): no interrupts,
 * as the laptops' PCIe lines are above the 8259's.  Every step is logged
 * ("rtl8168: ..."), so a machine's report shows where a chip stopped.
 *
 * The registers and sequences are Realtek's chips' (as the open drivers
 * document them: Linux's r8169, the BSDs' re); the driver is written for
 * SIEOS.  Not tested on the hardware yet (QEMU has no such device).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "net.h"
#include "ddi.h"
#include "pci.h"
#include "arch.h"
#include "mm.h"
#include "random.h"

/* registers */
#define MAC0          0x00
#define MAC4          0x04
#define MAR0          0x08
#define TX_DESC_LO    0x20
#define TX_DESC_HI    0x24
#define CHIP_CMD      0x37
#define   CMD_RESET     0x10
#define   CMD_RX_EN     0x08
#define   CMD_TX_EN     0x04
#define TX_POLL       0x38
#define   TXPOLL_NPQ    0x40
#define INTR_MASK     0x3C
#define INTR_STATUS   0x3E
#define TX_CONFIG     0x40
#define   TXCFG_AUTO_FIFO (1U << 7)
#define   TXCFG_EMPTY   (1U << 11)
#define RX_CONFIG     0x44
#define   RX_ACCEPT_ALLPHYS 0x01
#define   RX_ACCEPT_MYPHYS  0x02
#define   RX_ACCEPT_MULTI   0x04
#define   RX_ACCEPT_BCAST   0x08
#define   RX_DMA_BURST  (7U << 8)        /* unlimited */
#define   RX_EARLY_OFF  (1U << 11)
#define   RX_FIFO_THRESH (7U << 13)
#define   RX_MULTI_EN   (1U << 14)
#define   RX_128_INT_EN (1U << 15)
#define CFG9346       0x50
#define   CFG9346_LOCK   0x00
#define   CFG9346_UNLOCK 0xC0
#define PHYAR         0x60
#define PHY_STATUS    0x6C
#define   PHYS_FULLDUP  0x01
#define   PHYS_LINK     0x02
#define   PHYS_10       0x04
#define   PHYS_100      0x08
#define   PHYS_1000     0x10
#define PMCH          0x6F
#define ERIDR         0x70
#define ERIAR         0x74
#define   ERIAR_FLAG    0x80000000U
#define   ERIAR_WRITE   0x80000000U
#define   ERIAR_MASK_0011 (0x3U << 12)
#define   ERIAR_MASK_1111 (0xFU << 12)
#define OCPDR         0xB0
#define GPHY_OCP      0xB8
#define   OCPAR_FLAG    0x80000000U
#define MCU           0xD3
#define   MCU_NOW_IS_OOB  0x80
#define   MCU_TX_EMPTY    0x20
#define   MCU_RX_EMPTY    0x10
#define   MCU_LINK_LIST_RDY 0x02
#define RX_MAX_SIZE   0xDA
#define CPLUS_CMD     0xE0
#define   CPLUS_RX_VLAN  0x0040
#define   CPLUS_RX_CSUM  0x0020
#define INTR_MITIGATE 0xE2
#define RX_DESC_LO    0xE4
#define RX_DESC_HI    0xE8
#define MAX_TX_PKT    0xEC
#define MISC          0xF0
#define   MISC_RXDV_GATED (1U << 19)

/* descriptors */
#define DESC_OWN      (1U << 31)
#define DESC_RING_END (1U << 30)
#define DESC_FIRST    (1U << 29)
#define DESC_LAST     (1U << 28)
#define RX_RES        (1U << 21)        /* receive error summary */
#define RX_LEN_MASK   0x3FFF

/* MII */
#define MII_BMCR      0
#define   BMCR_ANRESTART 0x0200
#define   BMCR_PDOWN     0x0800
#define   BMCR_ANENABLE  0x1000
#define MII_PHYID1    2
#define MII_PHYID2    3
#define MII_ADVERTISE 4
#define MII_CTRL1000  9

#define NRX 256
#define NTX 256
#define BUFSZ 2048

struct desc {
    volatile uint32_t opts1, opts2;
    volatile uint64_t addr;
};

enum { FAM_CLASSIC, FAM_G, FAM_H };   /* FAM_H: g and later with the PLL power-up of the h ones */

struct rtl {
    volatile uint8_t *regs;
    struct netif *ifp;
    struct desc *rx, *tx;
    uint8_t *rxbuf, *txbuf;
    uint64_t rxbuf_phys, txbuf_phys;
    uint32_t rx_next, tx_next;
    uint32_t xid;
    int family;
    const char *chip;
    bool link;
    uint64_t rx_frames, tx_frames;
};

#define MAX_CARDS 4
static struct rtl cards[MAX_CARDS];
static int ncards;

static inline uint8_t  rd8(struct rtl *c, uint32_t r)  { return *(volatile uint8_t *)(c->regs + r); }
static inline uint16_t rd16(struct rtl *c, uint32_t r) { return *(volatile uint16_t *)(c->regs + r); }
static inline uint32_t rd32(struct rtl *c, uint32_t r) { return *(volatile uint32_t *)(c->regs + r); }
static inline void wr8(struct rtl *c, uint32_t r, uint8_t v)   { *(volatile uint8_t *)(c->regs + r) = v; }
static inline void wr16(struct rtl *c, uint32_t r, uint16_t v) { *(volatile uint16_t *)(c->regs + r) = v; }
static inline void wr32(struct rtl *c, uint32_t r, uint32_t v) { *(volatile uint32_t *)(c->regs + r) = v; }

static void udelay(unsigned us)
{
    uint64_t end = hrtime() + (uint64_t)us * 1000;
    while (hrtime() < end)
        __asm__ volatile("pause");
}

/* Wait up to n * us for (reg32 & mask) to be set (want) or clear. */
static bool wait32(struct rtl *c, uint32_t reg, uint32_t mask, bool want, unsigned us, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (!!(rd32(c, reg) & mask) == want)
            return true;
        udelay(us);
    }
    return false;
}

static bool wait8(struct rtl *c, uint32_t reg, uint8_t mask, uint8_t want, unsigned us, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if ((rd8(c, reg) & mask) == want)
            return true;
        udelay(us);
    }
    return false;
}

/* ERI: the extended registers (the MAC address, FIFO sizes, the packet filter) */
static void eri_write(struct rtl *c, uint32_t addr, uint32_t mask, uint32_t v)
{
    wr32(c, ERIDR, v);
    wr32(c, ERIAR, ERIAR_WRITE | mask | addr);
    if (!wait32(c, ERIAR, ERIAR_FLAG, false, 100, 100))
        kprintf("rtl8168: ERI write %#x timed out\n", addr);
}

static uint32_t eri_read(struct rtl *c, uint32_t addr)
{
    wr32(c, ERIAR, ERIAR_MASK_1111 | addr);
    return wait32(c, ERIAR, ERIAR_FLAG, true, 100, 100) ? rd32(c, ERIDR) : ~0U;
}

static void eri_modify(struct rtl *c, uint32_t addr, uint32_t clear, uint32_t set)
{
    eri_write(c, addr, ERIAR_MASK_1111, (eri_read(c, addr) & ~clear) | set);
}

/* MAC OCP registers (8168g and later) */
static void mac_ocp_write(struct rtl *c, uint32_t reg, uint16_t v)
{
    wr32(c, OCPDR, OCPAR_FLAG | (reg << 15) | v);
}

static uint16_t mac_ocp_read(struct rtl *c, uint32_t reg)
{
    wr32(c, OCPDR, reg << 15);
    return (uint16_t)rd32(c, OCPDR);
}

/* the PHY: through OCP (8168g and later: standard registers at 0xA400 + 2 * reg), else PHYAR */
static void mdio_write(struct rtl *c, int reg, uint16_t v)
{
    if (c->family != FAM_CLASSIC) {
        wr32(c, GPHY_OCP, OCPAR_FLAG | ((0xA400U + reg * 2) << 15) | v);
        wait32(c, GPHY_OCP, OCPAR_FLAG, false, 25, 10);
    } else {
        wr32(c, PHYAR, 0x80000000U | ((uint32_t)(reg & 0x1F) << 16) | v);
        wait32(c, PHYAR, 0x80000000U, false, 25, 20);
        udelay(20);
    }
}

static int mdio_read(struct rtl *c, int reg)
{
    if (c->family != FAM_CLASSIC) {
        wr32(c, GPHY_OCP, (0xA400U + reg * 2) << 15);
        return wait32(c, GPHY_OCP, OCPAR_FLAG, true, 25, 10) ? (int)(rd32(c, GPHY_OCP) & 0xFFFF) : -1;
    }
    wr32(c, PHYAR, (uint32_t)(reg & 0x1F) << 16);
    int v = wait32(c, PHYAR, 0x80000000U, true, 25, 20) ? (int)(rd32(c, PHYAR) & 0xFFFF) : -1;
    udelay(20);
    return v;
}

/* The chip from its XID. */
static const struct { uint32_t xid; int family; const char *name; } chips[] = {
    { 0x6C0, FAM_H, "RTL8168M" },
    { 0x54B, FAM_H, "RTL8168fp" }, { 0x54A, FAM_H, "RTL8168fp" },
    { 0x541, FAM_H, "RTL8168h/8111h" },
    { 0x5C8, FAM_H, "RTL8411b" },
    { 0x502, FAM_H, "RTL8168ep" }, { 0x501, FAM_H, "RTL8168ep" }, { 0x500, FAM_H, "RTL8168ep" },
    { 0x509, FAM_G, "RTL8168gu" },
    { 0x4C1, FAM_G, "RTL8168g/8111g" }, { 0x4C0, FAM_G, "RTL8168g/8111g" },
    { 0x488, FAM_CLASSIC, "RTL8411" },
    { 0x481, FAM_CLASSIC, "RTL8168f" }, { 0x480, FAM_CLASSIC, "RTL8168f" },
    { 0x2C8, FAM_CLASSIC, "RTL8168evl" }, { 0x2C1, FAM_CLASSIC, "RTL8168e" },
    { 0x2C2, FAM_CLASSIC, "RTL8168e" },
    { 0x28A, FAM_CLASSIC, "RTL8168dp" }, { 0x288, FAM_CLASSIC, "RTL8168d" }, { 0x281, FAM_CLASSIC, "RTL8168d" },
    { 0x3C8, FAM_CLASSIC, "RTL8168cp" }, { 0x3C0, FAM_CLASSIC, "RTL8168c" }, { 0x3C4, FAM_CLASSIC, "RTL8168c" },
    { 0x380, FAM_CLASSIC, "RTL8168b" }, { 0x300, FAM_CLASSIC, "RTL8168b" },
};

static void identify(struct rtl *c)
{
    c->xid = (rd32(c, TX_CONFIG) >> 20) & 0x7CF;
    for (size_t i = 0; i < ARRAY_SIZE(chips); i++)
        if (chips[i].xid == c->xid) {
            c->family = chips[i].family;
            c->chip = chips[i].name;
            return;
        }
    /* unknown: the later ones (XID 0x4xx up, but the 8168f/8411 above) are the g kind */
    c->family = c->xid >= 0x4C0 ? FAM_G : FAM_CLASSIC;
    c->chip = "an unknown RTL8168";
}

/* 8168g and later: out of the firmware's out-of-band mode */
static void hw_init_g(struct rtl *c)
{
    wr32(c, MISC, rd32(c, MISC) | MISC_RXDV_GATED);
    if (!wait32(c, TX_CONFIG, TXCFG_EMPTY, true, 100, 42))
        kprintf("rtl8168: the transmit FIFO did not drain\n");
    if (!wait8(c, MCU, MCU_TX_EMPTY | MCU_RX_EMPTY, MCU_TX_EMPTY | MCU_RX_EMPTY, 100, 42))
        kprintf("rtl8168: the FIFOs did not drain (MCU %#x)\n", rd8(c, MCU));
    wr8(c, CHIP_CMD, rd8(c, CHIP_CMD) & ~(CMD_TX_EN | CMD_RX_EN));
    udelay(1000);
    wr8(c, MCU, rd8(c, MCU) & ~MCU_NOW_IS_OOB);
    mac_ocp_write(c, 0xE8DE, mac_ocp_read(c, 0xE8DE) & ~(1U << 14));
    if (!wait8(c, MCU, MCU_LINK_LIST_RDY, MCU_LINK_LIST_RDY, 100, 42))
        kprintf("rtl8168: the link list is not ready (1)\n");
    mac_ocp_write(c, 0xE8DE, mac_ocp_read(c, 0xE8DE) | (1U << 15));
    if (!wait8(c, MCU, MCU_LINK_LIST_RDY, MCU_LINK_LIST_RDY, 100, 42))
        kprintf("rtl8168: the link list is not ready (2)\n");
}

static bool chip_reset(struct rtl *c)
{
    wr8(c, CHIP_CMD, CMD_RESET);
    return wait8(c, CHIP_CMD, CMD_RESET, 0, 100, 100);
}

static bool valid_mac(const uint8_t m[6])
{
    return !(m[0] & 1) && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]);
}

static void read_mac(struct rtl *c, uint8_t mac[6])
{
    if (c->family != FAM_CLASSIC || c->xid == 0x2C8 || c->xid == 0x480 || c->xid == 0x481 || c->xid == 0x488) {
        uint32_t lo = eri_read(c, 0xE0), hi = eri_read(c, 0xE4);   /* the factory's (8168evl and later) */
        for (int i = 0; i < 4; i++)
            mac[i] = lo >> (8 * i);
        mac[4] = hi;
        mac[5] = hi >> 8;
        if (valid_mac(mac))
            return;
    }
    for (int i = 0; i < 6; i++)
        mac[i] = rd8(c, MAC0 + i);
}

static void set_mac(struct rtl *c, const uint8_t mac[6])
{
    wr8(c, CFG9346, CFG9346_UNLOCK);
    wr32(c, MAC4, mac[4] | (mac[5] << 8));
    rd32(c, MAC4);
    wr32(c, MAC0, mac[0] | (mac[1] << 8) | (mac[2] << 16) | ((uint32_t)mac[3] << 24));
    rd32(c, MAC0);
    wr8(c, CFG9346, CFG9346_LOCK);
}

static void phy_start(struct rtl *c)
{
    if (c->family == FAM_H) {                    /* the PLL powered up (8168h, ep, fp, M, 8411b) */
        wr8(c, PMCH, rd8(c, PMCH) | 0xC0);
        eri_modify(c, 0x1A8, 0, 0xFC000000);
    }
    int id1 = mdio_read(c, MII_PHYID1), id2 = mdio_read(c, MII_PHYID2);
    int bmcr = mdio_read(c, MII_BMCR);
    kprintf("rtl8168: PHY id %04x:%04x, BMCR %04x\n", id1 & 0xFFFF, id2 & 0xFFFF, bmcr & 0xFFFF);
    if (bmcr >= 0 && (bmcr & BMCR_PDOWN)) {
        mdio_write(c, MII_BMCR, bmcr & ~BMCR_PDOWN);
        udelay(20000);                           /* (the 8168h's PHY is not ready at once) */
    }
    if (c->family != FAM_CLASSIC) {              /* the PHY's state (OCP 0xA420): 3 = LAN on */
        uint32_t st = 0;
        for (int i = 0; i < 100; i++) {
            wr32(c, GPHY_OCP, 0xA420U << 15);
            if (wait32(c, GPHY_OCP, OCPAR_FLAG, true, 25, 10) && ((st = rd32(c, GPHY_OCP)) & 7) == 3)
                break;
            udelay(1000);
        }
        if ((st & 7) != 3)
            kprintf("rtl8168: the PHY's state is %u, not 3 (LAN on)\n", st & 7);
    }
    mdio_write(c, MII_ADVERTISE, 0x01E1);        /* 10/100, half and full duplex (selector 802.3) */
    mdio_write(c, MII_CTRL1000, 0x0300);         /* 1000BASE-T, full and half duplex */
    mdio_write(c, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
}

static void hw_start(struct rtl *c)
{
    wr8(c, CFG9346, CFG9346_UNLOCK);
    wr16(c, CPLUS_CMD, rd16(c, CPLUS_CMD) & ~(CPLUS_RX_VLAN | CPLUS_RX_CSUM | 0x0003));
    wr16(c, INTR_MITIGATE, 0);
    if (c->family != FAM_CLASSIC) {
        eri_write(c, 0xC8, ERIAR_MASK_0011, (0x08U << 16) | 0x10);   /* the FIFOs: receive, transmit */
        eri_write(c, 0xE8, ERIAR_MASK_0011, (0x02U << 16) | 0x06);   /* (high, low) */
        eri_modify(c, 0xDC, 1, 0);               /* the packet filter reset */
        eri_modify(c, 0xDC, 0, 1);
        wr32(c, MISC, rd32(c, MISC) & ~MISC_RXDV_GATED);
        eri_write(c, 0xC0, ERIAR_MASK_0011, 0);
        eri_write(c, 0xB8, ERIAR_MASK_0011, 0);
    }
    wr16(c, RX_MAX_SIZE, BUFSZ);
    wr8(c, MAX_TX_PKT, 0x3F);                    /* (8064 bytes / 128) */
    uint64_t tx_pa = V2P(c->tx), rx_pa = V2P(c->rx);
    wr32(c, TX_DESC_HI, tx_pa >> 32);
    wr32(c, TX_DESC_LO, (uint32_t)tx_pa);
    wr32(c, RX_DESC_HI, rx_pa >> 32);
    wr32(c, RX_DESC_LO, (uint32_t)rx_pa);
    wr8(c, CFG9346, CFG9346_LOCK);
    rd8(c, CHIP_CMD);
    wr8(c, CHIP_CMD, CMD_TX_EN | CMD_RX_EN);

    uint32_t rxcfg = c->family != FAM_CLASSIC ? RX_128_INT_EN | RX_MULTI_EN | RX_DMA_BURST | RX_EARLY_OFF
                   : c->xid >= 0x2C0 ? RX_128_INT_EN | RX_MULTI_EN | RX_DMA_BURST
                   : RX_FIFO_THRESH | RX_DMA_BURST;
    wr32(c, RX_CONFIG, rxcfg | RX_ACCEPT_MYPHYS | RX_ACCEPT_BCAST | RX_ACCEPT_MULTI);
    wr32(c, MAR0, 0xFFFFFFFF);                   /* every multicast (IPv6 neighbour discovery) */
    wr32(c, MAR0 + 4, 0xFFFFFFFF);
    uint32_t txcfg = (7U << 8) | (3U << 24);     /* unlimited DMA bursts, the standard inter-frame gap */
    if (c->family != FAM_CLASSIC || c->xid == 0x2C8 || c->xid == 0x480 || c->xid == 0x481 || c->xid == 0x488)
        txcfg |= TXCFG_AUTO_FIFO;
    wr32(c, TX_CONFIG, txcfg);
    wr16(c, INTR_MASK, 0);                       /* polled */
    wr16(c, INTR_STATUS, 0xFFFF);
}

static int rtl_send(struct netif *ifp, const void *frame, size_t len)
{
    struct rtl *c = ifp->drv;
    if (len > BUFSZ)
        return -EMSGSIZE;
    struct desc *d = &c->tx[c->tx_next];
    if (d->opts1 & DESC_OWN)
        return -EAGAIN;                          /* ring full */
    uint8_t *buf = c->txbuf + c->tx_next * BUFSZ;
    memcpy(buf, frame, len);
    if (len < 60) {                              /* pad runt frames */
        memset(buf + len, 0, 60 - len);
        len = 60;
    }
    d->addr = c->txbuf_phys + c->tx_next * BUFSZ;
    d->opts2 = 0;
    __sync_synchronize();
    d->opts1 = DESC_OWN | DESC_FIRST | DESC_LAST | (c->tx_next == NTX - 1 ? DESC_RING_END : 0) | (uint32_t)len;
    __sync_synchronize();
    c->tx_next = (c->tx_next + 1) % NTX;
    wr8(c, TX_POLL, TXPOLL_NPQ);
    c->tx_frames++;
    return 0;
}

static void rx_give(struct rtl *c, uint32_t i)
{
    c->rx[i].addr = c->rxbuf_phys + i * BUFSZ;
    __sync_synchronize();
    c->rx[i].opts1 = DESC_OWN | (i == NRX - 1 ? DESC_RING_END : 0) | BUFSZ;
}

static void link_check(struct rtl *c)
{
    uint8_t s = rd8(c, PHY_STATUS);
    bool up = s & PHYS_LINK;
    if (up != c->link) {
        c->link = up;
        if (up)
            kprintf("rtl8168: %s: link up, %s Mb/s %s duplex\n", c->ifp->name,
                    s & PHYS_1000 ? "1000" : s & PHYS_100 ? "100" : s & PHYS_10 ? "10" : "?",
                    s & PHYS_FULLDUP ? "full" : "half");
        else
            kprintf("rtl8168: %s: link down\n", c->ifp->name);
    }
}

static void rtl_poll(struct netif *ifp)
{
    struct rtl *c = ifp->drv;
    uint16_t st = rd16(c, INTR_STATUS);
    if (st)
        wr16(c, INTR_STATUS, st);                /* (polled: the causes acknowledged) */
    if (st & 0x0020 || !(ticks % TIMER_HZ))      /* LinkChg, else once a second */
        link_check(c);
    for (int budget = 0; budget < NRX; budget++) {
        struct desc *d = &c->rx[c->rx_next];
        uint32_t o = d->opts1;
        if (o & DESC_OWN)
            break;
        __sync_synchronize();
        uint32_t len = o & RX_LEN_MASK;
        if ((o & (DESC_FIRST | DESC_LAST)) == (DESC_FIRST | DESC_LAST) && !(o & RX_RES) && len > 4) {
            net_rx(ifp, c->rxbuf + c->rx_next * BUFSZ, len - 4);   /* (without the frame check sequence) */
            c->rx_frames++;
        } else {
            ifp->rx_dropped++;
        }
        rx_give(c, c->rx_next);
        c->rx_next = (c->rx_next + 1) % NRX;
    }
    if (st & 0x0050)                             /* RxOverflow, RxFIFOOver: receiving goes on */
        wr8(c, CHIP_CMD, CMD_TX_EN | CMD_RX_EN);
}

static bool rtl_link(struct netif *ifp)
{
    struct rtl *c = ifp->drv;
    return rd8(c, PHY_STATUS) & PHYS_LINK;
}

static const struct nic_ops rtl_ops = { "rtl8168", rtl_send, rtl_poll, rtl_link };

/* PCI power management: the device in D0 (firmware may leave it in D3hot) */
static void pci_d0(const struct pci_dev *pd)
{
    uint8_t cap = pci_find_cap(pd, 0x01, 0);
    if (!cap)
        return;
    uint32_t v = pci_read32(pd->bus, pd->dev, pd->func, cap + 4);
    if (v & 3) {
        kprintf("rtl8168: the device was in D%u: to D0\n", v & 3);
        pci_write32(pd->bus, pd->dev, pd->func, cap + 4, v & ~3U);
        udelay(10000);
    }
}

static bool rtl_start(struct rtl *c, const struct pci_dev *pd)
{
    int bar = -1;
    uint64_t addr = 0;
    for (int i = 0; i < 6 && bar < 0; i++) {     /* the first memory BAR (BAR 2 on the 8168) */
        bool io = false;
        uint64_t a = pci_bar_addr(pd, i, &io);
        if (a && !io)
            bar = i, addr = a;
    }
    if (bar < 0) {
        kprintf("rtl8168: %02x:%02x.%u: no memory BAR\n", pd->bus, pd->dev, pd->func);
        return false;
    }
    pci_d0(pd);
    pci_enable_path(pd);
    pci_enable_bus_master(pd);
    uint64_t size = pci_bar_size(pd, bar);
    c->regs = mmio_map(addr, size && size < 0x10000 ? size : 0x1000);
    if (!c->regs)
        return false;
    if (rd32(c, TX_CONFIG) == 0xFFFFFFFF) {
        kprintf("rtl8168: %02x:%02x.%u: the registers read all ones\n", pd->bus, pd->dev, pd->func);
        return false;
    }
    identify(c);
    kprintf("rtl8168: %02x:%02x.%u: %s (XID %03x, rev %02x), registers at %#lx (BAR %d)\n",
            pd->bus, pd->dev, pd->func, c->chip, c->xid, pd->revision, (unsigned long)addr, bar);

    wr16(c, INTR_MASK, 0);
    wr16(c, INTR_STATUS, 0xFFFF);
    if (c->family != FAM_CLASSIC)
        hw_init_g(c);
    if (!chip_reset(c))
        kprintf("rtl8168: the reset did not complete\n");

    uint8_t mac[6];
    read_mac(c, mac);
    if (!valid_mac(mac)) {
        kprintf("rtl8168: no MAC address: a random one\n");
        random_bytes(mac, 6);
        mac[0] = (mac[0] & 0xFE) | 0x02;         /* locally administered, unicast */
    }
    set_mac(c, mac);

    /* rings and buffers */
    uint64_t rx_pa = pmm_alloc(), tx_pa = pmm_alloc();
    c->rxbuf_phys = pmm_alloc_contig(NRX * BUFSZ / PAGE_SIZE);
    c->txbuf_phys = pmm_alloc_contig(NTX * BUFSZ / PAGE_SIZE);
    if (!rx_pa || !tx_pa || !c->rxbuf_phys || !c->txbuf_phys) {
        kprintf("rtl8168: out of memory for the rings\n");
        return false;
    }
    c->rx = P2V(rx_pa);
    c->tx = P2V(tx_pa);
    c->rxbuf = P2V(c->rxbuf_phys);
    c->txbuf = P2V(c->txbuf_phys);
    memset(c->tx, 0, NTX * sizeof(struct desc));
    for (uint32_t i = 0; i < NRX; i++)
        rx_give(c, i);
    c->rx_next = c->tx_next = 0;

    hw_start(c);
    phy_start(c);
    c->ifp = netif_register(&rtl_ops, c, mac);
    if (!c->ifp)
        return false;
    kprintf("rtl8168: %s: %02x:%02x:%02x:%02x:%02x:%02x, MISC %08x, PHY status %02x\n", c->ifp->name,
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], rd32(c, MISC), rd8(c, PHY_STATUS));
    return true;
}

static void rtl_probe(void)
{
    static const uint16_t ids[] = { 0x8168, 0x8161 };
    struct pci_dev pd[MAX_CARDS];
    int n = pci_find_all(0x10EC, ids, ARRAY_SIZE(ids), pd, MAX_CARDS);
    for (int i = 0; i < n && ncards < MAX_CARDS; i++)
        if (rtl_start(&cards[ncards], &pd[i])) {
            pci_claim(&pd[i], "rtl8168");
            ncards++;
        }
}

DDI_DRIVER("rtl8168", DDI_PHASE_ROOT, "Realtek RTL8111/8168/8411 Gigabit Ethernet");
DDI_ALIAS("pci10ec,8168");
DDI_ALIAS("pci10ec,8161");

int _init(void)
{
    rtl_probe();
    return 0;
}
