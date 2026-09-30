/*
 * hello.c - The smallest driver: no device (no alias), loaded with modload;
 * it says hello and lists the PCI devices it could drive.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "ddi.h"
#include "pci.h"

DDI_DRIVER("hello", DDI_PHASE_ROOT, "an example driver: says hello");

int _init(void)
{
    kprintf("hello: loaded; %d PCI functions:\n", pci_count());
    for (int i = 0; i < pci_count(); i++) {
        const struct pci_dev *d = pci_at(i);
        kprintf("hello:   %02x:%02x.%x %04x:%04x class %02x%02x%02x\n", d->bus, d->dev, d->func, d->vendor,
                d->device, d->class_code, d->subclass, d->prog_if);
    }
    return 0;
}
