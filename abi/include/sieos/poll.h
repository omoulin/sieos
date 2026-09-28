/*
 * sieos/poll.h - pollsys() (ABI v2).  Values follow Solaris.
 */
#ifndef SIEOS_ABI_POLL_H
#define SIEOS_ABI_POLL_H

struct sieos_pollfd {
    int   fd;
    short events;
    short revents;
};

#define SIEOS_POLLIN     0x0001
#define SIEOS_POLLPRI    0x0002
#define SIEOS_POLLOUT    0x0004
#define SIEOS_POLLRDNORM 0x0040
#define SIEOS_POLLWRNORM SIEOS_POLLOUT
#define SIEOS_POLLRDBAND 0x0080
#define SIEOS_POLLWRBAND 0x0100
#define SIEOS_POLLERR    0x0008
#define SIEOS_POLLHUP    0x0010
#define SIEOS_POLLNVAL   0x0020

#endif
