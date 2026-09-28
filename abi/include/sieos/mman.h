/*
 * sieos/mman.h - mmap(), mprotect(), memcntl() (ABI v2).  Values follow Solaris.
 */
#ifndef SIEOS_ABI_MMAN_H
#define SIEOS_ABI_MMAN_H

#define SIEOS_PROT_NONE  0x0
#define SIEOS_PROT_READ  0x1
#define SIEOS_PROT_WRITE 0x2
#define SIEOS_PROT_EXEC  0x4

#define SIEOS_MAP_SHARED    0x001
#define SIEOS_MAP_PRIVATE   0x002
#define SIEOS_MAP_TYPE      0x00f
#define SIEOS_MAP_FIXED     0x010
#define SIEOS_MAP_NORESERVE 0x040
#define SIEOS_MAP_ANON      0x100
#define SIEOS_MAP_ANONYMOUS SIEOS_MAP_ANON
#define SIEOS_MAP_ALIGN     0x200      /* addr argument is the required alignment */
#define SIEOS_MAP_TEXT      0x400
#define SIEOS_MAP_INITDATA  0x800
#define SIEOS_MAP_FAILED    ((void *)-1)

/* memcntl() commands */
#define SIEOS_MC_SYNC     1            /* arg: SIEOS_MS_* */
#define SIEOS_MC_LOCK     2
#define SIEOS_MC_UNLOCK   3
#define SIEOS_MC_ADVISE   4            /* arg: SIEOS_MADV_* */
#define SIEOS_MC_LOCKAS   5
#define SIEOS_MC_UNLOCKAS 6

#define SIEOS_MS_ASYNC      0x1
#define SIEOS_MS_INVALIDATE 0x2
#define SIEOS_MS_SYNC       0x4

#define SIEOS_MADV_NORMAL     0
#define SIEOS_MADV_RANDOM     1
#define SIEOS_MADV_SEQUENTIAL 2
#define SIEOS_MADV_WILLNEED   3
#define SIEOS_MADV_DONTNEED   4
#define SIEOS_MADV_FREE       5

#endif
