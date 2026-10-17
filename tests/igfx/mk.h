/*
 * tests/igfx/mk.h - what igfx.c takes from SIEOS's mk.h, for the host test
 * (tests/igfx/test.c gives the functions).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t pa, size; uint32_t width, height, pitch, format; } mk_screen_t;
enum { FS_WRONLY = 1, FS_CREAT = 0x40, FS_TRUNC = 0x200 };
long sys_clock(void);
long sys_sleep(uint64_t ns);
long sys_debug(const char *s, size_t n);
void *map_phys(uint64_t pa, size_t size);
uint32_t pci_read(uint32_t a);
long fs_open(const char *path, int flags, int mode);
long fs_write(long h, uint64_t off, const void *buf, size_t n);
long fs_close(long h);
char *file_get(const char *path, size_t *len);
void vformat(void (*put)(void *ctx, char c), void *ctx, const char *fmt, va_list ap);
#define mode_t igfx_mode_t                              /* (the host has its own mode_t) */
