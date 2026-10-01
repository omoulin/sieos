/*
 * xhci.c - USB 3 host controllers (xHCI) and USB HID keyboards and mice.
 *
 * Every xHCI controller (PCI class 0C03, interface 30) is taken: the PCH's
 * (Raptor Lake, Ice Lake-LP 8086:34ED, ...) and the Thunderbolt ones (Ice
 * Lake 8086:8A13, ...).  The firmware gives it up (the USB legacy support
 * capability: its PS/2 emulation of USB keyboards ends there), the
 * controller is reset and runs without interrupts: the event ring is
 * polled from the timer tick (usb_poll, 100 Hz), which is fast enough for
 * keyboards and mice and needs no MSI.
 *
 * Devices are enumerated on the root ports and behind USB 2 hubs (a
 * keyboard with a hub, a dock, a desktop's internal hub; the transaction
 * translator for low and full-speed devices behind high-speed hubs), at
 * boot and when plugged in later.  USB 3 hubs are not: keyboards and mice
 * are USB 2 devices and show on a USB 3 hub's USB 2 half.
 *
 * USB Ethernet adapters of the CDC classes are network interfaces (ethN):
 * ECM (plain frames) and NCM (16-bit transfer blocks).  A device whose
 * first configuration is its vendor's (the Realtek RTL8153 of the Surface
 * USB-C adapter and docks, ...) is put in its CDC configuration.
 *
 * USB drives (mass storage, bulk-only transport, SCSI commands: sticks,
 * card readers, external disks) present at boot are disks (c8tNd0p0, their
 * partitions c8tNd0sM): blk_init reads their partition tables.  The
 * transfers are synchronous, from the reader's or writer's context: it
 * takes the controller from the tick (busy) for each command, 64 KiB at
 * most.  A drive that connects later (plugged in, or slow to train its
 * link) is read at the next system call (blk_scan_late): its partitions
 * and /dev/dsk nodes.
 *
 * Of the other devices only the HID interfaces with an interrupt IN endpoint are
 * used: boot keyboards and mice in the boot protocol (fixed reports),
 * anything else (tablets, keyboards and mice that are not boot devices) in
 * the report protocol with its report descriptor (hid.c).  Audio, ... are
 * left alone.
 *
 * Enumeration is synchronous (commands and control transfers wait for
 * their completion events, dispatching the others meanwhile), at boot and
 * from the timer tick when something is plugged in.  All the memory is
 * allocated when the controller starts, none in interrupt context.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "hid.h"
#include "ddi.h"
#include "pci.h"
#include "mm.h"
#include "poll.h"
#include "net.h"
#include "smp.h"
#include "blkdev.h"

#define MAX_HC      4
#define MAX_DEV     32
#define MAX_EP      3                /* interrupt IN endpoints used per device */
#define RING_TRBS   256              /* per ring, the last one the link */

/* TRB types */
#define TRB_NORMAL   1
#define TRB_SETUP    2
#define TRB_DATA     3
#define TRB_STATUS   4
#define TRB_LINK     6
#define TRB_NOOP     8
#define TRB_ENABLE_SLOT   9
#define TRB_DISABLE_SLOT  10
#define TRB_ADDRESS_DEV   11
#define TRB_CONFIG_EP     12
#define TRB_EVAL_CTX      13
#define TRB_RESET_EP      14
#define TRB_SET_DEQ       16
#define TRB_STOP_EP       15
#define TRB_EV_TRANSFER   32
#define TRB_EV_COMMAND    33
#define TRB_EV_PORT       34
#define TRB_TYPE(t)  ((uint32_t)(t) << 10)
#define TRB_IOC      (1U << 5)
#define TRB_ISP      (1U << 2)
#define TRB_IDT      (1U << 6)
#define CC_SUCCESS   1
#define CC_SHORT     13

/* operational registers */
#define USBCMD   0x00
#define USBSTS   0x04
#define CRCR     0x18
#define DCBAAP   0x30
#define CONFIG   0x38
#define PORTSC(p) (0x400 + 0x10 * ((p) - 1))
#define STS_HCH  (1U << 0)
#define STS_CNR  (1U << 11)
#define PORT_CCS (1U << 0)
#define PORT_PED (1U << 1)
#define PORT_PR  (1U << 4)
#define PORT_PP  (1U << 9)
#define PORT_CHANGES (0x7FU << 17)                /* CSC PEC WRC OCC PRC PLC CEC: write 1 to clear */
#define PORT_PRC (1U << 21)

enum { SPEED_FS = 1, SPEED_LS = 2, SPEED_HS = 3, SPEED_SS = 4 };

struct trb {
    uint64_t param;
    uint32_t status;
    uint32_t ctrl;
};

struct ring {
    volatile struct trb *trb;
    uint64_t pa;
    uint32_t idx, cycle;
};

struct xhci;

enum { EP_INT_IN, EP_BULK_IN, EP_BULK_OUT };

struct usbnet;
struct usbstor;

struct usb_ep {
    bool used, halted;
    uint8_t type;                   /* EP_* */
    uint8_t dci, addr;              /* device context index, endpoint address */
    uint8_t interval;               /* 2^interval x 125 us */
    uint8_t burst;                  /* SuperSpeed: packets per burst - 1 */
    uint16_t maxp;                  /* max packet size */
    uint16_t len;                   /* bytes per transfer (interrupt) */
    int errors;
    struct ring ring;
    uint8_t *buf;
    uint64_t buf_pa;
    struct hid *hid;                /* NULL: a hub's status change endpoint */
    struct usbnet *net;             /* a network adapter's bulk endpoint */
    struct usbstor *stor;           /* a drive's bulk endpoint (its transfers are waited for) */
    uint8_t nbuf;                   /* interrupt IN: transfers kept queued (buf split in nbuf) */
    int8_t tag[RING_TRBS];          /* ... the buffer of each TRB */
};

/* A CDC Ethernet adapter. */
#define NET_POOL   2
#define NET_RXMEM  (64 * 1024)      /* the receive buffers: 32 x 2 KiB (ECM) or 4 x 16 KiB (NCM) */
#define NET_NTX    16
#define NET_TXSZ   2048
struct usbnet {
    bool used, up, ncm;
    struct usb_dev *d;
    struct usb_ep *in, *out;
    struct netif *ifp;
    uint8_t mac[6];
    uint8_t *rx;
    uint64_t rx_pa;
    uint32_t rxsz, nrx;
    int8_t rx_tag[RING_TRBS];       /* the buffer of each IN TRB */
    uint8_t *tx;
    uint64_t tx_pa;
    volatile bool tx_busy[NET_NTX];
    int8_t tx_tag[RING_TRBS];       /* the buffer of each OUT TRB, -1 none */
    uint16_t seq, out_div, out_rem;
    struct spinlock lock;           /* the OUT ring: send may run on any CPU */
    uint64_t rx_frames, tx_frames, tx_drops;
};

/* A USB drive (bulk-only transport, LUN 0). */
#define STOR_POOL   4
#define STOR_MAX    (64 * 1024)     /* bytes a command: one TRB (its buffer may not cross 64 KiB) */
struct usbstor {
    bool used, gone;                /* gone: unplugged (its disk stays, and fails) */
    struct usb_dev *d;
    struct usb_ep *in, *out;
    int iface, dev;                 /* the interface; the disk's device number */
    uint32_t tag;
    uint32_t bsize;                 /* the drive's block size: 512 (others are not used) */
    uint64_t blocks;
    uint8_t *buf, *cmd;             /* STOR_MAX at a 64 KiB boundary; a page: CBW at 0, CSW at 512 */
    uint64_t buf_pa, cmd_pa;
    char what[48];                  /* "SanDisk Cruzer Blade" */
};

struct usb_dev {
    bool used;
    struct xhci *hc;
    int slot, speed, port;          /* port: the root port, or the port on the parent hub */
    int root_port, depth;
    uint32_t route;
    struct usb_dev *parent;
    int tt_slot, tt_port;           /* the high-speed hub whose translator a LS/FS device uses */
    uint16_t vendor, product;
    char name[16];                  /* "1-3.2" */
    struct ring ep0;
    void *ictx, *octx, *buf;
    uint64_t ictx_pa, octx_pa, buf_pa;
    int mps0;
    struct usb_ep ep[MAX_EP];
    int nep;
    bool hub;                       /* a USB 2 hub */
    int hub_ports;
    bool hub_changed;
    uint8_t hub_bits[4];            /* the ports that changed (the status change endpoint's report) */
    bool children_at[16];
};

struct xhci {
    struct pci_dev pci;
    int index;
    volatile uint8_t *cap, *op, *rt;
    volatile uint32_t *db;
    int max_slots, max_ports, ctx_size;
    bool ac64, ppc;
    uint64_t *dcbaa;
    uint64_t dcbaa_pa;
    struct ring cmd;
    volatile struct trb *ev;
    uint64_t ev_pa;
    uint32_t ev_idx, ev_cycle;
    bool ready;
    bool port_change[256];
    struct usb_dev *root[256];      /* the device on each root port */
    uint64_t watch_pa;              /* a data stage TRB whose short-packet residual is wanted */
    uint32_t watch_residual;
    bool watch_hit;
};

static struct xhci hcs[MAX_HC];
static int nhc;
static struct usb_dev devs[MAX_DEV];
static struct hid hids[MAX_DEV * MAX_EP];
static struct usbnet nets[NET_POOL];
static struct usbstor stors[STOR_POOL];
static int nstor;
static bool busy;                   /* enumerating, polling or a drive's command: the others keep out */
static bool debug;
static int nkbd, nmouse, nnet;
static void usbnet_event(struct usb_ep *e, volatile struct trb *ev, uint32_t cc, uint32_t residual);
static void usbnet_requeue(struct usbnet *n);

static inline uint32_t rd32(volatile uint8_t *b, uint32_t off) { return *(volatile uint32_t *)(b + off); }
static inline void wr32(volatile uint8_t *b, uint32_t off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; }
static inline void wr64(volatile uint8_t *b, uint32_t off, uint64_t v)
{
    *(volatile uint32_t *)(b + off) = (uint32_t)v;
    *(volatile uint32_t *)(b + off + 4) = (uint32_t)(v >> 32);
}
static inline void barrier(void) { __asm__ volatile("sfence" ::: "memory"); }

static inline uint64_t irq_off(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}

static inline void irq_restore(uint64_t f)
{
    if (f & 0x200)
        __asm__ volatile("sti" ::: "memory");
}


static void mdelay(unsigned ms)
{
    uint64_t end = hrtime() + (uint64_t)ms * 1000000;
    while (hrtime() < end)
        __asm__ volatile("pause");
}

/* A zeroed page for the controller (below 4 GiB if it cannot address more). */
static uint64_t dma_page(struct xhci *hc)
{
    uint64_t pa = pmm_alloc_contig(1);
    if (pa && !hc->ac64 && pa >= DIRECT_MAP_SIZE) {
        pmm_free_contig(pa, 1);
        return 0;
    }
    return pa;
}

/* ---------------------------------------------------------------- rings */

static bool ring_init(struct xhci *hc, struct ring *r)
{
    r->pa = dma_page(hc);
    if (!r->pa)
        return false;
    r->trb = P2V(r->pa);
    r->idx = 0;
    r->cycle = 1;
    return true;
}

static void ring_reset(struct ring *r)
{
    memset((void *)r->trb, 0, PAGE_SIZE);
    r->idx = 0;
    r->cycle = 1;
}

static uint64_t ring_push(struct ring *r, uint64_t param, uint32_t status, uint32_t ctrl)
{
    volatile struct trb *t = &r->trb[r->idx];
    uint64_t pa = r->pa + r->idx * sizeof(struct trb);
    t->param = param;
    t->status = status;
    barrier();
    t->ctrl = ctrl | r->cycle;
    if (++r->idx == RING_TRBS - 1) {
        volatile struct trb *l = &r->trb[r->idx];
        l->param = r->pa;
        l->status = 0;
        barrier();
        l->ctrl = TRB_TYPE(TRB_LINK) | 2 /* toggle cycle */ | r->cycle;
        r->idx = 0;
        r->cycle ^= 1;
    }
    return pa;
}

/* Room for n TRBs of one TD before the link (a TD must not straddle it here). */
static void ring_room(struct ring *r, int n)
{
    while (r->idx + n > RING_TRBS - 1)
        ring_push(r, 0, 0, TRB_TYPE(TRB_NOOP));
}

/* ---------------------------------------------------------------- events */

static struct usb_dev *dev_of(struct xhci *hc, int slot)
{
    for (int i = 0; i < MAX_DEV; i++)
        if (devs[i].used && devs[i].hc == hc && devs[i].slot == slot)
            return &devs[i];
    return NULL;
}

/*
 * An interrupt IN endpoint keeps nbuf transfers queued (4 for HID reports
 * up to 256 bytes): a touchpad or mouse sends faster than the 100 Hz poll,
 * and with one transfer the reports between two polls were lost.
 */
static void ep_queue_one(struct usb_dev *d, struct usb_ep *e, int k)
{
    uint32_t stride = 1024 / (e->nbuf ? e->nbuf : 1);
    uint64_t t = ring_push(&e->ring, e->buf_pa + k * stride, e->len, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    e->tag[(t - e->ring.pa) / sizeof(struct trb)] = k;
    barrier();
    d->hc->db[d->slot] = e->dci;
}

static void ep_queue(struct usb_dev *d, struct usb_ep *e)
{
    if (!e->nbuf)
        e->nbuf = 1;
    memset(e->tag, -1, sizeof(e->tag));
    for (int k = 0; k < e->nbuf; k++)
        ep_queue_one(d, e, k);
}

static void transfer_event(struct xhci *hc, volatile struct trb *ev)
{
    int slot = ev->ctrl >> 24, dci = (ev->ctrl >> 16) & 0x1F;
    uint32_t cc = ev->status >> 24, residual = ev->status & 0xFFFFFF;
    if (hc->watch_pa && ev->param == hc->watch_pa) {
        hc->watch_residual = residual;
        hc->watch_hit = true;
        return;
    }
    struct usb_dev *d = dev_of(hc, slot);
    if (!d)
        return;
    for (int i = 0; i < d->nep; i++) {
        struct usb_ep *e = &d->ep[i];
        if (!e->used || e->dci != dci)
            continue;
        if (e->stor)
            return;                              /* (late: its command timed out) */
        if (e->net) {
            usbnet_event(e, ev, cc, residual);
            return;
        }
        if (cc != CC_SUCCESS && cc != CC_SHORT) {
            e->halted = true;                    /* reset from usb_poll */
            return;
        }
        uint32_t n = residual <= e->len ? e->len - residual : 0;
        uint64_t idx = (ev->param - e->ring.pa) / sizeof(struct trb);
        int k = idx < RING_TRBS && e->tag[idx] >= 0 ? e->tag[idx] : 0;
        uint8_t *buf = e->buf + k * (1024 / (e->nbuf ? e->nbuf : 1));
        if (e->hid)
            hid_input(e->hid, buf, n);
        else {
            for (uint32_t b = 0; b < sizeof(d->hub_bits) && b < n; b++)
                d->hub_bits[b] |= buf[b];
            d->hub_changed = true;
        }
        e->errors = 0;
        ep_queue_one(d, e, k);
        return;
    }
}

/*
 * Handle the events so far.  If want is a TRB address, stop at its
 * completion (a command or a transfer) and return the event's status and
 * control words; the other events are handled on the way.
 */
static bool events(struct xhci *hc, uint64_t want, uint32_t *status, uint32_t *ctrl)
{
    bool found = false;
    int n = 0, used = 0;
    while (!found && n++ < 4 * RING_TRBS) {
        volatile struct trb *ev = &hc->ev[hc->ev_idx];
        if ((ev->ctrl & 1) != hc->ev_cycle)
            break;
        int type = (ev->ctrl >> 10) & 0x3F;
        if (want && (type == TRB_EV_COMMAND || type == TRB_EV_TRANSFER) && ev->param == want) {
            *status = ev->status;
            *ctrl = ev->ctrl;
            found = true;
        } else if (type == TRB_EV_TRANSFER) {
            transfer_event(hc, ev);
        } else if (type == TRB_EV_PORT) {
            hc->port_change[(ev->param >> 24) & 0xFF] = true;
        }
        if (++hc->ev_idx == RING_TRBS) {
            hc->ev_idx = 0;
            hc->ev_cycle ^= 1;
        }
        used++;
    }
    if (used)                                    /* (the one wanted, first, too: else the ring fills up) */
        wr64(hc->rt, 0x20 + 0x18, (hc->ev_pa + hc->ev_idx * sizeof(struct trb)) | 8);   /* ERDP, EHB */
    return found;
}

static bool wait_for(struct xhci *hc, uint64_t trb_pa, uint32_t *status, uint32_t *ctrl, unsigned ms)
{
    uint64_t end = hrtime() + (uint64_t)ms * 1000000;
    do {
        if (events(hc, trb_pa, status, ctrl))
            return true;
        __asm__ volatile("pause");
    } while (hrtime() < end);
    return false;
}

/* A command; its completion code (0: no answer), the slot ID in *slot. */
static int command(struct xhci *hc, uint64_t param, uint32_t ctrl, int *slot)
{
    uint64_t pa = ring_push(&hc->cmd, param, 0, ctrl);
    barrier();
    hc->db[0] = 0;
    uint32_t st, c;
    if (!wait_for(hc, pa, &st, &c, 1000))
        return 0;
    if (slot)
        *slot = c >> 24;
    return st >> 24;
}

/*
 * A control transfer on endpoint 0 through d->buf: the number of bytes
 * transferred, or < 0.
 */
static int control(struct usb_dev *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len)
{
    struct xhci *hc = d->hc;
    bool in = rt & 0x80;
    if (len > PAGE_SIZE)
        return -1;
    ring_room(&d->ep0, 3);
    uint64_t setup = rt | (uint64_t)req << 8 | (uint64_t)val << 16 | (uint64_t)idx << 32 | (uint64_t)len << 48;
    ring_push(&d->ep0, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (len ? (in ? 3U : 2U) << 16 : 0));
    uint64_t data = 0;
    if (len)
        data = ring_push(&d->ep0, d->buf_pa, len, TRB_TYPE(TRB_DATA) | TRB_ISP | (in ? 1U << 16 : 0));
    uint64_t pa = ring_push(&d->ep0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (len && in ? 0 : 1U << 16));
    hc->watch_pa = data;
    hc->watch_hit = false;
    barrier();
    hc->db[d->slot] = 1;
    uint32_t st, c;
    bool done = wait_for(hc, pa, &st, &c, 1000);
    hc->watch_pa = 0;
    if (!done) {
        if (debug)
            kprintf("usb %s: control %02x/%02x timed out\n", d->name, rt, req);
        return -1;
    }
    uint32_t cc = st >> 24;
    if (cc != CC_SUCCESS && cc != CC_SHORT) {
        if (debug)
            kprintf("usb %s: control %02x/%02x: completion %u\n", d->name, rt, req, cc);
        if (cc == 6)                             /* a stall: the control endpoint recovers by itself */
            return -2;
        return -1;
    }
    if (!len)
        return 0;
    uint32_t residual = hc->watch_hit ? hc->watch_residual : 0;
    return residual <= len ? (int)(len - residual) : 0;
}

/* ---------------------------------------------------------------- contexts */

static uint32_t *ictx(struct usb_dev *d, int i)      /* 0: control, 1: slot, 2..: endpoints (dci + 1) */
{
    return (uint32_t *)((uint8_t *)d->ictx + i * d->hc->ctx_size);
}

static uint32_t *octx(struct usb_dev *d, int i)      /* 0: slot, 1..: endpoints by dci */
{
    return (uint32_t *)((uint8_t *)d->octx + i * d->hc->ctx_size);
}

static void ep0_ctx(struct usb_dev *d, uint32_t *ep)
{
    ep[1] = 3U << 1 | 4U << 3 | (uint32_t)d->mps0 << 16;   /* CErr 3, control, max packet */
    ep[2] = (uint32_t)d->ep0.pa | d->ep0.cycle;
    ep[3] = (uint32_t)(d->ep0.pa >> 32);
    ep[4] = 8;                                   /* average TRB length */
}

static struct usb_dev *dev_alloc(void)
{
    for (int i = 0; i < MAX_DEV; i++)
        if (!devs[i].used && devs[i].ictx)
            return &devs[i];
    return NULL;
}

static void dev_free(struct usb_dev *d)
{
    for (int i = 0; i < d->nep; i++)
        if (d->ep[i].hid) {
            hid_release(d->ep[i].hid);
            if (d->ep[i].hid->has_kbd)
                nkbd--;
            if (d->ep[i].hid->has_mouse || d->ep[i].hid->boot_mouse)
                nmouse--;
            d->ep[i].hid = NULL;
        }
    for (int i = 0; i < d->nep; i++)
        if (d->ep[i].net) {
            struct usbnet *n = d->ep[i].net;
            uint64_t f = irq_off();
            spin_lock(&n->lock);
            n->up = false;                       /* (the interface stays, for the adapter's return) */
            n->d = NULL;
            n->used = false;
            spin_unlock(&n->lock);
            irq_restore(f);
            if (n->ifp)
                n->ifp->present = false;
            d->ep[i].net = NULL;
            nnet--;
        }
    for (int i = 0; i < d->nep; i++)
        if (d->ep[i].stor) {
            struct usbstor *st = d->ep[i].stor;
            if (!st->gone && i == 0) {
                kprintf("usb %s: drive %s removed: %s fails until the next boot\n", d->name, st->what, blk_name(st->dev));
                nstor--;
            }
            st->gone = true;
            st->d = NULL;
            d->ep[i].stor = NULL;
        }
    d->used = false;
}

static void dev_remove(struct usb_dev *d)
{
    for (int i = 0; i < MAX_DEV; i++)              /* what hangs off a hub first */
        if (devs[i].used && devs[i].parent == d)
            dev_remove(&devs[i]);
    struct xhci *hc = d->hc;
    if (d->slot) {
        command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (uint32_t)d->slot << 24, NULL);
        hc->dcbaa[d->slot] = 0;
    }
    if (!d->parent && hc->root[d->root_port] == d)
        hc->root[d->root_port] = NULL;
    if (d->parent && d->port < 16)
        d->parent->children_at[d->port] = false;
    kprintf("usb %s: removed\n", d->name);
    dev_free(d);
}

/* ---------------------------------------------------------------- configuration */

static int interval_of(int speed, uint8_t binterval)
{
    int v;
    if (speed == SPEED_HS || speed >= SPEED_SS) {
        v = binterval ? binterval - 1 : 0;       /* 2^(bInterval-1) microframes */
    } else {
        v = 3;                                   /* frames: log2(bInterval * 8) */
        while (v < 10 && (1 << (v + 1)) <= binterval * 8)
            v++;
    }
    return v > 15 ? 15 : v;
}

/* Configure the endpoints in d->ep (and the slot as a hub). */
static bool configure_endpoints(struct usb_dev *d, int ttt)
{
    memset(d->ictx, 0, PAGE_SIZE);
    uint32_t *icc = ictx(d, 0), *slot = ictx(d, 1);
    memcpy(slot, octx(d, 0), d->hc->ctx_size);
    int last = 1;
    icc[1] = 1;                                  /* the slot context */
    for (int i = 0; i < d->nep; i++) {
        struct usb_ep *e = &d->ep[i];
        uint32_t *ep = ictx(d, e->dci + 1);
        icc[1] |= 1U << e->dci;
        last = e->dci > last ? e->dci : last;
        static const uint32_t ctx_type[] = { 7, 6, 2 };         /* interrupt IN, bulk IN, bulk OUT */
        ep[0] = e->type == EP_INT_IN ? (uint32_t)e->interval << 16 : 0;
        ep[1] = 3U << 1 | ctx_type[e->type] << 3 | (uint32_t)e->burst << 8 | (uint32_t)e->maxp << 16;
        ep[2] = (uint32_t)e->ring.pa | e->ring.cycle;
        ep[3] = (uint32_t)(e->ring.pa >> 32);
        if (e->type == EP_INT_IN)
            ep[4] = e->len | (uint32_t)e->len << 16;           /* average TRB length, max ESIT payload */
        else
            ep[4] = 2048;
    }
    slot[0] = (slot[0] & ~(0x1FU << 27)) | (uint32_t)last << 27;
    slot[3] = 0;
    if (d->hub) {
        slot[0] |= 1U << 26;
        slot[1] = (slot[1] & 0x00FFFFFF) | (uint32_t)d->hub_ports << 24;
        slot[2] = (slot[2] & ~(3U << 16)) | (uint32_t)(ttt & 3) << 16;
    }
    int cc = command(d->hc, d->ictx_pa, TRB_TYPE(TRB_CONFIG_EP) | (uint32_t)d->slot << 24, NULL);
    if (cc != CC_SUCCESS) {
        kprintf("usb %s: configure endpoint failed (%d)\n", d->name, cc);
        return false;
    }
    return true;
}

static bool hid_setup(struct usb_dev *d, struct usb_ep *e, int iface, int sub, int proto, int rlen)
{
    struct hid *h = e->hid;
    memset(h, 0, sizeof(*h));
    snprintf(h->name, sizeof(h->name), "usb %s", d->name);
    h->debug = debug;
    const char *what = NULL;
    if (sub == 1 && (proto == 1 || proto == 2)) {       /* a boot device: its fixed reports */
        control(d, 0x21, 0x0B, 0, iface, 0);    /* SET_PROTOCOL boot */
        if (proto == 1) {
            control(d, 0x21, 0x0A, 0, iface, 0);    /* SET_IDLE: report changes only */
            h->boot_kbd = h->has_kbd = true;
            what = "keyboard";
        } else {
            h->boot_mouse = true;
            what = "mouse";
        }
    } else {
        int n = rlen > 0 ? control(d, 0x81, 6, 0x2200, iface, rlen > (int)PAGE_SIZE ? (int)PAGE_SIZE : rlen) : -1;
        if (n <= 0 || hid_parse(h, d->buf, n) < 0)
            return false;
        control(d, 0x21, 0x0A, 0, iface, 0);
        what = h->has_kbd && h->has_mouse ? "keyboard and pointer" : h->has_kbd ? "keyboard" : "pointer";
    }
    if (h->has_kbd)
        nkbd++;
    if (h->has_mouse || h->boot_mouse)
        nmouse++;
    kprintf("usb %s: %04x:%04x %s%s\n", d->name, d->vendor, d->product, what,
            h->boot_kbd || h->boot_mouse ? " (boot protocol)" : "");
    return true;
}


/* ---------------------------------------------------------------- CDC Ethernet (ECM, NCM) */

static inline void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static inline uint16_t get16(const uint8_t *p) { return p[0] | p[1] << 8; }

struct cdc_ep { uint8_t addr, burst; uint16_t maxp; bool have; };
struct cdc_info {
    bool ncm;
    int comm_if, data_if, data_alt, mac_idx;
    uint8_t cfg_value;
    struct cdc_ep in, out;
};

/* A configuration's CDC ECM or NCM function: the control and data interfaces, the bulk endpoints. */
static bool cdc_parse(const uint8_t *cfg, int n, struct cdc_info *ci)
{
    memset(ci, 0, sizeof(*ci));
    ci->comm_if = ci->data_if = -1;
    ci->cfg_value = cfg[5];
    int cur_if = -1, cur_alt = 0, cur_class = -1;
    struct cdc_ep *last = NULL;
    for (int i = 0; i + 2 <= n && cfg[i] >= 2 && i + cfg[i] <= n; i += cfg[i]) {
        const uint8_t *x = cfg + i;
        if (x[1] == 4 && x[0] >= 9) {
            cur_if = x[2];
            cur_alt = x[3];
            cur_class = x[5];
            last = NULL;
            if (x[5] == 2 && (x[6] == 6 || x[6] == 0x0D) && ci->comm_if < 0) {
                ci->comm_if = x[2];
                ci->ncm = x[6] == 0x0D;
            }
        } else if (x[1] == 0x24 && cur_class == 2 && cur_if == ci->comm_if && x[0] >= 3) {
            if (x[2] == 0x06 && x[0] >= 5)       /* union: the data interface */
                ci->data_if = x[4];
            if (x[2] == 0x0F && x[0] >= 13)      /* Ethernet networking: the MAC address string */
                ci->mac_idx = x[3];
        } else if (x[1] == 5 && x[0] >= 7 && cur_class == 0x0A && ci->comm_if >= 0 &&
                   (ci->data_if < 0 || cur_if == ci->data_if) && (x[3] & 3) == 2) {
            ci->data_if = cur_if;
            ci->data_alt = cur_alt;
            last = x[2] & 0x80 ? &ci->in : &ci->out;
            last->addr = x[2];
            last->maxp = (x[4] | x[5] << 8) & 0x7FF;
            last->burst = 0;
            last->have = true;
        } else if (x[1] == 0x30 && x[0] >= 6 && last) {   /* SuperSpeed companion */
            last->burst = x[2] > 15 ? 15 : x[2];
        }
    }
    return ci->comm_if >= 0 && ci->in.have && ci->out.have && ci->mac_idx;
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void usbnet_rx_queue(struct usbnet *n, int tag)
{
    struct usb_ep *e = n->in;
    uint64_t t = ring_push(&e->ring, n->rx_pa + (uint64_t)tag * n->rxsz, n->rxsz,
                           TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    n->rx_tag[(t - e->ring.pa) / sizeof(struct trb)] = tag;
}

static void usbnet_requeue(struct usbnet *n)
{
    memset(n->rx_tag, -1, sizeof(n->rx_tag));
    for (uint32_t i = 0; i < n->nrx; i++)
        usbnet_rx_queue(n, i);
    barrier();
    n->d->hc->db[n->d->slot] = n->in->dci;
}

/* The datagrams of an NCM transfer block (16-bit). */
static void ncm_rx(struct usbnet *n, const uint8_t *b, uint32_t len)
{
    if (len < 12 || memcmp(b, "NCMH", 4))
        return;
    uint32_t ndp = get16(b + 10);
    for (int guard = 0; ndp && guard < 8; guard++) {
        if (ndp + 16 > len || memcmp(b + ndp, "NCM0", 4))
            return;
        uint32_t nlen = get16(b + ndp + 4), next = get16(b + ndp + 6);
        for (uint32_t p = ndp + 8; p + 4 <= ndp + nlen && p + 4 <= len; p += 4) {
            uint32_t off = get16(b + p), l = get16(b + p + 2);
            if (!off || !l)
                break;
            if (off + l <= len) {
                n->rx_frames++;
                net_rx(n->ifp, b + off, l);
            }
        }
        ndp = next;
    }
}

static void usbnet_event(struct usb_ep *e, volatile struct trb *ev, uint32_t cc, uint32_t residual)
{
    struct usbnet *n = e->net;
    uint64_t idx = (ev->param - e->ring.pa) / sizeof(struct trb);
    if (idx >= RING_TRBS)
        return;
    if (e->type == EP_BULK_OUT) {
        int tag = n->tx_tag[idx];
        n->tx_tag[idx] = -1;
        if (tag >= 0)
            n->tx_busy[tag] = false;
        if (cc != CC_SUCCESS && cc != CC_SHORT)
            e->halted = true;
        return;
    }
    int tag = n->rx_tag[idx];
    if (cc != CC_SUCCESS && cc != CC_SHORT) {
        e->halted = true;
        return;
    }
    if (tag < 0 || (uint32_t)tag >= n->nrx)
        return;
    uint32_t len = residual <= n->rxsz ? n->rxsz - residual : 0;
    const uint8_t *b = n->rx + (uint64_t)tag * n->rxsz;
    if (n->up && n->ifp && len) {
        if (n->ncm) {
            ncm_rx(n, b, len);
        } else if (len >= ETH_HLEN) {
            n->rx_frames++;
            net_rx(n->ifp, b, len);
        }
    }
    usbnet_rx_queue(n, tag);
    barrier();
    n->d->hc->db[n->d->slot] = e->dci;
}

static int usbnet_send(struct netif *ifp, const void *frame, size_t len)
{
    struct usbnet *n = ifp->drv;
    if (len > 1600)
        return -EINVAL;
    uint64_t f = irq_off();
    spin_lock(&n->lock);
    int r = -EIO;
    if (!n->up || !n->d)
        goto out;
    int i = 0;
    while (i < NET_NTX && n->tx_busy[i])
        i++;
    if (i == NET_NTX) {
        n->tx_drops++;
        r = -EAGAIN;                             /* (the adapter is behind: dropped) */
        goto out;
    }
    uint8_t *b = n->tx + i * NET_TXSZ;
    uint32_t total;
    if (!n->ncm) {
        memcpy(b, frame, len);
        total = len;
    } else {                                     /* NTH16, NDP16 (one datagram), the datagram */
        uint32_t off = 28, div = n->out_div ? n->out_div : 4;
        while (off % div != n->out_rem % div)
            off++;
        if (off + len > NET_TXSZ)
            goto out;
        memset(b, 0, off);
        memcpy(b, "NCMH", 4);
        put16(b + 4, 12);
        put16(b + 6, n->seq++);
        put16(b + 8, off + len);
        put16(b + 10, 12);
        memcpy(b + 12, "NCM0", 4);
        put16(b + 16, 16);
        put16(b + 20, off);
        put16(b + 22, len);
        memcpy(b + off, frame, len);
        total = off + len;
    }
    n->tx_busy[i] = true;
    struct usb_ep *e = n->out;
    uint64_t t = ring_push(&e->ring, n->tx_pa + i * NET_TXSZ, total, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    n->tx_tag[(t - e->ring.pa) / sizeof(struct trb)] = i;
    if (total % e->maxp == 0) {                  /* a zero-length packet ends the transfer */
        t = ring_push(&e->ring, 0, 0, TRB_TYPE(TRB_NORMAL));
        n->tx_tag[(t - e->ring.pa) / sizeof(struct trb)] = -1;
    }
    barrier();
    n->d->hc->db[n->d->slot] = e->dci;
    n->tx_frames++;
    r = 0;
out:
    spin_unlock(&n->lock);
    irq_restore(f);
    return r;
}

static void usbnet_poll(struct netif *ifp) { (void)ifp; }   /* (frames arrive from usb_poll) */
static bool usbnet_link(struct netif *ifp) { return ((struct usbnet *)ifp->drv)->up; }
static const struct nic_ops usbnet_ops = { "usb-cdc", usbnet_send, usbnet_poll, usbnet_link };

static bool usbnet_setup(struct usb_dev *d, const struct cdc_info *ci)
{
    uint8_t *b = d->buf, mac[6];
    int len = control(d, 0x80, 6, 0x0300 | ci->mac_idx, 0x0409, 64);
    if (len < 26)
        return false;
    for (int i = 0; i < 6; i++) {
        int hi = hexval(b[2 + 4 * i]), lo = hexval(b[4 + 4 * i]);
        if (hi < 0 || lo < 0)
            return false;
        mac[i] = hi << 4 | lo;
    }
    struct usbnet *n = NULL;
    for (int i = 0; i < NET_POOL && !n; i++)     /* the same adapter again: its interface */
        if (!nets[i].used && nets[i].ifp && !memcmp(nets[i].mac, mac, 6))
            n = &nets[i];
    for (int i = 0; i < NET_POOL && !n; i++)
        if (!nets[i].used && !nets[i].ifp && nets[i].rx)
            n = &nets[i];
    if (!n) {
        kprintf("usb %s: %04x:%04x Ethernet: too many adapters\n", d->name, d->vendor, d->product);
        return true;
    }
    if (control(d, 0x00, 9, ci->cfg_value, 0, 0) < 0)     /* SET_CONFIGURATION */
        return false;
    const struct cdc_ep *src[2] = { &ci->in, &ci->out };
    for (int k = 0; k < 2; k++) {
        struct usb_ep *e = &d->ep[k];
        e->used = true;
        e->halted = false;
        e->errors = 0;
        e->type = k ? EP_BULK_OUT : EP_BULK_IN;
        e->addr = src[k]->addr;
        e->dci = (src[k]->addr & 0xF) * 2 + (k ? 0 : 1);
        e->maxp = src[k]->maxp ? src[k]->maxp : 512;
        e->burst = d->speed >= SPEED_SS ? src[k]->burst : 0;
        e->interval = 0;
        e->hid = NULL;
        e->stor = NULL;
        e->net = n;
        ring_reset(&e->ring);
    }
    d->nep = 2;
    if (!configure_endpoints(d, 0))
        return false;
    n->rxsz = 2048;
    n->out_div = 4;
    n->out_rem = 0;
    if (ci->ncm) {
        if (control(d, 0xA1, 0x80, 0, ci->comm_if, 28) >= 28) {   /* GET_NTB_PARAMETERS */
            uint32_t in_max = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
            n->out_div = get16(b + 20);
            n->out_rem = get16(b + 22);
            n->rxsz = in_max < 16384 ? (in_max < 2048 ? 2048 : in_max) : 16384;
            if (in_max > 16384) {
                uint32_t v = 16384;
                memcpy(b, &v, 4);
                control(d, 0x21, 0x86, 0, ci->comm_if, 4);        /* SET_NTB_INPUT_SIZE */
            }
        } else {
            n->rxsz = 16384;
        }
    }
    n->nrx = NET_RXMEM / n->rxsz;
    n->nrx = n->nrx > 32 ? 32 : n->nrx;
    control(d, 0x01, 11, 0, ci->data_if, 0);                     /* SET_INTERFACE: data off, then on */
    control(d, 0x01, 11, ci->data_alt, ci->data_if, 0);
    control(d, 0x21, 0x43, 0x0E, ci->comm_if, 0);                /* packet filter: directed, broadcast, multicast */
    uint64_t f = irq_off();
    spin_lock(&n->lock);
    n->used = true;
    n->ncm = ci->ncm;
    n->d = d;
    n->in = &d->ep[0];
    n->out = &d->ep[1];
    memcpy(n->mac, mac, 6);
    for (int i = 0; i < NET_NTX; i++)
        n->tx_busy[i] = false;
    memset(n->tx_tag, -1, sizeof(n->tx_tag));
    n->up = true;
    spin_unlock(&n->lock);
    irq_restore(f);
    usbnet_requeue(n);
    bool again = n->ifp != NULL;
    if (!n->ifp)
        n->ifp = netif_register(&usbnet_ops, n, mac);
    if (n->ifp) {
        n->ifp->present = true;
        if (net_started)
            net_attach(n->ifp);
    }
    nnet++;
    kprintf("usb %s: %04x:%04x Ethernet (CDC %s), %s %02x:%02x:%02x:%02x:%02x:%02x%s\n", d->name, d->vendor,
            d->product, n->ncm ? "NCM" : "ECM", n->ifp ? n->ifp->name : "no interface", mac[0], mac[1], mac[2],
            mac[3], mac[4], mac[5], again ? " (again)" : "");
    return true;
}

/* ---------------------------------------------------------------- drives (mass storage) */

struct stor_info { int iface; struct cdc_ep in, out; };

static inline void put32be(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static inline uint32_t get32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline void put32le(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static inline uint32_t get32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/*
 * An endpoint back to a known state after a stall, an error or a timeout:
 * stopped (if it ran) or reset (if it halted), its ring empty, the drive's
 * halt cleared.  The command that does not apply fails, harmlessly.
 */
static void stor_ep_reset(struct usb_dev *d, struct usb_ep *e)
{
    uint32_t ep = (uint32_t)d->slot << 24 | (uint32_t)e->dci << 16;
    command(d->hc, 0, TRB_TYPE(TRB_STOP_EP) | ep, NULL);
    command(d->hc, 0, TRB_TYPE(TRB_RESET_EP) | ep, NULL);
    ring_reset(&e->ring);
    command(d->hc, e->ring.pa | e->ring.cycle, TRB_TYPE(TRB_SET_DEQ) | ep, NULL);
    control(d, 0x02, 1, 0, e->addr, 0);          /* CLEAR_FEATURE ENDPOINT_HALT */
}

/* One bulk transfer, waited for: 0, -2 a stall (cleared), -1 an error or a timeout. */
static int stor_bulk(struct usbstor *s, struct usb_ep *e, uint64_t pa, uint32_t len, uint32_t *done, unsigned ms)
{
    struct usb_dev *d = s->d;
    ring_room(&e->ring, 1);
    uint64_t t = ring_push(&e->ring, pa, len, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    barrier();
    d->hc->db[d->slot] = e->dci;
    uint32_t st, c;
    if (!wait_for(d->hc, t, &st, &c, ms)) {
        if (debug)
            kprintf("usb %s: drive: endpoint %02x timed out\n", d->name, e->addr);
        stor_ep_reset(d, e);
        return -1;
    }
    uint32_t cc = st >> 24, residual = st & 0xFFFFFF;
    if (done)
        *done = residual <= len ? len - residual : 0;
    if (cc == CC_SUCCESS || cc == CC_SHORT)
        return 0;
    if (debug)
        kprintf("usb %s: drive: endpoint %02x completion %u\n", d->name, e->addr, cc);
    stor_ep_reset(d, e);
    return cc == 6 ? -2 : -1;
}

/* Bulk-only reset recovery: the drive's reset, both endpoints cleared. */
static void stor_recover(struct usbstor *s)
{
    control(s->d, 0x21, 0xFF, 0, s->iface, 0);
    stor_ep_reset(s->d, s->in);
    stor_ep_reset(s->d, s->out);
}

/*
 * A SCSI command through the bulk-only transport, its data in s->buf (len
 * bytes, in or out): 0 done (*got bytes), 1 the drive reports a failure
 * (its sense data tells), -1 the transport failed.
 */
static int stor_cmd(struct usbstor *s, const uint8_t *cdb, int cdblen, bool in, uint32_t len, uint32_t *got)
{
    uint8_t *w = s->cmd;
    memset(w, 0, 31);
    put32le(w, 0x43425355);                      /* "USBC" */
    put32le(w + 4, ++s->tag);
    put32le(w + 8, len);
    w[12] = in ? 0x80 : 0;
    w[14] = cdblen;
    memcpy(w + 15, cdb, cdblen);
    if (stor_bulk(s, s->out, s->cmd_pa, 31, NULL, 5000) < 0) {
        stor_recover(s);
        return -1;
    }
    uint32_t n = 0;
    if (len && stor_bulk(s, in ? s->in : s->out, s->buf_pa, len, &n, 20000) == -1) {
        stor_recover(s);                         /* (a stall: cleared, the status follows) */
        return -1;
    }
    uint8_t *csw = s->cmd + 512;
    int r = stor_bulk(s, s->in, s->cmd_pa + 512, 13, NULL, 20000);
    if (r == -2)
        r = stor_bulk(s, s->in, s->cmd_pa + 512, 13, NULL, 20000);
    if (r < 0 || get32le(csw) != 0x53425355 || get32le(csw + 4) != s->tag || csw[12] > 1) {
        stor_recover(s);                         /* (not "USBS", another command's, a phase error) */
        return -1;
    }
    uint32_t residue = get32le(csw + 8);
    if (got)
        *got = residue < n ? n - residue : n;
    return csw[12];
}

/* REQUEST SENSE: the sense key << 16 | ASC << 8 | ASCQ, or -1. */
static int stor_sense(struct usbstor *s)
{
    const uint8_t cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    uint32_t n = 0;
    if (stor_cmd(s, cdb, 6, true, 18, &n) != 0 || n < 14)
        return -1;
    return (s->buf[2] & 0xF) << 16 | s->buf[12] << 8 | s->buf[13];
}

/* Blocks [lba, lba + n) from or to s->buf (n x 512 <= STOR_MAX), a few tries. */
static bool stor_xfer(struct usbstor *s, uint64_t lba, uint32_t n, bool write)
{
    uint8_t cdb[16] = { 0 };
    int len;
    if (lba + n > 0xFFFFFFFFUL) {                /* READ (16), WRITE (16) */
        cdb[0] = write ? 0x8A : 0x88;
        put32be(cdb + 2, lba >> 32);
        put32be(cdb + 6, (uint32_t)lba);
        put32be(cdb + 10, n);
        len = 16;
    } else {                                     /* READ (10), WRITE (10) */
        cdb[0] = write ? 0x2A : 0x28;
        put32be(cdb + 2, (uint32_t)lba);
        cdb[7] = n >> 8;
        cdb[8] = n;
        len = 10;
    }
    for (int tries = 0; tries < 3; tries++) {
        uint32_t got = 0;
        int r = stor_cmd(s, cdb, len, !write, n * 512, &got);
        if (r == 0 && got == n * 512)
            return true;
        if (debug)
            kprintf("usb: drive %s: command %02x: status %d, %u of %u bytes\n", s->what, cdb[0], r, got, n * 512);
        if (r == 1)
            stor_sense(s);                       /* (a unit attention, ...: it clears) */
    }
    return false;
}

static void stor_lock(void)
{
    while (__atomic_exchange_n(&busy, true, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}

static void stor_unlock(void)
{
    __atomic_store_n(&busy, false, __ATOMIC_RELEASE);
}

static int stor_rw(void *drv, uint64_t lba, size_t count, void *buf, bool write)
{
    struct usbstor *s = drv;
    uint8_t *p = buf;
    while (count) {
        uint32_t n = count < STOR_MAX / 512 ? count : STOR_MAX / 512;
        stor_lock();                             /* (one command at a time: the tick gets in between) */
        bool ok = s->d && !s->gone;
        if (ok && write)
            memcpy(s->buf, p, n * 512);
        ok = ok && stor_xfer(s, lba, n, write);
        if (ok && !write)
            memcpy(p, s->buf, n * 512);
        stor_unlock();
        if (!ok) {
            kprintf("usb: drive %s: %s of %u blocks at %lu failed\n", s->what, write ? "writing" : "reading", n, lba);
            return -EIO;
        }
        lba += n;
        count -= n;
        p += n * 512;
    }
    return 0;
}

static int stor_read(void *drv, uint64_t lba, size_t count, void *buf) { return stor_rw(drv, lba, count, buf, false); }
static int stor_write(void *drv, uint64_t lba, size_t count, const void *buf)
{
    return stor_rw(drv, lba, count, (void *)buf, true);
}

static const struct blk_ops stor_ops = { stor_read, stor_write };

/* A drive's interface found: its endpoints, then the drive (INQUIRY, ready, capacity), then its disk. */
static bool stor_setup(struct usb_dev *d, const struct stor_info *si, uint8_t config_value)
{
    struct usbstor *s = NULL;                    /* (an unplugged drive keeps its slot: its disk stays) */
    for (int i = 0; i < STOR_POOL && !s; i++)
        if (!stors[i].used)
            s = &stors[i];
    if (!s) {
        kprintf("usb %s: %04x:%04x drive: too many drives\n", d->name, d->vendor, d->product);
        return true;
    }
    if (!s->buf) {                               /* 64 KiB at a 64 KiB boundary, below 4 GiB (lowest first) */
        uint64_t pa = pmm_alloc_contig(31), al = (pa + 0xFFFF) & ~0xFFFFUL;
        if (!s->cmd_pa)
            s->cmd_pa = dma_page(d->hc);
        if (!pa || !s->cmd_pa || (!d->hc->ac64 && al + STOR_MAX > DIRECT_MAP_SIZE)) {
            if (pa)
                pmm_free_contig(pa, 31);
            kprintf("usb %s: drive: no memory for its transfers\n", d->name);
            return true;
        }
        s->buf_pa = al;
        s->buf = P2V(al);
        s->cmd = P2V(s->cmd_pa);
    }
    if (control(d, 0x00, 9, config_value, 0, 0) < 0)     /* SET_CONFIGURATION */
        return false;
    const struct cdc_ep *src[2] = { &si->in, &si->out };
    for (int k = 0; k < 2; k++) {
        struct usb_ep *e = &d->ep[k];
        e->used = true;
        e->halted = false;
        e->errors = 0;
        e->type = k ? EP_BULK_OUT : EP_BULK_IN;
        e->addr = src[k]->addr;
        e->dci = (src[k]->addr & 0xF) * 2 + (k ? 0 : 1);
        e->maxp = src[k]->maxp ? src[k]->maxp : 512;
        e->burst = d->speed >= SPEED_SS ? src[k]->burst : 0;
        e->interval = 0;
        e->hid = NULL;
        e->net = NULL;
        e->stor = s;
        ring_reset(&e->ring);
    }
    d->nep = 2;
    if (!configure_endpoints(d, 0)) {
        d->nep = 0;
        return false;
    }
    s->d = d;
    s->in = &d->ep[0];
    s->out = &d->ep[1];
    s->iface = si->iface;
    control(d, 0xA1, 0xFE, 0, si->iface, 1);     /* GET MAX LUN (only LUN 0 is used; it may stall) */

    const uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
    char what[48] = "";
    uint32_t n = 0;
    if (stor_cmd(s, inquiry, 6, true, 36, &n) == 0 && n >= 32) {
        char v[9], p[17];
        memcpy(v, s->buf + 8, 8);
        memcpy(p, s->buf + 16, 16);
        v[8] = p[16] = 0;
        for (int i = 7; i >= 0 && (v[i] == ' ' || !v[i]); i--)
            v[i] = 0;
        for (int i = 15; i >= 0 && (p[i] == ' ' || !p[i]); i--)
            p[i] = 0;
        snprintf(what, sizeof(what), "%s%s%s", v, v[0] && p[0] ? " " : "", p);
    }
    const uint8_t tur[6] = { 0 };                /* TEST UNIT READY: a card reader without a card is not */
    int r = -1;
    for (int t = 0; t < 30 && (r = stor_cmd(s, tur, 6, false, 0, NULL)) != 0; t++) {
        if (r < 0 || stor_sense(s) >> 8 == 0x023A)   /* (NOT READY, MEDIUM NOT PRESENT) */
            break;
        mdelay(100);
    }
    uint64_t blocks = 0;
    uint32_t bsize = 0;
    const uint8_t cap10[10] = { 0x25 };
    if (r == 0 && stor_cmd(s, cap10, 10, true, 8, &n) == 0 && n >= 8) {
        uint32_t last = get32be(s->buf);
        bsize = get32be(s->buf + 4);
        blocks = (uint64_t)last + 1;
        uint8_t cap16[16] = { 0x9E, 0x10 };      /* READ CAPACITY (16), for 2 TiB and more */
        cap16[13] = 32;
        if (last == 0xFFFFFFFF && stor_cmd(s, cap16, 16, true, 32, &n) == 0 && n >= 12) {
            blocks = ((uint64_t)get32be(s->buf) << 32 | get32be(s->buf + 4)) + 1;
            bsize = get32be(s->buf + 8);
        }
    }
    if (!blocks || bsize != 512) {
        kprintf("usb %s: %04x:%04x drive %s: %s, not used\n", d->name, d->vendor, d->product, what,
                !blocks ? "no medium" : "blocks other than 512 bytes");
        s->d = NULL;
        d->ep[0].stor = d->ep[1].stor = NULL;
        d->nep = 0;
        return true;
    }
    char name[24], desc[64], sz[16];
    snprintf(name, sizeof(name), "c8t%dd0p0", (int)(s - stors));
    uint64_t mib = blocks / 2048;
    if (mib >= 10240)
        snprintf(sz, sizeof(sz), "%lu GiB", mib >> 10);
    else if (mib >= 1024)
        snprintf(sz, sizeof(sz), "%lu.%lu GiB", mib >> 10, (mib & 1023) * 10 / 1024);
    else
        snprintf(sz, sizeof(sz), "%lu MiB", mib);
    s->dev = blk_register(name, blocks, &stor_ops, s);
    if (s->dev < 0) {
        kprintf("usb %s: drive %s: no device number left\n", d->name, what);
        s->d = NULL;
        d->ep[0].stor = d->ep[1].stor = NULL;
        d->nep = 0;
        return true;
    }
    s->used = true;
    s->gone = false;
    s->blocks = blocks;
    s->bsize = bsize;
    strlcpy(s->what, what, sizeof(s->what));
    snprintf(desc, sizeof(desc), "USB %s, %s", what[0] ? what : "drive", sz);
    blk_set_desc(s->dev, desc);
    nstor++;
    kprintf("usb %s: %04x:%04x drive %s: %s, %s\n", d->name, d->vendor, d->product, what, name, sz);
    return true;
}

static void hub_scan(struct usb_dev *hub);

/* After the address: descriptors, configuration, the HID interfaces or the hub. */
static bool dev_configure(struct usb_dev *d)
{
    int n = control(d, 0x80, 6, 0x0100, 0, 8);
    if (n < 8)
        return false;
    uint8_t *b = d->buf;
    int mps = d->speed >= SPEED_SS ? 1 << b[7] : b[7];
    if (mps != d->mps0 && mps >= 8) {            /* EP0's max packet: Evaluate Context */
        d->mps0 = mps;
        memset(d->ictx, 0, PAGE_SIZE);
        ictx(d, 0)[1] = 2;
        ep0_ctx(d, ictx(d, 2));
        ictx(d, 2)[2] = (uint32_t)d->ep0.pa | d->ep0.cycle;     /* (ignored by Evaluate Context) */
        command(d->hc, d->ictx_pa, TRB_TYPE(TRB_EVAL_CTX) | (uint32_t)d->slot << 24, NULL);
    }
    if (control(d, 0x80, 6, 0x0100, 0, 18) < 18)
        return false;
    d->vendor = b[8] | b[9] << 8;
    d->product = b[10] | b[11] << 8;
    uint8_t dev_class = b[4];
    int nconfigs = b[17];
    if (control(d, 0x80, 6, 0x0200, 0, 9) < 9)
        return false;
    int total = b[2] | b[3] << 8;
    total = total > (int)PAGE_SIZE ? (int)PAGE_SIZE : total;
    if ((n = control(d, 0x80, 6, 0x0200, 0, total)) < 9)
        return false;
    static uint8_t cfg[PAGE_SIZE / 4];           /* (d->buf is reused; parsed before any recursion into a hub) */
    n = n > (int)sizeof(cfg) ? (int)sizeof(cfg) : n;
    memcpy(cfg, b, n);
    uint8_t config_value = cfg[5];

    /* the interfaces: HID ones with an interrupt IN endpoint, a hub's */
    struct { int iface, sub, proto, rlen; } hidif[MAX_EP];
    int cur_class = -1, cur_iface = 0, cur_sub = 0, cur_proto = 0, cur_alt = 0, cur_rlen = 0;
    bool is_hub = dev_class == 9;
    struct stor_info si = { .iface = -1 };
    struct cdc_ep *last_bulk = NULL;             /* (a SuperSpeed companion follows its endpoint) */
    d->nep = 0;
    for (int i = 0; i + 2 <= n && cfg[i] >= 2; i += cfg[i]) {
        uint8_t *x = cfg + i;
        if (i + x[0] > n)
            break;
        if (x[1] == 4 && x[0] >= 9) {            /* interface */
            cur_iface = x[2];
            cur_alt = x[3];
            cur_class = x[5];
            cur_sub = x[6];
            cur_proto = x[7];
            cur_rlen = 0;
            if (cur_class == 9)
                is_hub = true;
            if (cur_class == 8 && cur_sub == 6 && cur_proto == 0x50 && cur_alt == 0 && si.iface < 0)
                si.iface = cur_iface;            /* mass storage: SCSI, bulk-only */
            last_bulk = NULL;
        } else if (x[1] == 5 && x[0] >= 7 && cur_alt == 0 && si.iface >= 0 && cur_iface == si.iface &&
                   (x[3] & 3) == 2) {            /* the drive's bulk endpoints */
            last_bulk = (x[2] & 0x80) ? &si.in : &si.out;
            last_bulk->addr = x[2];
            last_bulk->maxp = (x[4] | x[5] << 8) & 0x7FF;
            last_bulk->burst = 0;
        } else if (x[1] == 0x30 && x[0] >= 6 && last_bulk) {
            last_bulk->burst = x[2] & 0xF;
            last_bulk = NULL;
        } else if (x[1] == 0x21 && x[0] >= 9 && cur_class == 3) {   /* HID: the report descriptor's length */
            cur_rlen = x[7] | x[8] << 8;
        } else if (x[1] == 5 && x[0] >= 7 && cur_alt == 0 && (cur_class == 3 || cur_class == 9) &&
                   (x[2] & 0x80) && (x[3] & 3) == 3 && d->nep < MAX_EP) {       /* interrupt IN */
            struct usb_ep *e = &d->ep[d->nep];
            int num = x[2] & 0xF;
            e->used = true;
            e->halted = false;
            e->errors = 0;
            e->addr = x[2];
            e->dci = num * 2 + 1;
            int maxp = (x[4] | x[5] << 8) & 0x7FF;
            e->len = maxp > 1024 ? 1024 : maxp ? maxp : 8;
            e->maxp = e->len;
            e->nbuf = e->len <= 256 && cur_class == 3 ? 4 : 1;
            e->type = EP_INT_IN;
            e->burst = 0;
            e->net = NULL;
            e->stor = NULL;
            ring_reset(&e->ring);
            e->hid = NULL;
            e->interval = interval_of(d->speed, x[6]);
            if (cur_class == 3)
                hidif[d->nep] = (typeof(hidif[0])){ cur_iface, cur_sub, cur_proto, cur_rlen };
            else
                hidif[d->nep].iface = -1;
            d->nep++;
        }
    }
    if (is_hub && d->speed >= SPEED_SS) {
        kprintf("usb %s: %04x:%04x USB 3 hub, not used (its USB 2 half is)\n", d->name, d->vendor, d->product);
        d->nep = 0;
        return true;
    }
    if (!d->nep && !is_hub && si.iface >= 0 && si.in.addr && si.out.addr)
        return stor_setup(d, &si, config_value);
    if (!d->nep && !is_hub) {                    /* a network adapter, perhaps in another configuration */
        struct cdc_info ci;
        bool found = cdc_parse(cfg, n, &ci);
        for (int k = 1; !found && k < nconfigs && k < 8; k++) {
            if (control(d, 0x80, 6, 0x0200 | k, 0, 9) < 9)
                break;
            int t = b[2] | b[3] << 8;
            t = t > (int)sizeof(cfg) ? (int)sizeof(cfg) : t;
            if ((n = control(d, 0x80, 6, 0x0200 | k, 0, t)) < 9)
                break;
            memcpy(cfg, b, n);
            found = cdc_parse(cfg, n, &ci);
        }
        if (found)
            return usbnet_setup(d, &ci);
    }
    if (!d->nep) {
        if (debug)
            kprintf("usb %s: %04x:%04x class %02x, not used\n", d->name, d->vendor, d->product, dev_class);
        return true;
    }
    if (control(d, 0x00, 9, config_value, 0, 0) < 0)     /* SET_CONFIGURATION */
        return false;

    int ttt = 0;
    if (is_hub) {
        if (control(d, 0xA0, 6, 0x2900, 0, 9) < 7)
            return false;
        d->hub = true;
        d->hub_ports = b[2] > 15 ? 15 : b[2];
        ttt = (b[3] >> 5) & 3;
        for (int i = 0; i < d->nep; i++)
            d->ep[i].len = (d->hub_ports + 1 + 7) / 8;
    }
    if (!configure_endpoints(d, ttt))
        return false;

    if (is_hub) {
        kprintf("usb %s: %04x:%04x hub, %d ports\n", d->name, d->vendor, d->product, d->hub_ports);
        for (int i = 0; i < d->nep; i++)
            ep_queue(d, &d->ep[i]);
        hub_scan(d);
        return true;
    }
    int used = 0;
    for (int i = 0; i < d->nep; i++) {
        struct usb_ep *e = &d->ep[i];
        e->hid = &hids[(d - devs) * MAX_EP + i];
        if (hidif[i].iface < 0 ||
            !hid_setup(d, e, hidif[i].iface, hidif[i].sub, hidif[i].proto, hidif[i].rlen)) {
            e->hid = NULL;
            e->used = false;
            continue;
        }
        used++;
        ep_queue(d, e);
    }
    if (!used)
        kprintf("usb %s: %04x:%04x HID, no keyboard or pointer\n", d->name, d->vendor, d->product);
    return true;
}

/* A device found on a port (root, or a hub's): slot, address, configuration. */
static struct usb_dev *dev_attach(struct xhci *hc, struct usb_dev *parent, int port, int speed)
{
    struct usb_dev *d = dev_alloc();
    if (!d) {
        kprintf("usb: too many devices\n");
        return NULL;
    }
    int slot = 0;
    if (command(hc, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot) != CC_SUCCESS || !slot || slot > hc->max_slots) {
        kprintf("usb: no device slot\n");
        return NULL;
    }
    d->used = true;
    d->hc = hc;
    d->slot = slot;
    d->speed = speed;
    d->port = port;
    d->parent = parent;
    d->nep = 0;
    d->hub = false;
    d->hub_changed = false;
    memset(d->children_at, 0, sizeof(d->children_at));
    if (parent) {
        d->root_port = parent->root_port;
        d->depth = parent->depth + 1;
        d->route = parent->route | (uint32_t)(port > 15 ? 15 : port) << (4 * parent->depth);
        snprintf(d->name, sizeof(d->name), "%s.%d", parent->name, port);
        if (speed != SPEED_HS && parent->speed == SPEED_HS) {
            d->tt_slot = parent->slot;
            d->tt_port = port;
        } else {
            d->tt_slot = parent->tt_slot;
            d->tt_port = parent->tt_port;
        }
    } else {
        d->root_port = port;
        d->depth = 0;
        d->route = 0;
        d->tt_slot = d->tt_port = 0;
        snprintf(d->name, sizeof(d->name), "%d-%d", hc->index + 1, port);
    }
    d->mps0 = speed >= SPEED_SS ? 512 : speed == SPEED_HS ? 64 : 8;
    ring_reset(&d->ep0);
    memset(d->octx, 0, PAGE_SIZE);
    memset(d->ictx, 0, PAGE_SIZE);
    hc->dcbaa[slot] = d->octx_pa;
    uint32_t *icc = ictx(d, 0), *sl = ictx(d, 1);
    icc[1] = 3;                                  /* slot, EP0 */
    sl[0] = d->route | (uint32_t)speed << 20 | 1U << 27;
    sl[1] = (uint32_t)d->root_port << 16;
    sl[2] = (uint32_t)d->tt_slot | (uint32_t)d->tt_port << 8;
    ep0_ctx(d, ictx(d, 2));
    int cc = command(hc, d->ictx_pa, TRB_TYPE(TRB_ADDRESS_DEV) | (uint32_t)slot << 24, NULL);
    if (cc != CC_SUCCESS) {
        kprintf("usb %s: address device failed (%d)\n", d->name, cc);
        command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (uint32_t)slot << 24, NULL);
        hc->dcbaa[slot] = 0;
        d->used = false;
        return NULL;
    }
    mdelay(2);
    if (!dev_configure(d)) {
        kprintf("usb %s: could not be configured\n", d->name);
        command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (uint32_t)slot << 24, NULL);
        hc->dcbaa[slot] = 0;
        dev_free(d);
        return NULL;
    }
    return d;
}

/* ---------------------------------------------------------------- hubs */

static bool hub_port_status(struct usb_dev *hub, int port, uint16_t *status, uint16_t *change)
{
    if (control(hub, 0xA3, 0, 0, port, 4) < 4)
        return false;
    uint8_t *b = hub->buf;
    *status = b[0] | b[1] << 8;
    *change = b[2] | b[3] << 8;
    return true;
}

static void hub_port(struct usb_dev *hub, int port)
{
    uint16_t st, ch;
    if (!hub_port_status(hub, port, &st, &ch))
        return;
    if (ch & 1)
        control(hub, 0x23, 1, 16, port, 0);      /* clear C_PORT_CONNECTION */
    if (ch & 2)
        control(hub, 0x23, 1, 17, port, 0);      /* C_PORT_ENABLE */
    if (ch & 8)
        control(hub, 0x23, 1, 19, port, 0);      /* C_PORT_OVER_CURRENT */
    if (ch & 16)
        control(hub, 0x23, 1, 20, port, 0);      /* C_PORT_RESET */
    bool present = hub->children_at[port];
    if (present && (!(st & 1) || (ch & 1))) {    /* gone (or replaced) */
        for (int i = 0; i < MAX_DEV; i++)
            if (devs[i].used && devs[i].parent == hub && devs[i].port == port)
                dev_remove(&devs[i]);
        present = false;
    }
    if (present || !(st & 1))
        return;
    mdelay(100);                                 /* debounce */
    control(hub, 0x23, 3, 4, port, 0);           /* SET_FEATURE PORT_RESET */
    for (int t = 0; t < 50; t++) {
        mdelay(10);
        if (!hub_port_status(hub, port, &st, &ch))
            return;
        if (ch & 16)
            break;
    }
    control(hub, 0x23, 1, 20, port, 0);
    if (!(st & 2))
        return;                                  /* not enabled */
    mdelay(10);
    int speed = st & (1U << 9) ? SPEED_LS : st & (1U << 10) ? SPEED_HS : SPEED_FS;
    if (dev_attach(hub->hc, hub, port, speed))
        hub->children_at[port] = true;
}

static void hub_scan(struct usb_dev *hub)
{
    for (int p = 1; p <= hub->hub_ports; p++)
        control(hub, 0x23, 3, 8, p, 0);          /* SET_FEATURE PORT_POWER */
    uint8_t *b = hub->buf;
    int good = control(hub, 0xA0, 6, 0x2900, 0, 9) >= 7 ? b[5] * 2 : 100;
    mdelay(good + 100);
    for (int p = 1; p <= hub->hub_ports; p++)
        hub_port(hub, p);
}

/* ---------------------------------------------------------------- root ports */

static uint32_t port_neutral(uint32_t v)
{
    return v & ~(PORT_PED | PORT_PR | PORT_CHANGES | (1U << 16) | (1U << 31));
}

static void root_port(struct xhci *hc, int p)
{
    uint32_t v = rd32(hc->op, PORTSC(p));
    wr32(hc->op, PORTSC(p), port_neutral(v) | (v & PORT_CHANGES));      /* acknowledge the changes */
    struct usb_dev *d = hc->root[p];
    if (d && (!(v & PORT_CCS) || (v & (1U << 17)))) {
        dev_remove(d);
        d = NULL;
    }
    if (d || !(v & PORT_CCS))
        return;
    if (!(v & PORT_PED)) {                       /* USB 2: reset to enable (USB 3 links train by themselves) */
        wr32(hc->op, PORTSC(p), port_neutral(v) | PORT_PR);
        for (int t = 0; t < 50; t++) {
            mdelay(10);
            v = rd32(hc->op, PORTSC(p));
            if ((v & PORT_PRC) || !(v & PORT_PR))
                break;
        }
        wr32(hc->op, PORTSC(p), port_neutral(v) | (v & PORT_CHANGES));
        mdelay(10);
        v = rd32(hc->op, PORTSC(p));
        if (!(v & PORT_PED))
            return;
    }
    int speed = (v >> 10) & 0xF;
    hc->root[p] = dev_attach(hc, NULL, p, speed ? speed : SPEED_FS);
}

/* ---------------------------------------------------------------- controller */

static void bios_handoff(struct xhci *hc)
{
    uint32_t off = (rd32(hc->cap, 0x10) >> 16) << 2;
    for (int guard = 0; off && guard < 64; guard++) {
        uint32_t v = rd32(hc->cap, off);
        if ((v & 0xFF) == 1) {                   /* USB legacy support */
            if (v & (1U << 16)) {
                *(volatile uint8_t *)(hc->cap + off + 3) = 1;       /* OS owned */
                for (int t = 0; t < 1000 && (rd32(hc->cap, off) & (1U << 16)); t++)
                    mdelay(1);
                if (rd32(hc->cap, off) & (1U << 16)) {
                    kprintf("xhci: the firmware did not give up the controller; taking it\n");
                    *(volatile uint8_t *)(hc->cap + off + 2) = 0;
                }
            }
            uint32_t ctl = rd32(hc->cap, off + 4);
            ctl &= (0x7U << 1) | (0xFFU << 5) | (0x7U << 17);        /* SMIs off */
            ctl |= 0x7U << 29;                                       /* (acknowledge the SMI events) */
            wr32(hc->cap, off + 4, ctl);
        }
        uint32_t next = (v >> 8) & 0xFF;
        off = next ? off + (next << 2) : 0;
    }
}

static bool hc_start(struct xhci *hc)
{
    const struct pci_dev *pd = &hc->pci;
    uint8_t pm = pci_find_cap(pd, 1, 0);         /* power management: D0 */
    if (pm) {
        uint32_t pmcsr = pci_read32(pd->bus, pd->dev, pd->func, pm + 4);
        if (pmcsr & 3) {
            pci_write32(pd->bus, pd->dev, pd->func, pm + 4, pmcsr & ~3U);
            mdelay(10);
        }
    }
    pci_enable_path(pd);
    uint32_t cmd = pci_read32(pd->bus, pd->dev, pd->func, 4);
    pci_write32(pd->bus, pd->dev, pd->func, 4, (cmd | 0x6) & ~0x400U);      /* memory, bus master, INTx off */
    bool io;
    uint64_t bar = pci_bar_addr(pd, 0, &io);
    uint64_t size = pci_bar_size(pd, 0);
    if (!bar || io)
        return false;
    hc->cap = mmio_map(bar, size ? size : 0x10000);
    if (!hc->cap)
        return false;
    uint32_t caplen = hc->cap[0], hcs1 = rd32(hc->cap, 4), hcs2 = rd32(hc->cap, 8), hcc1 = rd32(hc->cap, 0x10);
    hc->op = hc->cap + caplen;
    hc->rt = hc->cap + (rd32(hc->cap, 0x18) & ~0x1FU);
    hc->db = (volatile uint32_t *)(hc->cap + (rd32(hc->cap, 0x14) & ~3U));
    hc->max_slots = hcs1 & 0xFF;
    hc->max_slots = hc->max_slots > 64 ? 64 : hc->max_slots;
    hc->max_ports = (hcs1 >> 24) & 0xFF;
    hc->ac64 = hcc1 & 1;
    hc->ppc = hcc1 & 8;
    hc->ctx_size = hcc1 & 4 ? 64 : 32;
    if (rd32(hc->op, USBSTS) == 0xFFFFFFFF)
        return false;

    bios_handoff(hc);
    wr32(hc->op, USBCMD, rd32(hc->op, USBCMD) & ~1U);                       /* stop */
    for (int t = 0; t < 100 && !(rd32(hc->op, USBSTS) & STS_HCH); t++)
        mdelay(1);
    wr32(hc->op, USBCMD, 2);                                                /* reset */
    mdelay(1);
    for (int t = 0; t < 1000 && ((rd32(hc->op, USBCMD) & 2) || (rd32(hc->op, USBSTS) & STS_CNR)); t++)
        mdelay(1);
    if ((rd32(hc->op, USBCMD) & 2) || (rd32(hc->op, USBSTS) & STS_CNR))
        return false;
    mdelay(10);

    hc->dcbaa_pa = dma_page(hc);
    if (!hc->dcbaa_pa || !ring_init(hc, &hc->cmd))
        return false;
    hc->dcbaa = P2V(hc->dcbaa_pa);
    int nscratch = ((hcs2 >> 27) & 0x1F) | ((hcs2 >> 21) & 0x1F) << 5;
    if (nscratch) {
        uint64_t arr = dma_page(hc);
        if (!arr || nscratch > 512)
            return false;
        uint64_t *a = P2V(arr);
        for (int i = 0; i < nscratch; i++)
            if (!(a[i] = dma_page(hc)))
                return false;
        hc->dcbaa[0] = arr;
    }
    hc->ev_pa = dma_page(hc);
    uint64_t erst_pa = dma_page(hc);
    if (!hc->ev_pa || !erst_pa)
        return false;
    hc->ev = P2V(hc->ev_pa);
    hc->ev_idx = 0;
    hc->ev_cycle = 1;
    uint64_t *erst = P2V(erst_pa);
    erst[0] = hc->ev_pa;
    erst[1] = RING_TRBS;

    /* the device pool's memory (shared by the controllers: the first one allocates it) */
    for (int i = 0; i < MAX_DEV; i++) {
        struct usb_dev *d = &devs[i];
        if (d->ictx)
            continue;
        uint64_t a = dma_page(hc), b = dma_page(hc), c = dma_page(hc), e0 = dma_page(hc), eb = dma_page(hc);
        if (!a || !b || !c || !e0 || !eb)
            return false;
        d->ictx_pa = a, d->ictx = P2V(a);
        d->octx_pa = b, d->octx = P2V(b);
        d->buf_pa = c, d->buf = P2V(c);
        d->ep0.pa = e0, d->ep0.trb = P2V(e0);
        for (int k = 0; k < MAX_EP; k++) {
            uint64_t r = dma_page(hc);
            if (!r)
                return false;
            d->ep[k].ring.pa = r;
            d->ep[k].ring.trb = P2V(r);
            d->ep[k].buf_pa = eb + k * 1024;
            d->ep[k].buf = P2V(eb + k * 1024);
        }
    }

    for (int i = 0; i < NET_POOL; i++) {          /* the network adapters' buffers */
        struct usbnet *n = &nets[i];
        if (n->rx)
            continue;
        uint64_t rx = pmm_alloc_contig(NET_RXMEM / PAGE_SIZE), tx = pmm_alloc_contig(NET_NTX * NET_TXSZ / PAGE_SIZE);
        if (!rx || !tx || (!hc->ac64 && (rx + NET_RXMEM > DIRECT_MAP_SIZE || tx >= DIRECT_MAP_SIZE)))
            return false;
        n->rx_pa = rx, n->rx = P2V(rx);
        n->tx_pa = tx, n->tx = P2V(tx);
    }

    wr32(hc->op, CONFIG, hc->max_slots);
    wr64(hc->op, DCBAAP, hc->dcbaa_pa);
    wr64(hc->op, CRCR, hc->cmd.pa | 1);
    wr32(hc->rt, 0x20 + 0x08, 1);                                           /* ERSTSZ */
    wr64(hc->rt, 0x20 + 0x18, hc->ev_pa);                                   /* ERDP */
    wr64(hc->rt, 0x20 + 0x10, erst_pa);                                     /* ERSTBA */
    wr32(hc->rt, 0x20, 1);                                                  /* IMAN: acknowledge, IE off */
    wr32(hc->op, USBCMD, 1);                                                /* run (no interrupts) */
    for (int t = 0; t < 100 && (rd32(hc->op, USBSTS) & STS_HCH); t++)
        mdelay(1);
    if (rd32(hc->op, USBSTS) & STS_HCH)
        return false;
    if (hc->ppc)
        for (int p = 1; p <= hc->max_ports; p++) {
            uint32_t v = rd32(hc->op, PORTSC(p));
            if (!(v & PORT_PP))
                wr32(hc->op, PORTSC(p), port_neutral(v) | PORT_PP);
        }
    return true;
}

void usb_init(const char *cmdline)
{
    if (boot_option(cmdline, "nousb"))
        return;
    debug = boot_option(cmdline, "usbdebug");
    busy = true;
    for (int i = 0; i < pci_count() && nhc < MAX_HC; i++) {
        const struct pci_dev *pd = pci_at(i);
        if (pd->class_code != 0x0C || pd->subclass != 0x03 || pd->prog_if != 0x30)
            continue;
        struct xhci *hc = &hcs[nhc];
        hc->pci = *pd;
        hc->index = nhc;
        if (!hc_start(hc)) {
            kprintf("xhci: %04x:%04x at %02x:%02x.%x could not be started\n", pd->vendor, pd->device, pd->bus,
                    pd->dev, pd->func);
            continue;
        }
        pci_claim(pd, "xhci");
        kprintf("xhci %d: %04x:%04x, %d ports, %d slots%s\n", nhc + 1, pd->vendor, pd->device, hc->max_ports,
                hc->max_slots, hc->ac64 ? "" : ", 32-bit");
        nhc++;
    }
    /* connections show and USB 3 links train: 200 ms, then until the connected
     * ports have not changed for 300 ms and none is still training (2 s at most:
     * a USB drive the firmware booted from takes a moment after the reset) */
    uint64_t start = hrtime(), stable = start;
    uint32_t last = ~0U;
    while (nhc && hrtime() - start < 2000000000UL) {
        uint32_t conn = 0;
        bool training = false;
        for (int i = 0; i < nhc; i++)
            for (int p = 1; p <= hcs[i].max_ports; p++) {
                uint32_t v = rd32(hcs[i].op, PORTSC(p));
                if (v & PORT_CCS)
                    conn += (uint32_t)(i * 64 + p) * 2654435761U;   /* (a sum: which ports) */
                training |= ((v >> 5) & 0xF) == 7;                  /* PLS Polling */
            }
        if (conn != last) {
            last = conn;
            stable = hrtime();
        }
        if (hrtime() - start >= 200000000UL && !training && hrtime() - stable >= 300000000UL)
            break;
        mdelay(20);
    }
    for (int i = 0; i < nhc; i++) {
        struct xhci *hc = &hcs[i];
        for (int p = 1; p <= hc->max_ports; p++) {
            hc->port_change[p] = false;
            root_port(hc, p);
        }
        events(hc, 0, NULL, NULL);
        hc->ready = true;
    }
    busy = false;
}

bool usb_summary(char *buf, size_t n)
{
    if (!nhc)
        return false;
    int k = snprintf(buf, n, "USB: %d xHCI controller%s, %d keyboard%s, %d pointer%s", nhc, nhc > 1 ? "s" : "",
                     nkbd, nkbd == 1 ? "" : "s", nmouse, nmouse == 1 ? "" : "s");
    if (nnet && k > 0 && (size_t)k < n)
        k += snprintf(buf + k, n - k, ", %d network adapter%s", nnet, nnet == 1 ? "" : "s");
    if (nstor && k > 0 && (size_t)k < n)
        snprintf(buf + k, n - k, ", %d drive%s", nstor, nstor == 1 ? "" : "s");
    return true;
}

static void ep_recover(struct usb_dev *d, struct usb_ep *e)
{
    e->halted = false;
    if (++e->errors > 20) {
        kprintf("usb %s: endpoint %02x keeps failing, stopped\n", d->name, e->addr);
        e->used = false;
        return;
    }
    command(d->hc, 0, TRB_TYPE(TRB_RESET_EP) | (uint32_t)d->slot << 24 | (uint32_t)e->dci << 16, NULL);
    control(d, 0x02, 1, 0, e->addr, 0);          /* CLEAR_FEATURE ENDPOINT_HALT */
    struct usbnet *n = e->net;
    uint64_t f = 0;
    if (n && e->type == EP_BULK_OUT) {           /* (send pushes on this ring) */
        f = irq_off();
        spin_lock(&n->lock);
    }
    ring_reset(&e->ring);
    command(d->hc, e->ring.pa | e->ring.cycle,
            TRB_TYPE(TRB_SET_DEQ) | (uint32_t)d->slot << 24 | (uint32_t)e->dci << 16, NULL);
    if (!n) {
        ep_queue(d, e);
    } else if (e->type == EP_BULK_IN) {
        usbnet_requeue(n);
    } else {
        for (int i = 0; i < NET_NTX; i++)
            n->tx_busy[i] = false;
        memset(n->tx_tag, -1, sizeof(n->tx_tag));
        spin_unlock(&n->lock);
        irq_restore(f);
    }
}

/* From the timer tick (the boot CPU): events, plugged and unplugged devices, key repeat. */
void usb_poll(void)
{
    if (!nhc || __atomic_exchange_n(&busy, true, __ATOMIC_ACQUIRE))
        return;
    for (int i = 0; i < nhc; i++) {
        struct xhci *hc = &hcs[i];
        if (!hc->ready)
            continue;
        events(hc, 0, NULL, NULL);
        for (int p = 1; p <= hc->max_ports; p++)
            if (hc->port_change[p]) {
                hc->port_change[p] = false;
                root_port(hc, p);
            }
    }
    for (int i = 0; i < MAX_DEV; i++) {
        struct usb_dev *d = &devs[i];
        if (!d->used)
            continue;
        for (int k = 0; k < d->nep; k++) {
            if (d->ep[k].used && d->ep[k].halted && !d->ep[k].stor)
                ep_recover(d, &d->ep[k]);
            if (d->ep[k].hid)
                hid_tick(d->ep[k].hid);
        }
        if (d->hub && d->hub_changed) {
            d->hub_changed = false;
            uint8_t bits[4];
            memcpy(bits, d->hub_bits, sizeof(bits));
            memset(d->hub_bits, 0, sizeof(d->hub_bits));
            for (int p = 1; p <= d->hub_ports; p++)
                if (bits[p / 8] & (1 << (p % 8)))
                    hub_port(d, p);
        }
    }
    __atomic_store_n(&busy, false, __ATOMIC_RELEASE);
}

DDI_DRIVER("xhci", DDI_PHASE_BOOT, "USB 3 host controllers (xHCI): keyboards, pointers, Ethernet adapters");
DDI_ALIAS("pciclass,0c0330");

int _init(void)
{
    char msg[128];
    usb_init(ddi_cmdline());
    ddi_poll_register(usb_poll);
    if (usb_summary(msg, sizeof(msg)))
        ddi_report(msg);
    return 0;
}
