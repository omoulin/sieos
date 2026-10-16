/*
 * tests/net/tlsmk.h - Stands in for user/lib/mk.h when user/lib/tls.c and
 * x509.c are built on the host (tests/net/tlsclient.c): the host's C
 * library, and the few SIEOS types and calls they use.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

typedef struct { int64_t time; } net_info_t;
typedef struct { int unused; } net_sock_t;
long sys_random(void *buf, size_t n);
char *file_get(const char *path, size_t *len);
