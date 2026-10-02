/*
 * virtio_blk.c - virtio disks (virtio 1.0 over PCI): QEMU's virtio-blk-pci
 * (-drive if=virtio), and the same device in other hypervisors.  Each is a
 * disk, c9tNd0p0, its partitions c9tNd0sM.
 *
 * The "modern" interface only, as virtio_net: the common, notify and device
 * configuration structures through the PCI vendor capabilities.  One split
 * virtqueue (0); a request is three descriptors: the header (type, sector),
 * the data (64 KiB at most, through a bounce buffer) and the status byte.
 * Requests are synchronous and polled, one at a time (the device's lock), as
 * NVMe's: the device is told not to interrupt.  Features: VERSION_1, RO (a
 * read-only image: the disk is read-only), BLK_SIZE (other than 512 bytes:
 * not used).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "ddi.h"
#include "sync.h"
#include "pci.h"
#include "arch.h"
#include "mm.h"
#include "blkdev.h"

#define CAP_COMMON 1
#define CAP_NOTIFY 2
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

#define ST_ACK         1
#define ST_DRIVER      2
#define ST_DRIVER_OK   4
#define ST_FEATURES_OK 8
#define ST_FAILED      128

#define F_SIZE_MAX  (1ULL << 1)
#define F_RO        (1ULL << 5)
#define F_BLK_SIZE  (1ULL << 6)
#define F_VERSION_1 (1ULL << 32)

#define T_IN   0                                 /* read */
#define T_OUT  1                                 /* write */

#define DESC_F_NEXT  1
#define DESC_F_WRITE 2
#define AVAIL_F_NO_INTERRUPT 1

#define MAXBYTES (64 * 1024)                     /* a request's data: the bounce buffer */
#define QSIZE    8                               /* (one request at a time: three descriptors) */
#define TIMEOUT_NS 30000000000UL

struct vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;
} __attribute__((packed));

struct vblk {
    kmutex_t lock;                               /* one request at a time */
    volatile uint8_t *common, *devcfg, *notify_base;
    uint32_t notify_mult;
    volatile struct vq_desc *desc;
    volatile uint16_t *avail;                    /* flags, idx, ring[size] */
    volatile uint32_t *used;                     /* flags|idx, then (id, len) pairs */
    volatile uint16_t *notify;
    int qsize;
    uint16_t last_used;
    uint8_t *hdr, *bounce;                       /* hdr: the 16-byte header at 0, the status at 64 */
    uint64_t hdr_pa, bounce_pa;
    uint32_t max_bytes;
    uint64_t sectors;
    bool ro;
    int index, dev;
};

#define MAX_DISKS 4
static struct vblk disks[MAX_DISKS];
static int ndisks;

static inline void mb(void) { __sync_synchronize(); }
static inline uint8_t  c8(struct vblk *v, int o) { return v->common[o]; }
static inline uint16_t c16(struct vblk *v, int o) { return *(volatile uint16_t *)(v->common + o); }
static inline uint32_t c32(struct vblk *v, int o) { return *(volatile uint32_t *)(v->common + o); }
static inline void w8(struct vblk *v, int o, uint8_t x) { v->common[o] = x; }
static inline void w16(struct vblk *v, int o, uint16_t x) { *(volatile uint16_t *)(v->common + o) = x; }
static inline void w32(struct vblk *v, int o, uint32_t x) { *(volatile uint32_t *)(v->common + o) = x; }
static inline void w64(struct vblk *v, int o, uint64_t x)
{
    w32(v, o, (uint32_t)x);
    w32(v, o + 4, (uint32_t)(x >> 32));
}
static inline uint32_t cfg32(struct vblk *v, int o) { return *(volatile uint32_t *)(v->devcfg + o); }

/* One request of n sectors at lba through the bounce buffer: 0, or -EIO. */
static int request(struct vblk *v, uint32_t type, uint64_t lba, uint32_t n)
{
    uint8_t *h = v->hdr;
    memset(h, 0, 16);
    *(uint32_t *)h = type;
    *(uint64_t *)(h + 8) = lba;
    h[64] = 0xFF;                                /* the status, written by the device */
    v->desc[0] = (struct vq_desc){ v->hdr_pa, 16, DESC_F_NEXT, 1 };
    v->desc[1] = (struct vq_desc){ v->bounce_pa, n * 512, DESC_F_NEXT | (type == T_IN ? DESC_F_WRITE : 0), 2 };
    v->desc[2] = (struct vq_desc){ v->hdr_pa + 64, 1, DESC_F_WRITE, 0 };
    uint16_t i = v->avail[1];
    v->avail[2 + i % v->qsize] = 0;
    mb();
    v->avail[1] = i + 1;
    mb();
    *v->notify = 0;
    uint64_t end = hrtime() + TIMEOUT_NS;
    while ((uint16_t)(v->used[0] >> 16) == v->last_used) {
        if (hrtime() > end) {
            kprintf("virtio-blk %d: no answer to the request for sector %lu\n", v->index, lba);
            return -EIO;                         /* (the device is left as it is: its ring is not reused safely) */
        }
        __asm__ volatile("pause");
    }
    v->last_used++;
    mb();
    return h[64] == 0 ? 0 : -EIO;
}

static int rw(void *drv, uint64_t lba, size_t count, void *buf, bool write)
{
    struct vblk *v = drv;
    uint8_t *p = buf;
    if (write && v->ro)
        return -EROFS;
    while (count) {
        uint32_t n = count < v->max_bytes / 512 ? count : v->max_bytes / 512;
        if (write)
            memcpy(v->bounce, p, n * 512);
        if (request(v, write ? T_OUT : T_IN, lba, n) < 0) {
            kprintf("virtio-blk %d: %s of %u sectors at %lu failed\n", v->index, write ? "writing" : "reading", n, lba);
            return -EIO;
        }
        if (!write)
            memcpy(p, v->bounce, n * 512);
        lba += n;
        count -= n;
        p += n * 512;
    }
    return 0;
}

static int rw_locked(void *drv, uint64_t lba, size_t count, void *buf, bool write)
{
    struct vblk *v = drv;
    mutex_enter(&v->lock);
    int r = rw(drv, lba, count, buf, write);
    mutex_exit(&v->lock);
    return r;
}

static int vb_read(void *drv, uint64_t lba, size_t count, void *buf) { return rw_locked(drv, lba, count, buf, false); }
static int vb_write(void *drv, uint64_t lba, size_t count, const void *buf)
{
    return rw_locked(drv, lba, count, (void *)buf, true);
}
static const struct blk_ops vb_ops = { vb_read, vb_write };

/* The structure of a vendor capability (cfg_type), mapped; NULL if the device has none. */
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

static bool vb_start(struct vblk *v, const struct pci_dev *pd)
{
    pci_enable_bus_master(pd);
    v->common = find_cfg(pd, CAP_COMMON, NULL);
    v->notify_base = find_cfg(pd, CAP_NOTIFY, &v->notify_mult);
    v->devcfg = find_cfg(pd, CAP_DEVICE, NULL);
    if (!v->common || !v->notify_base || !v->devcfg) {
        kprintf("virtio-blk %02x:%02x.%d: no virtio 1.0 interface (legacy only): skipped\n", pd->bus, pd->dev,
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
    uint64_t want = have & (F_VERSION_1 | F_RO | F_BLK_SIZE | F_SIZE_MAX);
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
    v->sectors = cfg32(v, 0) | (uint64_t)cfg32(v, 4) << 32;
    uint32_t bsize = (want & F_BLK_SIZE) ? cfg32(v, 20) : 512;
    if (bsize != 512) {
        kprintf("virtio-blk %d: %u-byte blocks, not used\n", ndisks, bsize);
        w8(v, C_STATUS, ST_FAILED);
        return false;
    }
    v->ro = want & F_RO;
    v->max_bytes = MAXBYTES;
    if (want & F_SIZE_MAX) {                     /* the largest segment the device takes */
        uint32_t sm = cfg32(v, 8) & ~511U;
        if (sm >= 512 && sm < v->max_bytes)
            v->max_bytes = sm;
    }

    /* the queue (desc at 0, avail at 1024, used at 2048 of one page) and the buffers */
    w16(v, C_MSIX, 0xFFFF);
    w16(v, C_Q_SELECT, 0);
    int max = c16(v, C_Q_SIZE);
    uint64_t ring = pmm_alloc_contig(1);
    v->hdr_pa = pmm_alloc_contig(1);
    v->bounce_pa = pmm_alloc_contig(MAXBYTES / PAGE_SIZE);
    if (!max || !ring || !v->hdr_pa || !v->bounce_pa) {
        w8(v, C_STATUS, ST_FAILED);
        return false;
    }
    v->qsize = max < QSIZE ? max : QSIZE;
    uint8_t *r = P2V(ring);
    memset(r, 0, PAGE_SIZE);
    v->desc = (volatile struct vq_desc *)r;
    v->avail = (volatile uint16_t *)(r + 1024);
    v->used = (volatile uint32_t *)(r + 2048);
    v->avail[0] = AVAIL_F_NO_INTERRUPT;          /* polled */
    v->hdr = P2V(v->hdr_pa);
    v->bounce = P2V(v->bounce_pa);
    w16(v, C_Q_SIZE, v->qsize);
    w16(v, C_Q_MSIX, 0xFFFF);
    w64(v, C_Q_DESC, ring);
    w64(v, C_Q_DRIVER, ring + 1024);
    w64(v, C_Q_DEVICE, ring + 2048);
    v->notify = (volatile uint16_t *)(v->notify_base + (uint32_t)c16(v, C_Q_NOFF) * v->notify_mult);
    w16(v, C_Q_ENABLE, 1);
    w8(v, C_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);

    v->index = ndisks;
    char name[24], desc[64], sz[16];
    snprintf(name, sizeof(name), "c9t%dd0p0", ndisks);
    uint64_t mib = v->sectors / 2048;
    if (mib >= 10240)
        snprintf(sz, sizeof(sz), "%lu GiB", mib >> 10);
    else if (mib >= 1024)
        snprintf(sz, sizeof(sz), "%lu.%lu GiB", mib >> 10, (mib & 1023) * 10 / 1024);
    else
        snprintf(sz, sizeof(sz), "%lu MiB", mib);
    v->dev = blk_register(name, v->sectors, &vb_ops, v);
    if (v->dev < 0) {
        w8(v, C_STATUS, ST_FAILED);
        return false;
    }
    snprintf(desc, sizeof(desc), "virtio disk, %s%s", sz, v->ro ? ", read-only" : "");
    blk_set_desc(v->dev, desc);
    if (v->ro)
        blk_set_readonly(v->dev);
    kprintf("virtio-blk %d: %s, %s%s\n", ndisks, name, sz, v->ro ? ", read-only" : "");
    return true;
}

DDI_DRIVER("virtio_blk", DDI_PHASE_BOOT, "virtio disks (QEMU, KVM)");
DDI_ALIAS("pci1af4,1001");
DDI_ALIAS("pci1af4,1042");

int _init(void)
{
    static const uint16_t ids[] = { 0x1001, 0x1042 };     /* transitional, modern */
    struct pci_dev pd[MAX_DISKS];
    int n = pci_find_all(0x1AF4, ids, ARRAY_SIZE(ids), pd, MAX_DISKS);
    for (int i = 0; i < n && ndisks < MAX_DISKS; i++)
        if (vb_start(&disks[ndisks], &pd[i])) {
            pci_claim(&pd[i], "virtio-blk");
            ndisks++;
        }
    return 0;
}
