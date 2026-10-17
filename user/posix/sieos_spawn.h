/*
 * sieos_spawn.h - Between musl's posix_spawn (user/posix/musl/posix_spawn.c)
 * and the Linux personality's spawn.c: the file actions, in order.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
typedef struct { int cmd, fd, srcfd, oflag, mode; const char *path; } sieos_fa_t;   /* cmd: FDOP_* */
enum { SIEOS_FA_CLOSE = 1, SIEOS_FA_DUP2, SIEOS_FA_OPEN, SIEOS_FA_CHDIR, SIEOS_FA_FCHDIR };
/* -> 0 and *pid, or -errno. search: look for path in PATH (posix_spawnp). */
long __sieos_spawn(int *pid, const char *path, int search, const sieos_fa_t *fa, int nfa, int pgroup,
                   char *const argv[], char *const envp[]);
