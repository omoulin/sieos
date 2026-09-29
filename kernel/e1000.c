/*
 * e1000.c - Intel 8254x (e1000) Ethernet driver: every card found is an
 * interface of its own.
 *
 * Descriptor rings live in physical memory reached through the direct
 * map.  Received frames are taken at the card's interrupt (receive timer,
 * overrun, descriptors low, link change) when its PCI line has a handler;
 * the network timer (net_poll) polls the ring as well, so nothing waits
 * for long if an interrupt is lost.
 */
#include "net.h"
#include "pci.h"
#include "arch.h"
#include "mm.h"

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_IMS    0x00D0
#define REG_IMC    0x00D8
#define REG_RDTR   0x2820
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define CTRL_SLU  (1 << 6)
#define CTRL_RST  (1 << 26)
#define RCTL_EN   (1 << 1)
#define RCTL_BAM  (1 << 15)
#define RCTL_MPE  (1 << 4)       /* all multicast: IPv6 neighbor discovery */
#define RCTL_SECRC (1 << 26)
#define TCTL_EN   (1 << 1)
#define TCTL_PSP  (1 << 3)

#define NRX 64
#define NTX 64
#define BUFSZ 2048

struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

struct e1000 {
    volatile uint8_t *regs;
    struct netif *ifp;
    struct rx_desc *rx;
    struct tx_desc *tx;
    uint8_t *rxbuf, *txbuf;
    uint64_t rxbuf_phys, txbuf_phys;
    uint32_t rx_next, tx_next;
    int irq;                                     /* 0: polled only */
    uint64_t interrupts;
};

#define MAX_CARDS 4
static struct e1000 cards[MAX_CARDS];
static int ncards;

static inline uint32_t rd(struct e1000 *c, uint32_t r) { return *(volatile uint32_t *)(c->regs + r); }
static inline void wr(struct e1000 *c, uint32_t r, uint32_t v) { *(volatile uint32_t *)(c->regs + r) = v; }

static uint16_t eeprom_read(struct e1000 *c, uint8_t addr)
{
    wr(c, REG_EERD, ((uint32_t)addr << 8) | 1);
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(c, REG_EERD);
        if (v & (1 << 4))
            return v >> 16;
    }
    return 0;
}

static int e1000_send(struct netif *ifp, const void *frame, size_t len)
{
    struct e1000 *c = ifp->drv;
    if (len > BUFSZ)
        return -EMSGSIZE;
    struct tx_desc *d = &c->tx[c->tx_next];
    if (d->cmd && !(d->status & 1))
        return -EAGAIN;                          /* ring full */
    memcpy(c->txbuf + c->tx_next * BUFSZ, frame, len);
    d->addr = c->txbuf_phys + c->tx_next * BUFSZ;
    d->length = len < 60 ? 60 : len;             /* pad runt frames */
    if (len < 60)
        memset(c->txbuf + c->tx_next * BUFSZ + len, 0, 60 - len);
    d->cmd = (1 << 0) | (1 << 1) | (1 << 3);     /* EOP | IFCS | RS */
    d->status = 0;
    c->tx_next = (c->tx_next + 1) % NTX;
    wr(c, REG_TDT, c->tx_next);
    return 0;
}

static void e1000_poll(struct netif *ifp)
{
    struct e1000 *c = ifp->drv;
    for (int budget = 0; budget < NRX; budget++) {
        struct rx_desc *d = &c->rx[c->rx_next];
        if (!(d->status & 1))
            break;
        if ((d->status & 2) && !d->errors)
            net_rx(ifp, c->rxbuf + c->rx_next * BUFSZ, d->length);
        else
            ifp->rx_dropped++;
        d->status = 0;
        wr(c, REG_RDT, c->rx_next);              /* give the descriptor back */
        c->rx_next = (c->rx_next + 1) % NRX;
    }
}

#define ICR_LSC    (1 << 2)
#define ICR_RXDMT0 (1 << 4)
#define ICR_RXO    (1 << 6)
#define ICR_RXT0   (1 << 7)

static void e1000_irq(struct trapframe *tf, void *arg)
{
    (void)tf;
    struct e1000 *c = arg;
    uint32_t icr = rd(c, REG_ICR);               /* reading acknowledges (the line goes quiet) */
    if (!icr)
        return;                                  /* not ours (a shared line) */
    c->interrupts++;
    if (icr & (ICR_RXT0 | ICR_RXO | ICR_RXDMT0))
        e1000_poll(c->ifp);
}

static bool e1000_link(struct netif *ifp)
{
    return rd(ifp->drv, REG_STATUS) & 2;
}

static const struct nic_ops e1000_ops = { "e1000", e1000_send, e1000_poll, e1000_link };

static bool e1000_start(struct e1000 *c, const struct pci_dev *pd)
{
    uint64_t bar = pci_bar_addr(pd, 0, NULL);
    if (!bar)
        return false;
    pci_enable_bus_master(pd);
    c->regs = mmio_map(bar, 128 * 1024);
    if (!c->regs)
        return false;

    wr(c, REG_IMC, 0xFFFFFFFF);                  /* polled: no interrupts */
    wr(c, REG_CTRL, rd(c, REG_CTRL) | CTRL_RST);
    for (int i = 0; i < 100000 && (rd(c, REG_CTRL) & CTRL_RST); i++)
        io_wait();
    wr(c, REG_IMC, 0xFFFFFFFF);
    rd(c, REG_ICR);
    wr(c, REG_CTRL, rd(c, REG_CTRL) | CTRL_SLU);

    /* MAC address from the EEPROM, programmed into receive address 0. */
    uint8_t mac[6];
    for (int i = 0; i < 3; i++) {
        uint16_t w = eeprom_read(c, i);
        mac[i * 2] = w & 0xFF;
        mac[i * 2 + 1] = w >> 8;
    }
    if (!(mac[0] | mac[1] | mac[2])) {           /* no EEPROM: keep RA */
        uint32_t lo = rd(c, REG_RAL), hi = rd(c, REG_RAH);
        for (int i = 0; i < 4; i++)
            mac[i] = lo >> (8 * i);
        mac[4] = hi;
        mac[5] = hi >> 8;
    }
    wr(c, REG_RAL, mac[0] | (mac[1] << 8) | (mac[2] << 16) | ((uint32_t)mac[3] << 24));
    wr(c, REG_RAH, mac[4] | (mac[5] << 8) | (1U << 31));
    for (int i = 0; i < 128; i++)
        wr(c, REG_MTA + i * 4, 0);

    /* Rings and buffers */
    uint64_t rx_pa = pmm_alloc(), tx_pa = pmm_alloc();
    c->rxbuf_phys = pmm_alloc_contig(NRX * BUFSZ / PAGE_SIZE);
    c->txbuf_phys = pmm_alloc_contig(NTX * BUFSZ / PAGE_SIZE);
    if (!rx_pa || !tx_pa || !c->rxbuf_phys || !c->txbuf_phys)
        return false;
    c->rx = P2V(rx_pa);
    c->tx = P2V(tx_pa);
    c->rxbuf = P2V(c->rxbuf_phys);
    c->txbuf = P2V(c->txbuf_phys);
    for (int i = 0; i < NRX; i++) {
        c->rx[i].addr = c->rxbuf_phys + i * BUFSZ;
        c->rx[i].status = 0;
    }
    memset(c->tx, 0, NTX * sizeof(*c->tx));

    wr(c, REG_RDBAL, rx_pa & 0xFFFFFFFF);
    wr(c, REG_RDBAH, rx_pa >> 32);
    wr(c, REG_RDLEN, NRX * sizeof(struct rx_desc));
    wr(c, REG_RDH, 0);
    wr(c, REG_RDT, NRX - 1);
    c->rx_next = 0;
    wr(c, REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_MPE | RCTL_SECRC);      /* 2048-byte buffers */

    wr(c, REG_TDBAL, tx_pa & 0xFFFFFFFF);
    wr(c, REG_TDBAH, tx_pa >> 32);
    wr(c, REG_TDLEN, NTX * sizeof(struct tx_desc));
    wr(c, REG_TDH, 0);
    wr(c, REG_TDT, 0);
    c->tx_next = 0;
    wr(c, REG_TCTL, TCTL_EN | TCTL_PSP | (0x10 << 4) | (0x40 << 12));
    wr(c, REG_TIPG, 0x0060200A);

    c->ifp = netif_register(&e1000_ops, c, mac);
    if (!c->ifp)
        return false;
    if (pd->irq && pd->irq < 16 && irq_register_shared(pd->irq, e1000_irq, c)) {
        wr(c, REG_RDTR, 0);                      /* interrupts on the card's PCI line */
        rd(c, REG_ICR);
        wr(c, REG_IMS, ICR_RXT0 | ICR_RXO | ICR_RXDMT0 | ICR_LSC);
        c->irq = pd->irq;
    }
    return true;
}

void e1000_probe(void)
{
    static const uint16_t ids[] = { 0x100E, 0x100F, 0x10D3, 0x1004, 0x1015, 0x153A };
    struct pci_dev pd[MAX_CARDS];
    int n = pci_find_all(0x8086, ids, ARRAY_SIZE(ids), pd, MAX_CARDS);
    for (int i = 0; i < n && ncards < MAX_CARDS; i++)
        if (e1000_start(&cards[ncards], &pd[i])) {
            pci_claim(&pd[i], "e1000");
            ncards++;
        }
}
