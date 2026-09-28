#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "syscall.h"

/* The kernel implements AT_EACCESS itself. */
int faccessat(int fd, const char *filename, int amode, int flag)
{
	if (flag & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW))
		return __syscall_ret(-EINVAL);
	return syscall(SYS_faccessat, fd, filename, amode, flag);
}
