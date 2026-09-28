/*
 * pci.c - PCI configuration space access (mechanism #1, ports 0xCF8/0xCFC).
 */
#include "pci.h"

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint32_t addr = 0x80000000U | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)func << 8) | (off & 0xFC);
    __asm__ volatile("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)0xCF8));
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)0xCFC));
    return v;
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v)
{
    uint32_t addr = 0x80000000U | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)func << 8) | (off & 0xFC);
    __asm__ volatile("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)0xCF8));
    __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"((uint16_t)0xCFC));
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

int pci_scan(void)
{
    int n = 0;
    for (int bus = 0; bus < 256; bus++)
        for (int dev = 0; dev < 32; dev++)
            if ((pci_read32(bus, dev, 0, 0) & 0xFFFF) != 0xFFFF)
                n++;
    return n;
}

void pci_enable_bus_master(struct pci_dev *d)
{
    uint32_t cmd = pci_read32(d->bus, d->dev, d->func, 0x04);
    cmd |= 0x02 | 0x04;                         /* memory space + bus master */
    cmd &= ~0x400;                              /* interrupts enabled (INTx) */
    pci_write32(d->bus, d->dev, d->func, 0x04, cmd);
}
