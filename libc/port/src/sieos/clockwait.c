/*
 * clockwait.c - The waits with a clock of their own (POSIX.1-2024):
 * pthread_cond_clockwait, pthread_mutex_clocklock, pthread_rwlock_clock*lock,
 * sem_clockwait.  An absolute time on another clock than the object's
 * becomes one on the object's, the same time from now.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include "pthread_impl.h"

static int convert(clockid_t from, clockid_t to, const struct timespec *at, struct timespec *out)
{
	if (from != CLOCK_REALTIME && from != CLOCK_MONOTONIC)
		return EINVAL;
	if (at->tv_nsec < 0 || at->tv_nsec >= 1000000000L)
		return EINVAL;
	if (from == to) {
		*out = *at;
		return 0;
	}
	struct timespec nf, nt;
	clock_gettime(from, &nf);
	clock_gettime(to, &nt);
	long long d = (at->tv_sec - nf.tv_sec) * 1000000000LL + (at->tv_nsec - nf.tv_nsec);
	if (d < 0)
		d = 0;
	long long t = nt.tv_sec * 1000000000LL + nt.tv_nsec + d;
	out->tv_sec = t / 1000000000LL;
	out->tv_nsec = t % 1000000000LL;
	return 0;
}

int pthread_cond_clockwait(pthread_cond_t *restrict c, pthread_mutex_t *restrict m, clockid_t clk,
                           const struct timespec *restrict at)
{
	struct timespec ts;
	int r = convert(clk, c->_c_clock ^ CLOCK_REALTIME, at, &ts);
	return r ? r : pthread_cond_timedwait(c, m, &ts);
}

int pthread_mutex_clocklock(pthread_mutex_t *restrict m, clockid_t clk, const struct timespec *restrict at)
{
	struct timespec ts;
	int r = convert(clk, CLOCK_REALTIME, at, &ts);
	return r ? r : pthread_mutex_timedlock(m, &ts);
}

int pthread_rwlock_clockrdlock(pthread_rwlock_t *restrict l, clockid_t clk, const struct timespec *restrict at)
{
	struct timespec ts;
	int r = convert(clk, CLOCK_REALTIME, at, &ts);
	return r ? r : pthread_rwlock_timedrdlock(l, &ts);
}

int pthread_rwlock_clockwrlock(pthread_rwlock_t *restrict l, clockid_t clk, const struct timespec *restrict at)
{
	struct timespec ts;
	int r = convert(clk, CLOCK_REALTIME, at, &ts);
	return r ? r : pthread_rwlock_timedwrlock(l, &ts);
}

int sem_clockwait(sem_t *restrict s, clockid_t clk, const struct timespec *restrict at)
{
	struct timespec ts;
	int r = convert(clk, CLOCK_REALTIME, at, &ts);
	if (r) {
		errno = r;
		return -1;
	}
	return sem_timedwait(s, &ts);
}
