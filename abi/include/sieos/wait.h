/*
 * sieos/wait.h - waitid() (ABI v2).  Solaris has only waitid in the kernel;
 * wait(), waitpid() and wait3/4() are libc functions built on it.
 */
#ifndef SIEOS_ABI_WAIT_H
#define SIEOS_ABI_WAIT_H

/* idtype_t */
#define SIEOS_P_PID    0
#define SIEOS_P_PPID   1
#define SIEOS_P_PGID   2
#define SIEOS_P_SID    3
#define SIEOS_P_CID    4
#define SIEOS_P_UID    5
#define SIEOS_P_GID    6
#define SIEOS_P_ALL    7
#define SIEOS_P_LWPID  8
#define SIEOS_P_MYID   (-1)            /* "the caller" for priocntl/processor_bind */

/* waitid() options */
#define SIEOS_WEXITED    0001
#define SIEOS_WTRAPPED   0002
#define SIEOS_WSTOPPED   0004
#define SIEOS_WUNTRACED  SIEOS_WSTOPPED
#define SIEOS_WCONTINUED 0010
#define SIEOS_WNOHANG    0100
#define SIEOS_WNOWAIT    0200

/*
 * Traditional wait status word, as produced by libc from the siginfo:
 *   exited:    (status & 0xff) << 8
 *   signalled: sig | (core ? 0x80 : 0)
 *   stopped:   (sig << 8) | 0x7f
 *   continued: 0xffff
 */
#define SIEOS_WSTOPFLG  0177
#define SIEOS_WCONTFLG  0177777
#define SIEOS_WCOREFLG  0200

#endif
