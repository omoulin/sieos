#define SHMLBA 4096

struct shmid_ds {
	struct ipc_perm shm_perm;
	size_t shm_segsz;
	pid_t shm_lpid;
	pid_t shm_cpid;
	unsigned long shm_nattch;
	time_t shm_atime;
	time_t shm_dtime;
	time_t shm_ctime;
	long __unused[4];
};

struct shminfo {
	unsigned long shmmax, shmmin, shmmni, shmseg, shmall, __unused[4];
};

struct shm_info {
	int __used_ids;
	unsigned long shm_tot, shm_rss, shm_swp;
	unsigned long __swap_attempts, __swap_successes;
};
