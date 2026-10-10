/*
 * xhci.c - The USB host controller: xHCI, the standard one on PCs, on the
 * Raspberry Pi 4 (VL805 chip) and the Pi 5 (RP1 chip).
 *
 * The driver and the controller talk through memory:
 *   - the command ring: we ask ("enable a slot for a new device", "here are
 *     its endpoints"); a doorbell register says "look";
 *   - a transfer ring per endpoint: data to move, in TRBs (16 bytes each);
 *   - the event ring: the controller's answers ("command done", "transfer
 *     done", "a port changed"), then an interrupt.
 * A cycle bit in every TRB says whose turn it is, so neither side needs a
 * lock: the producer flips it each time round the ring.
 *
 * Threads: the interrupt thread reads the event ring. A thread that sends
 * a command or a transfer "arms" a waiter (the TRBs it waits for), rings
 * the doorbell and sleeps; the event wakes it (SYS_WAKE). Completions no
 * thread waits for (keyboards: an interrupt endpoint reports a key) call the
 * device's async function. When a platform's interrupt line is unknown (or
 * interrupts never come), a thread polls the event ring instead: every 0.5
 * ms while someone waits, every 8 ms otherwise.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "usb.h"

#define R32(b, o) (*(volatile uint32_t *)((volatile uint8_t *)(b) + (o)))
#define NEV 256                                /* event ring: one 4 KiB segment */

static volatile uint8_t *cap;                  /* capability registers ... */
static volatile uint8_t *op, *rt, *db;         /* ... operational, runtime, doorbells */
static int maxslots, csz, hcirq = -1;
int hc_ports;
volatile int hc_polling;
volatile uint32_t port_events;
long enum_tid;
static int64_t bus_off;
static int dma_low;                            /* DMA memory must be in the first GiB */
static ring_t cmdring;
static trb_t *ev;
static uint64_t ev_bus;
static int ev_i, ev_cyc = 1;
static usbdev_t *slots[256];
static long irq_port;

void lock(volatile int *l)   { while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) sys_yield(); }
void unlock(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

void *dmem(size_t n, uint64_t *bus)
{
    uint64_t pa;
    void *p = dma_alloc(((n + 4095) & ~4095UL) | (dma_low ? DMA_LOW : 0), &pa);
    if ((long)p < 0) return 0;
    *bus = pa + bus_off;
    return p;                                  /* (zeroed by the kernel) */
}
void dsync(volatile void *p, size_t n) { dma_sync(p, n); }
int ctx_size(void) { return csz; }
void slot_dev(int slot, usbdev_t *d) { slots[slot & 255] = d; }

/* ---- Rings. The last TRB is a link back to the first ("toggle cycle"). */
int ring_init(ring_t *r, int n)
{
    if (!(r->t = dmem(n * sizeof(trb_t), &r->bus))) return -1;
    r->n = n; r->i = 0; r->cyc = 1;
    r->t[n - 1] = (trb_t){ (uint32_t)r->bus, (uint32_t)(r->bus >> 32), 0, 6 << 10 | 2 };
    dsync(r->t, n * sizeof(trb_t));
    return 0;
}

/* Put a TRB (its cycle bit last, so the controller never sees half of it);
 * -> its bus address. Passing the link carries the chain bit over, so a
 * transfer may wrap around. */
static uint64_t put(ring_t *r, uint32_t p0, uint32_t p1, uint32_t st, uint32_t ctl)
{
    trb_t *t = &r->t[r->i];
    uint64_t at = r->bus + 16UL * r->i;
    t->p0 = p0; t->p1 = p1; t->st = st;
    __atomic_store_n(&t->ctl, ctl | r->cyc, __ATOMIC_RELEASE);
    dsync(t, 16);
    if (++r->i == r->n - 1) {
        trb_t *l = &r->t[r->n - 1];
        __atomic_store_n(&l->ctl, 6 << 10 | 2 | (ctl & 1 << 4) | r->cyc, __ATOMIC_RELEASE);
        dsync(l, 16);
        r->i = 0;
        r->cyc ^= 1;
    }
    return at;
}
static void ring_db(int slot, int target) { dma_wmb(); R32(db, 4 * slot) = target; }

/* ---- Ports. Writing PORTSC: keep its state bits, never write 1 to "port
 * enabled" (that disables it), and write 1 only to the change bits to clear. */
uint32_t portsc(int port) { return R32(op, 0x400 + 16 * (port - 1)); }
void port_write(int port, uint32_t set) { R32(op, 0x400 + 16 * (port - 1)) = (portsc(port) & 0x4F00FFE9) | set; }

/* ---- Waiting for completions. */
typedef struct {
    uint64_t trb[6];                 /* the TRBs a completion may name */
    uint32_t len[6];
    volatile int n;                  /* 0: free */
    long tid;
    volatile int done, code, slot;
    uint32_t moved;
} wait_t;
static wait_t waits[8];
static volatile int wlock, evlock, nwaiting;
static int misses;                   /* completions found by waiters, not by interrupts ... */
static volatile int irqs_seen;       /* ... while no interrupt has ever come */

static wait_t *arm(void)
{
    for (;;) {
        lock(&wlock);
        for (int i = 0; i < 8; i++)
            if (!waits[i].n && !waits[i].tid) {
                wait_t *w = &waits[i];
                w->tid = self_tid; w->done = 0; w->code = 0; w->moved = 0;
                nwaiting++;
                unlock(&wlock);
                return w;
            }
        unlock(&wlock);
        sys_sleep(100000);
    }
}
static void disarm(wait_t *w)
{
    lock(&wlock);
    w->n = 0; w->tid = 0;
    nwaiting--;
    unlock(&wlock);
}

/* One event. A completion: the waiter whose TRB it names, else the
 * device's async function. A port change: the enumeration thread's news. */
static void event(uint64_t ptr, uint32_t st, uint32_t ctl)
{
    int type = ctl >> 10 & 63, code = st >> 24;
    if (type == 34) {                                       /* port status change */
        int port = ptr >> 24 & 0xFF;
        __atomic_or_fetch(&port_events, port < 32 ? 1u << port : 1u, __ATOMIC_RELEASE);
        if (enum_tid) sys_wake(enum_tid);
        return;
    }
    if (type != 32 && type != 33) return;                   /* transfer / command completion */
    lock(&wlock);
    for (int i = 0; i < 8; i++) {
        wait_t *w = &waits[i];
        for (int k = 0; k < w->n; k++)
            if (w->trb[k] == ptr && !w->done) {
                uint32_t moved = 0;
                for (int j = 0; j < k; j++) moved += w->len[j];
                w->moved = moved + w->len[k] - (type == 32 ? (st & 0xFFFFFF) : 0);
                w->code = code; w->slot = ctl >> 24;
                __atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
                sys_wake(w->tid);
                unlock(&wlock);
                return;
            }
    }
    unlock(&wlock);
    if (type == 32) {
        usbdev_t *d = slots[ctl >> 24];
        int dci = ctl >> 16 & 31;
        if (d && d->async[dci]) d->async[dci](d, dci, code, st & 0xFFFFFF);
    }
}

/* Read the event ring up to where the controller has written, then tell it
 * how far we read (ERDP; bit 3 clears "busy"). One reader at a time. */
static int events(void)
{
    if (__atomic_exchange_n(&evlock, 1, __ATOMIC_ACQUIRE)) return 0;
    int any = 0;
    for (;;) {
        trb_t *e = &ev[ev_i];
        dsync(e, 16);
        uint32_t ctl = __atomic_load_n(&e->ctl, __ATOMIC_ACQUIRE);
        if ((int)(ctl & 1) != ev_cyc) break;
        event(e->p0 | (uint64_t)e->p1 << 32, e->st, ctl);
        if (++ev_i == NEV) { ev_i = 0; ev_cyc ^= 1; }
        any = 1;
    }
    if (any) {
        uint64_t p = ev_bus + 16UL * ev_i;
        R32(rt, 0x38) = (uint32_t)p | 8;
        R32(rt, 0x3C) = (uint32_t)(p >> 32);
    }
    unlock(&evlock);
    return any;
}

static void poll_thread(void *a)
{
    (void)a;
    for (;;) { events(); sys_sleep(nwaiting ? 500000 : 8000000); }
}
static void start_polling(const char *why)
{
    if (hc_polling) return;
    hc_polling = 1;
    log_line("usb: %s: reading the controller's events by polling\n", why);
    thread_start(poll_thread, 0, 8192);
}

static void irq_thread(void *a)
{
    (void)a;
    for (;;) {
        msg_t m = { 0 };
        ipc_recv(irq_port, &m);
        irqs_seen = 1;
        R32(rt, 0x20) = 3;                     /* interrupter 0: clear "pending", keep "enabled" */
        R32(op, 4) = 8;                        /* status: clear "event interrupt" */
        events();
        irq_ack(hcirq & ~IRQ_LEVEL);
    }
}

/* Sleep until the waiter is done or the time is up. If its completion is
 * found here rather than by the interrupt thread three times, and no
 * interrupt has ever come, interrupts are not arriving (an unknown route):
 * poll from then on. (Once one has come, a late one is only slowness.) */
static int wait_end(wait_t *w, int64_t ns)
{
    int64_t end = sys_clock() + ns;
    while (!__atomic_load_n(&w->done, __ATOMIC_ACQUIRE)) {
        int64_t left = end - sys_clock();
        if (left <= 0) break;
        int64_t slice = hc_polling ? 200000 : 50000000;
        sys_sleep(slice < left ? slice : left);
        if (__atomic_load_n(&w->done, __ATOMIC_ACQUIRE)) break;
        if (events() && __atomic_load_n(&w->done, __ATOMIC_ACQUIRE) && !hc_polling && !irqs_seen && ++misses >= 3)
            start_polling("its interrupts do not arrive");
    }
    return w->done ? w->code : CC_TIMEOUT;
}

/* ---- Commands, one at a time. -> the completion code; *slot: the slot it names. */
static volatile int cmdlock;
int xhci_cmd(uint32_t p0, uint32_t p1, uint32_t st, uint32_t ctl, int *slot)
{
    lock(&cmdlock);
    wait_t *w = arm();
    w->trb[0] = put(&cmdring, p0, p1, st, ctl);
    w->len[0] = 0;
    __atomic_store_n(&w->n, 1, __ATOMIC_RELEASE);
    ring_db(0, 0);
    int c = wait_end(w, 5000000000L);
    if (slot) *slot = w->slot;
    disarm(w);
    unlock(&cmdlock);
    return c;
}

/* Stop an endpoint whose transfer timed out, and move its dequeue pointer
 * to where we will write next (the abandoned TRBs are skipped). */
static void ep_reset(usbdev_t *d, int dci)
{
    ring_t *r = &d->ep[dci];
    xhci_cmd(0, 0, 0, 15 << 10 | dci << 16 | d->slot << 24, 0);              /* stop endpoint */
    uint64_t at = r->bus + 16UL * r->i;
    xhci_cmd((uint32_t)at | r->cyc, (uint32_t)(at >> 32), 0, 16 << 10 | dci << 16 | d->slot << 24, 0);
}

/* An endpoint that stalled (the device refused): reset it in the
 * controller, and skip what is still queued on it. */
void ep_halted(usbdev_t *d, int dci)
{
    ring_t *r = &d->ep[dci];
    xhci_cmd(0, 0, 0, 14 << 10 | dci << 16 | d->slot << 24, 0);              /* reset endpoint */
    uint64_t at = r->bus + 16UL * r->i;
    xhci_cmd((uint32_t)at | r->cyc, (uint32_t)(at >> 32), 0, 16 << 10 | dci << 16 | d->slot << 24, 0);
}

/* A transfer descriptor: nb TRBs (the cycle bit added here), waited for. */
int ring_td(usbdev_t *d, int dci, const trb_t *td, int nb, uint32_t *done, int64_t ns)
{
    ring_t *r = &d->ep[dci];
    if (d->gone || !r->t || nb > 6) return CC_STOPPED;
    wait_t *w = arm();
    for (int k = 0; k < nb; k++) {
        w->trb[k] = put(r, td[k].p0, td[k].p1, td[k].st, td[k].ctl);
        int type = td[k].ctl >> 10 & 63;
        w->len[k] = type == 1 || type == 3 ? td[k].st & 0x1FFFF : 0;   /* normal, data: their bytes */
    }
    __atomic_store_n(&w->n, nb, __ATOMIC_RELEASE);
    ring_db(d->slot, dci);
    int c = wait_end(w, ns);
    if (done) *done = w->moved;
    disarm(w);
    if (c == CC_TIMEOUT && !d->gone) ep_reset(d, dci);
    return c;
}

/* A bulk or interrupt transfer of pieces of memory: normal TRBs, chained,
 * at most 64 KiB each and never crossing a 64 KiB boundary (an xHCI rule). */
int xfer(usbdev_t *d, int dci, int in, const uint64_t *bus, const uint32_t *len, int nb, uint32_t *done, int64_t ns)
{
    (void)in;
    trb_t td[6];
    int n = 0;
    for (int k = 0; k < nb; k++)
        for (uint64_t a = bus[k], e = bus[k] + len[k]; a < e && n < 6; ) {
            uint64_t stop = (a | 0xFFFF) + 1;
            uint32_t l = (uint32_t)((stop < e ? stop : e) - a);
            td[n++] = (trb_t){ (uint32_t)a, (uint32_t)(a >> 32), l, 1 << 10 | 1 << 2 | 1 << 4 };
            a += l;
        }
    if (!n) return CC_SUCCESS;
    td[n - 1].ctl = 1 << 10 | 1 << 2 | 1 << 5;     /* the last: no chain, interrupt on completion */
    return ring_td(d, dci, td, n, done, ns);
}

int xfer_async(usbdev_t *d, int dci, uint64_t bus, uint32_t len)
{
    ring_t *r = &d->ep[dci];
    if (d->gone || !r->t) return -1;
    put(r, (uint32_t)bus, (uint32_t)(bus >> 32), len, 1 << 10 | 1 << 2 | 1 << 5);
    ring_db(d->slot, dci);
    return 0;
}

/* ---- Taking the controller from the firmware, and starting it. */
static int spin(volatile uint8_t *b, int off, uint32_t mask, uint32_t want, int ms)
{
    for (int i = 0; i < ms * 10; i++) {
        if ((R32(b, off) & mask) == want) return 0;
        sys_sleep(100000);
    }
    return -1;
}

int xhci_start(const hc_info_t *h)
{
    bus_off = h->bus_off;
    dma_low = h->low;
    cap = map_phys(h->mmio, h->len);
    if ((long)cap < 0) return -1;
    if (h->dwc3) {                                /* DWC3: global control, port direction = host */
        R32(cap, 0xC110) = (R32(cap, 0xC110) & ~(3u << 12)) | 1u << 12;
        sys_sleep(1000000);
    }
    op = cap + (R32(cap, 0) & 0xFF);
    rt = cap + (R32(cap, 0x18) & ~0x1Fu);
    db = cap + (R32(cap, 0x14) & ~3u);
    uint32_t hcs1 = R32(cap, 4), hcs2 = R32(cap, 8), hcc1 = R32(cap, 0x10);
    maxslots = hcs1 & 0xFF;
    if (maxslots > 64) maxslots = 64;
    hc_ports = hcs1 >> 24;
    csz = hcc1 & 4 ? 64 : 32;
    if (!(hcc1 & 1)) dma_low = 1;                 /* 32-bit addresses only: keep DMA low */

    /* The firmware may still use it (keyboards at boot): ask for it
     * ("USB legacy support" capability) and turn its SMIs off. */
    for (uint32_t x = (hcc1 >> 16) * 4; x; ) {
        uint32_t v = R32(cap, x);
        if ((v & 0xFF) == 1) {
            R32(cap, x) = v | 1u << 24;
            spin(cap, x, 1u << 16, 0, 1000);
            uint32_t c = R32(cap, x + 4);
            R32(cap, x + 4) = (c & ~0xE011u) | 0xE0000000u;
        }
        x = (v >> 8 & 0xFF) ? x + (v >> 8 & 0xFF) * 4 : 0;
    }

    R32(op, 0) &= ~1u;                            /* stop, then reset */
    spin(op, 4, 1, 1, 100);
    R32(op, 0) = 2;
    if (spin(op, 0, 2, 0, 1000) || spin(op, 4, 1u << 11, 0, 1000)) return -1;
    R32(op, 0x38) = maxslots;

    /* The device context array (entry 0: the scratchpad pages the
     * controller may ask for), the command ring, the event ring. */
    uint64_t dcbaa_bus, erst_bus = 0;
    uint64_t *dcbaa = dmem(2048, &dcbaa_bus);
    int nsp = (hcs2 >> 21 & 0x1F) << 5 | hcs2 >> 27;
    if (!dcbaa) return -1;
    if (nsp) {
        uint64_t arr_bus, pg;
        uint64_t *arr = dmem(nsp * 8, &arr_bus);
        for (int i = 0; arr && i < nsp; i++) { if (!dmem(4096, &pg)) return -1; arr[i] = pg; }
        if (!arr) return -1;
        dsync(arr, nsp * 8);
        dcbaa[0] = arr_bus;
    }
    dsync(dcbaa, 2048);
    R32(op, 0x30) = (uint32_t)dcbaa_bus; R32(op, 0x34) = (uint32_t)(dcbaa_bus >> 32);
    hc_dcbaa = dcbaa;
    if (ring_init(&cmdring, 64)) return -1;
    R32(op, 0x18) = (uint32_t)cmdring.bus | 1; R32(op, 0x1C) = (uint32_t)(cmdring.bus >> 32);
    uint32_t *erst = dmem(64, &erst_bus);
    if (!(ev = dmem(NEV * 16, &ev_bus)) || !erst) return -1;
    erst[0] = (uint32_t)ev_bus; erst[1] = (uint32_t)(ev_bus >> 32); erst[2] = NEV;
    dsync(erst, 64);
    R32(rt, 0x28) = 1;                            /* one segment */
    R32(rt, 0x38) = (uint32_t)ev_bus; R32(rt, 0x3C) = (uint32_t)(ev_bus >> 32);
    R32(rt, 0x30) = (uint32_t)erst_bus; R32(rt, 0x34) = (uint32_t)(erst_bus >> 32);
    R32(rt, 0x24) = 160;                          /* at most one interrupt per 40 us */
    R32(rt, 0x20) = 2;                            /* interrupter 0 enabled */

    hcirq = h->irq;
    if (hcirq >= 0) {
        irq_port = port_create(0);
        if (irq_bind(hcirq, irq_port)) hcirq = -1;
        else thread_start(irq_thread, 0, 8192);
    }
    if (hcirq < 0) start_polling("no interrupt line known");
    R32(op, 0) = 1 | 4;                           /* run, with interrupts */
    if (spin(op, 4, 1, 0, 100)) return -1;
    if (hcc1 & 8) for (int p = 1; p <= hc_ports; p++) port_write(p, 1u << 9);   /* port power on */
    return 0;
}
uint64_t *hc_dcbaa;
