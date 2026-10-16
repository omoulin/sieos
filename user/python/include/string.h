/*
 * string.h - For MicroPython's sources (SIEOS has no C library of this name):
 * the string functions of mk/lib.h (lib/string.c) and the few others it
 * uses (user/python/libc.c).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "mk/lib.h"
int    strncmp(const char *a, const char *b, size_t n);
char  *strrchr(const char *s, int c);
void  *memchr(const void *s, int c, size_t n);
