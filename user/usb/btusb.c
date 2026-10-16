/*
 * btusb.c - A Bluetooth adapter on USB (class 0xE0, subclass 1, protocol 1:
 * the laptop's Intel AX210, 8087:0032, and USB dongles), as a plain pipe
 * for the Bluetooth server (user/btd), which does everything else (Intel's
 * firmware, HCI, pairing). Port "bthci" (mk/proto.h):
 *   commands  -> the control endpoint (class request 0)
 *   events    <- the interrupt IN endpoint
 *   ACL data <-> the bulk endpoints (and Intel's bootloader commands and events)
 * One transfer stays queued on each IN endpoint, a packet at a time; what
 * arrives waits in a ring until btd asks (BTH_RECV: its reply is held until
 * then). A full ring leaves the endpoint unqueued: the adapter waits (USB's
 * own flow control), nothing is lost. No isochronous (SCO) endpoints.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "usb.h"

#define NREC 32
#define MPS  512
static struct {
    usbdev_t *d;
    int dci[2], mps[2], odci, oaddr;         /* [0] interrupt IN, [1] bulk IN; bulk OUT */
    uint8_t *ib[2], *ob;
    uint64_t ibus[2], obus;
    volatile int stopped[2];                 /* not queued: the ring was full */
    long tid, waiter;                        /* the port's thread; a BTH_RECV waiting */
} bt;
static struct { uint8_t ep, n0, n1, b[MPS]; } ring[NREC];
static int head, tail;
static volatile int rlock;
static uint8_t out[4096];

static int used(void) { return (head - tail + NREC) % NREC; }

/* The waiting BTH_RECV (if any) gets what the ring holds; endpoints left
 * unqueued are queued again once there is room. Called with rlock held. */
static void deliver(void)
{
    if (bt.waiter && head != tail) {
        int n = 0;
        while (head != tail) {
            int len = ring[tail].n0 | ring[tail].n1 << 8;
            if (n + 3 + len > (int)sizeof out) break;
            memcpy(out + n, &ring[tail], 3 + len);
            n += 3 + len;
            tail = (tail + 1) % NREC;
        }
        msg_t m = { .w = { (uint64_t)n }, .sbuf = out, .slen = (size_t)n };
        ipc_reply(bt.waiter, &m);
        bt.waiter = 0;
    }
    for (int e = 0; e < 2; e++)
        if (bt.stopped[e] && used() < NREC - 2) { bt.stopped[e] = 0; xfer_async(bt.d, bt.dci[e], bt.ibus[e], bt.mps[e]); }
}

static void bt_async(usbdev_t *d, int dci, int code, uint32_t left)
{
    int e = dci == bt.dci[1];
    lock(&rlock);
    if (code == CC_SUCCESS || code == CC_SHORT) {
        int n = bt.mps[e] - (int)left;
        dsync(bt.ib[e], bt.mps[e]);
        if (n > 0) {
            ring[head].ep = (uint8_t)e; ring[head].n0 = (uint8_t)n; ring[head].n1 = (uint8_t)(n >> 8);
            memcpy(ring[head].b, bt.ib[e], n);
            head = (head + 1) % NREC;
        }
    } else if (code == CC_STALL) ep_halted(d, dci);
    if (used() < NREC - 2) xfer_async(d, dci, bt.ibus[e], bt.mps[e]);
    else bt.stopped[e] = 1;
    deliver();
    unlock(&rlock);
}

static void bt_thread(void *a)
{
    (void)a;
    while (!bt.tid) sys_yield();
    self_tid = bt.tid;
    long port = port_create("bthci");
    static uint8_t in[2048];
    msg_t m = { .rbuf = in, .rlen = sizeof in };
    long from = ipc_recv(port, &m);
    for (;;) {
        long r = -ENOSYS;
        if (from <= 0 || m.uid != 0) r = -EPERM;
        else if (bt.d->gone) r = -EIO;
        else if (m.w[0] == BTH_RECV) {
            lock(&rlock);
            bt.waiter = from;                                /* answered by deliver(), now or later */
            deliver();
            unlock(&rlock);
            m = (msg_t){ .rbuf = in, .rlen = sizeof in };
            from = ipc_recv(port, &m);
            continue;
        } else if (m.w[0] == BTH_INFO) r = bt.d->vendor << 16 | bt.d->product;
        else if (m.w[0] == BTH_SEND && !m.w[1]) r = control(bt.d, 0x20, 0, 0, 0, in, (int)m.rlen) < 0 ? -EIO : 0;
        else if (m.w[0] == BTH_SEND && m.rlen <= 4096) {
            uint32_t len = (uint32_t)m.rlen, done;
            memcpy(bt.ob, in, len);
            dsync(bt.ob, len);
            int c = xfer(bt.d, bt.odci, 0, &bt.obus, &len, 1, &done, 2000000000L);
            if (c == CC_STALL) clear_halt(bt.d, bt.oaddr);
            r = c == CC_SUCCESS ? 0 : -EIO;
        }
        m = (msg_t){ .w = { (uint64_t)r }, .rbuf = in, .rlen = sizeof in };
        from = from > 0 ? ipc_reply_recv(from, port, &m) : ipc_recv(port, &m);
    }
}

/* ---- The HCI interface: an interrupt IN, a bulk IN and a bulk OUT endpoint. */
int btusb_attach(usbdev_t *d, const uint8_t *f, int len)
{
    const uint8_t *eps[3] = { 0 };
    if (f[6] != 1 || f[7] != 1) return -1;
    for (int i = f[0]; i + 1 < len; i += f[i])
        if (f[i + 1] == 5) {
            int in = f[i + 2] & 0x80, kind = f[i + 3] & 3;
            if (kind == 3 && in) eps[0] = f + i;
            else if (kind == 2) eps[in ? 1 : 2] = f + i;
        }
    if (!eps[0] || !eps[1] || !eps[2] || (bt.d && !bt.d->gone) || configure(d, eps, 3)) return -1;
    for (int e = 0; e < 2; e++) {
        bt.dci[e] = dci_of(eps[e][2]);
        bt.mps[e] = (eps[e][4] | eps[e][5] << 8) & 0x7FF;
        if (bt.mps[e] > MPS || !bt.mps[e]) bt.mps[e] = 64;
        if (!(bt.ib[e] = dmem(MPS, &bt.ibus[e]))) return -1;
        d->async[bt.dci[e]] = bt_async;
    }
    bt.oaddr = eps[2][2]; bt.odci = dci_of(bt.oaddr);
    if (!bt.ob && !(bt.ob = dmem(4096, &bt.obus))) return -1;
    bt.d = d;
    for (int e = 0; e < 2; e++) xfer_async(d, bt.dci[e], bt.ibus[e], bt.mps[e]);
    if (!bt.tid) bt.tid = thread_start(bt_thread, 0, 16384);
    return 0;
}

void btusb_detach(usbdev_t *d)
{
    if (d != bt.d) return;
    lock(&rlock);
    if (bt.waiter) { reply_val(bt.waiter, -EIO); bt.waiter = 0; }
    unlock(&rlock);
}
