/*
 * nvgpu.h - between the driver's parts (nvgpu.c: the GPU and GSP-RM;
 * dev.c: /dev/nvgpu0).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef NVGPU_H
#define NVGPU_H

#include "sieos/nvgpu.h"

const struct sieos_nvgpu_info *nvgpu_info(void);   /* NULL until the GPU is up */
uint64_t nvgpu_timestamp(void);                     /* the GPU's clock (PTIMER), ns */
void nvgpu_dev_init(void);                          /* /dev/nvgpu0 */

#endif
