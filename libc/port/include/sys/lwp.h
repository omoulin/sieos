#ifndef _SYS_LWP_H
#define _SYS_LWP_H
/* Solaris LWP interfaces; the constants and structures come from the kernel ABI. */
#ifdef __cplusplus
extern "C" {
#endif
#include <sys/types.h>
#include <sys/__lwp_abi.h>

typedef unsigned int lwpid_t;

lwpid_t _lwp_self(void);
int _lwp_kill(lwpid_t, int);
int _lwp_suspend(lwpid_t);
int _lwp_continue(lwpid_t);
int _lwp_info(void *);

#ifdef __cplusplus
}
#endif
#endif
