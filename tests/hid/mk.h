/*
 * tests/hid/mk.h - Stands in for user/lib/mk.h when the USB server's HID
 * driver (user/usb/hid.c) and user/lib/hidrep.c are built on the host
 * (tests/hid/test.c): the host's C library, SIEOS's types, and the few
 * calls they make (the test provides them).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "mk/abi.h"
#include "mk/proto.h"
#include "mk/lib.h"

long call_named(long *port, const char *name, msg_t *m, int repeat);
long thread_start(void (*fn)(void *), void *arg, size_t stack);
long sys_clock(void);
long sys_sleep(uint64_t ns);
long sys_wake(int tid);
long sys_yield(void);
