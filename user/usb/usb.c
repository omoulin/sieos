/*
 * usb - The USB server: the xHCI controller (xhci.c, found by pci.c) and
 * the devices plugged in it: USB disks (msc.c: a key can be SIEOS's whole
 * disk), keyboards and mice (hid.c), and hubs (here).
 *
 * A new device, step by step (enumeration):
 *   1. a port says "connected"; a USB 2 port is reset (USB 3 ones train
 *      their link by themselves) and reports the device's speed;
 *   2. "enable slot": the controller gives the device a slot number;
 *   3. "address device", with an input context (where it is: root port,
 *      route through hubs, speed; endpoint 0's ring);
 *   4. its descriptors over endpoint 0: the device (vendor, product,
 *      packet size), then the configuration (interfaces, endpoints);
 *   5. "set configuration", and each interface goes to its class driver,
 *      which adds its endpoints ("configure endpoint").
 * Hubs: their own ports, read and reset with control requests; a device
 * behind one carries a route string (4 bits per hub).
 *
 * Threads: main answers the port "usb" (how many disks, which); the
 * enumeration thread handles ports and hubs; each disk has a thread of its
 * own (msc.c); events arrive on the interrupt or polling thread (xhci.c).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "usb.h"

__thread long self_tid;
static usbdev_t *root[256];                 /* what is on each root port */
static volatile int settled;                /* the devices present at start are ready */
static long waiting[8];                     /* USB_DISKS asked before that: their tokens */
static int nwaiting_disks;
static volatile int wtok_lock;

/* A line in the kernel log (the serial port), from any thread. */
static struct { char b[200]; size_t n; } line;
static volatile int line_lock;
static void line_put(void *c, char ch) { (void)c; if (line.n < sizeof line.b) line.b[line.n++] = ch; }
void log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    lock(&line_lock);
    line.n = 0;
    vformat(line_put, 0, fmt, ap);
    sys_debug(line.b, line.n);
    unlock(&line_lock);
    va_end(ap);
}

struct sbuf { char *o; int cap, n; };
static void sput(void *c, char ch) { struct sbuf *b = c; if (b->n < b->cap - 1) b->o[b->n++] = ch; }
void sfmt(char *out, int cap, const char *fmt, ...)
{
    struct sbuf b = { out, cap, 0 };
    va_list ap;
    va_start(ap, fmt);
    vformat(sput, &b, fmt, ap);
    va_end(ap);
    out[b.n] = 0;
}

int dci_of(int ep) { return (ep & 15) * 2 + (ep & 0x80 ? 1 : 0); }

/* ---- Control transfers on endpoint 0: setup, data (if any), status. */
int control(usbdev_t *d, int type, int req, int value, int index, void *data, int len)
{
    if (len > 4096 || d->gone) return -EIO;
    int in = type & 0x80;
    lock(&d->ep0_lock);
    if (len && !in) { memcpy(d->cbuf, data, len); dsync(d->cbuf, len); }
    trb_t td[3];
    int n = 0;
    td[n++] = (trb_t){ (uint32_t)(type | req << 8 | value << 16), (uint32_t)(index | len << 16), 8,
                       2 << 10 | 1 << 6 | (len ? (in ? 3 : 2) : 0) << 16 };
    if (len) td[n++] = (trb_t){ (uint32_t)d->cbuf_bus, (uint32_t)(d->cbuf_bus >> 32), (uint32_t)len,
                                3 << 10 | (in ? 1 << 16 : 0) };
    td[n++] = (trb_t){ 0, 0, 0, 4 << 10 | 1 << 5 | (len && in ? 0 : 1 << 16) };
    int c = ring_td(d, 1, td, n, 0, 2000000000L);
    if (c == CC_STALL) ep_halted(d, 1);
    if ((c == CC_SUCCESS || c == CC_SHORT) && len && in) { dsync(d->cbuf, len); memcpy(data, d->cbuf, len); }
    unlock(&d->ep0_lock);
    return c == CC_SUCCESS || c == CC_SHORT ? len : -EIO;
}

/* An endpoint stalled: reset it in the controller, then tell the device. */
int clear_halt(usbdev_t *d, int ep)
{
    ep_halted(d, dci_of(ep));
    return control(d, 0x02, 1, 0, ep, 0, 0);          /* CLEAR_FEATURE(ENDPOINT_HALT) */
}

/* ---- Endpoints: "configure endpoint" with their descriptors (and the
 * SuperSpeed companion right after each, for its burst size). */
int configure(usbdev_t *d, const uint8_t **eps, int neps)
{
    int cs = ctx_size();
    memset(d->in, 0, cs * 33);
    uint32_t *icc = (uint32_t *)d->in, *sl = (uint32_t *)(d->in + cs);
    dsync(d->ctx, cs);
    memcpy(sl, d->ctx, cs);                            /* the slot as it is now */
    sl[3] = 0;
    int last = sl[0] >> 27;
    icc[1] = 1;
    for (int k = 0; k < neps; k++) {
        const uint8_t *e = eps[k];
        int addr = e[2], kind = e[3] & 3, mps = e[4] | e[5] << 8, dci = dci_of(addr), ival = e[6];
        if (kind == 0 || kind == 1) continue;          /* (no isochronous) */
        int burst = e[0] + 1 < 64 && e[e[0] + 1] == 0x30 ? e[e[0] + 2] : 0;   /* the companion, if next */
        if (ring_init(&d->ep[dci], kind == 2 ? 64 : 16)) return -ENOMEM;
        int type = (addr & 0x80 ? 4 : 0) + kind;       /* 2 bulk out, 3 int out, 6 bulk in, 7 int in */
        int interval = 0;
        if (kind == 3) {
            if (d->speed >= 3) interval = ival ? ival - 1 : 0;
            else for (interval = 3; interval < 10 && (1 << (interval + 1)) <= ival * 8; interval++) ;
        }
        uint32_t *ep = (uint32_t *)(d->in + cs * (dci + 1));
        ep[0] = interval << 16;
        ep[1] = 3 << 1 | type << 3 | burst << 8 | (mps & 0x7FF) << 16;
        ep[2] = (uint32_t)d->ep[dci].bus | 1;
        ep[3] = (uint32_t)(d->ep[dci].bus >> 32);
        ep[4] = (kind == 2 ? 1024 : mps) | (kind == 3 ? (mps & 0x7FF) * (burst + 1) << 16 : 0);
        icc[1] |= 1u << dci;
        if (dci > last) last = dci;
    }
    sl[0] = (sl[0] & ~(31u << 27) & ~(1u << 26) & ~(1u << 25)) | (uint32_t)last << 27
          | (d->nports ? 1u << 26 : 0) | (d->mtt ? 1u << 25 : 0);
    if (d->nports) { sl[1] = (sl[1] & 0x00FFFFFF) | (uint32_t)d->nports << 24; sl[2] = (sl[2] & ~(3u << 16)) | (uint32_t)d->ttt << 16; }
    dsync(d->in, cs * 33);
    int c = xhci_cmd((uint32_t)d->in_bus, (uint32_t)(d->in_bus >> 32), 0, 12 << 10 | d->slot << 24, 0);
    return c == CC_SUCCESS ? 0 : -EIO;
}

/* A string descriptor, as plain ASCII (other characters: '?'). */
static void string(usbdev_t *d, int idx, char *out, int cap)
{
    uint8_t b[256];
    out[0] = 0;
    if (!idx || control(d, 0x80, 6, 0x300 | idx, 0x409, b, 255) < 0 || b[1] != 3) return;
    int n = 0;
    for (int i = 2; i + 1 < b[0] && n < cap - 1; i += 2) out[n++] = b[i + 1] || b[i] < 32 || b[i] > 126 ? '?' : b[i];
    out[n] = 0;
}

static void detach(usbdev_t *d);
int hub_attach(usbdev_t *d, const uint8_t *f, int len);

/* ---- A new device on a root port (hub 0) or a hub's port. */
static usbdev_t *attach(usbdev_t *hub, int port, int speed)
{
    usbdev_t *d = malloc(sizeof *d);
    if (!d) return 0;
    memset(d, 0, sizeof *d);
    d->hub = hub; d->hub_port = port; d->speed = speed;
    if (hub) {
        d->root_port = hub->root_port;
        d->depth = hub->depth + 1;
        d->route = hub->route | (port > 15 ? 15 : port) << (4 * hub->depth);
        if (speed <= 2 && hub->speed == 3) { d->tt_slot = hub->slot; d->tt_port = port; }
        else { d->tt_slot = hub->tt_slot; d->tt_port = hub->tt_port; }
    } else d->root_port = port;
    d->mps0 = speed >= 4 ? 512 : speed == 3 ? 64 : 8;
    int cs = ctx_size();
    if (!(d->ctx = dmem(cs * 32, &d->ctx_bus)) || !(d->in = dmem(cs * 33, &d->in_bus)) ||
        !(d->cbuf = dmem(4096, &d->cbuf_bus)) || ring_init(&d->ep[1], 32) ||
        xhci_cmd(0, 0, 0, 9 << 10, &d->slot) != CC_SUCCESS) {
        log_line("usb: port %d: no slot for a new device\n", port);
        return 0;                                      /* (the memory stays: rare) */
    }
    slot_dev(d->slot, d);
    hc_dcbaa[d->slot] = d->ctx_bus;
    dsync(&hc_dcbaa[d->slot], 8);

    /* Address it: the slot (where it is) and endpoint 0. */
    uint32_t *icc = (uint32_t *)d->in, *sl = (uint32_t *)(d->in + cs), *e0 = (uint32_t *)(d->in + 2 * cs);
    icc[1] = 3;
    sl[0] = d->route | speed << 20 | 1u << 27;
    sl[1] = d->root_port << 16;
    sl[2] = d->tt_slot | d->tt_port << 8;
    e0[1] = 3 << 1 | 4 << 3 | d->mps0 << 16;
    e0[2] = (uint32_t)d->ep[1].bus | 1;
    e0[3] = (uint32_t)(d->ep[1].bus >> 32);
    e0[4] = 8;
    dsync(d->in, cs * 33);
    uint8_t dd[18];
    if (xhci_cmd((uint32_t)d->in_bus, (uint32_t)(d->in_bus >> 32), 0, 11 << 10 | d->slot << 24, 0) != CC_SUCCESS)
        goto fail;
    sys_sleep(10000000);                              /* (USB: 10 ms of recovery after the address) */
    if (control(d, 0x80, 6, 0x100, 0, dd, 8) < 0) goto fail;
    if (speed < 4 && dd[7] && dd[7] != d->mps0) {     /* endpoint 0's real packet size: evaluate context */
        d->mps0 = dd[7];
        memset(d->in, 0, cs * 3);
        icc[1] = 2;
        e0[1] = 3 << 1 | 4 << 3 | d->mps0 << 16;
        dsync(d->in, cs * 3);
        if (xhci_cmd((uint32_t)d->in_bus, (uint32_t)(d->in_bus >> 32), 0, 13 << 10 | d->slot << 24, 0) != CC_SUCCESS)
            goto fail;
    }
    if (control(d, 0x80, 6, 0x100, 0, dd, 18) < 0) goto fail;
    d->vendor = dd[8] | dd[9] << 8;
    d->product = dd[10] | dd[11] << 8;
    string(d, dd[15], d->name, sizeof d->name);
    string(d, dd[16], d->serial, sizeof d->serial);
    uint8_t c9[9];
    if (control(d, 0x80, 6, 0x200, 0, c9, 9) < 0) goto fail;
    d->cfglen = c9[2] | c9[3] << 8;
    if (d->cfglen > (int)sizeof d->cfg) d->cfglen = sizeof d->cfg;
    if (control(d, 0x80, 6, 0x200, 0, d->cfg, d->cfglen) < 0) goto fail;
    if (control(d, 0x00, 9, d->cfg[5], 0, 0, 0) < 0) goto fail;   /* set configuration */

    /* Each interface (alternate setting 0) to its class driver. */
    const char *what = "no driver";
    for (int i = 0; i + 1 < d->cfglen && d->cfg[i]; i += d->cfg[i]) {
        const uint8_t *f = d->cfg + i;
        if (f[1] != 4 || f[3] != 0) continue;
        int end = i + f[0];
        while (end + 1 < d->cfglen && d->cfg[end] && d->cfg[end + 1] != 4) end += d->cfg[end];
        int len = end - i, cls = f[5];
        if (cls == 9 && !hub_attach(d, f, len)) what = "hub";
        else if (cls == 8 && !msc_attach(d, f, len)) what = "disk";
        else if (cls == 3 && !hid_attach(d, f, len)) what = "keyboard/mouse";
    }
    static const char *sp[] = { "?", "full", "low", "high", "super", "super+" };
    char where[24];
    if (hub) sfmt(where, sizeof where, "port %d, hub port %d", d->root_port, port);
    else sfmt(where, sizeof where, "port %d", port);
    log_line("usb: %s: %04x:%04x %s, %s speed: %s\n", where, d->vendor, d->product,
             d->name[0] ? d->name : "(no name)", sp[speed < 6 ? speed : 0], what);
    return d;
fail:
    log_line("usb: port %d: a device that does not answer (speed %d)\n", port, speed);
    detach(d);
    return 0;
}

/* Unplugged: it and whatever hangs from it. Its memory is kept (a disk
 * thread may still be using it; unplugging is rare). */
static void detach(usbdev_t *d)
{
    d->gone = 1;
    for (int p = 0; p < 16; p++) if (d->child[p]) { detach(d->child[p]); d->child[p] = 0; }
    msc_detach(d);
    hid_detach(d);
    if (d->slot) { xhci_cmd(0, 0, 0, 10 << 10 | d->slot << 24, 0); slot_dev(d->slot, 0); }
}

/* ---- Hubs. Their status endpoint reports which ports changed (a bitmap);
 * we then read each such port with GET_STATUS, reset new devices, and
 * attach them. */
static void hub_async(usbdev_t *d, int dci, int code, uint32_t left)
{
    (void)code; (void)left;
    d->changed = 1;
    xfer_async(d, dci, d->cbuf_bus + 2048, (d->nports + 8) / 8);   /* listen again */
    if (enum_tid) sys_wake(enum_tid);
}

int hub_attach(usbdev_t *d, const uint8_t *f, int len)
{
    uint8_t h[12];
    int ss = d->speed >= 4;
    if (control(d, 0xA0, 6, (ss ? 0x2A : 0x29) << 8, 0, h, sizeof h) < 0) return -1;
    d->nports = h[2] > 15 ? 15 : h[2];
    d->ttt = (h[3] >> 5) & 3;
    d->mtt = d->speed == 3 && f[7] == 2;
    const uint8_t *ep = 0;
    for (int i = f[0]; i + 1 < len; i += f[i]) if (f[i + 1] == 5 && (f[i + 2] & 0x80)) { ep = f + i; break; }
    if (!ep || configure(d, &ep, 1)) return -1;
    if (ss) control(d, 0x20, 12, d->depth, 0, 0, 0);          /* SET_HUB_DEPTH */
    for (int p = 1; p <= d->nports; p++) control(d, 0x23, 3, 8, p, 0, 0);   /* SET_FEATURE(PORT_POWER) */
    sys_sleep((h[5] ? h[5] : 50) * 2000000L);                  /* power-on to power-good */
    int dci = dci_of(ep[2]);
    d->async[dci] = hub_async;
    xfer_async(d, dci, d->cbuf_bus + 2048, (d->nports + 8) / 8);
    d->changed = 1;
    return 0;
}

static void hub_scan(usbdev_t *d)
{
    for (int p = 1; p <= d->nports && !d->gone; p++) {
        uint8_t s[4];
        if (control(d, 0xA3, 0, 0, p, s, 4) < 0) continue;
        int st = s[0] | s[1] << 8, ch = s[2] | s[3] << 8;
        for (int b = 0; b < 16; b++) if (ch & 1 << b) control(d, 0x23, 1, 16 + b, p, 0, 0);   /* clear C_PORT_... */
        if ((st & 1) && !d->child[p]) {
            control(d, 0x23, 3, 4, p, 0, 0);                   /* SET_FEATURE(PORT_RESET) */
            for (int t = 0; t < 50; t++) {
                sys_sleep(10000000);
                if (control(d, 0xA3, 0, 0, p, s, 4) < 0) break;
                if ((s[2] | s[3] << 8) & 0x10) break;          /* C_PORT_RESET */
            }
            control(d, 0x23, 1, 20, p, 0, 0);
            st = s[0] | s[1] << 8;
            if (!(st & 2)) continue;                           /* not enabled */
            int speed = d->speed >= 4 ? 4 : st & 0x200 ? 2 : st & 0x400 ? 3 : 1;
            sys_sleep(10000000);
            d->child[p] = attach(d, p, speed);
        } else if (!(st & 1) && d->child[p]) {
            detach(d->child[p]);
            d->child[p] = 0;
        }
    }
}

static void hubs(usbdev_t *d)
{
    if (!d || d->gone || !d->nports) return;
    if (d->changed) { d->changed = 0; hub_scan(d); }
    for (int p = 1; p <= d->nports; p++) hubs(d->child[p]);
}

/* ---- Root ports. */
static void root_scan(int p)
{
    /* Clear the change bits, until none is left: a port reports nothing new
     * while one stays set (e.g. a link change just after the disconnect). */
    uint32_t v = portsc(p);
    for (int k = 0; k < 8 && (v & 0x00FE0000); k++) { port_write(p, v & 0x00FE0000); v = portsc(p); }
    if ((v & 1) && !root[p]) {
        if (!(v & 2)) {                                        /* USB 2: reset to enable it */
            port_write(p, 1u << 4);
            for (int t = 0; t < 50 && !(portsc(p) & 1u << 21); t++) sys_sleep(10000000);
            port_write(p, 1u << 21);
            v = portsc(p);
            if (!(v & 2)) return;
        }
        sys_sleep(10000000);
        root[p] = attach(0, p, v >> 10 & 15);
    } else if (!(v & 1) && root[p]) {
        detach(root[p]);
        root[p] = 0;
        log_line("usb: port %d: unplugged\n", p);
    }
}

/* The devices present at start: settled once things have been quiet a
 * moment (a key found: 0.2 s; none: 1.5 s), or after 5 s whatever happens. */
static void settle(int64_t start, int64_t last_news)
{
    if (settled) return;
    int64_t now = sys_clock(), up = now - start, quiet = now - last_news;
    if (!((msc_count() && up > 200000000L && quiet > 100000000L) || (up > 1500000000L && quiet > 300000000L) ||
          up > 5000000000L)) return;
    settled = 1;
    lock(&wtok_lock);
    for (int i = 0; i < nwaiting_disks; i++) reply_val(waiting[i], msc_count());
    nwaiting_disks = 0;
    unlock(&wtok_lock);
}

static volatile int enum_ready;
static void enum_thread(void *a)
{
    (void)a;
    while (!enum_ready) sys_yield();
    self_tid = enum_tid;
    int64_t start = sys_clock(), news = start;
    sys_sleep(20000000);
    for (int pass = 0; ; pass++) {
        uint32_t ev = __atomic_exchange_n(&port_events, 0, __ATOMIC_ACQUIRE);
        if (!pass || ev) { news = sys_clock(); for (int p = 1; p <= hc_ports; p++) root_scan(p); }
        for (int p = 1; p <= hc_ports; p++) hubs(root[p]);
        settle(start, news);
        sys_sleep(settled ? 60000000000L : 50000000L);        /* (a port event wakes us early) */
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    mk_ident_t id;
    sys_ident(0, &id);
    self_tid = id.pid;                                         /* the first thread's id is the pid */
    long port = port_create("usb");                            /* first: vblk may look for it */
    hc_info_t h;
    if (hc_find(&h, 0)) return 0;                              /* no controller: nothing to do */
    if (xhci_start(&h)) { log_line("usb: %s does not start\n", h.what); return 1; }
    log_line("usb: %s, %d ports, %s\n", h.what, hc_ports, hc_polling ? "polled" : "interrupts");
    if (h.irq >= 0) log_line("usb: interrupt line %d%s\n", h.irq & 0xFF, h.irq & IRQ_LEVEL ? " (level)" : "");
    enum_tid = thread_start(enum_thread, 0, 16384);
    enum_ready = 1;

    static usb_disk_t info;
    msg_t m = { 0 };
    long from = ipc_recv(port, &m);
    for (;;) {
        long r = -ENOSYS;
        int out = 0;
        if (from > 0) switch (m.w[0]) {
        case USB_DISKS:
            lock(&wtok_lock);
            if (!settled && nwaiting_disks < 8) { waiting[nwaiting_disks++] = from; unlock(&wtok_lock); from = 0; break; }
            unlock(&wtok_lock);
            r = msc_count();
            break;
        case USB_DISK: r = msc_info((int)m.w[1], &info); out = !r; break;
        }
        m = (msg_t){ .w = { r }, .sbuf = out ? &info : 0, .slen = out ? sizeof info : 0 };
        from = from > 0 ? ipc_reply_recv(from, port, &m) : ipc_recv(port, &m);
    }
}
