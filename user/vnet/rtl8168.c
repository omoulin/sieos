/*
 * rtl8168.c - vnet's backend (nic.h, x86-64) for Realtek's RTL8168h/8111h
 * (PCI 10ec:8168, revision 0x15: the target laptop's), after Linux's r8169
 * (rtl_hw_init_8168g, rtl_hw_start_8168h_1) less what only tunes: the
 * PHY's firmware patch and analog settings, the PCIe PHY's, EEE, power
 * saving. Other chips of the 10ec:8168 family are named by their XID (in
 * TxConfig) and left alone.
 *   Setup: PCIe power states (ASPM) off, as r8169 does; the chip taken back
 *   from its out-of-band firmware; reset; the MAC from the eFuse (ERI 0xE0);
 *   rings of 64 receive buffers of 2 KiB and 256 transmit descriptors (16
 *   bytes each, the "own" bit says whose); the PHY, inside the chip, is
 *   reached through the OCP bus (standard registers at 0xA400 + 2 * reg).
 *   Polled (vnet's receiver): the laptop's interrupt line comes from ACPI,
 *   which SIEOS does not read. The cable: PHYstatus, once a second.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "nic.h"
#include "pci.h"

enum { MAC0 = 0x00, MAC4 = 0x04, TXDESC = 0x20, CMD = 0x37, TXPOLL = 0x38, IMR = 0x3C, ISR = 0x3E,
       TXCFG = 0x40, RXCFG = 0x44, CFG9346 = 0x50, PHYSTATUS = 0x6C, ERIDR = 0x70, ERIAR = 0x74,
       OCPDR = 0xB0, GPHY_OCP = 0xB8, MCU = 0xD3, RXMAXSIZE = 0xDA, RXDESC = 0xE4, MAXTXPKT = 0xEC,
       MISC = 0xF0 };
enum { CMD_RST = 0x10, CMD_RX = 0x08, CMD_TX = 0x04 };
#define BUSY     (1u << 31)                      /* ERIAR, OCP: access in progress / write */
#define RXDV_GATE (1u << 19)                     /* MISC: receive path closed */
#define OWN (1u << 31)                           /* descriptor: the chip's */
#define EOR (1u << 30)                           /* last of its ring */
#define FS  (1u << 29)                           /* first and last segment of a frame */
#define LS  (1u << 28)
#define RES (1u << 21)                           /* receive error */
#define NRX 64
#define NTX 256
#define BUFSZ 2048
#define R8(r)  (*(volatile uint8_t *)(regs + (r)))
#define R16(r) (*(volatile uint16_t *)(regs + (r)))
#define R32(r) (*(volatile uint32_t *)(regs + (r)))

typedef struct { uint32_t opts1, opts2; uint64_t addr; } desc_t;

static volatile uint8_t *regs;
static volatile desc_t *rx, *tx;
static uint8_t *rxbuf, *txmem;
static uint64_t txmem_pa;
static unsigned rx_next, tx_tail;
static int was;                                  /* PHYstatus last seen */

/* Wait (~20 ms at most) until the register's bits m read want. 0 if so. */
static int until(int r, int byte, uint32_t m, uint32_t want)
{
    for (int i = 0; i < 1000; i++, sys_sleep(20000))
        if (((byte ? R8(r) : R32(r)) & m) == want) return 0;
    return -1;
}

/* The chip's internal buses: ERI (the MAC's settings, 32 bits), MAC OCP,
 * PHY OCP (16 bits). */
static uint32_t eri(int a, int write, uint32_t v)
{
    if (write) R32(ERIDR) = v;
    R32(ERIAR) = (write ? BUSY : 0) | 0xF << 12 | a;   /* all four bytes */
    until(ERIAR, 0, BUSY, write ? 0 : BUSY);
    return R32(ERIDR);
}
static void mac_ocp(int a, uint32_t clr, uint32_t set)
{
    R32(OCPDR) = (uint32_t)a << 15;
    R32(OCPDR) = BUSY | (uint32_t)a << 15 | ((R32(OCPDR) & 0xFFFF & ~clr) | set);
}
static int phy_rd(int r)
{
    R32(GPHY_OCP) = (0xA400u + 2 * r) << 15;
    return until(GPHY_OCP, 0, BUSY, BUSY) ? -1 : (int)(R32(GPHY_OCP) & 0xFFFF);
}
static void phy_wr(int r, int v)
{
    R32(GPHY_OCP) = BUSY | (0xA400u + 2 * r) << 15 | (uint32_t)v;
    until(GPHY_OCP, 0, BUSY, 0);
}

static int r_setup(nic_info_t *info, char *what, size_t cap)
{
    pci_t p;
    uint64_t len, pa, rx_pa, tx_pa, rxbuf_pa;
    if (pci_scan(&p, 0, 0x816810ECu, ~0u, 0) || !(pa = pci_bar(&p, 2, &len))) return -1;
    if ((long)(regs = map_phys(pa, 4096)) < 0) return -1;
    uint32_t xid = R32(TXCFG) >> 20 & 0xFCF;
    if ((xid & 0x7CE) != 0x540) {
        printf("vnet: Realtek 8168 with XID %03x: not an 8168h, left alone\n", xid);
        return -1;
    }
    R16(IMR) = 0; R16(ISR) = 0xFFFF;             /* polled: no interrupts */
    for (int c = pci_cfg(&p, 0x34) & 0xFC; c; c = pci_cfg(&p, c) >> 8 & 0xFC)
        if ((pci_cfg(&p, c) & 0xFF) == 0x10) {   /* PCIe: Link Control's ASPM L0s/L1 off */
            pci_cfgw(&p, c + 0x10, pci_cfg(&p, c + 0x10) & ~3u);
            break;
        }
    R32(MISC) |= RXDV_GATE;                      /* out of band (firmware, DASH) back to us */
    sys_sleep(2000000);
    until(TXCFG, 0, 1u << 11, 1u << 11);         /* FIFOs empty */
    until(MCU, 1, 0x30, 0x30);
    R8(CMD) &= ~(CMD_RX | CMD_TX);
    sys_sleep(1000000);
    R8(MCU) &= ~0x80;                            /* "now is OOB" off */
    mac_ocp(0xE8DE, 1u << 14, 0);
    until(MCU, 1, 2, 2);                         /* (link list ready) */
    mac_ocp(0xE8DE, 0, 1u << 15);
    until(MCU, 1, 2, 2);
    R8(CMD) = CMD_RST;
    if (until(CMD, 1, CMD_RST, 0)) { printf("vnet: RTL8168h: the reset does not finish\n"); return -1; }

    uint32_t lo = eri(0xE0, 0, 0), hi = eri(0xE4, 0, 0) & 0xFFFF;
    if (!(lo | hi) || lo & 1) { lo = R32(MAC0); hi = R32(MAC4) & 0xFFFF; }   /* none: the registers' */
    for (int i = 0; i < 6; i++) info->mac[i] = (uint8_t)(i < 4 ? lo >> 8 * i : hi >> 8 * (i - 4));
    info->mtu = 1500;
    R8(CFG9346) = 0xC0;                          /* configuration registers writable */
    R32(MAC4) = hi; R32(MAC0) = lo;
    eri(0xC8, 1, 0x08 << 16 | 0x02);             /* FIFO sizes, receive and transmit (r8169's) */
    eri(0xE8, 1, 0x10 << 16 | 0x06);
    uint32_t pf = eri(0xDC, 0, 0);               /* packet filter: reset */
    eri(0xDC, 1, pf & ~1u); eri(0xDC, 1, pf | 1);
    R32(MISC) &= ~RXDV_GATE;
    R8(MAXTXPKT) = 0x27;                         /* early transmit threshold (r8169's "EarlySize") */
    R16(RXMAXSIZE) = BUFSZ;

    rx = dma_alloc(4096, &rx_pa);
    tx = dma_alloc(NTX * sizeof(desc_t), &tx_pa);
    rxbuf = dma_alloc(NRX * BUFSZ, &rxbuf_pa);
    txmem = dma_alloc(NET_MAX, &txmem_pa);
    if ((long)rx < 0 || (long)tx < 0 || (long)rxbuf < 0 || (long)txmem < 0) return -1;
    for (int i = 0; i < NRX; i++) rx[i] = (desc_t){ OWN | (i == NRX - 1 ? EOR : 0) | BUFSZ, 0, rxbuf_pa + (uint64_t)i * BUFSZ };
    tx[NTX - 1].opts1 = EOR;
    R32(TXDESC + 4) = (uint32_t)(tx_pa >> 32); R32(TXDESC) = (uint32_t)tx_pa;
    R32(RXDESC + 4) = (uint32_t)(rx_pa >> 32); R32(RXDESC) = (uint32_t)rx_pa;
    R8(CFG9346) = 0;
    R8(CMD) = CMD_TX | CMD_RX;
    R32(RXCFG) = 1u << 15 | 1u << 14 | 1u << 11 | 7 << 8 | 0x08 | 0x02;   /* unlimited bursts; broadcasts, ours */
    R32(TXCFG) = 3u << 24 | 7 << 8 | 1u << 7;    /* shortest gap, unlimited bursts, automatic FIFO */

    mdio_t bus = { phy_rd, phy_wr };
    int id1 = phy_rd(PHY_ID1), id2 = phy_rd(PHY_ID2);
    phy_start(&bus);
    nic_fmt(what, cap, "Realtek RTL8168h/8111h (XID %03x), PHY %04x:%04x, polled", xid, id1 & 0xFFFF, id2 & 0xFFFF);
    return 0;
}

static uint8_t *r_txbuf(void) { return txmem; }

static size_t r_rx_take(uint8_t *batch, size_t cap)
{
    size_t n = 0;
    volatile desc_t *d;
    uint32_t s;
    while (!((s = __atomic_load_n(&(d = &rx[rx_next])->opts1, __ATOMIC_ACQUIRE)) & OWN)) {
        uint32_t fl = (s & 0x3FFF) - 4;          /* (the length counts the CRC) */
        if ((s & (FS | LS | RES)) == (FS | LS) && fl >= 14 && fl <= BUFSZ - 4) {
            if (n + 2 + fl > cap) break;         /* full: the rest goes in the next batch */
            batch[n] = (uint8_t)fl; batch[n + 1] = (uint8_t)(fl >> 8);
            memcpy(batch + n + 2, rxbuf + rx_next * BUFSZ, fl);
            n += 2 + fl;
        }
        __atomic_store_n(&d->opts1, OWN | (rx_next == NRX - 1 ? EOR : 0) | BUFSZ, __ATOMIC_RELEASE);
        rx_next = (rx_next + 1) % NRX;
    }
    return n;
}

/* One descriptor per frame, NTX - 1 at most at a time; the reply waits
 * until the chip has sent them (the last one's "own" bit clear). */
static long r_send(size_t len)
{
    unsigned queued = 0, start = tx_tail;
    for (size_t off = 0; off + 2 <= len; ) {
        uint32_t fl = txmem[off] | txmem[off + 1] << 8;
        if (fl < 14 || fl > 1514 || off + 2 + fl > len) {
            while (tx_tail != start) tx[tx_tail = (tx_tail + NTX - 1) % NTX].opts1 &= EOR;   /* not sent */
            return -EINVAL;
        }
        volatile desc_t *d = &tx[tx_tail];
        d->addr = txmem_pa + off + 2;
        __atomic_store_n(&d->opts1, OWN | FS | LS | (d->opts1 & EOR) | fl, __ATOMIC_RELEASE);
        tx_tail = (tx_tail + 1) % NTX;
        off += 2 + fl;
        if (++queued == NTX - 1 || off + 2 > len) {
            R8(TXPOLL) = 0x40;                   /* look at the ring */
            while (__atomic_load_n(&d->opts1, __ATOMIC_ACQUIRE) & OWN) sys_yield();
            queued = 0;
            start = tx_tail;
        }
    }
    return 0;
}

static int r_link(void)
{
    int s = R8(PHYSTATUS);
    if ((s ^ was) & 0x1F) {                      /* link, speed or duplex changed */
        if (s & 2) printf("vnet: link up, %d Mb/s %s duplex\n", s & 0x10 ? 1000 : s & 8 ? 100 : 10, s & 1 ? "full" : "half");
        else if (was & 2) printf("vnet: link down\n");
        was = s;
    }
    return s >> 1 & 1;
}

static void r_stop(void)
{
    R32(MISC) |= RXDV_GATE;
    sys_sleep(2000000);
    R8(CMD) = CMD_RST;                           /* no more DMA into our memory */
}

const nic_t nic_rtl8168 = { "rtl8168", r_setup, r_txbuf, r_send, 0, r_rx_take, r_link, r_stop };
