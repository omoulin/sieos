/* linux/version.h - the Linux whose interfaces liblxcompat gives: 5.10 (memfd seals, statx-less). */
/*
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef _LINUX_VERSION_H
#define _LINUX_VERSION_H
#define LINUX_VERSION_CODE 330240
#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + ((c) > 255 ? 255 : (c)))
#define LINUX_VERSION_MAJOR 5
#define LINUX_VERSION_PATCHLEVEL 10
#define LINUX_VERSION_SUBLEVEL 0
#endif
