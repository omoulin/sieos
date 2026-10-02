/*
 * ata.c - ATA/IDE disk driver (LBA48): bus-master DMA through the PCI IDE
 * controller when there is one, PIO otherwise.
 *
 * Up to four disks: the master and slave of the primary (0x1F0) and
 * secondary (0x170) buses, units 0-3.  DMA goes through a physically
 * contiguous bounce buffer (the callers' buffers may be anywhere in kernel
 * memory, stacks included) described by a PRD table.  Once the channels'
 * interrupts (IRQ 14, 15) are set up, a caller in process context sleeps
 * until a DMA transfer or a cache flush completes; the processor runs other
 * LWPs meanwhile.  At boot,
 * and for PIO, completion is polled.  A sleeping mutex serialises the
 * callers (one bounce buffer, one PRD table).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "kernel.h"
#include "abi.h"
#include "ata.h"
#include "pci.h"
#include "mm.h"
#include "arch.h"
#include "proc.h"
#include "blkdev.h"
#include "ddi.h"

#define ATA_DATA     0
#define ATA_ERROR    1
#define ATA_SECCOUNT 2
#define ATA_LBA0     3
#define ATA_LBA1     4
#define ATA_LBA2     5
#define ATA_DRIVE    6
#define ATA_STATUS   7
#define ATA_COMMAND  7

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DF   0x20
#define ST_BSY  0x80

#define CMD_READ_PIO_EXT  0x24
#define CMD_READ_DMA_EXT  0x25
#define CMD_WRITE_PIO_EXT 0x34
#define CMD_WRITE_DMA_EXT 0x35
#define CMD_FLUSH_EXT     0xEA
#define CMD_IDENTIFY      0xEC

/* bus mastering (SFF-8038i): command, status and PRD table registers */
#define BM_CMD    0
#define BM_STATUS 2
#define BM_PRDT   4
#define BMC_START 0x01
#define BMC_READ  0x08                /* device to memory */
#define BMS_ACTIVE 0x01
#define BMS_ERROR  0x02
#define BMS_IRQ    0x04

#define DMA_SECTORS 256               /* per command: the bounce buffer, 128 KiB */
struct prd {
    uint32_t addr;
    uint16_t bytes;                   /* 0 = 64 KiB */
    uint16_t flags;                   /* bit 15: last entry */
} __attribute__((packed));

struct ata_unit {
    bool present;
    uint16_t io, ctrl;
    uint8_t drive_sel;                /* 0x40 master, 0x50 slave (LBA mode) */
    uint64_t sectors;
    char model[41];
    uint16_t bm;                      /* bus-master registers, 0: PIO */
};

static struct ata_unit units[ATA_UNITS];
static uint16_t bm_base;              /* the controller's (primary bus) registers, 0: none */
static uint64_t bounce_pa, prdt_pa;
static struct prd *prdt;

static kmutex_t ata_lock;
static bool irq_ok;                   /* IRQ 14/15 handled: callers may sleep */
static volatile bool chan_done[2];    /* the channel interrupted since the command started */
uint64_t ata_sleeps;                  /* transfers waited for by sleeping */

static int chan_of(const struct ata_unit *u) { return u->io == 0x170; }

/* Sleep rather than poll?  In process context, with the interrupts working. */
static bool can_sleep(void)
{
    return irq_ok && kernel_running && curlwp && !curlwp->is_idle && curlwp->state == LWP_RUNNING;
}

static kmutex_t irq_lock = MUTEX_SPIN_INITIALIZER;   /* chan_done, between the interrupt and the waiter */

static void ata_irq(struct trapframe *tf)
{
    int c = tf->int_no - IRQ_BASE - 14;          /* 0 primary, 1 secondary */
    uint16_t io = c ? 0x170 : 0x1F0;
    if (bm_base)
        outb(bm_base + (c ? 8 : 0) + 2, 0x04);    /* clear the bus master's interrupt bit */
    (void)inb(io + 7);                           /* reading the status acknowledges the device */
    mutex_enter(&irq_lock);
    chan_done[c] = true;
    sleepq_wakeup((void *)&chan_done[c], -1);
    mutex_exit(&irq_lock);
}

/* Sleep until u's channel interrupts, or 5 s pass (then the caller polls).  Short
 * transfers finish within a few tens of microseconds: spin for up to 100 us
 * first, so that they do not pay for sleeping and waking. */
static void wait_irq(struct ata_unit *u)
{
    int c = chan_of(u);
    uint64_t spin_end = hrtime() + 100000;
    while (!chan_done[c] && hrtime() < spin_end) {
        uint8_t bst = u->bm ? inb(u->bm + BM_STATUS) : 0;
        if (u->bm && !(bst & BMS_ACTIVE) && !(inb(u->io + ATA_STATUS) & ST_BSY))
            return;                              /* done: the interrupt is handled when it comes */
        __builtin_ia32_pause();
    }
    uint64_t deadline = ticks + 5 * TIMER_HZ;
    mutex_enter(&irq_lock);
    while (!chan_done[c] && ticks < deadline) {
        curlwp->wake_tick = deadline;
        sleepq_block((void *)&chan_done[c], &irq_lock, false);
        curlwp->wake_tick = 0;
        mutex_enter(&irq_lock);
    }
    mutex_exit(&irq_lock);
    ata_sleeps++;
}

static inline void outl_(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(port)); }

static void ata_delay(struct ata_unit *u)
{
    for (int i = 0; i < 4; i++)
        inb(u->ctrl);
}

static int ata_wait(struct ata_unit *u, bool need_drq)
{
    for (int i = 0; i < 10000000; i++) {
        uint8_t st = inb(u->io + ATA_STATUS);
        if (st & ST_BSY)
            continue;
        if (st & (ST_ERR | ST_DF))
            return -EIO;
        if (!need_drq || (st & ST_DRQ))
            return 0;
    }
    return -EIO;
}

static bool probe(struct ata_unit *u, uint16_t io, uint16_t ctrl, uint8_t sel)
{
    u->io = io;
    u->ctrl = ctrl;
    outb(ctrl, 0x02);                         /* nIEN: no interrupts */
    outb(io + ATA_DRIVE, sel == 0 ? 0xA0 : 0xB0);
    ata_delay(u);
    if (inb(io + ATA_STATUS) == 0xFF)         /* floating bus */
        return false;
    outb(io + ATA_SECCOUNT, 0);
    outb(io + ATA_LBA0, 0);
    outb(io + ATA_LBA1, 0);
    outb(io + ATA_LBA2, 0);
    outb(io + ATA_COMMAND, CMD_IDENTIFY);
    ata_delay(u);
    if (inb(io + ATA_STATUS) == 0)
        return false;
    for (int i = 0; i < 1000000 && (inb(io + ATA_STATUS) & ST_BSY); i++)
        ;
    if (inb(io + ATA_LBA1) != 0 || inb(io + ATA_LBA2) != 0)
        return false;                         /* ATAPI / SATA, not a PATA disk */
    if (ata_wait(u, true) < 0)
        return false;
    uint16_t id[256];
    for (int i = 0; i < 256; i++)
        id[i] = inw(io + ATA_DATA);
    if (!(id[83] & (1 << 10)))
        u->sectors = id[60] | ((uint32_t)id[61] << 16);
    else
        u->sectors = id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32);
    for (int i = 0; i < 20; i++) {
        u->model[i * 2] = id[27 + i] >> 8;
        u->model[i * 2 + 1] = id[27 + i] & 0xFF;
    }
    u->model[40] = 0;
    for (int i = 39; i >= 0 && u->model[i] == ' '; i--)
        u->model[i] = 0;
    u->drive_sel = sel == 0 ? 0x40 : 0x50;
    return true;
}

/* The PCI IDE controller's bus-master registers, the bounce buffer and the PRD table. */
static void dma_init(void)
{
    struct pci_dev pd;
    if (!pci_find_class(0x01, 0x01, &pd) || !(pd.bar[4] & 1) || (pd.bar[4] & ~3u) == 0)
        return;
    uint32_t cmd = pci_read32(pd.bus, pd.dev, pd.func, 0x04);
    pci_write32(pd.bus, pd.dev, pd.func, 0x04, cmd | 0x05);   /* I/O space + bus master */
    /* the bounce buffer and, after it, the PRD table: one contiguous allocation, which
     * the allocator takes lowest first (a single page could come from above 4 GB) */
    bounce_pa = pmm_alloc_contig(DMA_SECTORS * SECTOR_SIZE / PAGE_SIZE + 1);
    prdt_pa = bounce_pa ? bounce_pa + DMA_SECTORS * SECTOR_SIZE : 0;
    if (!bounce_pa || prdt_pa + PAGE_SIZE > 0x100000000UL) {
        kprintf("ata: no DMA memory below 4 GB: PIO only\n");
        return;                                              /* the controller takes 32-bit addresses */
    }
    prdt = P2V(prdt_pa);
    bm_base = pd.bar[4] & ~3u;
    pci_claim(&pd, "ata");
}

static void setup(struct ata_unit *u, uint64_t lba, uint16_t count)
{
    outb(u->io + ATA_DRIVE, u->drive_sel);
    outb(u->io + ATA_SECCOUNT, count >> 8);
    outb(u->io + ATA_LBA0, (lba >> 24) & 0xFF);
    outb(u->io + ATA_LBA1, (lba >> 32) & 0xFF);
    outb(u->io + ATA_LBA2, (lba >> 40) & 0xFF);
    outb(u->io + ATA_SECCOUNT, count & 0xFF);
    outb(u->io + ATA_LBA0, lba & 0xFF);
    outb(u->io + ATA_LBA1, (lba >> 8) & 0xFF);
    outb(u->io + ATA_LBA2, (lba >> 16) & 0xFF);
}

/* One DMA command for n <= DMA_SECTORS sectors through the bounce buffer. */
static int dma_xfer(struct ata_unit *u, uint64_t lba, uint16_t n, bool write)
{
    uint64_t pa = bounce_pa, left = (uint64_t)n * SECTOR_SIZE;
    int k = 0;
    while (left) {                                           /* entries may not cross 64 KiB */
        uint64_t chunk = 0x10000 - (pa & 0xFFFF);
        if (chunk > left)
            chunk = left;
        prdt[k].addr = (uint32_t)pa;
        prdt[k].bytes = chunk == 0x10000 ? 0 : (uint16_t)chunk;
        prdt[k].flags = 0;
        pa += chunk;
        left -= chunk;
        k++;
    }
    prdt[k - 1].flags = 0x8000;
    if (ata_wait(u, false) < 0)
        return -EIO;
    outb(u->bm + BM_CMD, 0);
    outl_(u->bm + BM_PRDT, (uint32_t)prdt_pa);
    outb(u->bm + BM_STATUS, BMS_ERROR | BMS_IRQ);           /* write 1 to clear */
    outb(u->bm + BM_CMD, write ? 0 : BMC_READ);
    setup(u, lba, n);                                        /* LBA48: 16-bit count (0 would be 65536) */
    bool sleep = can_sleep();
    chan_done[chan_of(u)] = false;
    outb(u->io + ATA_COMMAND, write ? CMD_WRITE_DMA_EXT : CMD_READ_DMA_EXT);
    outb(u->bm + BM_CMD, (write ? 0 : BMC_READ) | BMC_START);
    if (sleep)
        wait_irq(u);                                         /* (other LWPs run meanwhile) */
    uint8_t bst = 0;
    int r = -EIO;
    for (long i = 0; i < 200000000L; i++) {
        bst = inb(u->bm + BM_STATUS);
        if (bst & BMS_ERROR)
            break;
        uint8_t st = inb(u->io + ATA_STATUS);
        if (!(bst & BMS_ACTIVE) && !(st & ST_BSY)) {
            r = (st & (ST_ERR | ST_DF)) ? -EIO : 0;
            break;
        }
        __builtin_ia32_pause();
    }
    outb(u->bm + BM_CMD, 0);
    outb(u->bm + BM_STATUS, BMS_ERROR | BMS_IRQ);
    return r;
}

void ata_init(void)
{
    static const struct { uint16_t io, ctrl; uint8_t sel; } buses[ATA_UNITS] = {
        { 0x1F0, 0x3F6, 0 }, { 0x1F0, 0x3F6, 1 }, { 0x170, 0x376, 0 }, { 0x170, 0x376, 1 },
    };
    dma_init();
    for (int i = 0; i < ATA_UNITS; i++) {
        struct ata_unit *u = &units[i];
        if (!probe(u, buses[i].io, buses[i].ctrl, buses[i].sel))
            continue;
        u->present = true;
        u->bm = bm_base ? bm_base + (buses[i].io == 0x170 ? 8 : 0) : 0;
        kprintf("ata: disk %d %s at %x/%d, %lu sectors (%lu MiB), %s\n", i, u->model, buses[i].io,
                buses[i].sel, u->sectors, u->sectors / 2048, u->bm ? "bus-master DMA" : "PIO");
    }
    /* completion interrupts: the channels with disks raise IRQ 14 and 15 */
    bool chan[2] = { units[0].present || units[1].present, units[2].present || units[3].present };
    for (int c = 0; c < 2; c++)
        if (chan[c]) {
            irq_register(14 + c, ata_irq);
            outb(c ? 0x376 : 0x3F6, 0x00);       /* nIEN clear: interrupts on */
        }
    irq_ok = chan[0] || chan[1];
}

const char *ata_model(int unit)
{
    return unit >= 0 && unit < ATA_UNITS && units[unit].present ? units[unit].model : "";
}

bool ata_present(int unit)
{
    return unit >= 0 && unit < ATA_UNITS && units[unit].present;
}

uint64_t ata_sectors(int unit)
{
    return ata_present(unit) ? units[unit].sectors : 0;
}

static int ata_read_locked(int unit, uint64_t lba, size_t count, void *buf);
static int ata_write_locked(int unit, uint64_t lba, size_t count, const void *buf);

int ata_read(int unit, uint64_t lba, size_t count, void *buf)
{
    mutex_enter(&ata_lock);
    int r = ata_read_locked(unit, lba, count, buf);
    mutex_exit(&ata_lock);
    return r;
}

int ata_write(int unit, uint64_t lba, size_t count, const void *buf)
{
    mutex_enter(&ata_lock);
    int r = ata_write_locked(unit, lba, count, buf);
    mutex_exit(&ata_lock);
    return r;
}

static int ata_read_locked(int unit, uint64_t lba, size_t count, void *buf)
{
    uint16_t *p = buf;
    if (!ata_present(unit))
        return -EIO;
    struct ata_unit *u = &units[unit];
    if (lba + count > u->sectors)
        return -EIO;
    while (u->bm && count) {
        uint16_t n = count > DMA_SECTORS ? DMA_SECTORS : count;
        if (dma_xfer(u, lba, n, false) < 0) {
            kprintf("ata: disk %d: DMA read failed at %lu, using PIO\n", unit, lba);
            u->bm = 0;
            break;
        }
        memcpy(p, P2V(bounce_pa), (size_t)n * SECTOR_SIZE);
        p += (size_t)n * SECTOR_SIZE / 2;
        lba += n;
        count -= n;
    }
    while (count) {
        uint16_t n = count > 256 ? 256 : count;
        if (ata_wait(u, false) < 0)
            return -EIO;
        setup(u, lba, n);
        outb(u->io + ATA_COMMAND, CMD_READ_PIO_EXT);
        for (int s = 0; s < n; s++) {
            ata_delay(u);
            if (ata_wait(u, true) < 0)
                return -EIO;
            uint64_t words = 256;
            __asm__ volatile("rep insw" : "+D"(p), "+c"(words) : "d"(u->io + ATA_DATA) : "memory");
        }
        lba += n;
        count -= n;
    }
    return 0;
}

static int ata_write_locked(int unit, uint64_t lba, size_t count, const void *buf)
{
    const uint16_t *p = buf;
    if (!ata_present(unit))
        return -EIO;
    struct ata_unit *u = &units[unit];
    if (lba + count > u->sectors)
        return -EIO;
    while (u->bm && count) {
        uint16_t n = count > DMA_SECTORS ? DMA_SECTORS : count;
        memcpy(P2V(bounce_pa), p, (size_t)n * SECTOR_SIZE);
        if (dma_xfer(u, lba, n, true) < 0) {
            kprintf("ata: disk %d: DMA write failed at %lu, using PIO\n", unit, lba);
            u->bm = 0;
            break;
        }
        p += (size_t)n * SECTOR_SIZE / 2;
        lba += n;
        count -= n;
    }
    while (count) {
        uint16_t n = count > 256 ? 256 : count;
        if (ata_wait(u, false) < 0)
            return -EIO;
        setup(u, lba, n);
        outb(u->io + ATA_COMMAND, CMD_WRITE_PIO_EXT);
        for (int s = 0; s < n; s++) {
            ata_delay(u);
            if (ata_wait(u, true) < 0)
                return -EIO;
            for (int i = 0; i < 256; i++)
                outw(u->io + ATA_DATA, *p++);
        }
        lba += n;
        count -= n;
    }
    ata_delay(u);
    if (ata_wait(u, false) < 0)
        return -EIO;
    bool sleep = can_sleep();
    chan_done[chan_of(u)] = false;
    outb(u->io + ATA_COMMAND, CMD_FLUSH_EXT);
    ata_delay(u);
    if (sleep)
        wait_irq(u);                                 /* the host may take a while to flush */
    return ata_wait(u, false);
}

static int blk_rd(void *drv, uint64_t lba, size_t count, void *buf)
{
    return ata_read((int)(uintptr_t)drv, lba, count, buf);
}

static int blk_wr(void *drv, uint64_t lba, size_t count, const void *buf)
{
    return ata_write((int)(uintptr_t)drv, lba, count, buf);
}

static const struct blk_ops ata_blk_ops = { blk_rd, blk_wr };

DDI_DRIVER("ata", DDI_PHASE_BOOT, "ATA/IDE disks (the legacy channels, bus-master DMA)");
DDI_ALIAS("pciclass,0101");

int _init(void)
{
    ata_init();
    for (int u = 0; u < ATA_UNITS; u++)
        if (ata_present(u)) {
            char name[16], desc[64];
            snprintf(name, sizeof(name), "c%dd%dp0", u / 2, u % 2);
            int dev = blk_register_at(u, name, ata_sectors(u), &ata_blk_ops, (void *)(uintptr_t)u);
            snprintf(desc, sizeof(desc), "ATA %s", ata_model(u));
            blk_set_desc(dev, desc);
        }
    return 0;
}
