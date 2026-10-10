/*
 * vnet - The network card server: port "nic0", whatever the card is. It
 * tries its backends in turn (nic.h) and serves with the first that finds a
 * card: virtio (QEMU: "-device virtio-net-pci,disable-modern=on" on x86-64,
 * "-device virtio-net-device" on arm64's virt), the Raspberry Pi 4's GENET
 * (genet.c), the Raspberry Pi 5's RP1 Ethernet (gem.c).
 *
 *   Receiving: a second thread waits for frames (rx_wait), packs a batch
 *   (rx_take) and sends it to the network server (port "net", NET_FRAMES):
 *   one message per batch, not per frame.
 *
 *   Sending: the network server sends a batch of frames (NIC_SEND). They
 *   arrive by IPC straight into the backend's DMA memory (txbuf), where the
 *   card reads them: no copy of our own.
 *
 *   The cable: for cards that have one, a third thread checks the link once
 *   a second and tells the network server when it comes up or goes down
 *   (NET_LINK), so it asks DHCP again after a cable is plugged in.
 *
 * It is also SIEOS's clock-chip reader for now (NIC_INFO.rtc): the real-time
 * clock gives the date, which TLS needs to check certificates (x86-64: the
 * CMOS, I/O ports 0x70/0x71; arm64: a PL031, found in the device tree; the
 * Raspberry Pis have none: 0, see docs/net.md); there is no separate clock
 * driver yet.
 *
 * The virtio backend is below; user/lib/virtio.c has the transports and
 * user/vblk/vblk.c explains virtqueues.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "nic.h"
#include "virtio.h"
#if defined(__aarch64__)
#include "mk/fdt.h"
#include "mbox.h"
#endif

/* ---- virtio: two virtqueues, 0 receives, 1 transmits. Every frame travels
 * with a small virtio header (10 bytes, all zeros: no offloads, the network
 * server computes its own checksums). */
enum { D_NEXT = 1, D_WRITE = 2 };
#define F_MAC (1u << 5)
#define HDR 10                                   /* the legacy virtio-net header */
#define NRX 64                                   /* receive buffers */
#define BUFSZ 2048                               /* one receive buffer: header + frame */

typedef struct { uint64_t addr; uint32_t len; uint16_t flags, next; } desc_t;
typedef struct {                                 /* one virtqueue, legacy layout */
    uint16_t size, last;                         /* entries; used entries already seen */
    desc_t *desc;
    volatile uint16_t *avail;                    /* flags, idx, ring[size] */
    volatile uint16_t *used;                     /* flags, idx, then {u32 id, u32 len}[size] */
} vq_t;

static vdev_t dev;
static vq_t rxq, txq;
static uint8_t *rxbuf;                           /* NRX buffers of BUFSZ (DMA) */
static uint64_t rxbuf_pa;
static uint8_t *txmem;                           /* where NIC_SEND's frames land (DMA) */
static uint64_t txmem_pa, zero_pa;               /* zero_pa: a zeroed header for every frame */
static long irq_port;
static int acked;                                /* this interrupt already acknowledged */

/* printf into a buffer (always 0-terminated). */
struct sbuf { char *p; size_t n, cap; };
static void sput(void *ctx, char c) { struct sbuf *b = ctx; if (b->n + 1 < b->cap) b->p[b->n++] = c; }
void nic_fmt(char *out, size_t cap, const char *fmt, ...)
{
    struct sbuf b = { out, 0, cap };
    va_list ap;
    va_start(ap, fmt);
    vformat(sput, &b, fmt, ap);
    va_end(ap);
    if (cap) out[b.n] = 0;
}

static int vq_setup(vq_t *q, int index)
{
    q->size = vdev_qmax(&dev, index);
    if (!q->size) return -1;
    uint64_t pa, pg = 4096;
    uint64_t used_off = (16UL * q->size + 6 + 2UL * q->size + pg - 1) & ~(pg - 1);
    char *ring = dma_alloc(used_off + ((6 + 8UL * q->size + pg - 1) & ~(pg - 1)), &pa);
    if ((long)ring < 0) return -1;
    q->desc = (desc_t *)ring;
    q->avail = (uint16_t *)(ring + 16UL * q->size);
    q->used = (uint16_t *)(ring + used_off);
    vdev_qset(&dev, index, q->size, pa);
    return 0;
}

static void vq_publish(vq_t *q, uint16_t head)   /* offer the chain at descriptor `head` */
{
    uint16_t i = q->avail[1];
    q->avail[2 + i % q->size] = head;
    __atomic_store_n(&q->avail[1], (uint16_t)(i + 1), __ATOMIC_RELEASE);
}

static uint16_t vq_used_idx(vq_t *q) { return __atomic_load_n(&q->used[1], __ATOMIC_ACQUIRE); }

static int v_setup(nic_info_t *info, char *what, size_t cap)
{
    if (vdev_find(&dev, VIRTIO_NET)) return -1;
    vdev_status(&dev, 0);
    vdev_status(&dev, VS_ACK | VS_DRIVER);
    int mac = !!(vdev_features(&dev) & F_MAC);
    vdev_accept(&dev, mac ? F_MAC : 0);
    for (int i = 0; i < 6; i++) info->mac[i] = mac ? vdev_cfg8(&dev, i) : (uint8_t)(i == 0 ? 0x02 : i);
    info->mtu = 1500;
    if (vq_setup(&rxq, 0) || vq_setup(&txq, 1)) return -1;
    rxbuf = dma_alloc(NRX * BUFSZ, &rxbuf_pa);
    txmem = dma_alloc(NET_MAX, &txmem_pa);
    uint8_t *zero = dma_alloc(4096, &zero_pa);
    if ((long)rxbuf < 0 || (long)txmem < 0 || (long)zero < 0) return -1;
    int n = NRX < rxq.size / 2 ? NRX : rxq.size / 2;
    for (int i = 0; i < n; i++) {                /* each buffer: header, then frame */
        rxq.desc[2 * i] = (desc_t){ rxbuf_pa + (uint64_t)i * BUFSZ, HDR, D_NEXT | D_WRITE, (uint16_t)(2 * i + 1) };
        rxq.desc[2 * i + 1] = (desc_t){ rxbuf_pa + (uint64_t)i * BUFSZ + 16, BUFSZ - 16, D_WRITE, 0 };
        vq_publish(&rxq, (uint16_t)(2 * i));
    }
    txq.avail[0] = 1;                            /* no interrupt when a frame is sent */
    irq_port = port_create(0);
    if (irq_bind(dev.irq, irq_port)) return -1;
    vdev_status(&dev, VS_ACK | VS_DRIVER | VS_OK);
    dma_wmb();
    vdev_notify(&dev, 0);
    nic_fmt(what, cap, "virtio, interrupt line %d", dev.irq & ~IRQ_LEVEL);
    return 0;
}

static uint8_t *v_txbuf(void) { return txmem; }

static void v_rx_wait(void)
{
    msg_t m = { 0 };
    ipc_recv(irq_port, &m);
    vdev_isr(&dev);                              /* lowers the interrupt line */
    acked = 0;
}

static size_t v_rx_take(uint8_t *batch, size_t cap)
{
    size_t n = 0;
    int given = 0;
    while (rxq.last != vq_used_idx(&rxq)) {
        volatile uint32_t *e = (volatile uint32_t *)(rxq.used + 2) + 2 * (rxq.last % rxq.size);
        uint32_t id = e[0], len = e[1];
        if (len > HDR && len - HDR <= BUFSZ - 16 && id % 2 == 0) {
            uint32_t fl = len - HDR;
            if (n + 2 + fl > cap) break;         /* full: the rest goes in the next batch */
            batch[n] = (uint8_t)fl; batch[n + 1] = (uint8_t)(fl >> 8);
            memcpy(batch + n + 2, rxbuf + (id / 2) * BUFSZ + 16, fl);
            n += 2 + fl;
        }
        rxq.last++;
        vq_publish(&rxq, (uint16_t)id);          /* the buffer goes back to the device */
        given = 1;
    }
    if (given) { dma_wmb(); vdev_notify(&dev, 0); }
    if (!acked && rxq.last == vq_used_idx(&rxq)) {   /* all taken: the line may fire again */
        irq_ack(dev.irq & ~IRQ_LEVEL);
        acked = 1;
    }
    return n;
}

static long v_send(size_t len)
{
    uint16_t d = 0, frames = 0;
    for (size_t off = 0; off + 2 <= len; frames++) {
        uint32_t fl = txmem[off] | txmem[off + 1] << 8;
        if (fl < 14 || fl > 1514 || off + 2 + fl > len || d + 2 > txq.size) return -EINVAL;
        txq.desc[d] = (desc_t){ zero_pa, HDR, D_NEXT, (uint16_t)(d + 1) };
        txq.desc[d + 1] = (desc_t){ txmem_pa + off + 2, fl, 0, 0 };
        vq_publish(&txq, d);
        d += 2;
        off += 2 + fl;
    }
    if (!frames) return 0;
    dma_wmb();
    vdev_notify(&dev, 1);
    uint16_t want = (uint16_t)(txq.last + frames);
    while (vq_used_idx(&txq) != want) sys_yield();   /* the device reads them quickly */
    txq.last = want;
    return 0;
}

static void v_stop(void) { vdev_status(&dev, 0); }   /* reset: no more DMA into our memory */

const nic_t nic_virtio = { "virtio", v_setup, v_txbuf, v_send, v_rx_wait, v_rx_take, 0, v_stop };

/* ---- The date from the clock chip, as seconds since 1970 (UTC). */
#if defined(__x86_64__)
static int cmos(int r) { outb(0x70, r); return inb(0x71); }
static int64_t rtc_now(void)
{
    int v[6], w[6], regs[6] = { 0, 2, 4, 7, 8, 9 };
    do {                                         /* read twice, until both reads agree */
        while (cmos(0x0A) & 0x80) ;              /* an update is in progress */
        for (int i = 0; i < 6; i++) v[i] = cmos(regs[i]);
        while (cmos(0x0A) & 0x80) ;
        for (int i = 0; i < 6; i++) w[i] = cmos(regs[i]);
    } while (memcmp(v, w, sizeof v));
    int b = cmos(0x0B), pm = v[2] & 0x80;        /* status B: bit 2 binary, bit 1 24-hour */
    v[2] &= 0x7F;                                /* (in 12-hour mode, bit 7 of the hour: PM) */
    if (!(b & 4)) for (int i = 0; i < 6; i++) v[i] = (v[i] & 15) + (v[i] >> 4) * 10;   /* BCD */
    if (!(b & 2)) v[2] = v[2] % 12 + (pm ? 12 : 0);
    int64_t y = 2000 + v[5], m = v[4], d = v[3];
    y -= m <= 2;                                 /* days from 1970-01-01 (civil calendar) */
    int64_t era = y / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return days * 86400 + v[2] * 3600 + v[1] * 60 + v[0];
}

#elif defined(__aarch64__)
/* The device tree, loaded once for every backend's setup, then dropped
 * (about 1 MiB on QEMU's virt: kept, it would triple vnet's memory). */
static char *fdt;
static void fdt_drop(void) { free(fdt); fdt = 0; }
const void *nic_fdt(void)
{
    if (!fdt) {
        long size = sys_fdt(0, 0);
        char *f = size > 0 ? malloc(size) : 0;
        if (f && sys_fdt(f, size) == size) fdt = f;
        else free(f);
    }
    return fdt;
}

void nic_udelay(uint64_t us)
{
    long end = sys_clock() + (long)us * 1000;
    while (sys_clock() < end) ;
}

/* The board's MAC address, from the firmware (mailbox tag 0x00010003). */
int nic_fw_mac(uint8_t mac[6])
{
    uint32_t b[8] = { sizeof b, 0, 0x00010003, 6, 0, 0, 0, 0 };
    if (mbox_call(b, 8)) return -1;
    memcpy(mac, &b[5], 6);
    return mac[0] | mac[1] | mac[2] ? 0 : -1;
}

/* Auto-negotiation: advertise everything, restart it. */
void phy_start(const mdio_t *m)
{
    m->wr(PHY_ADV, 0x01E1);                      /* 100 and 10, full and half, IEEE 802.3 */
    m->wr(PHY_CTRL1000, 0x0300);                 /* 1000 full and half */
    m->wr(PHY_BMCR, 0x1200);                     /* enable and restart auto-negotiation */
}

/* BCM54213PE: receive delay = "RGMII skew" in its misc control (auxiliary
 * register 0x18, shadow 7); transmit delay = "GTXCLK delay" in shadow
 * register 0x1C, page 3. Each read selects the shadow, then reads it back. */
void phy_delays(const mdio_t *m, const void *fdt, int mac_node)
{
    int len;
    const char *mode = fdt_prop(fdt, mac_node, "phy-mode", &len);
    if (!mode) mode = fdt_prop(fdt, mac_node, "phy-connection-type", &len);
    if (!mode || len < 6 || memcmp(mode, "rgmii", 5)) return;
    int rx = !strcmp(mode, "rgmii-id") || !strcmp(mode, "rgmii-rxid");
    int tx = !strcmp(mode, "rgmii-id") || !strcmp(mode, "rgmii-txid");
    m->wr(0x18, 0x7007);                         /* read shadow 7 */
    int misc = m->rd(0x18);
    if (misc >= 0) m->wr(0x18, 0x8000 | (misc & 0x0EF8) | (rx ? 0x0100 : 0) | 7);   /* write-enable */
    m->wr(0x1C, 3 << 10);                        /* read shadow 0x1C, page 3 */
    int clk = m->rd(0x1C);
    if (clk >= 0) m->wr(0x1C, 0x8000 | 3 << 10 | (clk & 0x1FF) | (tx ? 0x0200 : 0));
}

int phy_link(const mdio_t *m, int *mbps, int *full)
{
    m->rd(PHY_BMSR);                             /* the link bit latches "down": read it twice */
    int s = m->rd(PHY_BMSR);
    if (s < 0 || !(s & 0x0004) || !(s & 0x0020)) return 0;   /* link, negotiation complete */
    int adv = m->rd(PHY_ADV), lpa = m->rd(PHY_LPA);
    int c1000 = m->rd(PHY_CTRL1000), s1000 = m->rd(PHY_STAT1000);
    int g = c1000 & s1000 >> 2;                  /* partner bits 11/10 line up with ours 9/8 */
    if (g & 0x0200) { *mbps = 1000; *full = 1; }
    else if (g & 0x0100) { *mbps = 1000; *full = 0; }
    else {
        int both = adv & lpa;
        if (both & 0x0100) { *mbps = 100; *full = 1; }
        else if (both & 0x0080) { *mbps = 100; *full = 0; }
        else if (both & 0x0040) { *mbps = 10; *full = 1; }
        else { *mbps = 10; *full = 0; }
    }
    return 1;
}

/* The date from the clock chip, a PL031 (QEMU's virt): its data register
 * counts the seconds since 1970 itself. 0 if there is none. */
static int64_t rtc_now(void)
{
    const void *f = nic_fdt();
    uint64_t a, len;
    int n;
    if (f && (n = fdt_find(f, -1, "arm,pl031")) >= 0 && !fdt_reg(f, n, 0, &a, &len)) {
        volatile uint32_t *r = map_phys(a, 4096);
        if ((long)r > 0) return r[0];
    }
    return 0;
}
#endif

/* ---- The server. */
#if defined(__aarch64__)
static const nic_t *const cards[] = { &nic_virtio, &nic_genet, &nic_gem };
#else
static const nic_t *const cards[] = { &nic_virtio };
#endif
static const nic_t *card;
static nic_info_t info;

/* Frames go to netd only while it runs: looking "net" up would start it
 * (it is started on demand), and frames from the wire (a broadcast, an
 * advertisement) must not wake the network when nobody uses it. */
static void to_net(long *net, msg_t *f)
{
    msg_t copy = *f;
    if (*net <= 0 && (*net = port_find("net")) < 0) return;    /* no netd: dropped */
    if (ipc_call(*net, f) == -ENOENT && (*net = port_find("net")) > 0) {
        *f = copy;
        ipc_call(*net, f);
    }
}

static void receiver(void *arg)
{
    (void)arg;
    static uint8_t batch[NET_MAX];
    long net = 0;
    for (;;) {
        card->rx_wait();
        size_t n;
        while ((n = card->rx_take(batch, sizeof batch)) > 0) {
            msg_t f = { .w = { NET_FRAMES }, .sbuf = batch, .slen = n };
            to_net(&net, &f);
        }
    }
}

/* The cable, checked once a second (the PHY has no interrupt wired here). */
static void watcher(void *arg)
{
    (void)arg;
    long net = 0;
    int was = 0;
    for (;;) {
        int up = card->link();
        if (up != was) {
            msg_t f = { .w = { NET_LINK, (uint64_t)up } };
            to_net(&net, &f);
            was = up;
        }
        sys_sleep(1000000000UL);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char what[96];
    for (size_t i = 0; i < sizeof cards / sizeof cards[0] && !card; i++)
        if (!cards[i]->setup(&info, what, sizeof what)) card = cards[i];
    info.rtc = rtc_now();
    info.rtc_ns = sys_clock();
#if defined(__aarch64__)
    fdt_drop();
#endif
    if (!card) {
        printf("vnet: no network card\n");
        return 2;                                /* init gives up; netd then answers "unreachable" */
    }
    printf("vnet: card %02x:%02x:%02x:%02x:%02x:%02x, %s\n", info.mac[0], info.mac[1], info.mac[2],
           info.mac[3], info.mac[4], info.mac[5], what);
    long port = port_create("nic0");
    thread_start(receiver, 0, 16384);
    if (card->link) thread_start(watcher, 0, 8192);
    uint8_t *tx = card->txbuf();
    msg_t m = { .rbuf = tx, .rlen = NET_MAX };
    uint64_t seen = 0;
    long from = ipc_recv(port, &m);
    for (;;) {
        long r = 0;
        const void *out = 0;
        size_t outn = 0;
        if (from <= 0 || m.uid != 0) r = -EPERM; /* only system servers may send frames */
        else switch (m.w[0]) {
        case NIC_INFO: out = &info; outn = sizeof info; break;
        case NIC_SEND: r = card->send(m.rlen); break;
        case SVC_MAYSTOP:                        /* started on demand: busy while netd runs */
            if ((r = maystop_answer(&m, 0, &seen, port_find("net") > 0)) == 0) {
                card->stop();
                reply_val(from, 0);
                return 0;
            }
            break;
        default:       r = -ENOSYS;
        }
        m = (msg_t){ .w = { r }, .sbuf = out, .slen = outn, .rbuf = tx, .rlen = NET_MAX };
        from = from > 0 ? ipc_reply_recv(from, port, &m) : ipc_recv(port, &m);
    }
}
