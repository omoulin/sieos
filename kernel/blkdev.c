/*
 * blkdev.c - Block devices: the ATA disks (devices 0-3, /dev/dsk/c<bus>d<unit>p0),
 * the RAM disk GRUB loads from the ISO (device 4), lofi devices, files
 * seen as disks (devices 8-15, /dev/lofi/1-8, set up with lofiadm), and
 * from device 16 the disks of the other drivers (NVMe: /dev/dsk/c<4+n>t0d<ns>p0)
 * and the partitions of every disk: GPT partitions as slices (c4t0d0s0 is
 * the first), MBR primary partitions as p1-p4 (as on Solaris for x86).
 *
 * The root file system is on the device named by root= on the boot command
 * line ("root=c4t0d0s2", ext4); else on the first ATA disk that holds ext4
 * (changes persist); otherwise on the RAM disk.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "blkdev.h"
#include "ata.h"                                 /* (SECTOR_SIZE, ATA_UNITS) */
#include "ext4.h"
#include "fs.h"
#include "bcache.h"
#include "mm.h"
#include "sieos/dkio.h"

enum { BK_NONE, BK_RAM, BK_LOFI, BK_DRV, BK_PART };

struct blkdev {
    int kind;
    uint64_t sectors;
    bool ro;
    int users;                        /* mounts on it */
    uint8_t *ram;                     /* BK_RAM */
    struct inode *ip;                 /* BK_LOFI: the backing file (referenced) */
    char file[128];                   /* BK_LOFI: its path, for lofiadm */
    const struct blk_ops *ops;        /* BK_DRV */
    void *drv;
    int parent;                       /* BK_PART: the disk, and where on it */
    uint64_t start;
    uint32_t lbsize;                  /* the disk's block size (0: 512) */
    char name[24];                    /* under /dev/dsk */
    char desc[64];
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

static int next_disk = BLK_DISK0;           /* (above every device in use) */

/* A free device number from BLK_DISK0; -1 if none. */
static int slot_alloc(void)
{
    for (int dev = BLK_DISK0; dev < NBLKDEV; dev++)
        if (devs[dev].kind == BK_NONE) {
            if (dev >= next_disk)
                next_disk = dev + 1;
            return dev;
        }
    return -1;
}

int blk_register(const char *name, uint64_t sectors, const struct blk_ops *ops, void *drv)
{
    return blk_register_at(slot_alloc(), name, sectors, ops, drv);
}

/* At a given device number (the ATA units' 0-3: c0d0p0 ... c1d1p0), or -1. */
int blk_register_at(int dev, const char *name, uint64_t sectors, const struct blk_ops *ops, void *drv)
{
    if (dev < 0 || dev >= NBLKDEV || devs[dev].kind != BK_NONE)
        return -1;
    struct blkdev *d = &devs[dev];
    memset(d, 0, sizeof(*d));
    d->kind = BK_DRV;
    d->sectors = sectors;
    d->ops = ops;
    d->drv = drv;
    strlcpy(d->name, name, sizeof(d->name));
    return dev;
}

const char *blk_name(int dev)
{
    return valid(dev) && devs[dev].name[0] ? devs[dev].name : NULL;
}

const char *blk_desc(int dev)
{
    return valid(dev) ? devs[dev].desc : "";
}

void blk_set_desc(int dev, const char *desc)
{
    if (valid(dev))
        strlcpy(devs[dev].desc, desc, sizeof(devs[dev].desc));
}

void blk_set_lbsize(int dev, uint32_t bytes)
{
    if (valid(dev))
        devs[dev].lbsize = bytes;
}

static void add_part(int disk, const char *suffix, int n, uint64_t start, uint64_t count)
{
    struct blkdev *p = &devs[disk];
    if (!count || start + count > p->sectors)
        return;
    int dev = slot_alloc();
    if (dev < 0)
        return;
    struct blkdev *d = &devs[dev];
    memset(d, 0, sizeof(*d));
    d->kind = BK_PART;
    d->parent = disk;
    d->start = start;
    d->sectors = count;
    d->ro = p->ro;
    size_t len = strlen(p->name);                    /* "c4t0d0p0" -> "c4t0d0s1" */
    char base[24];
    strlcpy(base, p->name, sizeof(base));
    if (len >= 2 && base[len - 2] == 'p' && base[len - 1] == '0')
        base[len - 2] = 0;
    snprintf(d->name, sizeof(d->name), "%s%s%d", base, suffix, n);
    snprintf(d->desc, sizeof(d->desc), "partition %d of %s", n + (suffix[0] == 's'), p->name);
}

/*
 * The partitions of a disk: a GPT (its header in sector 1, or at 4096 bytes
 * for a 4K-sector disk), else the four MBR primary partitions.
 */
static void scan_partitions(int disk)
{
    static uint8_t sec[4096];
    if (blk_read(disk, 0, 1, sec) < 0 || sec[510] != 0x55 || sec[511] != 0xAA)
        return;
    struct { uint8_t type; uint32_t start, count; } mbr[4];
    bool gpt = false;
    for (int i = 0; i < 4; i++) {
        uint8_t *e = sec + 0x1BE + 16 * i;
        mbr[i].type = e[4];
        mbr[i].start = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
        mbr[i].count = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24;
        gpt |= mbr[i].type == 0xEE;
    }
    if (gpt) {
        for (int unit = 1; unit <= 8; unit *= 8) {   /* 512-byte, then 4K logical blocks */
            if (blk_read(disk, unit, 1, sec) < 0 || memcmp(sec, "EFI PART", 8))
                continue;
            uint64_t table = *(uint64_t *)(sec + 72);
            uint32_t n = *(uint32_t *)(sec + 80), esize = *(uint32_t *)(sec + 84);
            if (esize < 128 || esize > 512 || n > 128)
                return;
            int slice = 0;
            for (uint32_t i = 0; i < n && slice < 16; i++) {
                uint64_t off = i * esize;
                if (blk_read(disk, table * unit + off / SECTOR_SIZE, 1, sec) < 0)
                    return;
                uint8_t *e = sec + off % SECTOR_SIZE;
                static const uint8_t zero[16];
                if (!memcmp(e, zero, 16))
                    continue;                        /* unused entry */
                uint64_t first = *(uint64_t *)(e + 32), last = *(uint64_t *)(e + 40);
                if (last >= first)
                    add_part(disk, "s", slice, first * unit, (last - first + 1) * unit);
                slice++;
            }
            return;
        }
        return;
    }
    for (int i = 0; i < 4; i++)
        if (mbr[i].type && mbr[i].count)
            add_part(disk, "p", i + 1, mbr[i].start, mbr[i].count);
}

static bool has_ext4(int dev)
{
    uint8_t sb[1024];
    if (blk_read(dev, 2, 2, sb) < 0)
        return false;
    return ((struct ext4_super *)sb)->s_magic == EXT4_SUPER_MAGIC;
}

const char *blk_init(const char *cmdline)
{
    if (ramdisk) {
        devs[BLK_RAMDISK].kind = BK_RAM;
        devs[BLK_RAMDISK].sectors = ramdisk_sectors;
        devs[BLK_RAMDISK].ram = ramdisk;
    }
    int ndisks = next_disk;                          /* (the partitions go after the disks) */
    for (int u = 0; u < ATA_UNITS; u++)
        if (devs[u].kind == BK_DRV)                  /* (the ATA units, drv/ata's) */
            scan_partitions(u);
    for (int dev = BLK_DISK0; dev < ndisks; dev++)
        scan_partitions(dev);
    for (int dev = 0; dev < next_disk; dev++)
        if (valid(dev) && devs[dev].kind == BK_PART)
            kprintf("disk: %s: partition of %s, %lu MiB\n", devs[dev].name, devs[devs[dev].parent].name,
                    devs[dev].sectors / 2048);

    const char *want = NULL;                         /* root=NAME (or root=/dev/dsk/NAME) */
    for (const char *p = cmdline; p && (p = strchr(p, 'r')); p++)
        if (!strncmp(p, "root=", 5) && (p == cmdline || p[-1] == ' ')) {
            want = p + 5;
            if (!strncmp(want, "/dev/dsk/", 9))
                want += 9;
            break;
        }
    if (want) {
        size_t n = strchr(want, ' ') ? (size_t)(strchr(want, ' ') - want) : strlen(want);
        for (int dev = 0; dev < next_disk; dev++)
            if (valid(dev) && devs[dev].name[0] && strlen(devs[dev].name) == n && !strncmp(devs[dev].name, want, n)) {
                if (!has_ext4(dev)) {
                    kprintf("disk: root=%s: no ext4 file system there\n", devs[dev].name);
                    break;
                }
                root_dev = dev;
                static char what[48];
                snprintf(what, sizeof(what), "%s (root=)", devs[dev].name);
                return what;
            }
        if (root_dev < 0)
            kprintf("disk: root=%.*s: no such disk or partition\n", (int)n, want);
    }
    for (int u = 0; u < ATA_UNITS; u++)
        if (devs[u].kind == BK_DRV && has_ext4(u)) {
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
    case BK_RAM:
        memcpy(buf, d->ram + lba * SECTOR_SIZE, count * SECTOR_SIZE);
        return 0;
    case BK_LOFI:
        return lofi_rw(d, lba, count, buf, false);
    case BK_DRV:
        return d->ops->read(d->drv, lba, count, buf);
    case BK_PART:
        return blk_read(d->parent, d->start + lba, count, buf);
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
    case BK_RAM:
        memcpy(d->ram + lba * SECTOR_SIZE, buf, count * SECTOR_SIZE);
        return 0;
    case BK_LOFI:
        return lofi_rw(d, lba, count, (void *)buf, true);
    case BK_DRV:
        return d->ops->write ? d->ops->write(d->drv, lba, count, buf) : -EROFS;
    case BK_PART:
        return blk_write(d->parent, d->start + lba, count, buf);
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

/* ---------------- as files (root: the installer, mkfs) ---------------- */

/* dev, its disk, or (for a whole disk) one of its partitions is mounted or holds the root. */
static bool busy_tree(int dev)
{
    int disk = devs[dev].kind == BK_PART ? devs[dev].parent : dev;
    if (blk_in_use(dev) || blk_in_use(disk))
        return true;
    if (devs[dev].kind != BK_PART)
        for (int i = 0; i < NBLKDEV; i++)
            if (devs[i].kind == BK_PART && devs[i].parent == dev && blk_in_use(i))
                return true;
    return false;
}

/* Forget the cached blocks of dev's disk and of all its partitions (raw writes bypass the cache). */
static void forget_tree(int dev)
{
    int disk = devs[dev].kind == BK_PART ? devs[dev].parent : dev;
    bcache_forget(disk);
    for (int i = 0; i < NBLKDEV; i++)
        if (devs[i].kind == BK_PART && devs[i].parent == disk)
            bcache_forget(i);
}

int blk_file_open(int dev, bool write)
{
    if (!valid(dev))
        return -ENXIO;
    if (write) {
        if (devs[dev].ro)
            return -EROFS;
        if (busy_tree(dev))
            return -EBUSY;
        forget_tree(dev);
    }
    return 0;
}

#define FILE_BOUNCE (64 * 1024)

long blk_file_io(int dev, uint64_t off, void *buf, size_t n, bool write)
{
    if (!valid(dev))
        return -ENXIO;
    uint64_t size = devs[dev].sectors * SECTOR_SIZE;
    if (off >= size)
        return write && n ? -ENOSPC : 0;
    if (off + n > size)
        n = size - off;
    if (write && busy_tree(dev))
        return -EBUSY;
    uint8_t *bounce = kmalloc(FILE_BOUNCE);
    if (!bounce)
        return -ENOMEM;
    size_t done = 0;
    int r = 0;
    while (done < n) {
        uint64_t pos = off + done, sec = pos / SECTOR_SIZE;
        size_t skip = pos % SECTOR_SIZE, chunk = MIN(n - done, FILE_BOUNCE - skip);
        size_t nsec = (skip + chunk + SECTOR_SIZE - 1) / SECTOR_SIZE;
        if (write) {
            bool partial = skip || (skip + chunk) % SECTOR_SIZE;
            if (partial && (r = blk_read(dev, sec, nsec, bounce)) < 0)     /* (the edges: read, patch, write) */
                break;
            memcpy(bounce + skip, (const uint8_t *)buf + done, chunk);
            if ((r = blk_write(dev, sec, nsec, bounce)) < 0)
                break;
        } else {
            if ((r = blk_read(dev, sec, nsec, bounce)) < 0)
                break;
            memcpy((uint8_t *)buf + done, bounce + skip, chunk);
        }
        done += chunk;
    }
    kfree(bounce);
    return done ? (long)done : r;
}

int blk_info(int dev, struct sieos_dk_info *di)
{
    if (!valid(dev))
        return -ENXIO;
    struct blkdev *d = &devs[dev];
    memset(di, 0, sizeof(*di));
    di->dki_sectors = d->sectors;
    di->dki_minor = dev;
    di->dki_lbsize = d->lbsize ? d->lbsize : d->kind == BK_PART && devs[d->parent].lbsize ? devs[d->parent].lbsize : 512;
    strlcpy(di->dki_name, d->name[0] ? d->name : dev == BLK_RAMDISK ? "ramdisk" : "lofi", sizeof(di->dki_name));
    strlcpy(di->dki_desc, d->desc[0] ? d->desc : dev == BLK_RAMDISK ? "RAM disk (from the boot medium)"
                                                 : d->kind == BK_LOFI ? d->file : "", sizeof(di->dki_desc));
    di->dki_flags = d->kind == BK_PART ? SIEOS_DK_PART : SIEOS_DK_DISK;
    if (d->kind == BK_PART)
        strlcpy(di->dki_parent, devs[d->parent].name, sizeof(di->dki_parent));
    if (busy_tree(dev))
        di->dki_flags |= SIEOS_DK_INUSE;
    int disk = d->kind == BK_PART ? d->parent : dev;
    if (root_dev == dev || root_dev == disk || (root_dev >= 0 && devs[root_dev].kind == BK_PART &&
                                                (devs[root_dev].parent == dev || devs[root_dev].parent == disk)))
        di->dki_flags |= SIEOS_DK_ROOT;
    if (d->ro)
        di->dki_flags |= SIEOS_DK_RDONLY;
    if (d->kind == BK_RAM || d->kind == BK_LOFI)
        di->dki_flags |= SIEOS_DK_VIRTUAL;
    return 0;
}

int blk_reread(int dev)
{
    if (!valid(dev) || devs[dev].kind != BK_DRV)
        return -EINVAL;
    if (busy_tree(dev))
        return -EBUSY;
    forget_tree(dev);
    for (int i = 0; i < NBLKDEV; i++)
        if (devs[i].kind == BK_PART && devs[i].parent == dev)
            memset(&devs[i], 0, sizeof(devs[i]));
    scan_partitions(dev);
    for (int i = 0; i < NBLKDEV; i++)
        if (devs[i].kind == BK_PART && devs[i].parent == dev)
            kprintf("disk: %s: partition of %s, %lu MiB\n", devs[i].name, devs[dev].name, devs[i].sectors / 2048);
    vfs_blk_nodes();
    return 0;
}
