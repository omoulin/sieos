/*
 * sieos/types.h - Fundamental types of the SIEOS ABI v2 (amd64, LP64).
 *
 * Solaris-inspired: 64-bit dev_t with a 32-bit major in the high half,
 * 32-bit uid/gid/pid, 64-bit off_t/ino_t (no separate "64" interfaces).
 *
 * All ABI v2 headers are self-contained (no libc headers) and use the
 * sieos_/SIEOS_ prefix so the kernel can include them next to its ABI v1
 * definitions; tools/abi2musl strips the prefix for the musl port.
 */
#ifndef SIEOS_ABI_TYPES_H
#define SIEOS_ABI_TYPES_H

typedef signed char        sieos_int8_t;
typedef unsigned char      sieos_uint8_t;
typedef short              sieos_int16_t;
typedef unsigned short     sieos_uint16_t;
typedef int                sieos_int32_t;
typedef unsigned int       sieos_uint32_t;
typedef long               sieos_int64_t;
typedef unsigned long      sieos_uint64_t;

typedef long               sieos_ssize_t;
typedef unsigned long      sieos_size_t;
typedef long               sieos_off_t;       /* 64-bit file offsets everywhere */
typedef unsigned long      sieos_ino_t;
typedef unsigned long      sieos_dev_t;       /* major << 32 | minor */
typedef unsigned int       sieos_mode_t;
typedef unsigned int       sieos_nlink_t;
typedef int                sieos_uid_t;
typedef int                sieos_gid_t;
typedef int                sieos_pid_t;
typedef unsigned int       sieos_id_t;        /* generic id (waitid, priocntl) */
typedef unsigned int       sieos_lwpid_t;
typedef int                sieos_processorid_t;
typedef int                sieos_psetid_t;
typedef int                sieos_clockid_t;
typedef int                sieos_timer_t;
typedef long               sieos_time_t;
typedef long               sieos_clock_t;
typedef long               sieos_suseconds_t;
typedef long               sieos_hrtime_t;    /* nanoseconds, monotonic */
typedef int                sieos_blksize_t;
typedef long               sieos_blkcnt_t;
typedef unsigned long      sieos_fsblkcnt_t;
typedef unsigned long      sieos_fsfilcnt_t;
typedef unsigned long      sieos_rlim_t;
typedef unsigned int       sieos_socklen_t;
typedef unsigned short     sieos_sa_family_t;
typedef unsigned short     sieos_in_port_t;
typedef unsigned int       sieos_in_addr_t;
typedef unsigned int       sieos_tcflag_t;
typedef unsigned char      sieos_cc_t;
typedef unsigned int       sieos_speed_t;
typedef unsigned long      sieos_uintptr_t;
typedef long               sieos_intptr_t;

#define SIEOS_NBITSMAJOR 32
#define SIEOS_MAXMIN     0xFFFFFFFFUL
#define SIEOS_MAKEDEV(ma, mi) (((sieos_dev_t)(ma) << SIEOS_NBITSMAJOR) | ((sieos_dev_t)(mi) & SIEOS_MAXMIN))
#define SIEOS_MAJOR(d)   ((unsigned int)((d) >> SIEOS_NBITSMAJOR))
#define SIEOS_MINOR(d)   ((unsigned int)((d) & SIEOS_MAXMIN))

/* Seconds + nanoseconds ("timestruc_t" on Solaris). */
struct sieos_timespec {
    sieos_time_t tv_sec;
    long         tv_nsec;
};

struct sieos_timeval {
    sieos_time_t      tv_sec;
    sieos_suseconds_t tv_usec;
};

struct sieos_iovec {
    void        *iov_base;
    sieos_size_t iov_len;
};

/* Compile-time checks shared by all ABI headers. */
#if defined(__cplusplus)
#define SIEOS_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define SIEOS_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif
#define SIEOS_OFFSETOF(t, f) __builtin_offsetof(t, f)

SIEOS_STATIC_ASSERT(sizeof(struct sieos_timespec) == 16, "timespec size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_iovec) == 16, "iovec size");

#endif
