struct semid_ds {
	struct ipc_perm sem_perm;
	unsigned short sem_nsems;
	short __pad1;
	int __pad2;
	time_t sem_otime;
	time_t sem_ctime;
	long __unused[3];
};
