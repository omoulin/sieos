/*
 * linux/memfd.h - memfd_create's flags, as Linux's uapi header names them,
 * for ported code that includes it (WebKit's shared memory: memfd, not
 * shm_open). libposix implements memfd_create (linux.c).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef MFD_HUGETLB
#define MFD_HUGETLB 0x0004U
#endif
