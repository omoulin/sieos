#define _BSD_SOURCE
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include "syscall.h"

/* struct stat is the kernel's; a NULL path makes the kernel stat the descriptor. */
int __fstatat(int fd, const char *restrict path, struct stat *restrict st, int flag)
{
	if (flag & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT))
		return __syscall_ret(-EINVAL);
	if ((flag & AT_EMPTY_PATH) && !*path) {
		if (fd == AT_FDCWD)
			path = ".";
		else
			path = 0;
	}
	return syscall(SYS_newfstatat, fd, path, st, flag & AT_SYMLINK_NOFOLLOW);
}

weak_alias(__fstatat, fstatat);
