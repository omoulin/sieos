/*
 * virtio.h - Finding and talking to a virtio device, whatever the "transport":
 *   legacy PCI (x86-64): registers in an I/O port range (QEMU's
 *           "-device virtio-...-pci" on a PCI bus, or with disable-modern=on);
 *   legacy MMIO (arm64): registers in memory, found in the device tree
 *           (QEMU's virt, "-device virtio-...-device");
 *   virtio 1.0 PCI ("modern", both): registers in memory, in pieces the
 *           device's PCI capabilities point to (QEMU's virtio-...-pci on
 *           PCIe, on arm64's virt, or with disable-legacy=on).
 * All three take the same "legacy" virtqueue layout (descriptors, available
 * ring, then the used ring on the next page), given as its address. The
 * drivers (vblk, vnet, con) only see these functions; the one difference
 * left to them: a modern network card's frame header is 12 bytes, not 10.
 * virtio.c.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

enum { VIRTIO_NET = 1, VIRTIO_BLK = 2, VIRTIO_INPUT = 18 };   /* device types */
enum { VS_ACK = 1, VS_DRIVER = 2, VS_OK = 4 };      /* device status bits */

typedef struct {
    int irq;                     /* its interrupt line (irq_bind; IRQ_LEVEL included when level) */
    uint16_t io;                 /* legacy PCI: the I/O port range */
    volatile uint8_t *mm;        /* legacy MMIO: the registers */
    volatile uint8_t *cc, *nt, *isr, *dc;   /* modern: common configuration (0: not modern),
                                               notifications, interrupt status, device's own */
    uint32_t nmul;               /* modern: notification address = nt + queue's offset * nmul */
    uint16_t noff[4];            /* modern: queues 0-3's offsets */
} vdev_t;

int      vdev_find(vdev_t *d, int type);            /* the first device of that type: 0 if found */
int      vdev_find_n(vdev_t *d, int type, int nth); /* the nth (0, 1...) */
uint32_t vdev_features(vdev_t *d);                  /* what it offers */
void     vdev_accept(vdev_t *d, uint32_t f);        /* what we use */
void     vdev_status(vdev_t *d, int s);             /* 0: reset */
uint16_t vdev_qmax(vdev_t *d, int q);               /* queue q's largest size (0: none) */
void     vdev_qset(vdev_t *d, int q, uint16_t size, uint64_t ring_pa);   /* its ring, page-aligned */
void     vdev_notify(vdev_t *d, int q);             /* "look at queue q" (after dma_wmb) */
uint8_t  vdev_isr(vdev_t *d);                       /* why it interrupted; lowers its line */
uint8_t  vdev_cfg8(vdev_t *d, int off);             /* its configuration space (MAC, capacity) */
uint32_t vdev_cfg32(vdev_t *d, int off);
