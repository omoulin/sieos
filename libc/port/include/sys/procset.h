#ifndef _SYS_PROCSET_H
#define _SYS_PROCSET_H
/* Solaris process sets: idtype_t (P_PID, P_LWPID, ... from <sys/wait.h>) and sigsend. */
#ifdef __cplusplus
extern "C" {
#endif
#include <sys/types.h>
#include <sys/wait.h>
#define P_MYID (-1)
int sigsend(idtype_t, id_t, int);
#ifdef __cplusplus
}
#endif
#endif
