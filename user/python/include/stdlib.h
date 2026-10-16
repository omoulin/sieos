/*
 * stdlib.h - For MicroPython's sources: SIEOS's malloc (user/lib/lib.c) and
 * the little else they use (user/python/libc.c).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stddef.h>
void *malloc(size_t n);
void *realloc(void *p, size_t n);
void  free(void *p);
_Noreturn void abort(void);
_Noreturn void exit(int code);
