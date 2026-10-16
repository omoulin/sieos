/*
 * nvme.c - NVMe drives (PCIe SSDs: PCI class 01:08:02), a backend of the
 * disk server. vblk.c tries each controller's namespaces in turn and keeps
 * the first with a SieFS partition; the others are only listed.
 *
 * The controller reads commands (64 bytes: opcode, namespace, data
 * pointers, the command's words) from a submission queue in our memory
 * and writes 16-byte completions (status, a phase bit that flips on each
 * pass around the ring) to a completion queue; a "doorbell" register per
 * queue tells it where the tail is, and where we have read up to. Queue 0
 * is the admin queue (identify, create queues); queue 1 does the I/O.
 * Data is described by "PRP" entries: PRP1 the first page, PRP2 the second
 * or, beyond two pages, the address of a list of the following pages.
 *
 * One command at a time (the file server sends one at a time), so each
 * queue has two entries, and the PRP list is built once: the data buffer
 * never moves. Polled: the drive answers in microseconds, and a PCI line
 * needs routing SIEOS does not read from ACPI (and MSI-X is not supported):
 * the controller's interrupts stay off. No copies: the client's data
 * arrives by IPC in the buffer the drive reads, and is replied from where
 * it writes.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "pci.h"
#include "nvme.h"

enum { CAP = 0, CC = 0x14 / 4, CSTS = 0x1C / 4, AQA = 0x24 / 4, ASQ = 0x28 / 4, ACQ = 0x30 / 4 };   /* registers (dwords) */
#define TMO 30000000000L                     /* a command not done in 30 s: the drive is gone */

typedef struct { uint32_t *sq; volatile uint32_t *cq, *db; uint32_t tail, head, phase; } q_t;
static q_t aq, ioq;                          /* admin, I/O */
static volatile uint32_t *r;                 /* the registers (BAR 0) */
static uint8_t *mem;                         /* DMA: data (DISK_MAX), then admin SQ, admin CQ, I/O SQ (+ PRP list), I/O CQ */
static uint64_t bus;                         /* its bus address */
static uint32_t dstrd, nn, nsid, lbads;      /* doorbell stride, namespaces, the one in use, log2(sector size) */
static int cur = -1, dead;                   /* the controller set up; a command timed out */

/* One command, waited for: -> 0 done, -EIO failed. Spin a little (the
 * usual answer), then yield until the timeout. */
static long cmd(q_t *q, uint32_t op, uint64_t p1, uint64_t p2, uint32_t c10, uint32_t c11, uint32_t c12)
{
    if (dead) return -EIO;
    uint32_t *e = q->sq + 16 * q->tail;
    memset(e, 0, 64);
    e[0] = op; e[1] = nsid; e[6] = (uint32_t)p1; e[7] = p1 >> 32; e[8] = (uint32_t)p2; e[9] = p2 >> 32;
    e[10] = c10; e[11] = c11; e[12] = c12;
    q->tail ^= 1;
    dma_wmb();
    q->db[0] = q->tail;                                  /* SQ tail doorbell */
    volatile uint32_t *c = q->cq + 4 * q->head;
    long t0 = 0;
    for (int n = 0; (__atomic_load_n(&c[3], __ATOMIC_ACQUIRE) >> 16 & 1) != q->phase; n++)
        if (n > 1000) {
            long t = sys_clock();
            if (!t0) t0 = t;
            else if (t - t0 > TMO) { dead = 1; printf("vblk: NVMe: no answer, giving up on the drive\n"); return -EIO; }
            sys_yield();
        }
    uint32_t st = c[3] >> 17;
    if (!(q->head ^= 1)) q->phase ^= 1;
    q->db[1 << dstrd] = q->head;                         /* CQ head doorbell */
    return st ? -EIO : 0;
}
static long admin(uint32_t op, uint64_t p1, uint32_t c10, uint32_t c11) { return cmd(&aq, op, p1, 0, c10, c11, 0); }

static int wait_rdy(uint32_t want, long ns)
{
    for (long t0 = sys_clock(); (r[CSTS] & 1) != want; sys_sleep(1000000))
        if (r[CSTS] & 2 || sys_clock() - t0 > ns) return -1;      /* (CFS: a fatal controller error) */
    return 0;
}

/* Controller nth: reset, admin queues, enabled, identified, I/O queues. */
static int start(int nth)
{
    pci_t p;
    uint64_t len, pa;
    if (pci_scan(&p, 8, 0x010802 << 8, 0xFFFFFF00, nth)) return -2;
    if (!(pa = pci_bar(&p, 0, &len)) || (long)(r = map_phys(pa, len)) <= 0) return -1;
    pci_cfgw(&p, 4, pci_cfg(&p, 4) | 1 << 10);          /* its line's interrupt off (we poll) */
    long to = ((r[CAP] >> 24) + 1) * 500000000L;        /* CAP.TO: ready within this many 500 ms */
    dstrd = r[CAP + 1] & 15;
    if (r[CAP + 1] >> 16 & 15) return -1;               /* MPSMIN: 4 KiB pages must be allowed */
    if (!mem) {
        if ((long)(mem = dma_alloc(DISK_MAX + 4 * 4096, &bus)) < 0) { mem = 0; return -1; }
        bus += p.bus_off;
    }
    r[CC] = 0;                                           /* reset (the firmware may have used it) */
    if (wait_rdy(0, to)) return -1;
    uint8_t *qm = mem + DISK_MAX;
    uint64_t qb = bus + DISK_MAX, *prp = (uint64_t *)(qm + 2 * 4096 + 2048);
    memset(qm, 0, 4 * 4096);
    for (int i = 0; i < DISK_MAX / 4096 - 1; i++) prp[i] = bus + (i + 1) * 4096ULL;   /* pages 2..32 */
    aq = (q_t){ (uint32_t *)qm, (uint32_t *)(qm + 4096), r + 0x400, 0, 0, 1 };
    ioq = (q_t){ (uint32_t *)(qm + 2 * 4096), (uint32_t *)(qm + 3 * 4096), r + 0x400 + (2 << dstrd), 0, 0, 1 };
    r[AQA] = 1 << 16 | 1;                                /* two entries each */
    r[ASQ] = (uint32_t)qb; r[ASQ + 1] = qb >> 32;
    r[ACQ] = (uint32_t)(qb + 4096); r[ACQ + 1] = (qb + 4096) >> 32;
    r[CC] = 1 | 6 << 16 | 4 << 20;                       /* on: NVM commands, 4 KiB pages, 64 B SQ / 16 B CQ entries */
    nsid = dead = 0;
    if (wait_rdy(1, to) || admin(6, bus, 1, 0)) return -1;   /* identify the controller (CNS 1) */
    memcpy(&nn, mem + 516, 4);                           /* (both processors are little-endian, like NVMe) */
    char m[41] = { 0 };                                  /* the model: 40 characters, space-padded */
    memcpy(m, mem + 24, 40);
    for (int n = 40; n && m[n - 1] == ' ';) m[--n] = 0;
    printf("vblk: NVMe %02x:%02x.%x: %s\n", p.at >> 16 & 0xFF, p.at >> 11 & 31, p.at >> 8 & 7, m);
    if (mem[77] && mem[77] < 5) return -1;              /* MDTS (in 4 KiB pages, log2): 128 KiB transfers must be allowed */
    if (admin(5, qb + 3 * 4096, 1 << 16 | 1, 1) ||                    /* create I/O CQ 1 (contiguous, no interrupts) */
        admin(1, qb + 2 * 4096, 1 << 16 | 1, 1 << 16 | 1)) return -1;   /* create I/O SQ 1 -> CQ 1 */
    return 0;
}

int nvme_open(int nth, uint32_t ns, uint64_t *sectors, uint32_t *bs)
{
    if (nth != cur) {
        if (cur >= 0) r[CC] = 0;                         /* the previous one: off */
        int e = start(nth);
        cur = e ? -1 : nth;
        if (e) return e;
    }
    if (ns > nn || ns > 8) return -1;
    nsid = ns;
    if (admin(6, bus, 0, 0)) return -1;                     /* identify namespace (CNS 0) */
    uint64_t nsze;
    memcpy(&nsze, mem, 8);
    lbads = mem[128 + 4 * (mem[26] & 15) + 2];           /* the LBA format in use: log2 of the sector size */
    if (!nsze) return 1;                                 /* (not an active namespace) */
    printf("vblk: NVMe namespace %u: %lu MiB, %u-byte sectors\n", ns, (unsigned long)(nsze << lbads >> 20), 1u << lbads);
    if (lbads < 9 || lbads > 12) return 1;
    *sectors = nsze;
    *bs = 1u << lbads;
    return 0;
}

long nvme_io(int op, uint64_t lba, uint32_t count)
{
    uint32_t bytes = count << lbads;
    uint64_t p2 = bytes <= 4096 ? 0 : bytes <= 8192 ? bus + 4096 : bus + DISK_MAX + 2 * 4096 + 2048;
    return cmd(&ioq, op, bus, p2, (uint32_t)lba, lba >> 32, count - 1);
}

void *nvme_buffer(void) { return mem; }
