/* linux/memfd.h - memfd_create's flags (sys/mman.h has them). */
/*
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef _LINUX_MEMFD_H
#define _LINUX_MEMFD_H
#include <sys/mman.h>
#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#define MFD_EXEC        0x0010U
#endif
#endif
