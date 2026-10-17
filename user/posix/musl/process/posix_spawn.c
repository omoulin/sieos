/*
 * posix_spawn.c - musl's posix_spawn on SIEOS (replaces src/process/posix_spawn.c,
 * which runs the child in the parent's memory, clone(CLONE_VM), until it
 * calls exec: SIEOS has neither). The file actions are handed over in order
 * to the Linux personality (spawn.c), which starts the program with
 * SYS_SPAWN and gives it its descriptors, arguments and environment
 * through unixd.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#define _GNU_SOURCE
#include <errno.h>
#include <spawn.h>
#include <stdlib.h>
#include "../fdop.h"
#include "sieos_spawn.h"

int posix_spawn(pid_t *restrict res, const char *restrict path, const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *restrict attr, char *const argv[restrict], char *const envp[restrict])
{
    sieos_fa_t v[64];
    int n = 0;
    struct fdop *op = fa ? fa->__actions : 0;
    if (op) while (op->next) op = op->next;             /* (the list is newest first) */
    for (; op; op = op->prev) {
        if (n == 64) return ENOMEM;
        v[n++] = (sieos_fa_t){ op->cmd, op->fd, op->srcfd, op->oflag, (int)op->mode, op->path };
    }
    int pid = 0, pg = attr && (attr->__flags & POSIX_SPAWN_SETPGROUP);
    long r = __sieos_spawn(&pid, path, attr && attr->__fn, v, n, pg, argv, envp);
    if (r < 0) return (int)-r;
    if (res) *res = pid;
    return 0;
}
