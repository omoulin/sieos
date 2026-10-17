/*
 * linux/futex.h - The futex operations as Linux names them, for ported
 * code that calls futex() itself (libc++'s atomic waits). SIEOS's Linux
 * personality (user/posix/linux.c) does WAIT, WAKE and their BITSET forms.
 * Values from Linux's uapi header.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <linux/types.h>
#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET     10
#define FUTEX_PRIVATE_FLAG    128
#define FUTEX_CLOCK_REALTIME  256
#define FUTEX_WAIT_PRIVATE    (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE    (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)
#define FUTEX_BITSET_MATCH_ANY 0xffffffff
