/*
 * sd.h - The SD card backend of the disk server (sd.c, arm64).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

int  sd_setup(uint64_t *sectors);                               /* 0: a card, ready */
long sd_io(int write, uint64_t sector, uint32_t count, void *buf);   /* 0 or -EIO */
void *sd_buffer(void);                                          /* its DMA buffer (DISK_MAX bytes): use it as the
                                                                   request buffer, and sd_io copies nothing */
