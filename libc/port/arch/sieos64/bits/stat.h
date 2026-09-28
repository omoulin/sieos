/* SIEOS: struct stat is the kernel's (Solaris amd64 layout with st_fstype). */
struct stat {
	dev_t st_dev;
	ino_t st_ino;
	mode_t st_mode;
	nlink_t st_nlink;
	uid_t st_uid;
	gid_t st_gid;
	dev_t st_rdev;
	off_t st_size;
	struct timespec st_atim;
	struct timespec st_mtim;
	struct timespec st_ctim;
	blksize_t st_blksize;
	int __st_pad;
	blkcnt_t st_blocks;
	char st_fstype[16];
};
