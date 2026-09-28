/*
 * sieos/fcntl.h - open() flags, fcntl() commands, record locks, *at()
 * constants (ABI v2).  Values follow Solaris.
 */
#ifndef SIEOS_ABI_FCNTL_H
#define SIEOS_ABI_FCNTL_H

#include "types.h"

/* open() flags */
#define SIEOS_O_RDONLY    0x00000000
#define SIEOS_O_WRONLY    0x00000001
#define SIEOS_O_RDWR      0x00000002
#define SIEOS_O_SEARCH    0x00200000
#define SIEOS_O_EXEC      0x00400000
#define SIEOS_O_ACCMODE   (SIEOS_O_RDONLY | SIEOS_O_WRONLY | SIEOS_O_RDWR | SIEOS_O_SEARCH | SIEOS_O_EXEC)
#define SIEOS_O_NDELAY    0x00000004
#define SIEOS_O_APPEND    0x00000008
#define SIEOS_O_SYNC      0x00000010
#define SIEOS_O_DSYNC     0x00000040
#define SIEOS_O_NONBLOCK  0x00000080
#define SIEOS_O_CREAT     0x00000100
#define SIEOS_O_TRUNC     0x00000200
#define SIEOS_O_EXCL      0x00000400
#define SIEOS_O_NOCTTY    0x00000800
#define SIEOS_O_LARGEFILE 0x00002000   /* accepted and ignored: offsets are always 64-bit */
#define SIEOS_O_XATTR     0x00004000
#define SIEOS_O_RSYNC     0x00008000
#define SIEOS_O_NOFOLLOW  0x00020000
#define SIEOS_O_NOLINKS   0x00040000
#define SIEOS_O_CLOEXEC   0x00800000
#define SIEOS_O_DIRECTORY 0x01000000

/* fcntl() commands */
#define SIEOS_F_DUPFD          0
#define SIEOS_F_GETFD          1
#define SIEOS_F_SETFD          2
#define SIEOS_F_GETFL          3
#define SIEOS_F_SETFL          4
#define SIEOS_F_SETLK          6
#define SIEOS_F_SETLKW         7
#define SIEOS_F_DUP2FD         9
#define SIEOS_F_FREESP        11   /* free file space: struct sieos_flock describes the range */
#define SIEOS_F_GETLK         14
#define SIEOS_F_GETOWN        23
#define SIEOS_F_SETOWN        24
#define SIEOS_F_DUP2FD_CLOEXEC 36
#define SIEOS_F_DUPFD_CLOEXEC 37

#define SIEOS_FD_CLOEXEC 1

/* struct flock: l_type */
#define SIEOS_F_RDLCK 1
#define SIEOS_F_WRLCK 2
#define SIEOS_F_UNLCK 3

struct sieos_flock {
    short       l_type;
    short       l_whence;
    int         __pad;
    sieos_off_t l_start;
    sieos_off_t l_len;                 /* 0 = to end of file */
    int         l_sysid;
    sieos_pid_t l_pid;
    long        l_pad[4];
};

/* lseek() whence */
#define SIEOS_SEEK_SET  0
#define SIEOS_SEEK_CUR  1
#define SIEOS_SEEK_END  2
#define SIEOS_SEEK_DATA 3
#define SIEOS_SEEK_HOLE 4

/* *at() */
#define SIEOS_AT_FDCWD            (-3041965)          /* 0xffd19553 as an int, usable in #if */
#define SIEOS_AT_SYMLINK_NOFOLLOW 0x1000
#define SIEOS_AT_SYMLINK_FOLLOW   0x2000
#define SIEOS_AT_REMOVEDIR        0x0001
#define SIEOS_AT_EACCESS          0x0004

/* access modes */
#define SIEOS_F_OK 0
#define SIEOS_X_OK 1
#define SIEOS_W_OK 2
#define SIEOS_R_OK 4

/* utimensat() special tv_nsec values */
#define SIEOS_UTIME_NOW  (-1L)
#define SIEOS_UTIME_OMIT (-2L)

SIEOS_STATIC_ASSERT(sizeof(struct sieos_flock) == 64, "flock size");

#endif
