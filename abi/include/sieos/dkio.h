/*
 * sieos/dkio.h - disks (ABI v2), after Solaris dkio(4I).
 *
 * The block devices under /dev/dsk (major 8) can be opened by root and read
 * and written at byte offsets (lseek, read, write, pread, pwrite).  Writing
 * is refused (EBUSY) while the device, a partition of it, or the disk it is
 * a partition of is mounted or holds the root file system.
 */
#ifndef SIEOS_ABI_DKIO_H
#define SIEOS_ABI_DKIO_H

#define SIEOS_DKIOC_BASE     (('D' << 16) | ('K' << 8))
#define SIEOS_DKIOCINFO      (SIEOS_DKIOC_BASE | 0x01)   /* struct sieos_dk_info */
#define SIEOS_DKIOCREREAD    (SIEOS_DKIOC_BASE | 0x02)   /* a whole disk: read its partition table again */

#define SIEOS_DK_DISK      0x01          /* a whole disk (not a partition) */
#define SIEOS_DK_PART      0x02          /* a partition of the disk named in dki_parent */
#define SIEOS_DK_INUSE     0x04          /* mounted, or holds the root (itself, a partition or its disk) */
#define SIEOS_DK_ROOT      0x08          /* the root file system is on it (or on a partition of it) */
#define SIEOS_DK_RDONLY    0x10
#define SIEOS_DK_VIRTUAL   0x20          /* the RAM disk or a lofi device: not a real disk */

struct sieos_dk_info {
    unsigned long long dki_sectors;      /* 512-byte sectors */
    unsigned int dki_flags;              /* SIEOS_DK_* */
    unsigned int dki_minor;
    unsigned int dki_lbsize;             /* the disk's logical block size (512, 4096): its GPT's unit */
    char dki_name[24];                   /* under /dev/dsk: "c4t0d0p0" */
    char dki_parent[24];                 /* a partition's disk, else "" */
    char dki_desc[64];                   /* "NVMe KXG60ZNV256G, 238 GiB", "QEMU HARDDISK" */
};

#endif
