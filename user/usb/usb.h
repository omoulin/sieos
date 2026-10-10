/*
 * usb.h - The USB server's parts (user/usb): finding the controller
 * (pci.c), driving it (xhci.c), the devices and hubs (usb.c), and the
 * class drivers: USB disks (msc.c), keyboards and mice (hid.c).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "mk.h"

/* ---- The controller, as pci.c found it. */
typedef struct {
    uint64_t mmio, len;          /* its registers (physical) */
    int irq;                     /* for irq_bind (IRQ_LEVEL included), or -1: we poll */
    int64_t bus_off;             /* the address the controller uses for RAM = physical + bus_off */
    int dwc3;                    /* a DWC3 core (the Pi 5's RP1): switch it to host mode first */
    int low;                     /* its DMA must be in the first GiB (the Pis' PCIe window) */
    char what[40];               /* for the log */
} hc_info_t;
int hc_find(hc_info_t *h, int nth);            /* 0: the nth controller, ready */
int pci_find(uint32_t cls, uint32_t mask, hc_info_t *h, int nth);   /* any PCI class (class code & mask) */

/* ---- Rings of TRBs ("transfer request blocks", 16 bytes): the driver
 * writes requests into a ring, the controller reads them; the controller
 * writes events into the event ring. A cycle bit tells whose turn a TRB is. */
typedef struct { uint32_t p0, p1, st, ctl; } trb_t;
typedef struct { trb_t *t; uint64_t bus; int n, i, cyc; } ring_t;

/* ---- A device. */
typedef struct usbdev usbdev_t;
typedef void async_fn(usbdev_t *d, int dci, int code, uint32_t left);
struct usbdev {
    int slot, speed;             /* speed: 1 full, 2 low, 3 high, 4+ super (xHCI's numbers) */
    int root_port, route, depth; /* where: the root port, the route through hubs (4 bits a hub) */
    int tt_slot, tt_port;        /* a low/full-speed device behind a high-speed hub: that hub */
    usbdev_t *hub;               /* its hub (0: a root port) ... */
    int hub_port;                /* ... and the port there */
    int mps0;                    /* endpoint 0's packet size */
    uint8_t *ctx;                /* its output ("device") context, the controller's copy */
    uint64_t ctx_bus;
    uint8_t *in;                 /* the input context: what we ask the controller to set */
    uint64_t in_bus;
    uint8_t *cbuf;               /* 4 KiB of DMA memory for control transfers' data */
    uint64_t cbuf_bus;
    int ttt, mtt;                /* a high-speed hub: its think time, multi-TT */
    ring_t ep[32];               /* a transfer ring per endpoint, by index (DCI: 1 = endpoint 0) */
    async_fn *async[32];         /* completions nobody waits for (interrupt endpoints) */
    volatile int ep0_lock;       /* one control transfer at a time */
    uint16_t vendor, product;
    char name[38], serial[32];
    uint8_t cfg[1024];           /* its configuration descriptor (with interfaces, endpoints) */
    int cfglen;
    volatile int gone;           /* unplugged */
    void *msc;                   /* its disk (msc.c) */
    void *hid[4];                /* its keyboard/pointer interfaces (hid.c) */
    int nports;                  /* a hub: its ports */
    volatile int changed;        /* a hub: its ports changed (the interrupt endpoint said so) */
    usbdev_t *child[16];         /* a hub: what is plugged in its ports */
};

/* ---- xhci.c */
extern int hc_ports;
extern volatile int hc_polling;
int  xhci_start(const hc_info_t *h);
void *dmem(size_t n, uint64_t *bus);           /* zeroed DMA memory (in the first GiB when the platform needs it); its bus address */
void dsync(volatile void *p, size_t n);        /* before the device reads it / after it wrote */
int  ring_init(ring_t *r, int n);
uint32_t portsc(int port);
void port_write(int port, uint32_t set);       /* keeps the port's state, sets `set` (change bits: clear them) */
int  xhci_cmd(uint32_t p0, uint32_t p1, uint32_t st, uint32_t ctl, int *slot);   /* -> completion code */
int  ctx_size(void);
void ep_halted(usbdev_t *d, int dci);          /* a stalled endpoint: reset it, skip what is queued */
/* A transfer: nb pieces (bus address, length) on endpoint dci, then wait
 * (ns: the time limit). -> completion code (1: success, 13: short), *done:
 * the bytes moved. */
int  xfer(usbdev_t *d, int dci, int in, const uint64_t *bus, const uint32_t *len, int nb, uint32_t *done, int64_t ns);
int  xfer_async(usbdev_t *d, int dci, uint64_t bus, uint32_t len);   /* queue one, no waiting */
/* nb TRBs (control transfers: setup, data, status) on endpoint dci, waited for. */
int  ring_td(usbdev_t *d, int dci, const trb_t *td, int nb, uint32_t *done, int64_t ns);
extern uint64_t *hc_dcbaa;                    /* the device context array: [slot] = its context */
void slot_dev(int slot, usbdev_t *d);         /* which device a slot is (for events) */
void lock(volatile int *l);
void unlock(volatile int *l);
extern long enum_tid;                         /* woken when a port changes */
extern volatile uint32_t port_events;         /* root ports with news (bit per port) */
enum { CC_SUCCESS = 1, CC_TX = 4, CC_STALL = 6, CC_SHORT = 13, CC_STOPPED = 26, CC_TIMEOUT = 0x100 };

/* ---- usb.c */
extern __thread long self_tid;
int  control(usbdev_t *d, int type, int req, int value, int index, void *data, int len);   /* -> bytes or -error */
int  clear_halt(usbdev_t *d, int ep_addr);
int  configure(usbdev_t *d, const uint8_t **eps, int neps);           /* add endpoints (their descriptors) */
int  dci_of(int ep_addr);
void log_line(const char *fmt, ...);
void sfmt(char *out, int cap, const char *fmt, ...);   /* printf into out */

/* ---- class drivers: attach (0: taken) and detach */
int  msc_attach(usbdev_t *d, const uint8_t *iface, int len);
void msc_detach(usbdev_t *d);
int  msc_count(void);
int  msc_info(int i, usb_disk_t *out);
int  msc_settled(void);                       /* every disk found at start is ready (or given up) */
int  hid_attach(usbdev_t *d, const uint8_t *iface, int len);
void hid_detach(usbdev_t *d);
int  hid_count(void);
