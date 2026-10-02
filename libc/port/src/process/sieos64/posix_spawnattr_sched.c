/*
 * posix_spawnattr_[gs]etsched* - SIEOS: the scheduling attributes of
 * posix_spawn, applied in the child (POSIX_SPAWN_SETSCHEDULER,
 * POSIX_SPAWN_SETSCHEDPARAM; src/process/posix_spawn.c, sieos-port.py).
 * musl answers ENOSYS (Linux's sched_setscheduler is per thread); SIEOS's is
 * per process (priocntl).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <spawn.h>
#include <sched.h>
#include <errno.h>

int posix_spawnattr_getschedparam(const posix_spawnattr_t *restrict attr,
	struct sched_param *restrict schedparam)
{
	*schedparam = (struct sched_param){ .sched_priority = attr->__prio };
	return 0;
}

int posix_spawnattr_setschedparam(posix_spawnattr_t *restrict attr,
	const struct sched_param *restrict schedparam)
{
	attr->__prio = schedparam->sched_priority;
	return 0;
}

int posix_spawnattr_getschedpolicy(const posix_spawnattr_t *restrict attr, int *restrict policy)
{
	*policy = attr->__pol;
	return 0;
}

int posix_spawnattr_setschedpolicy(posix_spawnattr_t *attr, int policy)
{
	if (policy != SCHED_OTHER && policy != SCHED_FIFO && policy != SCHED_RR &&
	    policy != SCHED_BATCH && policy != SCHED_IDLE)
		return EINVAL;
	attr->__pol = policy;
	return 0;
}
