/*
 * vblk - The disk server: the port "disk" (mk/proto.h), over a virtio block
 * device (what QEMU offers with "-device virtio-blk-pci") or, on arm64, an
 * SD card (the Raspberry Pis: sd.c). On a card with partitions (an MBR),
 * the disk is the SieFS partition (type 0x5E): the Pis boot from the first
 * one, a FAT partition the firmware reads.
 *
 * Or a USB key (the USB server, user/usb, serves each USB disk on a port of
 * its own, "usbdisk0"...): when there is no virtio disk or SD card, the
 * first USB disk with a SieFS partition is SIEOS's disk. Partitions: a GUID
 * partition table (GPT, mk/gpt.h: the SieFS partition by its type) or an MBR
 * (type 0x5E); a disk without either is SieFS whole if a superblock says so.
 *
 * Virtio is a simple standard for virtual devices. The driver and the device
 * share a "virtqueue" in memory: a table of buffer descriptors, a ring where
 * the driver lists requests ("available"), and a ring where the device lists
 * the finished ones ("used"). A request is three buffers chained together:
 * a header (read or write, which sector), the data, and one status byte the
 * device fills in. The driver writes the queue's number to a "notify"
 * register; the device answers with an interrupt.
 *
 * We use the "legacy" interface, the smallest one to drive: on x86-64 over
 * PCI (QEMU needs "disable-modern=on"), on arm64 over MMIO (QEMU's virt:
 * "-device virtio-blk-device"); user/lib/virtio.c hides the difference.
 *
 * No copies of our own: a client's data arrives (IPC) straight into the
 * buffer the device reads, and the device writes read data where the reply
 * is copied from. One request at a time: the file server (our only client)
 * sends one at a time anyway, and a request moves up to 128 KiB.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

#include "virtio.h"
#include "mk/gpt.h"
#if defined(__aarch64__)
#include "sd.h"
#endif

enum { D_NEXT = 1, D_WRITE = 2 };                       /* descriptor flags: chained, device writes */
enum { T_IN = 0, T_OUT = 1, T_FLUSH = 4 };              /* request types */
#define F_FLUSH (1u << 9)                               /* the device has a write cache to flush */

typedef struct { uint64_t addr; uint32_t len; uint16_t flags, next; } desc_t;
typedef struct { uint32_t type, reserved; uint64_t sector; } req_t;

static vdev_t dev;
static uint16_t qsz, last;              /* queue size, used entries seen */
static desc_t *desc;
static volatile uint16_t *avail;        /* flags, idx, ring[qsz] */
static volatile uint16_t *used_idx;     /* the used ring's idx */
static req_t *hdr;                      /* header and status, in one DMA page */
static volatile uint8_t *status;
static uint8_t *data;                   /* DISK_MAX bytes of DMA memory */
static uint64_t hdr_pa, data_pa, nblocks;
static int has_flush;
static long done_port;                  /* request() waits here (irq_thread, below) */
static void irq_thread(void *a);
static long irq_port;

static int setup(void)
{
    if (vdev_find(&dev, VIRTIO_BLK)) return -1;
    vdev_status(&dev, 0);                               /* reset */
    vdev_status(&dev, VS_ACK | VS_DRIVER);
    has_flush = !!(vdev_features(&dev) & F_FLUSH);
    vdev_accept(&dev, has_flush ? F_FLUSH : 0);         /* the only feature we want */

    /* Queue 0. Legacy layout, in one block: descriptors, then the available
     * ring, then (at the next page) the used ring. */
    qsz = vdev_qmax(&dev, 0);
    if (!qsz) return -1;
    uint64_t ring_pa, pg = 4096;
    uint64_t used_off = (16UL * qsz + 6 + 2UL * qsz + pg - 1) & ~(pg - 1);
    uint64_t size = used_off + ((6 + 8UL * qsz + pg - 1) & ~(pg - 1));
    char *ring = dma_alloc(size, &ring_pa);
    hdr = dma_alloc(pg, &hdr_pa);
    data = dma_alloc(DISK_MAX, &data_pa);
    if ((long)ring < 0 || (long)hdr < 0 || (long)data < 0) return -1;
    desc = (desc_t *)ring;
    avail = (uint16_t *)(ring + 16UL * qsz);
    used_idx = (uint16_t *)(ring + used_off + 2);
    status = (uint8_t *)hdr + sizeof *hdr;
    vdev_qset(&dev, 0, qsz, ring_pa);

    nblocks = ((uint64_t)vdev_cfg32(&dev, 4) << 32 | vdev_cfg32(&dev, 0)) / 8;   /* capacity in 512 B sectors */
    irq_port = port_create(0);
    done_port = port_create(0);
    if (irq_bind(dev.irq, irq_port)) return -1;
    thread_start(irq_thread, 0, 8192);
    vdev_status(&dev, VS_ACK | VS_DRIVER | VS_OK);
    return 0;
}

/* The interrupt thread. The disk's interrupt line may be shared with
 * other devices (PCI lines are): every interrupt on it must be answered
 * at once, even while the disk is idle, or the line stays masked for all.
 * Reading the ISR register lowers our device's line; then, if our request
 * is done, the waiting request is told (a message on done_port); then
 * irq_ack: the kernel unmasks the line once every driver on it has. */
static volatile int busy;                                /* a request is with the device */
static void irq_thread(void *a)
{
    (void)a;
    for (;;) {
        msg_t m = { 0 };
        ipc_recv(irq_port, &m);
        vdev_isr(&dev);
        if (busy && __atomic_load_n(used_idx, __ATOMIC_ACQUIRE) != last) {
            busy = 0;
            msg_t d = { 0 };
            ipc_call(done_port, &d);
        }
        irq_ack(dev.irq & ~IRQ_LEVEL);
    }
}

/* One request: header, data (unless a flush), status; then wait until the
 * interrupt thread says the device has answered. */
static long request(uint32_t type, uint64_t blk, uint32_t bytes)
{
    hdr->type = type;
    hdr->sector = blk * (DISK_BS / 512);
    *status = 0xFF;
    desc[0] = (desc_t){ hdr_pa, sizeof *hdr, D_NEXT, 1 };
    desc[1] = (desc_t){ data_pa, bytes, D_NEXT | (type == T_IN ? D_WRITE : 0), 2 };
    desc[2] = (desc_t){ hdr_pa + sizeof *hdr, 1, D_WRITE, 0 };
    if (!bytes) desc[0].next = 2;                       /* a flush has no data */
    avail[2 + avail[1] % qsz] = 0;                      /* the chain starts at descriptor 0 */
    busy = 1;
    __atomic_store_n(&avail[1], avail[1] + 1, __ATOMIC_RELEASE);   /* publish it ... */
    dma_wmb();
    vdev_notify(&dev, 0);                               /* ... and tell the device */
    msg_t m = { 0 };
    long t = ipc_recv(done_port, &m);                   /* the interrupt thread: done */
    if (t > 0) reply_val(t, 0);
    last++;
    return *status ? -EIO : 0;
}

/* ---- The backends: virtio (requests above), an SD card (sd.c), or a USB
 * disk (the USB server's port). All move whole 4 KiB blocks of the disk;
 * `first` is where SIEOS's part starts (the SieFS partition's first block). */
enum { B_VIRTIO, B_SD, B_USB };
static int backend;
static uint64_t first;
static uint32_t sector = 512;                           /* the device's sector size */
static long usb_port;
static char usb_name[16];
static long io(int op, uint64_t blk, uint32_t bytes)
{
#if defined(__aarch64__)
    if (backend == B_SD) return op == T_FLUSH ? 0 : sd_io(op == T_OUT, (first + blk) * (DISK_BS / 512), bytes / 512, data);
#endif
    if (backend == B_USB) {
        /* The USB server restarted (it crashed or was killed): wait for the
         * new one and send again. Every request here is safe to repeat
         * (reads, a write of the same data, a flush), and the file server
         * must not see the restart: for it, a disk error is serious. */
        long e = -EPIPE;
        for (int t = 0; t < 20 && (e == -EPIPE || e == -ENOENT); t++) {
            msg_t q = { .w = { op == T_IN ? DISK_READ : op == T_OUT ? DISK_WRITE : DISK_FLUSH, first + blk, bytes / DISK_BS },
                        .sbuf = op == T_OUT ? data : 0, .slen = op == T_OUT ? bytes : 0,
                        .rbuf = data, .rlen = op == T_IN ? bytes : 0 };
            if ((e = call_named(&usb_port, usb_name, &q, 1)) >= 0) return (long)q.w[0];
            usb_port = 0;
        }
        return -EIO;
    }
    return request(op, first + blk, bytes);
}

/* Where SieFS is on the disk: a GPT's SieFS partition, an MBR's type 0x5E
 * partition, or the whole disk if its block 1 is a SieFS superblock.
 * -> 1 found (first, nblocks set), 0 not. Partitions must start on a 4 KiB
 * boundary. */
static int find_siefs(void)
{
    uint64_t total = nblocks;
    static const uint8_t type[16] = GPT_SIEFS_TYPE;
    uint64_t hdr_byte = sector;                          /* the GPT header: sector 1 */
    if (!io(T_IN, hdr_byte / DISK_BS, DISK_BS)) {
        gpt_header_t h;
        memcpy(&h, data + hdr_byte % DISK_BS, sizeof h);
        if (!memcmp(h.sig, "EFI PART", 8) && h.entsize >= sizeof(gpt_entry_t) && h.nentries <= 256) {
            uint64_t cached = ~0ULL;
            for (uint32_t i = 0; i < h.nentries; i++) {
                uint64_t at = h.entries_lba * sector + (uint64_t)i * h.entsize;
                if (at / DISK_BS != cached) { if (io(T_IN, at / DISK_BS, DISK_BS)) break; cached = at / DISK_BS; }
                gpt_entry_t e;
                memcpy(&e, data + at % DISK_BS, sizeof e);
                if (memcmp(e.type, type, 16)) continue;
                uint64_t a = e.first * sector, z = (e.last + 1) * sector;
                if (a % DISK_BS || z <= a || z / DISK_BS > total) continue;
                first = a / DISK_BS;
                nblocks = (z - a) / DISK_BS;
                return 1;
            }
        }
    }
    if (!io(T_IN, 0, DISK_BS) && data[510] == 0x55 && data[511] == 0xAA)
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = data + 446 + 16 * i;
            uint32_t lba = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
            uint32_t n = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24;
            uint64_t a = (uint64_t)lba * sector, z = a + (uint64_t)n * sector;
            if (e[4] == 0x5E && a % DISK_BS == 0 && z / DISK_BS <= total) {
                first = a / DISK_BS;
                nblocks = (z - a) / DISK_BS;
                return 1;
            }
        }
    uint64_t magic = 0;
    if (!io(T_IN, 1, DISK_BS)) memcpy(&magic, data, 8);
    return magic == 0x0031765346454953ULL;               /* "SIEFSv1": SieFS on the whole disk */
}

/* No virtio disk nor SD card: a USB disk? Asks the USB server (if it runs:
 * it registers "usb" first thing, so a short wait covers its start), which
 * answers once the devices present at boot are ready. */
static int usb_disk(void)
{
    long p = -1;
    for (int t = 0; t < 30 && (p = port_find("usb")) < 0; t++) sys_sleep(10000000);
    if (p < 0) return 0;
    msg_t m = { .w = { USB_DISKS } };
    if (ipc_call(p, &m) < 0 || (long)m.w[0] <= 0) return 0;
    long n = m.w[0];
    static usb_disk_t u;
    for (long i = 0; i < n; i++) {
        msg_t q = { .w = { USB_DISK, i }, .rbuf = &u, .rlen = sizeof u };
        if (ipc_call(p, &q) < 0 || q.w[0] || !u.present) continue;
        strlcpy(usb_name, u.port, sizeof usb_name);
        if ((usb_port = port_find(usb_name)) < 0) continue;
        backend = B_USB;
        sector = u.sector;
        first = 0;
        nblocks = u.blocks;
        if (find_siefs()) return 1;
    }
    backend = B_VIRTIO;
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int ok = !setup();                                   /* without a disk, answer -EIO to all */
    static uint32_t buf[DISK_MAX / 4];                   /* the data buffer, when not virtio's DMA one */
#if defined(__aarch64__)
    uint64_t sectors;
    if (!ok && !sd_setup(&sectors)) {                    /* no virtio disk: an SD card? */
        data = sd_buffer() ? sd_buffer() : (uint8_t *)buf;   /* (its DMA buffer: no copy) */
        nblocks = sectors / (DISK_BS / 512);
        ok = 1;
        backend = B_SD;
    }
#endif
    if (ok) find_siefs();                                /* (none found: the whole disk, as before) */
    else {
        data = (uint8_t *)buf;
        ok = usb_disk();
    }
    static const char *kind[] = { "virtio", "SD card", "USB" };
    if (!ok) printf("vblk: no disk (virtio, SD card, or a USB key with SieFS)\n");
    else printf("vblk: %s disk%s%s, %lu MiB%s\n", kind[backend], backend == B_USB ? " " : "",
                backend == B_USB ? usb_name : "", (unsigned long)(nblocks >> 8), first ? " (SieFS partition)" : "");
    long port = port_create("disk");
    msg_t m = { .rbuf = data, .rlen = DISK_MAX };
    long from = ipc_recv(port, &m);
    for (;;) {
        uint64_t blk = m.w[1], n = m.w[2], bytes = n * DISK_BS;
        long r = 0;
        const void *out = 0;
        if (from <= 0) r = -EINVAL;                     /* (no interrupts arrive on this port) */
        else if (!ok) r = -EIO;
        else switch (m.w[0]) {
        case DISK_INFO:  r = nblocks; break;
        case DISK_READ:
            if (!n || bytes > DISK_MAX || blk + n > nblocks) r = -EINVAL;
            else if (!(r = io(T_IN, blk, bytes))) out = data;
            break;
        case DISK_WRITE:
            if (!n || bytes > DISK_MAX || blk + n > nblocks || m.rlen != bytes) r = -EINVAL;
            else r = io(T_OUT, blk, bytes);            /* the data arrived in `data` */
            break;
        case DISK_FLUSH: r = has_flush || backend != B_VIRTIO ? io(T_FLUSH, 0, 0) : 0; break;
        default:         r = -ENOSYS;
        }
        m = (msg_t){ .w = { r }, .sbuf = out, .slen = out ? bytes : 0, .rbuf = data, .rlen = DISK_MAX };
        from = from > 0 ? ipc_reply_recv(from, port, &m) : ipc_recv(port, &m);
    }
}
