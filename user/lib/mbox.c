/*
 * mbox.c - The Raspberry Pis' firmware mailbox (mbox.h).
 *
 * The mailbox is two FIFOs in the "brcm,bcm2835-mbox" registers: we write
 * (buffer's bus address | channel 8) to mailbox 1 and read the answer from
 * mailbox 0. The firmware does not see the CPU's caches: the buffer is
 * written to memory before, and its cached lines dropped after (dma_sync).
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "mk/fdt.h"
#include "mbox.h"

enum { READ = 0x00 / 4, STATUS0 = 0x18 / 4, WRITE = 0x20 / 4, STATUS1 = 0x38 / 4 };
#define FULL  0x80000000u
#define EMPTY 0x40000000u
#define CHAN  8                                 /* properties, ARM -> VideoCore */

static volatile uint32_t *mb;
static uint32_t *dbuf;                          /* our DMA page (first GiB) */
static uint64_t dbus;                           /* its bus address, as the firmware sees it */

static int init(void)
{
    if (mb) return 0;
    long size = sys_fdt(0, 0);
    char *f = size > 0 ? malloc(size) : 0;
    uint64_t a, len, pa;
    int n, r = -1;
    if (f && sys_fdt(f, size) == size && (n = fdt_find(f, -1, "brcm,bcm2835-mbox")) >= 0 && !fdt_reg(f, n, 0, &a, &len)) {
        uint32_t *d = dma_alloc(4096 | DMA_LOW, &pa);
        volatile uint32_t *m = map_phys(a, 0x40);
        if ((long)d > 0 && (long)m > 0 && !fdt_bus_addr(f, n, pa, &dbus)) { mb = m; dbuf = d; r = 0; }
    }
    free(f);
    return r;
}

int mbox_call(uint32_t *buf, int n)
{
    if (init() || n * 4 > 4096) return -1;
    memcpy(dbuf, buf, n * 4);
    dma_sync(dbuf, n * 4);                                  /* ours, in memory */
    while (mb[STATUS1] & FULL) sys_yield();
    mb[WRITE] = (uint32_t)dbus | CHAN;
    for (int k = 0; k < 1000000; k++) {
        if (mb[STATUS0] & EMPTY) { sys_yield(); continue; }
        if (mb[READ] == ((uint32_t)dbus | CHAN)) {
            dma_sync(dbuf, n * 4);                          /* the firmware's answer, not cached lines */
            memcpy(buf, dbuf, n * 4);
            return buf[1] == 0x80000000u ? 0 : -1;
        }
    }
    return -1;
}
