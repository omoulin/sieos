#define _GNU_SOURCE
#include <sys/time.h>
#include "syscall.h"
#include "sieos/syscall.h"

int adjtime(const struct timeval *in, struct timeval *out)
{
	return syscall(SIEOS_SYS_adjtime, in, out);
}
