#ifndef SIEOS_ATA_H
#define SIEOS_ATA_H

#include "kernel.h"

#define SECTOR_SIZE 512
#define ATA_UNITS   4              /* primary master, slave, secondary master, slave */

void ata_init(void);
bool ata_present(int unit);
uint64_t ata_sectors(int unit);
int  ata_read(int unit, uint64_t lba, size_t count, void *buf);
int  ata_write(int unit, uint64_t lba, size_t count, const void *buf);

#endif
