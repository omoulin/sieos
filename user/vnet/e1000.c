/*
 * e1000.c - vnet's backend (nic.h, x86-64) for Intel's gigabit cards as QEMU
 * emulates them: the 82540EM ("e1000", QEMU's default card on x86-64's "pc"
 * machine), the 82545EM, and the 82574L ("e1000e", the default on "q35").
 * The same registers and the same "legacy" descriptors serve all three:
 * a ring of 64 receive buffers of 2 KiB and a ring of 256 transmit
 * descriptors, in DMA memory, on the card's PCI line (lib/pci.c). The card
 * interrupts when frames came; the line is acknowledged once all are taken,
 * as with virtio. The cable: STATUS, read once a second by vnet's watcher.
 * The card filters what it receives: our address and broadcasts.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "nic.h"
#include "pci.h"

enum { CTRL = 0x0000, STATUS = 0x0008, ICR = 0x00C0, IMS = 0x00D0, IMC = 0x00D8, RCTL = 0x0100,
       TCTL = 0x0400, TIPG = 0x0410, RDBAL = 0x2800, RDBAH = 0x2804, RDLEN = 0x2808, RDH = 0x2810,
       RDT = 0x2818, TDBAL = 0x3800, TDBAH = 0x3804, TDLEN = 0x3808, TDH = 0x3810, TDT = 0x3818,
       MTA = 0x5200, RAL = 0x5400, RAH = 0x5404 };
#define CTRL_RST (1u << 26)
#define NRX 64
#define NTX 256
#define BUFSZ 2048

typedef struct { uint64_t addr; uint16_t len, csum; uint8_t status, err; uint16_t special; } rxd_t;
typedef struct { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; } txd_t;
enum { DD = 1, EOP = 2 };                        /* status: descriptor done, end of packet */

static volatile uint8_t *regs;
static volatile rxd_t *rx;
static volatile txd_t *tx;
static uint8_t *rxbuf, *txmem;
static uint64_t txmem_pa;
static unsigned rx_next, tx_tail;
static int irq, acked;
static long irq_port;

static uint32_t rd(int r)              { return mmio_r32(regs + r); }
static void     wr(int r, uint32_t v)  { mmio_w32(regs + r, v); }

static const struct { uint16_t id; const char *name; } models[] = {
    { 0x100E, "82540EM" }, { 0x100F, "82545EM" }, { 0x10D3, "82574L" },
};

static int e_setup(nic_info_t *info, char *what, size_t cap)
{
    pci_t p;
    size_t k = 0;
    while (k < sizeof models / sizeof models[0] && pci_scan(&p, 0, 0x8086u | (uint32_t)models[k].id << 16, ~0u, 0)) k++;
    uint64_t len, pa, rx_pa, tx_pa, rxbuf_pa;
    if (k == sizeof models / sizeof models[0] || !(pa = pci_bar(&p, 0, &len))) return -1;
    if ((long)(regs = map_phys(pa, len < 0x20000 ? len : 0x20000)) < 0) return -1;
    wr(IMC, ~0u);
    wr(CTRL, rd(CTRL) | CTRL_RST);               /* reset: the address comes back from the EEPROM */
    for (int i = 0; i < 100 && rd(CTRL) & CTRL_RST; i++) sys_sleep(1000000);
    wr(IMC, ~0u);
    rd(ICR);
    wr(CTRL, rd(CTRL) | 1u << 6 | 1u << 5);      /* set link up, speed from the PHY */
    uint32_t lo = rd(RAL), hi = rd(RAH);
    for (int i = 0; i < 6; i++) info->mac[i] = (uint8_t)((i < 4 ? lo >> 8 * i : hi >> 8 * (i - 4)));
    wr(RAH, hi | 1u << 31);                      /* (address valid) */
    info->mtu = 1500;
    for (int i = 0; i < 128; i++) wr(MTA + 4 * i, 0);   /* no multicast */

    rx = dma_alloc(4096, &rx_pa);
    tx = dma_alloc(NTX * sizeof(txd_t), &tx_pa);
    rxbuf = dma_alloc(NRX * BUFSZ, &rxbuf_pa);
    txmem = dma_alloc(NET_MAX, &txmem_pa);
    if ((long)rx < 0 || (long)tx < 0 || (long)rxbuf < 0 || (long)txmem < 0) return -1;
    rx_pa += p.bus_off; tx_pa += p.bus_off; txmem_pa += p.bus_off;
    for (int i = 0; i < NRX; i++) rx[i] = (rxd_t){ .addr = rxbuf_pa + p.bus_off + (uint64_t)i * BUFSZ };
    wr(RDBAL, (uint32_t)rx_pa); wr(RDBAH, (uint32_t)(rx_pa >> 32));
    wr(RDLEN, NRX * sizeof(rxd_t));
    wr(RDH, 0); wr(RDT, NRX - 1);                /* the card owns all but one */
    wr(TDBAL, (uint32_t)tx_pa); wr(TDBAH, (uint32_t)(tx_pa >> 32));
    wr(TDLEN, NTX * sizeof(txd_t));
    wr(TDH, 0); wr(TDT, 0);
    wr(TIPG, 10 | 8 << 10 | 6 << 20);            /* the inter-packet gaps (IEEE 802.3) */
    wr(TCTL, 1u << 1 | 1u << 3 | 0x10 << 4 | 0x40 << 12);   /* on, pad short frames, collisions */
    wr(RCTL, 1u << 1 | 1u << 15 | 1u << 26);     /* on, broadcasts, strip the CRC; 2 KiB buffers */
    irq = p.irq;
    irq_port = port_create(0);
    if (irq < 0 || irq_bind(irq, irq_port)) return -1;
    wr(IMS, 1u << 7 | 1u << 6 | 1u << 4);        /* frames received, overrun, ring running low */
    nic_fmt(what, cap, "Intel %s, interrupt line %d", models[k].name, irq & ~IRQ_LEVEL);
    return 0;
}

static uint8_t *e_txbuf(void) { return txmem; }

static void e_rx_wait(void)
{
    msg_t m = { 0 };
    ipc_recv(irq_port, &m);
    rd(ICR);                                     /* (reading clears it and lowers the line) */
    acked = 0;
}

static size_t e_rx_take(uint8_t *batch, size_t cap)
{
    size_t n = 0;
    int given = 0;
    volatile rxd_t *d;
    while (__atomic_load_n(&(d = &rx[rx_next])->status, __ATOMIC_ACQUIRE) & DD) {
        uint32_t fl = d->len;
        if (d->status & EOP && !d->err && fl >= 14 && fl <= BUFSZ) {
            if (n + 2 + fl > cap) break;         /* full: the rest goes in the next batch */
            batch[n] = (uint8_t)fl; batch[n + 1] = (uint8_t)(fl >> 8);
            memcpy(batch + n + 2, rxbuf + rx_next * BUFSZ, fl);
            n += 2 + fl;
        }
        d->status = 0;
        rx_next = (rx_next + 1) % NRX;
        given = 1;
    }
    if (given) { dma_wmb(); wr(RDT, (rx_next + NRX - 1) % NRX); }   /* the buffers go back to the card */
    if (!acked && !(rx[rx_next].status & DD)) {  /* all taken: the line may fire again */
        irq_ack(irq & ~IRQ_LEVEL);
        acked = 1;
    }
    return n;
}

/* One descriptor per frame, NTX - 1 at most at a time; the reply waits
 * until the card has sent them (the last one's DD). */
static long e_send(size_t len)
{
    unsigned queued = 0, start = tx_tail;
    for (size_t off = 0; off + 2 <= len; ) {
        uint32_t fl = txmem[off] | txmem[off + 1] << 8;
        if (fl < 14 || fl > 1514 || off + 2 + fl > len) { tx_tail = start; return -EINVAL; }
        tx[tx_tail] = (txd_t){ .addr = txmem_pa + off + 2, .len = (uint16_t)fl, .cmd = 0x0B };   /* end of packet, add the CRC, report */
        volatile txd_t *last = &tx[tx_tail];
        tx_tail = (tx_tail + 1) % NTX;
        off += 2 + fl;
        if (++queued == NTX - 1 || off + 2 > len) {
            dma_wmb();
            wr(TDT, tx_tail);
            while (!(__atomic_load_n(&last->status, __ATOMIC_ACQUIRE) & DD)) sys_yield();
            queued = 0;
            start = tx_tail;
        }
    }
    return 0;
}

static int e_link(void) { return rd(STATUS) & 2 ? 1 : 0; }

static void e_stop(void)
{
    wr(IMC, ~0u);
    wr(RCTL, 0); wr(TCTL, 0);
    wr(CTRL, rd(CTRL) | CTRL_RST);               /* no more DMA into our memory */
}

const nic_t nic_e1000 = { "e1000", e_setup, e_txbuf, e_send, e_rx_wait, e_rx_take, e_link, e_stop };
