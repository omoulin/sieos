/*
 * lxcompat.h - forced into every Chromium compilation on SIEOS (-include):
 * the Linux names SIEOS's headers do not have, for what liblxcompat
 * answers (its calls are the C library's names: nothing to rename).
 */
/*
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef LXCOMPAT_H
#define LXCOMPAT_H

/* prctl(2) */
#define LXCOMPAT_PR_SET_VMA           0x53564d41
#define LXCOMPAT_PR_SET_VMA_ANON_NAME 0
#ifndef PR_SET_VMA
#define PR_SET_VMA           LXCOMPAT_PR_SET_VMA
#define PR_SET_VMA_ANON_NAME LXCOMPAT_PR_SET_VMA_ANON_NAME
#endif

/* memfd_create(2): accepted, dropped (SIEOS has no exec seals) */
#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#define MFD_EXEC        0x0010U
#endif
#ifndef F_SEAL_EXEC
#define F_SEAL_EXEC 0x0020
#endif

#endif
