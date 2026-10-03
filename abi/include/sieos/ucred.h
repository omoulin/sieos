/*
 * sieos/ucred.h - Credentials of a process or of a socket's peer, as
 * Solaris's ucred_get(3C) and getpeerucred(3C) give them (system call 155,
 * ucredsys(op, id, sieos_ucred *)):
 *
 *   SIEOS_UCREDSYS_UCREDGET       id: a pid (0: the caller)
 *   SIEOS_UCREDSYS_GETPEERUCRED   id: a connected AF_UNIX socket's descriptor:
 *                                 its peer's when it connected (socketpair: the creator)
 *
 * Another process's: the caller's own user, or root.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_ABI_UCRED_H
#define SIEOS_ABI_UCRED_H

#include "types.h"

#define SIEOS_UCREDSYS_UCREDGET     0
#define SIEOS_UCREDSYS_GETPEERUCRED 1

#define SIEOS_UCRED_NGROUPS 16

struct sieos_ucred {
    sieos_pid_t uc_pid;
    sieos_uid_t uc_euid, uc_ruid, uc_suid;
    sieos_gid_t uc_egid, uc_rgid, uc_sgid;
    int uc_ngroups;
    sieos_gid_t uc_groups[SIEOS_UCRED_NGROUPS];
};

#endif
