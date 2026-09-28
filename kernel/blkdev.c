/*
 * blkdev.c - Block devices: the ATA disks (devices 0-3, /dev/dsk/c<bus>d<unit>p0),
 * the RAM disk GRUB loads from the ISO (device 4), and lofi devices, files
 * seen as disks (devices 8-15, /dev/lofi/1-8, set up with lofiadm).
 *
 * The root file system is on the first ATA disk that holds ext4 (changes
 * persist); otherwise on the RAM disk.
 */
#include "blkdev.h"
#include "ata.h"
#include "ext4.h"
#include "fs.h"

enum { BK_NONE, BK_ATA, BK_RAM, BK_LOFI };

struct blkdev {
    int kind;
    uint64_t sectors;
    bool ro;
    int users;                        /* mounts on it */
    uint8_t *ram;                     /* BK_RAM */
    struct inode *ip;                 /* BK_LOFI: the backing file (referenced) */
    char file[128];                   /* BK_LOFI: its path, for lofiadm */
};

static struct blkdev devs[NBLKDEV];
static int root_dev = -1;
static uint8_t *ramdisk;
static uint64_t ramdisk_sectors;

void blk_set_ramdisk(uint64_t phys, uint64_t size)
{
    ramdisk = P2V(phys);
    ramdisk_sectors = size / SECTOR_SIZE;
}

static bool valid(int dev)
{
    return dev >= 0 && dev < NBLKDEV && devs[dev].kind != BK_NONE;
}

static bool has_ext4(int dev)
{
    uint8_t sb[1024];
    if (blk_read(dev, 2, 2, sb) < 0)
        return false;
    return ((struct ext4_super *)sb)->s_magic == EXT4_SUPER_MAGIC;
}

const char *blk_init(void)
{
    for (int u = 0; u < ATA_UNITS; u++)
        if (ata_present(u)) {
            devs[u].kind = BK_ATA;
            devs[u].sectors = ata_sectors(u);
        }
    if (ramdisk) {
        devs[BLK_RAMDISK].kind = BK_RAM;
        devs[BLK_RAMDISK].sectors = ramdisk_sectors;
        devs[BLK_RAMDISK].ram = ramdisk;
    }
    for (int u = 0; u < ATA_UNITS; u++)
        if (devs[u].kind == BK_ATA && has_ext4(u)) {
            root_dev = u;
            return "ATA disk";
        }
    if (ramdisk) {
        root_dev = BLK_RAMDISK;
        return "RAM disk (from ISO, changes are not saved)";
    }
    return NULL;
}

int blk_root(void)
{
    return root_dev;
}

bool blk_is_ramdisk(void)
{
    return root_dev == BLK_RAMDISK;
}

bool blk_present(int dev)
{
    return valid(dev);
}

uint64_t blk_sectors(int dev)
{
    return valid(dev) ? devs[dev].sectors : 0;
}

bool blk_readonly(int dev)
{
    return valid(dev) && devs[dev].ro;
}

/* A file system is mounted on dev (delta +1), or no longer (-1). */
void blk_use(int dev, int delta)
{
    if (valid(dev))
        devs[dev].users += delta;
}

bool blk_in_use(int dev)
{
    return valid(dev) && (devs[dev].users || dev == root_dev);
}

static int lofi_rw(struct blkdev *d, uint64_t lba, size_t count, void *buf, bool write)
{
    uint64_t off = lba * SECTOR_SIZE, n = (uint64_t)count * SECTOR_SIZE;
    long r = write ? writei(d->ip, buf, off, n) : readi(d->ip, buf, off, n);
    if (r < 0)
        return r;
    if ((uint64_t)r < n) {
        if (write)
            return -EIO;
        memset((uint8_t *)buf + r, 0, n - r);    /* past the end of the file: zeros */
    }
    return 0;
}

int blk_read(int dev, uint64_t lba, size_t count, void *buf)
{
    if (!valid(dev) || lba + count > devs[dev].sectors)
        return -EIO;
    struct blkdev *d = &devs[dev];
    switch (d->kind) {
    case BK_ATA:
        return ata_read(dev, lba, count, buf);
    case BK_RAM:
        memcpy(buf, d->ram + lba * SECTOR_SIZE, count * SECTOR_SIZE);
        return 0;
    case BK_LOFI:
        return lofi_rw(d, lba, count, buf, false);
    }
    return -EIO;
}

int blk_write(int dev, uint64_t lba, size_t count, const void *buf)
{
    if (!valid(dev) || lba + count > devs[dev].sectors)
        return -EIO;
    struct blkdev *d = &devs[dev];
    if (d->ro)
        return -EROFS;
    switch (d->kind) {
    case BK_ATA:
        return ata_write(dev, lba, count, buf);
    case BK_RAM:
        memcpy(d->ram + lba * SECTOR_SIZE, buf, count * SECTOR_SIZE);
        return 0;
    case BK_LOFI:
        return lofi_rw(d, lba, count, (void *)buf, true);
    }
    return -EIO;
}

/* ---------------- lofi ---------------- */

int blk_lofi_attach(struct inode *ip, const char *path, bool ro)
{
    for (int dev = BLK_LOFI0; dev < BLK_LOFI0 + NLOFI; dev++)
        if (valid(dev) && same_inode(devs[dev].ip, ip))
            return -EBUSY;                           /* the file is attached already */
    for (int dev = BLK_LOFI0; dev < BLK_LOFI0 + NLOFI; dev++) {
        struct blkdev *d = &devs[dev];
        if (d->kind != BK_NONE)
            continue;
        memset(d, 0, sizeof(*d));
        d->kind = BK_LOFI;
        d->ip = idup(ip);
        d->ro = ro;
        d->sectors = inode_size(ip) / SECTOR_SIZE;
        strlcpy(d->file, path, sizeof(d->file));
        return dev;
    }
    return -EBUSY;                                   /* all lofi devices in use */
}

int blk_lofi_detach(int dev)
{
    if (dev < BLK_LOFI0 || dev >= BLK_LOFI0 + NLOFI || !valid(dev))
        return -ENXIO;
    if (devs[dev].users)
        return -EBUSY;
    iput(devs[dev].ip);
    memset(&devs[dev], 0, sizeof(devs[dev]));
    return 0;
}

int blk_lofi_file(int dev, char *buf, size_t size)
{
    if (dev < BLK_LOFI0 || dev >= BLK_LOFI0 + NLOFI || !valid(dev))
        return -ENXIO;
    strlcpy(buf, devs[dev].file, size);
    return 0;
}
