/*
 * virtio.c - The virtio transports (virtio.h): legacy PCI (x86-64), legacy
 * MMIO (arm64), and virtio 1.0 on PCI (both).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "virtio.h"
#include "pci.h"

/* ---- virtio 1.0 on PCI ("modern"): the common configuration's registers
 * (one queue at a time, chosen by C_QSEL), notifications at the queue's
 * offset, the interrupt status byte, the device's own configuration. */
enum { C_DFSEL = 0, C_DF = 4, C_GFSEL = 8, C_GF = 12, C_STATUS = 20, C_QSEL = 22, C_QSIZE = 24,
       C_QENABLE = 28, C_QNOFF = 30, C_QDESC = 32, C_QDRIVER = 40, C_QDEVICE = 48 };
#define VS_FEATURES_OK 8
#define C8(d, o)  (*(volatile uint8_t *)((d)->cc + (o)))
#define C16(d, o) (*(volatile uint16_t *)((d)->cc + (o)))
#define C32(d, o) (*(volatile uint32_t *)((d)->cc + (o)))

static uint32_t m_features(vdev_t *d)            { C32(d, C_DFSEL) = 0; return C32(d, C_DF); }
static void     m_accept(vdev_t *d, uint32_t f)
{
    C32(d, C_GFSEL) = 0; C32(d, C_GF) = f;
    C32(d, C_GFSEL) = 1; C32(d, C_GF) = 1;           /* feature 32, VERSION_1: the modern interface */
    C8(d, C_STATUS) |= VS_FEATURES_OK;
}
static void     m_status(vdev_t *d, int s)       { C8(d, C_STATUS) = (uint8_t)(s & VS_OK ? s | VS_FEATURES_OK : s); }
static uint16_t m_qmax(vdev_t *d, int q)         { C16(d, C_QSEL) = (uint16_t)q; return C16(d, C_QSIZE) > 256 ? 256 : C16(d, C_QSIZE); }
static void     m_qset(vdev_t *d, int q, uint16_t size, uint64_t pa)
{
    uint64_t parts[3] = { pa, pa + 16UL * size, pa + ((16UL * size + 6 + 2UL * size + 4095) & ~4095UL) };
    C16(d, C_QSEL) = (uint16_t)q;
    C16(d, C_QSIZE) = size;
    for (int i = 0; i < 3; i++) {                    /* descriptors, available ring, used ring */
        C32(d, C_QDESC + 8 * i) = (uint32_t)parts[i];
        C32(d, C_QDESC + 8 * i + 4) = (uint32_t)(parts[i] >> 32);
    }
    d->noff[q & 3] = C16(d, C_QNOFF);
    C16(d, C_QENABLE) = 1;
}
static void     m_notify(vdev_t *d, int q)       { *(volatile uint16_t *)(d->nt + (uint32_t)d->noff[q & 3] * d->nmul) = (uint16_t)q; }

/* The nth virtio device of that type on PCI: vendor 0x1AF4, device 0x1040 +
 * type, or a "transitional" one (0x1000-0x103F) whose subsystem is the
 * type. Its legacy I/O range if it has one (x86-64), else its modern
 * registers: the capabilities (vendor-specific, 9) say which BAR holds each
 * piece and where. */
static int pci_virtio(vdev_t *d, int type, int nth)
{
    pci_t p;
    for (int k = 0; !pci_scan(&p, 0, 0x1AF4, 0xFFFF, k); k++) {
        uint32_t dev = p.id >> 16, t = dev >= 0x1040 ? dev - 0x1040 : pci_cfg(&p, 0x2C) >> 16;
        if (dev < 0x1000 || t != (uint32_t)type || nth--) continue;
        *d = (vdev_t){ .irq = p.irq };
        uint64_t a, len;
#if defined(__x86_64__)
        if ((a = pci_bar(&p, 0, &len)) && !len) { d->io = (uint16_t)a; return 0; }
#endif
        volatile uint8_t *part[5] = { 0 };
        for (int c = pci_cfg(&p, 4) >> 16 & 0x10 ? pci_cfg(&p, 0x34) & 0xFC : 0; c; c = pci_cfg(&p, c) >> 8 & 0xFC) {
            uint32_t h = pci_cfg(&p, c), kind = h >> 24, off = pci_cfg(&p, c + 8), n = pci_cfg(&p, c + 12);
            if ((h & 0xFF) != 9 || kind < 1 || kind > 4 || part[kind]) continue;
            if (!(a = pci_bar(&p, pci_cfg(&p, c + 4) & 0xFF, &len)) || off + n > len) continue;
            volatile uint8_t *m = map_phys(a + off, n);
            if ((long)m < 0) continue;
            part[kind] = m;
            if (kind == 2) d->nmul = pci_cfg(&p, c + 16);
        }
        d->cc = part[1]; d->nt = part[2]; d->isr = part[3]; d->dc = part[4];
        return d->cc && d->nt && d->isr ? 0 : -1;
    }
    return -1;
}

#if defined(__x86_64__)
/* ---- Legacy PCI: the device's registers in its I/O range (BAR 0). Its
 * line is a PCI one, routed by the chipset to an ISA IRQ: level-triggered. */
enum { V_FEAT = 0, V_GFEAT = 4, V_QADDR = 8, V_QSIZE = 12, V_QSEL = 14, V_QNOTIFY = 16,
       V_STATUS = 18, V_ISR = 19, V_CFG = 20 };

int vdev_find_n(vdev_t *d, int type, int nth) { return pci_virtio(d, type, nth); }
static uint32_t l_features(vdev_t *d)            { return inl(d->io + V_FEAT); }
static void     l_accept(vdev_t *d, uint32_t f)  { outl(d->io + V_GFEAT, f); }
static void     l_status(vdev_t *d, int s)       { outb(d->io + V_STATUS, s); }
static uint16_t l_qmax(vdev_t *d, int q)         { outw(d->io + V_QSEL, q); return inw(d->io + V_QSIZE); }
static void     l_qset(vdev_t *d, int q, uint16_t size, uint64_t pa)
{
    (void)size;                                    /* legacy PCI: the size is the device's */
    outw(d->io + V_QSEL, q);
    outl(d->io + V_QADDR, (uint32_t)(pa >> 12));
}
static void     l_notify(vdev_t *d, int q)       { outw(d->io + V_QNOTIFY, q); }
static uint8_t  l_isr(vdev_t *d)                 { return inb(d->io + V_ISR); }
static uint8_t  l_cfg8(vdev_t *d, int off)       { return inb(d->io + V_CFG + off); }
static uint32_t l_cfg32(vdev_t *d, int off)      { return inl(d->io + V_CFG + off); }

#elif defined(__aarch64__)
#include "mk/fdt.h"
/* ---- Legacy MMIO: the registers at the address the device tree gives
 * ("virtio,mmio" nodes; QEMU's virt has 32 slots, empty ones with device
 * type 0). All 32-bit accesses. None of that type: PCI. */
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
        *d = (vdev_t){ .mm = mm };
        if (R(d, M_MAGIC) != 0x74726976 || R(d, M_VERSION) != 1 || R(d, M_TYPE) != (uint32_t)type || nth--) continue;
        d->irq = spi | IRQ_LEVEL;                      /* the line stays up until the interrupt is acknowledged */
        R(d, M_PAGESIZE) = 4096;
        found = 0;
    }
    free(f);
    return found ? pci_virtio(d, type, nth) : 0;
}
static uint32_t l_features(vdev_t *d)            { return R(d, M_FEAT); }
static void     l_accept(vdev_t *d, uint32_t f)  { R(d, M_GFEAT) = f; }
static void     l_status(vdev_t *d, int s)       { R(d, M_STATUS) = s; }
static uint16_t l_qmax(vdev_t *d, int q)         { R(d, M_QSEL) = q; return R(d, M_QMAX) > 256 ? 256 : R(d, M_QMAX); }
static void     l_qset(vdev_t *d, int q, uint16_t size, uint64_t pa)
{
    R(d, M_QSEL) = q;
    R(d, M_QNUM) = size;
    R(d, M_QALIGN) = 4096;
    R(d, M_QPFN) = (uint32_t)(pa >> 12);
}
static void     l_notify(vdev_t *d, int q)       { R(d, M_QNOTIFY) = q; }
static uint8_t  l_isr(vdev_t *d)                 { uint32_t v = R(d, M_ISR); R(d, M_ISRACK) = v; return (uint8_t)v; }
static uint8_t  l_cfg8(vdev_t *d, int off)       { return *(volatile uint8_t *)(d->mm + M_CFG + off); }
static uint32_t l_cfg32(vdev_t *d, int off)      { return R(d, M_CFG + off); }
#endif

int      vdev_find(vdev_t *d, int type)                 { return vdev_find_n(d, type, 0); }
uint32_t vdev_features(vdev_t *d)                       { return d->cc ? m_features(d) : l_features(d); }
void     vdev_accept(vdev_t *d, uint32_t f)             { if (d->cc) m_accept(d, f); else l_accept(d, f); }
void     vdev_status(vdev_t *d, int s)                  { if (d->cc) m_status(d, s); else l_status(d, s); }
uint16_t vdev_qmax(vdev_t *d, int q)                    { return d->cc ? m_qmax(d, q) : l_qmax(d, q); }
void     vdev_qset(vdev_t *d, int q, uint16_t size, uint64_t pa) { if (d->cc) m_qset(d, q, size, pa); else l_qset(d, q, size, pa); }
void     vdev_notify(vdev_t *d, int q)                  { if (d->cc) m_notify(d, q); else l_notify(d, q); }
uint8_t  vdev_isr(vdev_t *d)                            { return d->cc ? *d->isr : l_isr(d); }
uint8_t  vdev_cfg8(vdev_t *d, int off)                  { return d->cc ? d->dc[off] : l_cfg8(d, off); }
uint32_t vdev_cfg32(vdev_t *d, int off)                 { return d->cc ? *(volatile uint32_t *)(d->dc + off) : l_cfg32(d, off); }
