/* SIEOS: struct ipc_perm is the kernel's (sieos/ipc.h). */
struct ipc_perm {
	uid_t uid;
	gid_t gid;
	uid_t cuid;
	gid_t cgid;
	mode_t mode;
	int __ipc_perm_seq;
	key_t __ipc_perm_key;
	int __pad[5];
};
