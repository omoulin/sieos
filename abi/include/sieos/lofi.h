/*
 * sieos/lofi.h - loopback file devices (ABI v2), as Solaris lofi(4D).
 *
 * A regular file is attached to /dev/lofi/N through ioctls on /dev/lofictl
 * (root only); the block device can then be mounted (mount -F ext4).
 */
#ifndef SIEOS_ABI_LOFI_H
#define SIEOS_ABI_LOFI_H

#define SIEOS_LOFI_IOC_BASE          (('L' << 16) | ('F' << 8))
#define SIEOS_LOFI_MAP_FILE          (SIEOS_LOFI_IOC_BASE | 0x01)   /* attach li_filename; sets li_minor */
#define SIEOS_LOFI_UNMAP_FILE_MINOR  (SIEOS_LOFI_IOC_BASE | 0x04)   /* detach li_minor */
#define SIEOS_LOFI_GET_FILENAME      (SIEOS_LOFI_IOC_BASE | 0x05)   /* li_filename of li_minor */
#define SIEOS_LOFI_MAX_FILES         8
#define SIEOS_LOFI_PATH_MAX          1024

struct sieos_lofi_ioctl {
    char li_filename[SIEOS_LOFI_PATH_MAX];
    unsigned int li_minor;           /* 1 .. SIEOS_LOFI_MAX_FILES: /dev/lofi/<minor> */
    int li_readonly;
};

/* Block device numbers: major 8; minor 0-3 the ATA disks (/dev/dsk/c<bus>d<unit>p0),
 * 4 the RAM disk, 8 + (n - 1) /dev/lofi/n.  /dev/lofictl is character 147,0. */
#define SIEOS_DEV_BLK_MAJOR   8
#define SIEOS_DEV_LOFI_MAJOR  147

#endif
