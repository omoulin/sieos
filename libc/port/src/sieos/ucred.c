/*
 * ucred.c - Credentials of a process or of a socket's peer, as Solaris's
 * ucred_get(3C) and getpeerucred(3C): the ucredsys system call (155).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <ucred.h>
#include "syscall.h"
#include "sieos/syscall.h"
#include "sieos/ucred.h"

struct ucred_s {
	struct sieos_ucred k;
};

size_t ucred_size(void)
{
	return sizeof(struct ucred_s);
}

ucred_t *ucred_get(pid_t pid)
{
	ucred_t *uc = malloc(sizeof *uc);
	if (!uc)
		return 0;
	if (syscall(SIEOS_SYS_ucredsys, SIEOS_UCREDSYS_UCREDGET, pid == (pid_t)-1 ? 0 : pid, &uc->k) < 0) {
		int e = errno;
		free(uc);
		errno = e;
		return 0;
	}
	return uc;
}

int getpeerucred(int fd, ucred_t **ucp)
{
	ucred_t *uc = *ucp ? *ucp : malloc(sizeof *uc);
	if (!uc)
		return -1;
	if (syscall(SIEOS_SYS_ucredsys, SIEOS_UCREDSYS_GETPEERUCRED, fd, &uc->k) < 0) {
		if (!*ucp) {
			int e = errno;
			free(uc);
			errno = e;
		}
		return -1;
	}
	*ucp = uc;
	return 0;
}

void ucred_free(ucred_t *uc)
{
	free(uc);
}

uid_t ucred_geteuid(const ucred_t *uc) { return uc->k.uc_euid; }
uid_t ucred_getruid(const ucred_t *uc) { return uc->k.uc_ruid; }
uid_t ucred_getsuid(const ucred_t *uc) { return uc->k.uc_suid; }
gid_t ucred_getegid(const ucred_t *uc) { return uc->k.uc_egid; }
gid_t ucred_getrgid(const ucred_t *uc) { return uc->k.uc_rgid; }
gid_t ucred_getsgid(const ucred_t *uc) { return uc->k.uc_sgid; }
pid_t ucred_getpid(const ucred_t *uc) { return uc->k.uc_pid; }
int ucred_getzoneid(const ucred_t *uc) { (void)uc; return 0; }
int ucred_getprojid(const ucred_t *uc) { (void)uc; return 0; }

int ucred_getgroups(const ucred_t *uc, const gid_t **groups)
{
	*groups = (const gid_t *)uc->k.uc_groups;
	return uc->k.uc_ngroups;
}
