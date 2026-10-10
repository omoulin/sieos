/*
 * virtio.c - The two legacy virtio transports (virtio.h).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "virtio.h"

#if defined(__x86_64__)
/* ---- PCI: configuration space (through the kernel, SYS_PCI: drivers
 * scanning at the same time never mix their accesses), and the device's
 * registers in its I/O range (BAR 0). */
static uint32_t cfg_read(int dev, int fn, int reg)            { return pci_read(dev << 11 | fn << 8 | reg); }
static void cfg_write(int dev, int fn, int reg, uint32_t v)   { pci_write(dev << 11 | fn << 8 | reg, v); }
enum { V_FEAT = 0, V_GFEAT = 4, V_QADDR = 8, V_QSIZE = 12, V_QSEL = 14, V_QNOTIFY = 16,
       V_STATUS = 18, V_ISR = 19, V_CFG = 20 };

/* The first "transitional" virtio device of that type on bus 0 (vendor
 * 0x1AF4, device 0x1000 + type - 1: network 0x1000, block 0x1001). Its
 * line is a PCI one, routed by the chipset to an ISA IRQ: level-triggered. */
int vdev_find_n(vdev_t *d, int type, int nth)
{
    for (int dv = 0; dv < 32; dv++)
        for (int f = 0; f < 8; f++) {
            uint32_t id = cfg_read(dv, f, 0);
            if (id == 0xFFFFFFFF) { if (!f) break; continue; }
            if (id == ((0x1000u + type - 1) << 16 | 0x1AF4) && !nth--) {
                uint32_t bar = cfg_read(dv, f, 0x10);
                if (!(bar & 1)) return -1;             /* the legacy interface is an I/O range */
                d->io = bar & ~3u;
                d->irq = (cfg_read(dv, f, 0x3C) & 0xFF) | IRQ_LEVEL;
                cfg_write(dv, f, 4, cfg_read(dv, f, 4) | 5);   /* I/O space on, bus mastering (DMA) on */
                return 0;
            }
            if (!f && !(cfg_read(dv, 0, 12) >> 16 & 0x80)) break;   /* not multi-function */
        }
    return -1;
}
uint32_t vdev_features(vdev_t *d)            { return inl(d->io + V_FEAT); }
void     vdev_accept(vdev_t *d, uint32_t f)  { outl(d->io + V_GFEAT, f); }
void     vdev_status(vdev_t *d, int s)       { outb(d->io + V_STATUS, s); }
uint16_t vdev_qmax(vdev_t *d, int q)         { outw(d->io + V_QSEL, q); return inw(d->io + V_QSIZE); }
void     vdev_qset(vdev_t *d, int q, uint16_t size, uint64_t pa)
{
    (void)size;                                    /* legacy PCI: the size is the device's */
    outw(d->io + V_QSEL, q);
    outl(d->io + V_QADDR, (uint32_t)(pa >> 12));
}
void     vdev_notify(vdev_t *d, int q)       { outw(d->io + V_QNOTIFY, q); }
uint8_t  vdev_isr(vdev_t *d)                 { return inb(d->io + V_ISR); }
uint8_t  vdev_cfg8(vdev_t *d, int off)       { return inb(d->io + V_CFG + off); }
uint32_t vdev_cfg32(vdev_t *d, int off)      { return inl(d->io + V_CFG + off); }

#elif defined(__aarch64__)
#include "mk/fdt.h"
/* ---- MMIO: the registers at the address the device tree gives
 * ("virtio,mmio" nodes; QEMU's virt has 32 slots, empty ones with device
 * type 0). All 32-bit accesses. */
enum { M_MAGIC = 0x00, M_VERSION = 0x04, M_TYPE = 0x08, M_FEAT = 0x10, M_GFEAT = 0x20, M_PAGESIZE = 0x28,
       M_QSEL = 0x30, M_QMAX = 0x34, M_QNUM = 0x38, M_QALIGN = 0x3C, M_QPFN = 0x40, M_QNOTIFY = 0x50,
       M_ISR = 0x60, M_ISRACK = 0x64, M_STATUS = 0x70, M_CFG = 0x100 };
#define R(d, o)    (*(volatile uint32_t *)((d)->mm + (o)))

int vdev_find_n(vdev_t *d, int type, int nth)
{
    long size = sys_fdt(0, 0);
    char *f = size > 0 ? malloc(size) : 0;
    int found = -1;
    if (!f || sys_fdt(f, size) != size) { free(f); return -1; }
    for (int n = -1; found && (n = fdt_find(f, n, "virtio,mmio")) >= 0; ) {
        uint64_t a, len;
        int spi, flags;
        if (fdt_reg(f, n, 0, &a, &len) || fdt_spi(f, n, 0, &spi, &flags)) continue;
        volatile uint8_t *mm = map_phys(a, 0x200);     /* (each slot: a page of its own mapping) */
        if ((long)mm < 0) continue;
        d->mm = mm;
        if (R(d, M_MAGIC) != 0x74726976 || R(d, M_VERSION) != 1 || R(d, M_TYPE) != (uint32_t)type || nth--) continue;
        d->irq = spi | IRQ_LEVEL;                      /* the line stays up until the interrupt is acknowledged */
        R(d, M_PAGESIZE) = 4096;
        found = 0;
    }
    free(f);
    return found;
}
uint32_t vdev_features(vdev_t *d)            { return R(d, M_FEAT); }
void     vdev_accept(vdev_t *d, uint32_t f)  { R(d, M_GFEAT) = f; }
void     vdev_status(vdev_t *d, int s)       { R(d, M_STATUS) = s; }
uint16_t vdev_qmax(vdev_t *d, int q)         { R(d, M_QSEL) = q; return R(d, M_QMAX) > 256 ? 256 : R(d, M_QMAX); }
void     vdev_qset(vdev_t *d, int q, uint16_t size, uint64_t pa)
{
    R(d, M_QSEL) = q;
    R(d, M_QNUM) = size;
    R(d, M_QALIGN) = 4096;
    R(d, M_QPFN) = (uint32_t)(pa >> 12);
}
void     vdev_notify(vdev_t *d, int q)       { R(d, M_QNOTIFY) = q; }
uint8_t  vdev_isr(vdev_t *d)                 { uint32_t v = R(d, M_ISR); R(d, M_ISRACK) = v; return (uint8_t)v; }
uint8_t  vdev_cfg8(vdev_t *d, int off)       { return *(volatile uint8_t *)(d->mm + M_CFG + off); }
uint32_t vdev_cfg32(vdev_t *d, int off)      { return R(d, M_CFG + off); }
#endif

int vdev_find(vdev_t *d, int type) { return vdev_find_n(d, type, 0); }
