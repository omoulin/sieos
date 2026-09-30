/*
 * blkdev.h
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_BLKDEV_H
#define SIEOS_BLKDEV_H

#include "kernel.h"

/* Device numbers: ATA units 0-3, the RAM disk, lofi devices, then the other
 * disks (NVMe, ...) and the partitions of every disk (see blkdev.c). */
#define NBLKDEV     64
#define BLK_RAMDISK 4
#define BLK_LOFI0   8
#define NLOFI       8
#define BLK_DISK0   16

/* A disk driver (NVMe, ...): 512-byte sectors, whatever its own block size. */
struct blk_ops {
    int (*read)(void *drv, uint64_t lba, size_t count, void *buf);
    int (*write)(void *drv, uint64_t lba, size_t count, const void *buf);
};
/* A disk found by its driver at boot (before blk_init): its device number, or -1. */
int  blk_register(const char *name, uint64_t sectors, const struct blk_ops *ops, void *drv);
int  blk_register_at(int dev, const char *name, uint64_t sectors, const struct blk_ops *ops, void *drv);
/* Its name under /dev/dsk ("c0d0p0", "c4t0d0s1"); NULL if none. */
const char *blk_name(int dev);
/* A description: "NVMe KXG60ZNV256G, 238 GiB". */
const char *blk_desc(int dev);
void blk_set_desc(int dev, const char *desc);
void blk_set_lbsize(int dev, uint32_t bytes);   /* the disk's own block size (default 512) */
/* A block device opened as a file (root): bytes at off; writes refused while in use. */
long blk_file_io(int dev, uint64_t off, void *buf, size_t n, bool write);
int  blk_file_open(int dev, bool write);
struct sieos_dk_info;
int  blk_info(int dev, struct sieos_dk_info *di);
int  blk_reread(int dev);             /* a whole disk's partitions again (none may be in use) */

struct inode;

void blk_set_ramdisk(uint64_t phys, uint64_t size);
const char *blk_init(const char *cmdline);   /* partitions, the root device (root=NAME); a description, NULL if none */
int  blk_root(void);                 /* the root device, -1 */
bool blk_is_ramdisk(void);           /* the root is the RAM disk */
bool blk_present(int dev);
uint64_t blk_sectors(int dev);
bool blk_readonly(int dev);
int  blk_read(int dev, uint64_t lba, size_t count, void *buf);
int  blk_write(int dev, uint64_t lba, size_t count, const void *buf);
void blk_use(int dev, int delta);    /* mounts on dev */
bool blk_in_use(int dev);
int  blk_lofi_attach(struct inode *ip, const char *path, bool ro);   /* the device, or -errno */
int  blk_lofi_detach(int dev);
int  blk_lofi_file(int dev, char *buf, size_t size);

#endif
