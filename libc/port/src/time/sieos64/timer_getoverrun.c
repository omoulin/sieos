#include <time.h>
#include "syscall.h"
#include "sieos/syscall.h"
#include "sieos_timer.h"

int timer_getoverrun(timer_t t)
{
	return syscall(SIEOS_SYS_timer_getoverrun, __timer_kid(t));
}
