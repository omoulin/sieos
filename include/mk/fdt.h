/*
 * mk/fdt.h - Reading a "device tree" (flattened, FDT): the description of
 * the machine that the firmware or boot loader hands over on most ARM
 * systems (QEMU's virt, the Raspberry Pis): its memory, processors,
 * interrupt controller and devices, each with its addresses ("reg") and
 * interrupts. Used by the kernel at boot (kernel/arch/arm64) and by drivers
 * (SYS_FDT gives them a copy) to find their devices. lib/fdt.c.
 *
 * A node is named by its offset in the blob; nothing is allocated.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

uint32_t fdt_be32(const void *p);                       /* a big-endian 32-bit cell */
uint64_t fdt_cells(const void *p, int n);               /* n cells (1 or 2) as one number */
uint32_t fdt_valid(const void *fdt, uint64_t max);      /* its size if it is a sane blob, else 0 */
int fdt_next(const void *fdt, int node, int *depth);    /* the node after node (-1: start), or -1 */
const char *fdt_name(const void *fdt, int node);        /* "virtio_mmio@a000000" */
const void *fdt_prop(const void *fdt, int node, const char *name, int *len);
int fdt_compatible(const void *fdt, int node, const char *compat);   /* 1 if it is one */
int fdt_find(const void *fdt, int after, const char *compat);        /* the next such node, or -1 */
int fdt_path(const void *fdt, const char *path);        /* "/chosen", "/psci" (unit address
                                                           optional: "/memory" finds "memory@40000000") */
/* The #address-cells and #size-cells that apply to node's "reg" (its parent's). */
void fdt_reg_cells(const void *fdt, int node, int *ac, int *sc);
/* Its i-th "reg" entry: 0 if there is one (address and size), -1 otherwise.
 * The address is translated through the parents' "ranges" to a CPU
 * physical address. */
int fdt_reg(const void *fdt, int node, int i, uint64_t *addr, uint64_t *size);
/* Its i-th interrupt as the GIC's shared line number (SPI), with the flags
 * cell (4: level-triggered, active high; 1: edge): 0 if there is one. */
int fdt_spi(const void *fdt, int node, int i, int *spi, int *flags);
/* CPU physical address -> the address the device sees, through the buses'
 * "dma-ranges": 0, or -1 if the device cannot reach that memory. */
int fdt_bus_addr(const void *fdt, int node, uint64_t pa, uint64_t *bus);
/* 1 if the device sees the CPU's caches ("dma-coherent"). */
int fdt_dma_coherent(const void *fdt, int node);
