#include <time.h>
#include "syscall.h"
#include "sieos/syscall.h"
#include "sieos_timer.h"

int timer_gettime(timer_t t, struct itimerspec *val)
{
	return syscall(SIEOS_SYS_timer_gettime, __timer_kid(t), val);
}
