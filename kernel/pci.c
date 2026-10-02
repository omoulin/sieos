/*
 * pci.c - PCI configuration space access (mechanism #1, ports 0xCF8/0xCFC).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "pci.h"
#include "smp.h"

static struct spinlock cf8_lock;                 /* the address port, then the data port */

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint32_t addr = 0x80000000U | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)func << 8) | (off & 0xFC);
    uint32_t v;
    spin_lock(&cf8_lock);
    __asm__ volatile("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)0xCF8));
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)0xCFC));
    spin_unlock(&cf8_lock);
    return v;
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v)
{
    uint32_t addr = 0x80000000U | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)func << 8) | (off & 0xFC);
    spin_lock(&cf8_lock);
    __asm__ volatile("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)0xCF8));
    __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"((uint16_t)0xCFC));
    spin_unlock(&cf8_lock);
}

static void fill(struct pci_dev *d, uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t id = pci_read32(bus, dev, func, 0);
    uint32_t cls = pci_read32(bus, dev, func, 8);
    d->bus = bus;
    d->dev = dev;
    d->func = func;
    d->vendor = id & 0xFFFF;
    d->device = id >> 16;
    d->class_code = cls >> 24;
    d->subclass = (cls >> 16) & 0xFF;
    d->prog_if = (cls >> 8) & 0xFF;
    d->revision = cls & 0xFF;
    d->irq = pci_read32(bus, dev, func, 0x3C) & 0xFF;
    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_read32(bus, dev, func, 0x10 + i * 4);
}

bool pci_find(uint16_t vendor, const uint16_t *devices, int ndev, struct pci_dev *out)
{
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_read32(bus, dev, func, 0);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (func == 0)
                        break;
                    continue;
                }
                if ((id & 0xFFFF) == vendor) {
                    for (int k = 0; k < ndev; k++) {
                        if ((id >> 16) == devices[k]) {
                            fill(out, bus, dev, func);
                            return true;
                        }
                    }
                }
                if (func == 0 && !(pci_read32(bus, dev, 0, 0x0C) & 0x00800000))
                    break;                      /* not multi-function */
            }
        }
    }
    return false;
}

bool pci_find_class(uint8_t class_code, uint8_t subclass, struct pci_dev *out)
{
    for (int bus = 0; bus < 256; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_read32(bus, dev, func, 0);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (func == 0)
                        break;
                    continue;
                }
                uint32_t cls = pci_read32(bus, dev, func, 8);
                if ((cls >> 24) == class_code && ((cls >> 16) & 0xFF) == subclass) {
                    fill(out, bus, dev, func);
                    return true;
                }
                if (func == 0 && !(pci_read32(bus, dev, 0, 0x0C) & 0x00800000))
                    break;
            }
    return false;
}

static struct pci_dev table[PCI_MAX];
static const char *driver[PCI_MAX];
static int ntable = -1;

static void scan_table(void)
{
    ntable = 0;
    for (int bus = 0; bus < 256; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_read32(bus, dev, func, 0);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (func == 0)
                        break;
                    continue;
                }
                if (ntable < PCI_MAX)
                    fill(&table[ntable++], bus, dev, func);
                if (func == 0 && !(pci_read32(bus, dev, 0, 0x0C) & 0x00800000))
                    break;
            }
}

int pci_count(void)
{
    if (ntable < 0)
        scan_table();
    return ntable;
}

void pci_claim(const struct pci_dev *d, const char *name)
{
    for (int i = 0; i < pci_count(); i++)
        if (table[i].bus == d->bus && table[i].dev == d->dev && table[i].func == d->func)
            driver[i] = name;
}

const char *pci_driver(int i)
{
    return i >= 0 && i < pci_count() ? driver[i] : NULL;
}

const struct pci_dev *pci_at(int i)
{
    return i >= 0 && i < pci_count() ? &table[i] : NULL;
}

int pci_find_all(uint16_t vendor, const uint16_t *devices, int ndev, struct pci_dev *out, int max)
{
    int n = 0;
    for (int i = 0; i < pci_count() && n < max; i++)
        if (table[i].vendor == vendor)
            for (int k = 0; k < ndev; k++)
                if (table[i].device == devices[k]) {
                    out[n++] = table[i];
                    break;
                }
    return n;
}

uint8_t pci_read8(const struct pci_dev *d, uint8_t off)
{
    return pci_read32(d->bus, d->dev, d->func, off) >> (8 * (off & 3));
}

uint16_t pci_read16(const struct pci_dev *d, uint8_t off)
{
    return pci_read32(d->bus, d->dev, d->func, off) >> (8 * (off & 2));
}

uint8_t pci_find_cap(const struct pci_dev *d, uint8_t id, uint8_t after)
{
    if (!(pci_read16(d, 0x06) & 0x10))                 /* no capability list */
        return 0;
    uint8_t p = after ? pci_read8(d, after + 1) : pci_read8(d, 0x34);
    for (int guard = 0; p >= 0x40 && guard < 48; guard++) {
        if (pci_read8(d, p) == id)
            return p;
        p = pci_read8(d, p + 1);
    }
    return 0;
}

uint64_t pci_bar_addr(const struct pci_dev *d, int i, bool *io)
{
    uint32_t b = d->bar[i];
    if (io)
        *io = b & 1;
    if (b & 1)
        return b & ~3U;
    uint64_t a = b & ~0xFU;
    if ((b & 6) == 4 && i < 5)                          /* 64-bit */
        a |= (uint64_t)d->bar[i + 1] << 32;
    return a;
}

uint64_t pci_bar_size(const struct pci_dev *d, int i)
{
    uint8_t off = 0x10 + 4 * i;
    uint32_t cmd = pci_read32(d->bus, d->dev, d->func, 0x04);
    pci_write32(d->bus, d->dev, d->func, 0x04, cmd & ~3U);          /* decoding off while sizing */
    uint32_t old = pci_read32(d->bus, d->dev, d->func, off);
    pci_write32(d->bus, d->dev, d->func, off, 0xFFFFFFFF);
    uint32_t lo = pci_read32(d->bus, d->dev, d->func, off);
    pci_write32(d->bus, d->dev, d->func, off, old);
    uint64_t mask;
    if (old & 1) {
        mask = 0xFFFF0000U | (lo & ~3U);
    } else if ((old & 6) == 4 && i < 5) {
        uint32_t oldhi = pci_read32(d->bus, d->dev, d->func, off + 4);
        pci_write32(d->bus, d->dev, d->func, off + 4, 0xFFFFFFFF);
        uint32_t hi = pci_read32(d->bus, d->dev, d->func, off + 4);
        pci_write32(d->bus, d->dev, d->func, off + 4, oldhi);
        mask = (uint64_t)hi << 32 | (lo & ~0xFU);
    } else {
        mask = 0xFFFFFFFF00000000UL | (lo & ~0xFU);
    }
    pci_write32(d->bus, d->dev, d->func, 0x04, cmd);
    return mask ? ~mask + 1 : 0;
}

int pci_scan(void)
{
    return pci_count();
}

/*
 * The bridges between the root and d's bus: memory space and bus master on.
 * A PCIe root port forwards a device's DMA upstream only with its own bus
 * master bit (the firmware may leave it off when it did not use the device).
 */
void pci_enable_path(const struct pci_dev *d)
{
    for (int i = 0; i < pci_count(); i++) {
        const struct pci_dev *b = pci_at(i);
        if (((pci_read32(b->bus, b->dev, b->func, 0x0C) >> 16) & 0x7F) != 1)
            continue;                                /* (not a PCI-to-PCI bridge) */
        uint32_t buses = pci_read32(b->bus, b->dev, b->func, 0x18);
        uint8_t sec = (buses >> 8) & 0xFF, sub = (buses >> 16) & 0xFF;
        if (d->bus < sec || d->bus > sub || !sec)
            continue;
        uint32_t cmd = pci_read32(b->bus, b->dev, b->func, 0x04);
        if ((cmd & 0x06) != 0x06) {
            pci_write32(b->bus, b->dev, b->func, 0x04, cmd | 0x06);
            kprintf("pci: bridge %02x:%02x.%x (to bus %02x-%02x): memory and bus master turned on\n", b->bus, b->dev,
                    b->func, sec, sub);
        }
    }
}

void pci_enable_bus_master(const struct pci_dev *d)
{
    uint32_t cmd = pci_read32(d->bus, d->dev, d->func, 0x04);
    cmd |= 0x02 | 0x04;                         /* memory space + bus master */
    cmd &= ~0x400;                              /* interrupts enabled (INTx) */
    pci_write32(d->bus, d->dev, d->func, 0x04, cmd);
}
