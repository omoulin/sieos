#ifndef SIEOS_BLKDEV_H
#define SIEOS_BLKDEV_H

#include "kernel.h"

/* Device numbers: ATA units 0-3, the RAM disk, lofi devices (see blkdev.c). */
#define NBLKDEV     16
#define BLK_RAMDISK 4
#define BLK_LOFI0   8
#define NLOFI       8

struct inode;

void blk_set_ramdisk(uint64_t phys, uint64_t size);
const char *blk_init(void);          /* picks the root device; returns a description, NULL if none */
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
