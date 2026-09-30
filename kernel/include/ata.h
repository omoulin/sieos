/*
 * ata.h
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_ATA_H
#define SIEOS_ATA_H

#include "kernel.h"

#define SECTOR_SIZE 512
#define ATA_UNITS   4              /* primary master, slave, secondary master, slave */

/* (the driver: drv/ata, registered with blk_register_at at the units' numbers) */

#endif
