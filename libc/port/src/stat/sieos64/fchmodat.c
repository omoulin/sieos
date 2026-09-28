#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include "syscall.h"

/* The kernel takes the flag (AT_SYMLINK_NOFOLLOW on a link: EOPNOTSUPP). */
int fchmodat(int fd, const char *path, mode_t mode, int flag)
{
	if (flag & ~AT_SYMLINK_NOFOLLOW)
		return __syscall_ret(-EINVAL);
	return syscall(SYS_fchmodat, fd, path, mode, flag);
}
