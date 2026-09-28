#include <sys/statvfs.h>
#include <sys/statfs.h>
#include <string.h>
#include "syscall.h"
#include "sieos/syscall.h"
#include "sieos/stat.h"

static int kstatvfs(int fd, const char *path, struct sieos_statvfs *k)
{
	memset(k, 0, sizeof *k);
	return path ? syscall(SIEOS_SYS_statvfs, path, k) : syscall(SIEOS_SYS_fstatvfs, fd, k);
}

static void to_statvfs(struct statvfs *out, const struct sieos_statvfs *k)
{
	*out = (struct statvfs){0};
	out->f_bsize = k->f_bsize;
	out->f_frsize = k->f_frsize ? k->f_frsize : k->f_bsize;
	out->f_blocks = k->f_blocks;
	out->f_bfree = k->f_bfree;
	out->f_bavail = k->f_bavail;
	out->f_files = k->f_files;
	out->f_ffree = k->f_ffree;
	out->f_favail = k->f_favail;
	out->f_fsid = k->f_fsid;
	out->f_flag = k->f_flag;
	out->f_namemax = k->f_namemax;
}

static void to_statfs(struct statfs *out, const struct sieos_statvfs *k)
{
	*out = (struct statfs){0};
	out->f_type = k->f_fsid >> 32;
	out->f_bsize = k->f_bsize;
	out->f_frsize = k->f_frsize ? k->f_frsize : k->f_bsize;
	out->f_blocks = k->f_blocks;
	out->f_bfree = k->f_bfree;
	out->f_bavail = k->f_bavail;
	out->f_files = k->f_files;
	out->f_ffree = k->f_ffree;
	out->f_fsid.__val[0] = k->f_fsid;
	out->f_fsid.__val[1] = k->f_fsid >> 32;
	out->f_namelen = k->f_namemax;
	out->f_flags = k->f_flag;
}

int statvfs(const char *restrict path, struct statvfs *restrict buf)
{
	struct sieos_statvfs k;
	if (kstatvfs(-1, path, &k) < 0) return -1;
	to_statvfs(buf, &k);
	return 0;
}

int fstatvfs(int fd, struct statvfs *buf)
{
	struct sieos_statvfs k;
	if (kstatvfs(fd, 0, &k) < 0) return -1;
	to_statvfs(buf, &k);
	return 0;
}

static int __statfs(const char *path, struct statfs *buf)
{
	struct sieos_statvfs k;
	if (kstatvfs(-1, path, &k) < 0) return -1;
	to_statfs(buf, &k);
	return 0;
}

static int __fstatfs(int fd, struct statfs *buf)
{
	struct sieos_statvfs k;
	if (kstatvfs(fd, 0, &k) < 0) return -1;
	to_statfs(buf, &k);
	return 0;
}

weak_alias(__statfs, statfs);
weak_alias(__fstatfs, fstatfs);
