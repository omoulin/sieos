/*
 * sieos/fdext.h - Linux's descriptor kinds and transfers in SIEOS's ABI
 * (system calls 213-227, sieos/syscall.h): epoll, eventfd, timerfd, memfd,
 * pidfd, splice, copy_file_range, preadv/pwritev, mremap.  The values are
 * Linux's, as the C library's headers have them; open flags (O_CLOEXEC,
 * O_NONBLOCK), clocks and signals are SIEOS's.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_ABI_FDEXT_H
#define SIEOS_ABI_FDEXT_H

#include "types.h"
#include "time.h"

/* epoll: the events are poll's (POLLIN 0x1, POLLPRI 0x2, POLLOUT 0x4, POLLERR 0x8,
 * POLLHUP 0x10), and these */
#define SIEOS_EPOLL_CTL_ADD   1
#define SIEOS_EPOLL_CTL_DEL   2
#define SIEOS_EPOLL_CTL_MOD   3
#define SIEOS_EPOLLRDHUP      0x2000
#define SIEOS_EPOLLEXCLUSIVE  (1U << 28)        /* accepted, not applied */
#define SIEOS_EPOLLWAKEUP     (1U << 29)        /* accepted, not applied */
#define SIEOS_EPOLLONESHOT    (1U << 30)        /* reported once, until EPOLL_CTL_MOD */
#define SIEOS_EPOLLET         (1U << 31)        /* edge-triggered: reported when it becomes ready */
struct sieos_epoll_event {
    unsigned int events;
    unsigned long long data;
} __attribute__((packed));

#define SIEOS_EFD_SEMAPHORE   1                 /* eventfd: a read takes 1 */

#define SIEOS_TFD_TIMER_ABSTIME 1               /* timerfd_settime: an absolute time (sieos_itimerspec: time.h) */

#define SIEOS_MFD_CLOEXEC       1
#define SIEOS_MFD_ALLOW_SEALING 2               /* accepted (no seals: F_ADD_SEALS fails) */

#define SIEOS_MREMAP_MAYMOVE  1
#define SIEOS_MREMAP_FIXED    2

#endif
