/*
 * pci.h - Finding a PCI device and making it usable (pci.c): its
 * configuration space, a BAR's registers at an address with memory or I/O
 * decoding and DMA ("bus mastering") on, its interrupt line.
 *   x86-64: configuration through the kernel (SYS_PCI), every bus behind
 *           the bridges; the firmware gave the BARs addresses and the lines.
 *   arm64:  QEMU's virt: a "pci-host-ecam-generic" bridge in the device
 *           tree, configuration in memory (ECAM), bus 0. Nobody gave the
 *           BARs addresses (we start without firmware): pci_bar does, from
 *           the bridge's memory window; the line is in its "interrupt-map".
 *           (The Raspberry Pis' PCIe bridges: user/usb/pci.c.)
 * Used by virtio.c (virtio on PCI), vnet's e1000.c, the USB server (xHCI)
 * and the disk server (SD controllers on PCI).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "mk.h"

typedef struct {
    uint32_t id;                 /* vendor | device << 16 */
    int irq;                     /* for irq_bind (IRQ_LEVEL included), or -1: none */
    int64_t bus_off;             /* the address the device uses for RAM = physical + bus_off */
    uint32_t at;                 /* bus << 16 | device << 11 | function << 8 */
    volatile uint32_t *cfg;      /* arm64: its configuration space (0 on x86-64: SYS_PCI) */
    uint64_t wpci, wcpu;         /* the bridge's memory window, PCI and CPU addresses (x86-64: 0) */
} pci_t;

/* The nth device (0, 1...) whose configuration dword at reg, masked, is
 * val: reg 0 for vendor | device << 16, reg 8 for the class code << 8. 0 if found. */
int pci_scan(pci_t *p, int reg, uint32_t val, uint32_t mask, int nth);
uint32_t pci_cfg(const pci_t *p, int reg);
void pci_cfgw(const pci_t *p, int reg, uint32_t v);
/* BAR i: the CPU's physical address of its registers (an I/O BAR: the
 * port), *len their size; given an address in the window if it had none;
 * decoding and bus mastering turned on. 0 if there is no such BAR. */
uint64_t pci_bar(pci_t *p, int i, uint64_t *len);
/* Its interrupt as a message (MSI, else MSI-X), delivered to port: -> the
 * irq for irq_ack, *how "MSI" or "MSI-X"; -1: neither (or no kernel MSI:
 * arm64), the line stays. */
int pci_msi(pci_t *p, long port, const char **how);

#if defined(__aarch64__)
/* The bridge's 32-bit memory window in its "ranges" (or "dma-ranges": where
 * devices see our RAM), and the GIC line of a device's pin (1-4) from its
 * "interrupt-map" (devfn_hi: the bus/device/function cell). Also used for
 * the Raspberry Pi 4's bridge (user/usb/pci.c). */
int pci_window(const void *fdt, int node, const char *prop, uint64_t *pci, uint64_t *cpu, uint64_t *size);
int pci_pin_irq(const void *fdt, int node, uint32_t devfn_hi, int pin);
#endif
