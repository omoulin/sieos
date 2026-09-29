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
 * Of each device only the HID interfaces with an interrupt IN endpoint are
 * used: boot keyboards and mice in the boot protocol (fixed reports),
 * anything else (tablets, keyboards and mice that are not boot devices) in
 * the report protocol with its report descriptor (hid.c).  Mass storage,
 * audio, ... are left alone: the root file system is already in memory.
 *
 * Enumeration is synchronous (commands and control transfers wait for
 * their completion events, dispatching the others meanwhile), at boot and
 * from the timer tick when something is plugged in.  All the memory is
 * allocated when the controller starts, none in interrupt context.
 */
#include "hid.h"
#include "pci.h"
#include "mm.h"
#include "poll.h"

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

struct usb_ep {
    bool used, halted;
    uint8_t dci, addr;              /* device context index, endpoint address */
    uint8_t interval;               /* 2^interval x 125 us */
    uint16_t len;                   /* bytes per transfer */
    int errors;
    struct ring ring;
    uint8_t *buf;
    uint64_t buf_pa;
    struct hid *hid;                /* NULL: a hub's status change endpoint */
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
static bool busy;                   /* enumerating or polling: the tick keeps out */
static bool debug;
static int nkbd, nmouse;

static inline uint32_t rd32(volatile uint8_t *b, uint32_t off) { return *(volatile uint32_t *)(b + off); }
static inline void wr32(volatile uint8_t *b, uint32_t off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; }
static inline void wr64(volatile uint8_t *b, uint32_t off, uint64_t v)
{
    *(volatile uint32_t *)(b + off) = (uint32_t)v;
    *(volatile uint32_t *)(b + off + 4) = (uint32_t)(v >> 32);
}
static inline void barrier(void) { __asm__ volatile("sfence" ::: "memory"); }

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

static void ep_queue(struct usb_dev *d, struct usb_ep *e)
{
    ring_push(&e->ring, e->buf_pa, e->len, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    barrier();
    d->hc->db[d->slot] = e->dci;
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
        if (cc != CC_SUCCESS && cc != CC_SHORT) {
            e->halted = true;                    /* reset from usb_poll */
            return;
        }
        uint32_t n = residual <= e->len ? e->len - residual : 0;
        if (e->hid)
            hid_input(e->hid, e->buf, n);
        else {
            for (uint32_t k = 0; k < sizeof(d->hub_bits) && k < n; k++)
                d->hub_bits[k] |= e->buf[k];
            d->hub_changed = true;
        }
        e->errors = 0;
        ep_queue(d, e);
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
    int n = 0;
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
    }
    if (n > 1)
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
        ep[0] = (uint32_t)e->interval << 16;
        ep[1] = 3U << 1 | 7U << 3 | (uint32_t)e->len << 16;    /* CErr 3, interrupt IN */
        ep[2] = (uint32_t)e->ring.pa | e->ring.cycle;
        ep[3] = (uint32_t)(e->ring.pa >> 32);
        ep[4] = e->len | (uint32_t)e->len << 16;               /* average TRB length, max ESIT payload */
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

/* A word of the boot command line. */
bool boot_option(const char *cmdline, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = cmdline; *p;) {
        while (*p == ' ')
            p++;
        const char *e = strchr(p, ' ');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncmp(p, word, n))
            return true;
        p += len;
    }
    return false;
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
    if (nhc)
        mdelay(200);                             /* connections show, USB 3 links train */
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
    snprintf(buf, n, "USB: %d xHCI controller%s, %d keyboard%s, %d pointer%s", nhc, nhc > 1 ? "s" : "", nkbd,
             nkbd == 1 ? "" : "s", nmouse, nmouse == 1 ? "" : "s");
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
    ring_reset(&e->ring);
    command(d->hc, e->ring.pa | e->ring.cycle,
            TRB_TYPE(TRB_SET_DEQ) | (uint32_t)d->slot << 24 | (uint32_t)e->dci << 16, NULL);
    ep_queue(d, e);
}

/* From the timer tick (the boot CPU): events, plugged and unplugged devices, key repeat. */
void usb_poll(void)
{
    if (busy || !nhc)
        return;
    busy = true;
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
            if (d->ep[k].used && d->ep[k].halted)
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
    busy = false;
}
