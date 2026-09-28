/*
 * sieos/stat.h - struct stat, directory entries and statvfs (ABI v2).
 * Layouts follow Solaris amd64 (including st_fstype and the d_type-less
 * struct dirent).
 */
#ifndef SIEOS_ABI_STAT_H
#define SIEOS_ABI_STAT_H

#include "types.h"

/* st_mode file types and permission bits */
#define SIEOS_S_IFMT   0xF000
#define SIEOS_S_IFIFO  0x1000
#define SIEOS_S_IFCHR  0x2000
#define SIEOS_S_IFDIR  0x4000
#define SIEOS_S_IFNAM  0x5000          /* XENIX special named file */
#define SIEOS_S_IFBLK  0x6000
#define SIEOS_S_IFREG  0x8000
#define SIEOS_S_IFLNK  0xA000
#define SIEOS_S_IFSOCK 0xC000
#define SIEOS_S_IFDOOR 0xD000
#define SIEOS_S_IFPORT 0xE000
#define SIEOS_S_ISUID  0004000
#define SIEOS_S_ISGID  0002000
#define SIEOS_S_ISVTX  0001000
#define SIEOS_S_IRWXU  0000700
#define SIEOS_S_IRWXG  0000070
#define SIEOS_S_IRWXO  0000007

#define SIEOS_ST_FSTYPSZ 16

struct sieos_stat {
    sieos_dev_t     st_dev;
    sieos_ino_t     st_ino;
    sieos_mode_t    st_mode;
    sieos_nlink_t   st_nlink;
    sieos_uid_t     st_uid;
    sieos_gid_t     st_gid;
    sieos_dev_t     st_rdev;
    sieos_off_t     st_size;
    struct sieos_timespec st_atim;
    struct sieos_timespec st_mtim;
    struct sieos_timespec st_ctim;
    sieos_blksize_t st_blksize;
    int             __pad;
    sieos_blkcnt_t  st_blocks;         /* 512-byte units */
    char            st_fstype[SIEOS_ST_FSTYPSZ];
};

/* getdents(): variable-length records, 8-byte aligned. */
struct sieos_dirent {
    sieos_ino_t d_ino;
    sieos_off_t d_off;                 /* opaque cookie of the next entry */
    unsigned short d_reclen;
    char d_name[1];                 /* NUL-terminated */
};
#define SIEOS_DIRENT_NAME_OFFSET 18

#define SIEOS_FSTYPSZ 16
struct sieos_statvfs {
    unsigned long    f_bsize;
    unsigned long    f_frsize;
    sieos_fsblkcnt_t f_blocks;
    sieos_fsblkcnt_t f_bfree;
    sieos_fsblkcnt_t f_bavail;
    sieos_fsfilcnt_t f_files;
    sieos_fsfilcnt_t f_ffree;
    sieos_fsfilcnt_t f_favail;
    unsigned long    f_fsid;
    char f_basetype[SIEOS_FSTYPSZ];
    unsigned long f_flag;           /* SIEOS_ST_RDONLY, SIEOS_ST_NOSUID */
    unsigned long f_namemax;
    char f_fstr[32];
};
#define SIEOS_ST_RDONLY 0x01
#define SIEOS_ST_NOSUID 0x02

SIEOS_STATIC_ASSERT(sizeof(struct sieos_stat) == 128, "stat size");
SIEOS_STATIC_ASSERT(SIEOS_OFFSETOF(struct sieos_stat, st_size) == 40, "st_size offset");
SIEOS_STATIC_ASSERT(SIEOS_OFFSETOF(struct sieos_dirent, d_name) == SIEOS_DIRENT_NAME_OFFSET, "d_name offset");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_statvfs) == 136, "statvfs size");

#endif
