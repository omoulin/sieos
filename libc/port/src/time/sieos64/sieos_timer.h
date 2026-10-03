/*
 * sieos_timer.h - POSIX timers over the kernel's (system calls 126-129, 112).
 *
 * A timer_t is a kernel timer's id, as (id << 1 | 1), or, for SIGEV_THREAD,
 * a struct thr_timer (aligned: even): the kernel timer notifies an event
 * port (SIGEV_PORT) which a thread of its own waits on, calling the
 * function for each expiration, as Solaris's C library does it.
 */
#include <signal.h>
#include <time.h>
#include <stdint.h>

struct thr_timer {
	int id;                         /* the kernel's */
	int port;
	void (*fn)(union sigval);
	union sigval val;
};

static inline int __timer_kid(timer_t t)
{
	uintptr_t v = (uintptr_t)t;
	return (v & 1) ? (int)(v >> 1) : ((struct thr_timer *)t)->id;
}
