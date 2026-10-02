/*
 * virtio_net.c - virtio network cards (virtio 1.0 over PCI): QEMU's
 * virtio-net-pci, and the same device in other hypervisors.  Every card is
 * an interface of its own.
 *
 * The "modern" interface only: the common, notify, ISR and device
 * configuration structures found through the PCI vendor capabilities (a
 * card without them, legacy-only, is skipped).  Two split virtqueues,
 * receive (0) and transmit (1), of up to 64 buffers of 2 KB, each buffer
 * a virtio_net_hdr (12 bytes) and a frame.  Features: VERSION_1 and MAC,
 * nothing offloaded.  Interrupts on the card's INTx line (shared), with
 * polling from net_poll as for the e1000.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "smp.h"
#include "net.h"
#include "ddi.h"
#include "pci.h"
#include "arch.h"
#include "mm.h"

#define CAP_COMMON 1
#define CAP_NOTIFY 2
#define CAP_ISR    3
#define CAP_DEVICE 4

/* the common configuration structure (virtio 1.0, 4.1.4.3) */
#define C_DFSELECT   0x00
#define C_DF         0x04
#define C_GFSELECT   0x08
#define C_GF         0x0C
#define C_MSIX       0x10
#define C_NUMQ       0x12
#define C_STATUS     0x14
#define C_Q_SELECT   0x16
#define C_Q_SIZE     0x18
#define C_Q_MSIX     0x1A
#define C_Q_ENABLE   0x1C
#define C_Q_NOFF     0x1E
#define C_Q_DESC     0x20
#define C_Q_DRIVER   0x28
#define C_Q_DEVICE   0x30

#define ST_ACK        1
#define ST_DRIVER     2
#define ST_DRIVER_OK  4
#define ST_FEATURES_OK 8
#define ST_FAILED     128

#define F_MAC       (1ULL << 5)
#define F_VERSION_1 (1ULL << 32)

#define DESC_F_WRITE 2
#define QMAX  64
#define BUFSZ 2048
#define HDR   12                                 /* struct virtio_net_hdr with num_buffers */

struct vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;
} __attribute__((packed));

struct vq {
    int size;
    volatile struct vq_desc *desc;
    volatile uint16_t *avail;                    /* flags, idx, ring[size] */
    volatile uint32_t *used;                     /* flags|idx, then (id, len) pairs */
    uint16_t last_used;
    uint16_t next;                               /* transmit: the next buffer */
    int inflight;
    volatile uint16_t *notify;
    int index;
    uint8_t *buf;
    uint64_t buf_phys;
};

struct vnet {
    volatile uint8_t *common, *isr, *devcfg;
    volatile uint8_t *notify_base;
    uint32_t notify_mult;
    struct vq rx, tx;
    struct netif *ifp;
    int irq;
    uint64_t interrupts;
    struct spinlock rxlock;                      /* the receive queue: its interrupt, and the network thread's poll */
};

#define MAX_CARDS 4
static struct vnet cards[MAX_CARDS];
static int ncards;

static inline void mb(void) { __sync_synchronize(); }
static inline uint8_t  c8(struct vnet *v, int o) { return v->common[o]; }
static inline uint16_t c16(struct vnet *v, int o) { return *(volatile uint16_t *)(v->common + o); }
static inline uint32_t c32(struct vnet *v, int o) { return *(volatile uint32_t *)(v->common + o); }
static inline void w8(struct vnet *v, int o, uint8_t x) { v->common[o] = x; }
static inline void w16(struct vnet *v, int o, uint16_t x) { *(volatile uint16_t *)(v->common + o) = x; }
static inline void w32(struct vnet *v, int o, uint32_t x) { *(volatile uint32_t *)(v->common + o) = x; }
static inline void w64(struct vnet *v, int o, uint64_t x)
{
    w32(v, o, (uint32_t)x);
    w32(v, o + 4, (uint32_t)(x >> 32));
}

static uint16_t avail_idx(struct vq *q) { return q->avail[1]; }
static uint16_t used_idx(struct vq *q) { return (uint16_t)(q->used[0] >> 16); }

static void kick(struct vq *q)
{
    mb();
    *q->notify = (uint16_t)q->index;
}

/* Offer buffer id (a whole descriptor) to the device. */
static void offer(struct vq *q, uint16_t id)
{
    uint16_t i = avail_idx(q);
    q->avail[2 + i % q->size] = id;
    mb();
    q->avail[1] = i + 1;
}

static bool setup_queue(struct vnet *v, struct vq *q, int index, bool receive)
{
    w16(v, C_Q_SELECT, index);
    int max = c16(v, C_Q_SIZE);
    if (!max)
        return false;
    q->size = MIN(max, QMAX);
    q->index = index;
    uint64_t ring = pmm_alloc();                 /* desc 0..1023, avail 1024.., used 2048.. */
    q->buf_phys = pmm_alloc_contig(q->size * BUFSZ / PAGE_SIZE);
    if (!ring || !q->buf_phys)
        return false;
    uint8_t *r = P2V(ring);
    memset(r, 0, PAGE_SIZE);
    q->desc = (volatile struct vq_desc *)r;
    q->avail = (volatile uint16_t *)(r + 1024);
    q->used = (volatile uint32_t *)(r + 2048);
    q->buf = P2V(q->buf_phys);
    for (int i = 0; i < q->size; i++) {
        q->desc[i].addr = q->buf_phys + (uint64_t)i * BUFSZ;
        q->desc[i].len = BUFSZ;
        q->desc[i].flags = receive ? DESC_F_WRITE : 0;
        q->desc[i].next = 0;
    }
    w16(v, C_Q_SIZE, q->size);
    w16(v, C_Q_MSIX, 0xFFFF);                    /* no MSI-X vector: INTx */
    w64(v, C_Q_DESC, ring);
    w64(v, C_Q_DRIVER, ring + 1024);
    w64(v, C_Q_DEVICE, ring + 2048);
    q->notify = (volatile uint16_t *)(v->notify_base + (uint32_t)c16(v, C_Q_NOFF) * v->notify_mult);
    w16(v, C_Q_ENABLE, 1);
    if (receive) {
        for (int i = 0; i < q->size; i++)
            offer(q, i);
    }
    return true;
}

static int vnet_send(struct netif *ifp, const void *frame, size_t len)
{
    struct vnet *v = ifp->drv;
    struct vq *q = &v->tx;
    if (len + HDR > BUFSZ)
        return -EMSGSIZE;
    while (q->last_used != used_idx(q)) {        /* buffers the device has sent */
        q->last_used++;
        q->inflight--;
    }
    if (q->inflight >= q->size)
        return -EAGAIN;
    uint16_t id = q->next;
    q->next = (q->next + 1) % q->size;
    uint8_t *b = q->buf + (size_t)id * BUFSZ;
    memset(b, 0, HDR);                           /* no offloads */
    memcpy(b + HDR, frame, len);
    q->desc[id].len = HDR + len;
    q->inflight++;
    offer(q, id);
    kick(q);
    return 0;
}

static void vnet_poll(struct netif *ifp)
{
    struct vnet *v = ifp->drv;
    struct vq *q = &v->rx;
    int got = 0;
    spin_lock(&v->rxlock);
    for (int budget = 0; budget < q->size && q->last_used != used_idx(q); budget++) {
        mb();
        uint16_t slot = q->last_used % q->size;
        uint32_t id = q->used[1 + 2 * slot], len = q->used[2 + 2 * slot];
        q->last_used++;
        if (id < (uint32_t)q->size && len > HDR && len <= BUFSZ)
            net_rx(ifp, q->buf + (size_t)id * BUFSZ + HDR, len - HDR);
        else
            ifp->rx_dropped++;
        if (id < (uint32_t)q->size)
            offer(q, (uint16_t)id);              /* the buffer goes back to the device */
        got++;
    }
    if (got)
        kick(q);
    spin_unlock(&v->rxlock);
}

static void vnet_irq(struct trapframe *tf, void *arg)
{
    (void)tf;
    struct vnet *v = arg;
    uint8_t isr = *v->isr;                       /* reading acknowledges */
    if (!(isr & 1))
        return;                                  /* not ours (a shared line) */
    v->interrupts++;
    vnet_poll(v->ifp);
}

static const struct nic_ops vnet_ops = { "virtio-net", vnet_send, vnet_poll, NULL };

/* The structure of a vendor capability (cfg_type), mapped; NULL if the card has none. */
static volatile uint8_t *find_cfg(const struct pci_dev *pd, int type, uint32_t *mult)
{
    for (uint8_t cap = pci_find_cap(pd, 0x09, 0); cap; cap = pci_find_cap(pd, 0x09, cap)) {
        if (pci_read8(pd, cap + 3) != type)
            continue;
        int bar = pci_read8(pd, cap + 4);
        if (bar > 5)
            continue;
        uint32_t off = pci_read32(pd->bus, pd->dev, pd->func, cap + 8);
        uint32_t len = pci_read32(pd->bus, pd->dev, pd->func, cap + 12);
        bool io;
        uint64_t base = pci_bar_addr(pd, bar, &io);
        if (io || !base || !len)
            continue;
        if (mult)
            *mult = pci_read32(pd->bus, pd->dev, pd->func, cap + 16);
        return mmio_map(base + off, len);
    }
    return NULL;
}

static bool vnet_start(struct vnet *v, const struct pci_dev *pd)
{
    pci_enable_bus_master(pd);
    v->common = find_cfg(pd, CAP_COMMON, NULL);
    v->notify_base = find_cfg(pd, CAP_NOTIFY, &v->notify_mult);
    v->isr = find_cfg(pd, CAP_ISR, NULL);
    v->devcfg = find_cfg(pd, CAP_DEVICE, NULL);
    if (!v->common || !v->notify_base || !v->isr) {
        kprintf("virtio-net %02x:%02x.%d: no virtio 1.0 interface (legacy only): skipped\n", pd->bus, pd->dev,
                pd->func);
        return false;
    }
    w8(v, C_STATUS, 0);                          /* reset */
    for (int i = 0; i < 100000 && c8(v, C_STATUS); i++)
        io_wait();
    w8(v, C_STATUS, ST_ACK);
    w8(v, C_STATUS, ST_ACK | ST_DRIVER);
    w32(v, C_DFSELECT, 0);
    uint64_t have = c32(v, C_DF);
    w32(v, C_DFSELECT, 1);
    have |= (uint64_t)c32(v, C_DF) << 32;
    uint64_t want = have & (F_MAC | F_VERSION_1);
    if (!(want & F_VERSION_1)) {
        w8(v, C_STATUS, ST_FAILED);
        return false;
    }
    w32(v, C_GFSELECT, 0);
    w32(v, C_GF, (uint32_t)want);
    w32(v, C_GFSELECT, 1);
    w32(v, C_GF, (uint32_t)(want >> 32));
    w8(v, C_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
    if (!(c8(v, C_STATUS) & ST_FEATURES_OK)) {
        w8(v, C_STATUS, ST_FAILED);
        return false;
    }
    w16(v, C_MSIX, 0xFFFF);
    if (c16(v, C_NUMQ) < 2 || !setup_queue(v, &v->rx, 0, true) || !setup_queue(v, &v->tx, 1, false)) {
        w8(v, C_STATUS, ST_FAILED);
        return false;
    }
    uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x56, 0x4E, (uint8_t)(0x10 + ncards) };
    if ((want & F_MAC) && v->devcfg)
        for (int i = 0; i < 6; i++)
            mac[i] = v->devcfg[i];
    v->ifp = netif_register(&vnet_ops, v, mac);
    if (!v->ifp) {
        w8(v, C_STATUS, ST_FAILED);
        return false;
    }
    w8(v, C_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);
    kick(&v->rx);
    if (pd->irq && pd->irq < 16 && irq_register_shared(pd->irq, vnet_irq, v))
        v->irq = pd->irq;
    return true;
}

void virtio_net_probe(void)
{
    static const uint16_t ids[] = { 0x1000, 0x1041 };      /* transitional, modern */
    struct pci_dev pd[MAX_CARDS];
    int n = pci_find_all(0x1AF4, ids, ARRAY_SIZE(ids), pd, MAX_CARDS);
    for (int i = 0; i < n && ncards < MAX_CARDS; i++)
        if (vnet_start(&cards[ncards], &pd[i])) {
            pci_claim(&pd[i], "virtio-net");
            ncards++;
        }
}

DDI_DRIVER("virtio_net", DDI_PHASE_ROOT, "virtio network device (QEMU, KVM)");
DDI_ALIAS("pci1af4,1000");
DDI_ALIAS("pci1af4,1041");

int _init(void)
{
    virtio_net_probe();
    return 0;
}
