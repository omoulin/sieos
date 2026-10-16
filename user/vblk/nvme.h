/*
 * nvme.h - The NVMe backend of the disk server (nvme.c).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

/* Controller nth (0, 1...), namespace ns (1...): -> 0 ready (*sectors of
 * *bs bytes), 1 no such namespace (try the next), -1 no more on this
 * controller (try the next), -2 no such controller. */
int  nvme_open(int nth, uint32_t ns, uint64_t *sectors, uint32_t *bs);
long nvme_io(int op, uint64_t lba, uint32_t count);     /* op 2 read, 1 write, 0 flush -> 0 or -EIO */
void *nvme_buffer(void);                                /* the DMA buffer (DISK_MAX bytes) nvme_io moves */
