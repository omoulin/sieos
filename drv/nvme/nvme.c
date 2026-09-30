/*
 * nvme.c - NVMe SSDs (PCI class 01/08/02): the Surface Pro 7's, laptops',
 * QEMU's "nvme" device.
 *
 * Each controller is reset and given an admin queue and one I/O queue (32
 * entries each), and runs without interrupts: a command is submitted and
 * its completion polled for, under the controller's lock.  Every active
 * namespace becomes a disk, /dev/dsk/c<4+controller>t0d<namespace-1>p0,
 * with its partitions (blkdev.c).
 *
 * Data goes through a 128 KiB bounce buffer (physically contiguous, one
 * PRP list page), so the callers' buffers need not be: transfers are split
 * at the bounce size and at the controller's limit (MDTS).  Namespaces
 * with 4096-byte blocks are served in 512-byte sectors: an unaligned read
 * reads the blocks around it, an unaligned write reads, patches and writes
 * them back.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "blkdev.h"
#include "ddi.h"
#include "ata.h"
#include "pci.h"
#include "mm.h"
#include "smp.h"

#define MAX_CTRL   4
#define MAX_NS     4
#define QSIZE      32
#define BOUNCE_PAGES 32                          /* 128 KiB */

#define REG_CAP   0x00
#define REG_CC    0x14
#define REG_CSTS  0x1C
#define REG_AQA   0x24
#define REG_ASQ   0x28
#define REG_ACQ   0x30

struct sqe {
    uint32_t cdw0, nsid, rsvd[2];
    uint64_t mptr, prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};

struct cqe {
    uint32_t dw0, dw1;
    uint16_t sq_head, sq_id, cid, status;        /* status bit 0: the phase */
};

struct queue {
    volatile struct sqe *sq;
    volatile struct cqe *cq;
    uint64_t sq_pa, cq_pa;
    uint16_t tail, head, phase, id;
};

struct nvme;

struct nvme_ns {
    struct nvme *c;
    uint32_t nsid;
    uint64_t blocks;
    uint32_t shift;                              /* log2 of the block size (9 or 12) */
};

struct nvme {
    struct pci_dev pci;
    volatile uint8_t *regs;
    uint32_t dstrd;
    uint64_t timeout_ns;
    struct queue admin, io;
    uint16_t cid;
    uint8_t *bounce;
    uint64_t bounce_pa, prp_pa;
    uint64_t *prp;
    uint32_t max_bytes;                          /* per command */
    struct spinlock lock;
    struct nvme_ns ns[MAX_NS];
    int nns;
    char model[41];
};

static struct nvme ctrls[MAX_CTRL];
static int nctrl;

static inline uint32_t rd32(struct nvme *c, uint32_t r) { return *(volatile uint32_t *)(c->regs + r); }
static inline void wr32(struct nvme *c, uint32_t r, uint32_t v) { *(volatile uint32_t *)(c->regs + r) = v; }
static inline uint64_t rd64(struct nvme *c, uint32_t r) { return rd32(c, r) | (uint64_t)rd32(c, r + 4) << 32; }
static inline void wr64(struct nvme *c, uint32_t r, uint64_t v) { wr32(c, r, (uint32_t)v); wr32(c, r + 4, v >> 32); }

static void doorbell(struct nvme *c, int qid, bool cq, uint32_t v)
{
    wr32(c, 0x1000 + (2 * qid + cq) * (4U << c->dstrd), v);
}

/* Submit on q and wait for its completion: the status (0 success), or -1 (timeout); *dw0 the result. */
static int submit(struct nvme *c, struct queue *q, struct sqe *cmd, uint32_t *dw0)
{
    uint16_t cid = ++c->cid;
    cmd->cdw0 = (cmd->cdw0 & 0xFFFF) | (uint32_t)cid << 16;
    volatile struct sqe *slot = &q->sq[q->tail];
    memcpy((void *)slot, cmd, sizeof(*cmd));
    q->tail = (q->tail + 1) % QSIZE;
    __asm__ volatile("sfence" ::: "memory");
    doorbell(c, q->id, false, q->tail);
    uint64_t end = hrtime() + c->timeout_ns;
    for (;;) {
        volatile struct cqe *e = &q->cq[q->head];
        if ((e->status & 1) == q->phase) {
            uint16_t st = e->status >> 1, ecid = e->cid;
            uint32_t r = e->dw0;
            if (++q->head == QSIZE) {
                q->head = 0;
                q->phase ^= 1;
            }
            doorbell(c, q->id, true, q->head);
            if (ecid != cid)
                continue;                        /* (an old completion) */
            if (dw0)
                *dw0 = r;
            return st & 0x7FFF;
        }
        if (hrtime() > end)
            return -1;
        __asm__ volatile("pause");
    }
}

static int admin(struct nvme *c, uint8_t op, uint32_t nsid, uint64_t prp1, uint32_t cdw10, uint32_t cdw11)
{
    struct sqe cmd = { 0 };
    cmd.cdw0 = op;
    cmd.nsid = nsid;
    cmd.prp1 = prp1;
    cmd.cdw10 = cdw10;
    cmd.cdw11 = cdw11;
    return submit(c, &c->admin, &cmd, NULL);
}

/* n logical blocks at lba through the bounce buffer. */
static int io(struct nvme_ns *ns, bool write, uint64_t lba, uint32_t n)
{
    struct nvme *c = ns->c;
    uint64_t bytes = (uint64_t)n << ns->shift;
    struct sqe cmd = { 0 };
    cmd.cdw0 = write ? 0x01 : 0x02;
    cmd.nsid = ns->nsid;
    cmd.prp1 = c->bounce_pa;
    if (bytes > 2 * PAGE_SIZE) {
        for (uint64_t i = 1; i < (bytes + PAGE_SIZE - 1) / PAGE_SIZE; i++)
            c->prp[i - 1] = c->bounce_pa + i * PAGE_SIZE;
        cmd.prp2 = c->prp_pa;
    } else if (bytes > PAGE_SIZE) {
        cmd.prp2 = c->bounce_pa + PAGE_SIZE;
    }
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = n - 1;
    int st = submit(c, &c->io, &cmd, NULL);
    if (st) {
        kprintf("nvme: %s of %u blocks at %lu failed (status %#x)\n", write ? "write" : "read", n, lba, st);
        return -EIO;
    }
    return 0;
}

/* 512-byte sectors [sec, sec + count) <-> buf. */
static int rw(struct nvme_ns *ns, uint64_t sec, size_t count, uint8_t *buf, bool write)
{
    struct nvme *c = ns->c;
    uint32_t per = 1U << (ns->shift - 9);        /* sectors per block */
    int r = 0;
    spin_lock(&c->lock);
    while (count && !r) {
        uint64_t lba = sec / per, skip = sec % per;
        uint64_t max_blocks = c->max_bytes >> ns->shift;
        uint64_t blocks = (skip + count + per - 1) / per;
        blocks = blocks > max_blocks ? max_blocks : blocks;
        uint64_t take = blocks * per - skip;     /* sectors of the caller's in these blocks */
        take = take > count ? count : take;
        bool partial = skip || (take % per);
        if (write) {
            if (partial && (r = io(ns, false, lba, blocks)) < 0)
                break;
            memcpy(c->bounce + skip * SECTOR_SIZE, buf, take * SECTOR_SIZE);
            r = io(ns, true, lba, blocks);
        } else if (!(r = io(ns, false, lba, blocks))) {
            memcpy(buf, c->bounce + skip * SECTOR_SIZE, take * SECTOR_SIZE);
        }
        sec += take;
        buf += take * SECTOR_SIZE;
        count -= take;
    }
    spin_unlock(&c->lock);
    return r;
}

static int ns_read(void *drv, uint64_t lba, size_t count, void *buf) { return rw(drv, lba, count, buf, false); }
static int ns_write(void *drv, uint64_t lba, size_t count, const void *buf)
{
    return rw(drv, lba, count, (uint8_t *)buf, true);
}
static const struct blk_ops nvme_ops = { ns_read, ns_write };

static bool queue_alloc(struct queue *q, int id)
{
    q->sq_pa = pmm_alloc_contig(1);
    q->cq_pa = pmm_alloc_contig(1);
    if (!q->sq_pa || !q->cq_pa)
        return false;
    q->sq = P2V(q->sq_pa);
    q->cq = P2V(q->cq_pa);
    q->tail = q->head = 0;
    q->phase = 1;
    q->id = id;
    return true;
}

static bool wait_ready(struct nvme *c, bool ready)
{
    uint64_t end = hrtime() + c->timeout_ns;
    while ((rd32(c, REG_CSTS) & 1) != ready) {
        if (rd32(c, REG_CSTS) == 0xFFFFFFFF || hrtime() > end)
            return false;
        __asm__ volatile("pause");
    }
    return true;
}

static const char *why_failed;

static bool why(const char *reason)
{
    why_failed = reason;
    return false;
}

static bool start(struct nvme *c, int index)
{
    const struct pci_dev *pd = &c->pci;
    uint8_t pm = pci_find_cap(pd, 1, 0);
    if (pm) {
        uint32_t pmcsr = pci_read32(pd->bus, pd->dev, pd->func, pm + 4);
        if (pmcsr & 3)
            pci_write32(pd->bus, pd->dev, pd->func, pm + 4, pmcsr & ~3U);
    }
    pci_enable_path(pd);                         /* (its root port forwards the DMA) */
    uint32_t cmd = pci_read32(pd->bus, pd->dev, pd->func, 4);
    pci_write32(pd->bus, pd->dev, pd->func, 4, (cmd | 0x6) | 0x400);   /* memory, bus master, INTx off */
    bool isio;
    uint64_t bar = pci_bar_addr(pd, 0, &isio);
    if (!bar || isio || !(c->regs = mmio_map(bar, 0x4000)))
        return why("no register BAR");
    uint64_t cap = rd64(c, REG_CAP);
    c->dstrd = (cap >> 32) & 0xF;
    c->timeout_ns = (((cap >> 24) & 0xFF) + 1) * 500000000UL;
    if (((cap >> 48) & 0xF) > 0)                 /* MPSMIN above 4 KiB */
        return why("its smallest page is above 4 KiB");
    uint32_t cc = rd32(c, REG_CC);
    if (cc & 1) {                                /* the firmware's: off first */
        wr32(c, REG_CC, cc & ~1U);
        if (!wait_ready(c, false))
            return why("it did not stop (CSTS.RDY stays 1)");
    }
    if (!queue_alloc(&c->admin, 0) || !queue_alloc(&c->io, 1))
        return why("no memory for the queues");
    wr32(c, REG_AQA, (QSIZE - 1) << 16 | (QSIZE - 1));
    wr64(c, REG_ASQ, c->admin.sq_pa);
    wr64(c, REG_ACQ, c->admin.cq_pa);
    wr32(c, REG_CC, 1 | 6U << 16 | 4U << 20);   /* enable, NVM commands, 4 KiB pages, SQE 64, CQE 16 bytes */
    if (!wait_ready(c, true))
        return why("it did not become ready (CSTS.RDY)");

    c->bounce_pa = pmm_alloc_contig(BOUNCE_PAGES);
    c->prp_pa = pmm_alloc_contig(1);
    if (!c->bounce_pa || !c->prp_pa)
        return why("no memory for the bounce buffer");
    c->bounce = P2V(c->bounce_pa);
    c->prp = P2V(c->prp_pa);
    uint8_t *id = c->bounce;
    int st = admin(c, 0x06, 0, c->bounce_pa, 1, 0);   /* identify the controller */
    if (st) {
        static char reason[64];
        if (st < 0)
            snprintf(reason, sizeof(reason), "IDENTIFY (controller): no completion in %lu s", c->timeout_ns / 1000000000);
        else
            snprintf(reason, sizeof(reason), "IDENTIFY (controller): status %#x (type %u, code %#x)", st, (st >> 8) & 7,
                     st & 0xFF);
        return why(reason);
    }
    memcpy(c->model, id + 24, 40);
    c->model[40] = 0;
    for (int i = 39; i >= 0 && (c->model[i] == ' ' || !c->model[i]); i--)
        c->model[i] = 0;
    uint8_t mdts = id[77];
    uint32_t nn = *(uint32_t *)(id + 516);
    c->max_bytes = BOUNCE_PAGES * PAGE_SIZE;
    if (mdts && (PAGE_SIZE << mdts) < c->max_bytes)
        c->max_bytes = PAGE_SIZE << mdts;

    if (admin(c, 0x05, 0, c->io.cq_pa, (QSIZE - 1) << 16 | 1, 1))            /* create the I/O CQ */
        return why("creating the I/O completion queue failed");
    if (admin(c, 0x01, 0, c->io.sq_pa, (QSIZE - 1) << 16 | 1, 1U << 16 | 1))  /* the I/O SQ, on CQ 1 */
        return why("creating the I/O submission queue failed");

    for (uint32_t nsid = 1; nsid <= nn && nsid <= 16 && c->nns < MAX_NS; nsid++) {
        if (admin(c, 0x06, nsid, c->bounce_pa, 0, 0))   /* identify the namespace */
            continue;
        uint64_t size = *(uint64_t *)id;
        uint8_t fmt = id[26] & 0xF;
        uint32_t lbaf = *(uint32_t *)(id + 128 + 4 * fmt);
        uint32_t shift = (lbaf >> 16) & 0xFF;
        if (!size || shift < 9 || shift > 12) {
            if (size)
                kprintf("nvme %d: namespace %u: %u-byte blocks, not used\n", index, nsid, 1U << shift);
            continue;
        }
        struct nvme_ns *ns = &c->ns[c->nns++];
        ns->c = c;
        ns->nsid = nsid;
        ns->blocks = size;
        ns->shift = shift;
        char name[24], desc[64];
        snprintf(name, sizeof(name), "c%dt0d%up0", 4 + index, nsid - 1);
        int dev = blk_register(name, size << (shift - 9), &nvme_ops, ns);
        if (dev < 0)
            break;
        uint64_t mib = (size << shift) >> 20;
        char sz[16];
        snprintf(sz, sizeof(sz), mib >= 1024 ? "%lu GiB" : "%lu MiB", mib >= 1024 ? mib >> 10 : mib);
        snprintf(desc, sizeof(desc), "NVMe %s, %s", c->model, sz);
        blk_set_desc(dev, desc);
        blk_set_lbsize(dev, 1U << shift);
        kprintf("nvme %d: %s: %s, %s, %u-byte blocks\n", index, name, c->model, sz, 1U << shift);
    }
    return true;
}

void nvme_init(void)
{
    for (int i = 0; i < pci_count() && nctrl < MAX_CTRL; i++) {
        const struct pci_dev *pd = pci_at(i);
        if (pd->class_code != 0x01 || pd->subclass != 0x08 || pd->prog_if != 0x02)
            continue;
        struct nvme *c = &ctrls[nctrl];
        c->pci = *pd;
        if (!start(c, nctrl)) {
            kprintf("nvme: %04x:%04x at %02x:%02x.%x did not start: %s", pd->vendor, pd->device, pd->bus, pd->dev,
                    pd->func, why_failed ? why_failed : "?");
            if (c->regs)
                kprintf(" (CAP %lx, CC %x, CSTS %x)", rd64(c, REG_CAP), rd32(c, REG_CC), rd32(c, REG_CSTS));
            kprintf("\n");
            memset(c, 0, sizeof(*c));
            continue;
        }
        pci_claim(pd, "nvme");
        nctrl++;
    }
}

DDI_DRIVER("nvme", DDI_PHASE_BOOT, "NVM Express disks");
DDI_ALIAS("pciclass,010802");

int _init(void)
{
    nvme_init();
    return 0;
}
