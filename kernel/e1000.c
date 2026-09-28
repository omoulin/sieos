/*
 * e1000.c - Intel 8254x (e1000) Ethernet driver.
 *
 * Descriptor rings live in physical memory reached through the direct
 * map; the receive ring is polled from the network timer (net_poll), so
 * no interrupt routing is needed.
 */
#include "net.h"
#include "pci.h"
#include "mm.h"

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_IMC    0x00D8
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

static volatile uint8_t *regs;
static struct rx_desc *rx;
static struct tx_desc *tx;
static uint8_t *rxbuf, *txbuf;
static uint64_t rxbuf_phys, txbuf_phys;
static uint32_t rx_next, tx_next;

static inline uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(regs + r); }
static inline void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(regs + r) = v; }

static uint16_t eeprom_read(uint8_t addr)
{
    wr(REG_EERD, ((uint32_t)addr << 8) | 1);
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(REG_EERD);
        if (v & (1 << 4))
            return v >> 16;
    }
    return 0;
}

static int e1000_send(const void *frame, size_t len)
{
    if (len > BUFSZ)
        return -EMSGSIZE;
    struct tx_desc *d = &tx[tx_next];
    if (d->cmd && !(d->status & 1))
        return -EAGAIN;                          /* ring full */
    memcpy(txbuf + tx_next * BUFSZ, frame, len);
    d->addr = txbuf_phys + tx_next * BUFSZ;
    d->length = len < 60 ? 60 : len;             /* pad runt frames */
    if (len < 60)
        memset(txbuf + tx_next * BUFSZ + len, 0, 60 - len);
    d->cmd = (1 << 0) | (1 << 1) | (1 << 3);     /* EOP | IFCS | RS */
    d->status = 0;
    tx_next = (tx_next + 1) % NTX;
    wr(REG_TDT, tx_next);
    return 0;
}

static void e1000_poll(void)
{
    for (int budget = 0; budget < NRX; budget++) {
        struct rx_desc *d = &rx[rx_next];
        if (!(d->status & 1))
            break;
        if ((d->status & 2) && !d->errors)
            net_rx(rxbuf + rx_next * BUFSZ, d->length);
        else
            netif.rx_dropped++;
        d->status = 0;
        wr(REG_RDT, rx_next);                    /* give the descriptor back */
        rx_next = (rx_next + 1) % NRX;
    }
}

static bool e1000_link(void)
{
    return rd(REG_STATUS) & 2;
}

static const struct nic_ops e1000_ops = { "e1000", e1000_send, e1000_poll, e1000_link };

const struct nic_ops *e1000_probe(void)
{
    static const uint16_t ids[] = { 0x100E, 0x100F, 0x10D3, 0x1004, 0x1015, 0x153A };
    struct pci_dev pd;
    if (!pci_find(0x8086, ids, ARRAY_SIZE(ids), &pd))
        return NULL;
    uint64_t bar = pd.bar[0] & ~0xFUL;
    if ((pd.bar[0] & 0x6) == 0x4)                /* 64-bit BAR */
        bar |= (uint64_t)pd.bar[1] << 32;
    if (!bar || bar >= DIRECT_MAP_SIZE)
        return NULL;
    pci_enable_bus_master(&pd);
    vmm_set_uncached(bar);
    regs = P2V(bar);

    wr(REG_IMC, 0xFFFFFFFF);                     /* polled: no interrupts */
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    for (int i = 0; i < 100000 && (rd(REG_CTRL) & CTRL_RST); i++)
        io_wait();
    wr(REG_IMC, 0xFFFFFFFF);
    rd(REG_ICR);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_SLU);

    /* MAC address from the EEPROM, programmed into receive address 0. */
    for (int i = 0; i < 3; i++) {
        uint16_t w = eeprom_read(i);
        netif.mac[i * 2] = w & 0xFF;
        netif.mac[i * 2 + 1] = w >> 8;
    }
    if (!(netif.mac[0] | netif.mac[1] | netif.mac[2])) {         /* no EEPROM: keep RA */
        uint32_t lo = rd(REG_RAL), hi = rd(REG_RAH);
        for (int i = 0; i < 4; i++)
            netif.mac[i] = lo >> (8 * i);
        netif.mac[4] = hi;
        netif.mac[5] = hi >> 8;
    }
    wr(REG_RAL, netif.mac[0] | (netif.mac[1] << 8) | (netif.mac[2] << 16) | ((uint32_t)netif.mac[3] << 24));
    wr(REG_RAH, netif.mac[4] | (netif.mac[5] << 8) | (1U << 31));
    for (int i = 0; i < 128; i++)
        wr(REG_MTA + i * 4, 0);

    /* Rings and buffers */
    uint64_t rx_pa = pmm_alloc(), tx_pa = pmm_alloc();
    rxbuf_phys = pmm_alloc_contig(NRX * BUFSZ / PAGE_SIZE);
    txbuf_phys = pmm_alloc_contig(NTX * BUFSZ / PAGE_SIZE);
    if (!rx_pa || !tx_pa || !rxbuf_phys || !txbuf_phys)
        return NULL;
    rx = P2V(rx_pa);
    tx = P2V(tx_pa);
    rxbuf = P2V(rxbuf_phys);
    txbuf = P2V(txbuf_phys);
    for (int i = 0; i < NRX; i++) {
        rx[i].addr = rxbuf_phys + i * BUFSZ;
        rx[i].status = 0;
    }
    memset(tx, 0, NTX * sizeof(*tx));

    wr(REG_RDBAL, rx_pa & 0xFFFFFFFF);
    wr(REG_RDBAH, rx_pa >> 32);
    wr(REG_RDLEN, NRX * sizeof(struct rx_desc));
    wr(REG_RDH, 0);
    wr(REG_RDT, NRX - 1);
    rx_next = 0;
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);      /* 2048-byte buffers */

    wr(REG_TDBAL, tx_pa & 0xFFFFFFFF);
    wr(REG_TDBAH, tx_pa >> 32);
    wr(REG_TDLEN, NTX * sizeof(struct tx_desc));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    tx_next = 0;
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10 << 4) | (0x40 << 12));
    wr(REG_TIPG, 0x0060200A);
    return &e1000_ops;
}
