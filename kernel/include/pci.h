#ifndef SIEOS_PCI_H
#define SIEOS_PCI_H

#include "kernel.h"

struct pci_dev {
    uint8_t bus, dev, func;
    uint16_t vendor, device;
    uint8_t class_code, subclass, irq;
    uint32_t bar[6];
};

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v);
bool     pci_find(uint16_t vendor, const uint16_t *devices, int ndev, struct pci_dev *out);
bool     pci_find_class(uint8_t class_code, uint8_t subclass, struct pci_dev *out);
void     pci_enable_bus_master(struct pci_dev *d);
int      pci_scan(void);                /* returns number of devices, logs nothing */

#endif
