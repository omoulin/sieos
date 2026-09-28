#include "syscall.h"
#include "sieos/syscall.h"
#include "sieos/lwp.h"

/* The TLS pointer is the %fs base of the calling LWP. */
int __set_thread_area(void *p)
{
	return __syscall(SIEOS_SYS_lwp_private, SIEOS_LWP_SETPRIVATE, SIEOS_LWP_FSBASE, p);
}
