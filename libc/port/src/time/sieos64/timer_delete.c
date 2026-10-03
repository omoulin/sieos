#include <errno.h>
#include <port.h>
#include "syscall.h"
#include "sieos/syscall.h"
#include "sieos_timer.h"

int timer_delete(timer_t t)
{
	int r = __syscall(SIEOS_SYS_timer_delete, __timer_kid(t));
	if (r == 0 && !((uintptr_t)t & 1))
		port_send(((struct thr_timer *)t)->port, 0, 0);   /* its thread ends, and frees it */
	return __syscall_ret(r);
}
