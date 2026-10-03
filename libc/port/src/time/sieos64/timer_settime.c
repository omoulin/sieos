#include <time.h>
#include "syscall.h"
#include "sieos/syscall.h"
#include "sieos_timer.h"

int timer_settime(timer_t t, int flags, const struct itimerspec *restrict val, struct itimerspec *restrict old)
{
	return syscall(SIEOS_SYS_timer_settime, __timer_kid(t), flags, val, old);
}
