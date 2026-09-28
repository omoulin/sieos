#include <stdlib.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include "syscall.h"

/* /dev/ptmx with the Solaris ISPTM/UNLKPT/PTSNAME ioctls */
int posix_openpt(int flags)
{
	int r = open("/dev/ptmx", flags);
	if (r < 0 && errno == ENOSPC) errno = EAGAIN;
	return r;
}

int grantpt(int fd)
{
	return ioctl(fd, ISPTM) < 0 ? -1 : 0;
}

int unlockpt(int fd)
{
	return ioctl(fd, UNLKPT);
}

int __ptsname_r(int fd, char *buf, size_t len)
{
	char name[32];
	int err;
	if (!buf) len = 0;
	if ((err = __syscall(SYS_ioctl, fd, PTSNAME, name))) return -err;
	if (strlen(name) >= len) return ERANGE;
	strcpy(buf, name);
	return 0;
}

weak_alias(__ptsname_r, ptsname_r);
