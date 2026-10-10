/*
 * msc.c - USB disks ("mass storage": keys, card readers, external drives),
 * each served on its own port, "usbdisk0", "usbdisk1"..., with the same
 * DISK_* protocol as vblk: 4 KiB blocks, up to 128 KiB a message. The disk
 * server (vblk) uses one as SIEOS's disk when SIEOS boots from a key.
 *
 * The protocol is "bulk-only transport": for each command, the driver sends
 * a 31-byte Command Block Wrapper (CBW: a tag, the length, the direction,
 * and a SCSI command) on the bulk-out endpoint; then the data moves on the
 * bulk-in or bulk-out endpoint; then the device answers with a 13-byte
 * Command Status Wrapper (CSW: same tag, status 0 = done, 1 = failed, 2 =
 * confused: reset it). The SCSI commands: INQUIRY, TEST UNIT READY, REQUEST
 * SENSE, READ CAPACITY, READ/WRITE (10 or 16), SYNCHRONIZE CACHE.
 *
 * No copies of our own: a client's data arrives by IPC straight into the
 * DMA buffer the controller reads, and read data is replied from where the
 * controller wrote it. Unplugged, a disk answers -EIO; plugged in again (the
 * same key: vendor, product, serial number, size), it is the same disk on
 * the same port, so the file server can carry on.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "usb.h"

#define MAXD 8
typedef struct {
    usbdev_t *d;
    int in, out, iface, lun;                 /* endpoint addresses, interface number */
    uint32_t bs;                             /* sector size */
    uint64_t nsect;
    char port[16];
    uint8_t *buf, *cb;                       /* DISK_MAX for data; CBW (0), CSW (64), sense (128) */
    uint64_t buf_bus, cb_bus;
    uint32_t tag;
    volatile int present;
    volatile long tid;
} msc_t;
static msc_t *disks[MAXD];
static int ndisks;

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* One bulk transfer: 0 done, 1 stalled (halt cleared), -1 failed. */
static int bulk(msc_t *s, int ep, uint64_t bus, uint32_t len, uint32_t *got)
{
    int c = xfer(s->d, dci_of(ep), ep & 0x80, &bus, &len, 1, got, 20000000000L);
    if (c == CC_STALL) { clear_halt(s->d, ep); return 1; }
    return c == CC_SUCCESS || c == CC_SHORT ? 0 : -1;
}

/* The device lost track: "bulk-only mass storage reset", then both halts. */
static void reset_recovery(msc_t *s)
{
    control(s->d, 0x21, 0xFF, 0, s->iface, 0, 0);
    clear_halt(s->d, s->in);
    clear_halt(s->d, s->out);
}

/* One SCSI command (data to or from p at bus address pb): -> 0 done, 1
 * failed (the sense says why), -EIO when the transport itself failed. */
static int scsi(msc_t *s, const uint8_t *cdb, int cl, int in, void *p, uint64_t pb, uint32_t len)
{
    if (!s->present || s->d->gone) return -EIO;
    uint8_t *w = s->cb;
    memset(w, 0, 31);
    w[0] = 'U'; w[1] = 'S'; w[2] = 'B'; w[3] = 'C';
    s->tag++;
    memcpy(w + 4, &s->tag, 4);
    w[8] = len; w[9] = len >> 8; w[10] = len >> 16; w[11] = len >> 24;
    w[12] = in ? 0x80 : 0;
    w[13] = s->lun;
    w[14] = cl;
    memcpy(w + 15, cdb, cl);
    dsync(w, 31);
    if (bulk(s, s->out, s->cb_bus, 31, 0)) { reset_recovery(s); return -EIO; }
    if (len) {
        dsync(p, len);
        if (bulk(s, in ? s->in : s->out, pb, len, 0) < 0) { reset_recovery(s); return -EIO; }
        if (in) dsync(p, len);
    }
    int r = bulk(s, s->in, s->cb_bus + 64, 13, 0);
    if (r == 1) r = bulk(s, s->in, s->cb_bus + 64, 13, 0);      /* the CSW stalled once: read it again */
    uint8_t *c = s->cb + 64;
    dsync(c, 13);
    if (r || le32(c) != 0x53425355 || le32(c + 4) != s->tag || c[12] > 1) { reset_recovery(s); return -EIO; }
    return c[12];
}

static int sense(msc_t *s)                    /* -> the sense key (6: unit attention) */
{
    uint8_t cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    return scsi(s, cdb, 6, 1, s->cb + 128, s->cb_bus + 128, 18) == 0 ? (s->cb[128 + 2] & 15) : -1;
}

/* Read or write cnt sectors at lba through the data buffer; one retry
 * after a "unit attention" (the device's way of saying it was reset). */
static int rw(msc_t *s, int write, uint64_t lba, uint32_t cnt)
{
    uint8_t cdb[16] = { 0 };
    int cl;
    if (lba + cnt <= 0xFFFFFFFFULL && cnt <= 0xFFFF) {
        cdb[0] = write ? 0x2A : 0x28;
        be32(cdb + 2, (uint32_t)lba);
        cdb[7] = cnt >> 8; cdb[8] = cnt;
        cl = 10;
    } else {
        cdb[0] = write ? 0x8A : 0x88;
        be32(cdb + 2, (uint32_t)(lba >> 32)); be32(cdb + 6, (uint32_t)lba);
        be32(cdb + 10, cnt);
        cl = 16;
    }
    for (int t = 0; t < 2; t++) {
        int r = scsi(s, cdb, cl, !write, s->buf, s->buf_bus, cnt * s->bs);
        if (r == 0) return 0;
        if (r < 0 || sense(s) != 6) return -EIO;
    }
    return -EIO;
}

/* ---- Each disk's thread: its port, the DISK_* protocol (as vblk's). */
static void disk_thread(void *a)
{
    msc_t *s = a;
    while (!s->tid) sys_yield();
    self_tid = s->tid;
    long port = port_create(s->port);
    msg_t m = { .rbuf = s->buf, .rlen = DISK_MAX };
    long from = ipc_recv(port, &m);
    for (;;) {
        uint64_t blk = m.w[1], n = m.w[2], bytes = n * DISK_BS, per = DISK_BS / s->bs;
        uint64_t nblocks = s->nsect / per;
        long r = 0;
        const void *out = 0;
        if (from <= 0) r = -EINVAL;
        else if (!s->present) r = -EIO;
        else switch (m.w[0]) {
        case DISK_INFO: r = nblocks; break;
        case DISK_READ:
            if (!n || bytes > DISK_MAX || blk + n > nblocks) r = -EINVAL;
            else if (!(r = rw(s, 0, blk * per, n * per))) out = s->buf;
            break;
        case DISK_WRITE:
            if (!n || bytes > DISK_MAX || blk + n > nblocks || m.rlen != bytes) r = -EINVAL;
            else r = rw(s, 1, blk * per, n * per);                /* the data arrived in buf */
            break;
        case DISK_FLUSH: {
            uint8_t cdb[10] = { 0x35 };
            r = scsi(s, cdb, 10, 0, 0, 0, 0) < 0 ? -EIO : 0;     /* (refused: no cache to flush) */
            break;
        }
        default: r = -ENOSYS;
        }
        m = (msg_t){ .w = { r }, .sbuf = out, .slen = out ? bytes : 0, .rbuf = s->buf, .rlen = DISK_MAX };
        from = from > 0 ? ipc_reply_recv(from, port, &m) : ipc_recv(port, &m);
    }
}

/* ---- A mass-storage interface (class 8, SCSI 6, bulk-only 0x50). */
int msc_attach(usbdev_t *d, const uint8_t *f, int len)
{
    if (f[6] != 6 || f[7] != 0x50) return -1;
    const uint8_t *eps[2] = { 0, 0 };
    for (int i = f[0]; i + 1 < len; i += f[i])
        if (f[i + 1] == 5 && (f[i + 3] & 3) == 2) eps[f[i + 2] & 0x80 ? 0 : 1] = f + i;
    if (!eps[0] || !eps[1] || configure(d, eps, 2)) return -1;
    msc_t t = { .d = d, .in = eps[0][2], .out = eps[1][2], .iface = f[2], .present = 1 }, *s = &t;
    if (!(t.cb = dmem(4096, &t.cb_bus))) return -1;

    /* Ready? A key may need a moment (its "unit attention" after power on). */
    uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 }, tur[6] = { 0 }, cap[10] = { 0x25 };
    if (scsi(s, inq, 6, 1, t.cb + 128, t.cb_bus + 128, 36) < 0) return -1;
    if ((t.cb[128] & 0x1F) != 0) return -1;                      /* not a direct-access disk */
    int r = 1;
    for (int k = 0; k < 50 && r; k++) {
        if ((r = scsi(s, tur, 6, 0, 0, 0, 0)) < 0) return -1;
        if (r) { sense(s); sys_sleep(100000000); }
    }
    if (r || scsi(s, cap, 10, 1, t.cb + 128, t.cb_bus + 128, 8)) return -1;
    t.nsect = (uint64_t)rd32(t.cb + 128) + 1;
    t.bs = rd32(t.cb + 132);
    if (t.nsect == 0x100000000ULL) {                             /* over 2 TiB: READ CAPACITY(16) */
        uint8_t c16[16] = { 0x9E, 0x10 };
        be32(c16 + 10, 32);
        if (scsi(s, c16, 16, 1, t.cb + 128, t.cb_bus + 128, 32)) return -1;
        t.nsect = ((uint64_t)rd32(t.cb + 128) << 32 | rd32(t.cb + 132)) + 1;
        t.bs = rd32(t.cb + 136);
    }
    if (t.bs < 512 || t.bs > DISK_BS || DISK_BS % t.bs) {
        log_line("usb: a disk with %u-byte sectors: not supported\n", t.bs);
        return -1;
    }

    /* The same key plugged in again: the same disk, the same port. */
    for (int i = 0; i < ndisks; i++) {
        msc_t *o = disks[i];
        if (!o->present && o->d->vendor == d->vendor && o->d->product == d->product &&
            !strcmp(o->d->serial, d->serial) && o->nsect == t.nsect && o->bs == t.bs) {
            o->d = d; o->in = t.in; o->out = t.out; o->iface = t.iface; o->tag = 0;
            o->present = 1;
            d->msc = o;
            log_line("usb: %s is back\n", o->port);
            return 0;
        }
    }
    if (ndisks == MAXD || !(s = malloc(sizeof *s))) return -1;
    *s = t;
    if (!(s->buf = dmem(DISK_MAX, &s->buf_bus))) { free(s); return -1; }
    sfmt(s->port, sizeof s->port, "usbdisk%d", ndisks);
    disks[ndisks++] = s;
    d->msc = s;
    s->tid = thread_start(disk_thread, s, 16384);
    log_line("usb: %s: %lu MiB, %u-byte sectors\n", s->port, (unsigned long)(s->nsect * s->bs >> 20), s->bs);
    return 0;
}

void msc_detach(usbdev_t *d)
{
    for (int i = 0; i < ndisks; i++)
        if (disks[i]->d == d && disks[i]->present) {
            disks[i]->present = 0;
            log_line("usb: %s unplugged: it answers errors until it is back\n", disks[i]->port);
        }
}

int msc_count(void) { return ndisks; }
int msc_settled(void) { return 1; }             /* (disks are made ready as they are found) */
int msc_info(int i, usb_disk_t *o)
{
    if (i < 0 || i >= ndisks) return -EINVAL;
    msc_t *s = disks[i];
    memset(o, 0, sizeof *o);
    strlcpy(o->port, s->port, sizeof o->port);
    o->sector = s->bs;
    o->present = s->present;
    o->blocks = s->nsect / (DISK_BS / s->bs);
    o->vendor = s->d->vendor;
    o->product = s->d->product;
    strlcpy(o->name, s->d->name, sizeof o->name);
    return 0;
}
