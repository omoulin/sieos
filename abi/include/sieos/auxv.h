/*
 * sieos/auxv.h - process start-up (ABI v2).
 *
 * At the entry point (SysV amd64 conventions):
 *
 *   rsp ->  argc                      (16-byte aligned)
 *           argv[0] .. argv[argc-1], NULL
 *           envp[0] .. envp[n-1], NULL
 *           auxv pairs { a_type, a_val } ..., { SIEOS_AT_NULL, 0 }
 *           padding, then the string and random-byte areas
 *   rdx = 0 (no run-time linker termination function), other registers 0,
 *   %fs base 0, FPU in its default state (MXCSR 0x1F80, x87 control 0x37F).
 *
 * Both the common SysV entries and the Solaris AT_SUN_* ones are supplied.
 */
#ifndef SIEOS_ABI_AUXV_H
#define SIEOS_ABI_AUXV_H

typedef struct {
    long a_type;
    union {
        long a_val;
        void *a_ptr;
    } a_un;
} sieos_auxv_t;

#define SIEOS_AT_NULL     0
#define SIEOS_AT_IGNORE   1
#define SIEOS_AT_EXECFD   2
#define SIEOS_AT_PHDR     3
#define SIEOS_AT_PHENT    4
#define SIEOS_AT_PHNUM    5
#define SIEOS_AT_PAGESZ   6
#define SIEOS_AT_BASE     7        /* interpreter base (0: static) */
#define SIEOS_AT_FLAGS    8
#define SIEOS_AT_ENTRY    9
#define SIEOS_AT_UID      11       /* real and effective ids (the dynamic linker */
#define SIEOS_AT_EUID     12       /* treats a program as set-id unless all four */
#define SIEOS_AT_GID      13       /* are present) */
#define SIEOS_AT_EGID     14
#define SIEOS_AT_HWCAP    16       /* CPUID.1:EDX */
#define SIEOS_AT_CLKTCK   17
#define SIEOS_AT_SECURE   23       /* 1 for set-id programs */
#define SIEOS_AT_RANDOM   25       /* 16 random bytes */
#define SIEOS_AT_HWCAP2   26       /* CPUID.1:ECX */
#define SIEOS_AT_EXECFN   31

#define SIEOS_AT_SUN_UID      2000
#define SIEOS_AT_SUN_RUID     2001
#define SIEOS_AT_SUN_GID      2002
#define SIEOS_AT_SUN_RGID     2003
#define SIEOS_AT_SUN_PLATFORM 2008 /* "i86pc" */
#define SIEOS_AT_SUN_HWCAP    2009
#define SIEOS_AT_SUN_EXECNAME 2014 /* full path of the executable */
#define SIEOS_AT_SUN_AUXFLAGS 2017 /* SIEOS_AF_SUN_* */

#define SIEOS_AF_SUN_SETUGID  0x00000001

#define SIEOS_PLATFORM "i86pc"

#endif
