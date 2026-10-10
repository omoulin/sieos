/*
 * virtio.h - Finding and talking to a virtio device, whatever the "transport":
 *   x86-64: the legacy PCI interface (registers in an I/O port range, QEMU's
 *           "-device virtio-...-pci,disable-modern=on");
 *   arm64:  the legacy MMIO interface (registers in memory, found in the
 *           device tree: QEMU's virt, "-device virtio-...-device").
 * Both use the same "legacy" virtqueue layout (descriptors, available ring,
 * then the used ring on the next page), given to the device as a page
 * number. The drivers (vblk, vnet) only see these functions. virtio.c.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

enum { VIRTIO_NET = 1, VIRTIO_BLK = 2, VIRTIO_INPUT = 18 };   /* device types */
enum { VS_ACK = 1, VS_DRIVER = 2, VS_OK = 4 };      /* device status bits */

typedef struct {
    int irq;                     /* its interrupt line (irq_bind; IRQ_LEVEL included when level) */
    uint16_t io;                 /* x86-64: the I/O port range */
    volatile uint8_t *mm;        /* arm64: the registers */
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
