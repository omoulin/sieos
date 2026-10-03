/* linux/magic.h - the file system magic numbers liblxcompat's statfs gives. */
/*
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef _LINUX_MAGIC_H
#define _LINUX_MAGIC_H
#define EXT2_SUPER_MAGIC   0xEF53
#define EXT3_SUPER_MAGIC   0xEF53
#define EXT4_SUPER_MAGIC   0xEF53
#define TMPFS_MAGIC        0x01021994
#define PROC_SUPER_MAGIC   0x9fa0
#define DEVPTS_SUPER_MAGIC 0x1cd1
#define MSDOS_SUPER_MAGIC  0x4d44
#define ISOFS_SUPER_MAGIC  0x9660
#define NFS_SUPER_MAGIC    0x6969
#define SMB_SUPER_MAGIC    0x517B
#define CIFS_SUPER_MAGIC   0xFF534D42
#define USBDEVICE_SUPER_MAGIC 0x9fa2
#define RAMFS_MAGIC        0x858458f6
#define SYSFS_MAGIC        0x62656572
#define CGROUP_SUPER_MAGIC 0x27e0eb
#define BTRFS_SUPER_MAGIC  0x9123683E
#endif
