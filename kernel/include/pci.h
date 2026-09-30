/*
 * pci.h
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_PCI_H
#define SIEOS_PCI_H

#include "kernel.h"

struct pci_dev {
    uint8_t bus, dev, func;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if, revision, irq;
    uint32_t bar[6];
};

#define PCI_MAX 64

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v);
bool     pci_find(uint16_t vendor, const uint16_t *devices, int ndev, struct pci_dev *out);
bool     pci_find_class(uint8_t class_code, uint8_t subclass, struct pci_dev *out);
void     pci_enable_bus_master(const struct pci_dev *d);
void     pci_enable_path(const struct pci_dev *d);   /* the bridges above d: memory space, bus master */
int      pci_scan(void);                /* returns number of devices, logs nothing */

/* The devices found at boot (every function), in bus order. */
int      pci_count(void);
const struct pci_dev *pci_at(int i);
/* Every device of vendor with one of the device ids, into out (up to max). */
int      pci_find_all(uint16_t vendor, const uint16_t *devices, int ndev, struct pci_dev *out, int max);
uint8_t  pci_read8(const struct pci_dev *d, uint8_t off);
uint16_t pci_read16(const struct pci_dev *d, uint8_t off);
/* The next capability with this id after offset 'after' (0: the first); 0 if none. */
uint8_t  pci_find_cap(const struct pci_dev *d, uint8_t id, uint8_t after);
/* A BAR's address (memory or I/O, 64-bit BARs joined) and its size. */
uint64_t pci_bar_addr(const struct pci_dev *d, int i, bool *io);
uint64_t pci_bar_size(const struct pci_dev *d, int i);
/* A driver has taken the device (shown by devinfo); the driver's name, or NULL. */
void     pci_claim(const struct pci_dev *d, const char *driver);
const char *pci_driver(int i);

#endif
