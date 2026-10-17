/*
 * install.h - The installer's parts: install.c (the drives, the GPT, the
 * plan, SieFS) and fat.c (FAT32: the EFI partition's files).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

/* install.c: the drive's sectors. disk_wr checks every write against the
 * ranges the plan allows, and ends the program before any other. */
int  disk_rd(uint64_t lba, uint64_t n, void *buf);
void disk_wr(uint64_t lba, uint64_t n, const void *buf);
void die(const char *why) __attribute__((noreturn));
void *zalloc(uint64_t n);

/* fat.c */
const char *fat_open(uint64_t first, uint64_t n, uint32_t sector);   /* -> 0, or what is wrong */
void     fat_format(uint64_t first, uint64_t n, uint32_t sector, const char *label);
int      fat_to83(const char *s, char *n83);
uint32_t fat_dir(uint32_t dir, const char *name83, int ours);   /* dir 0: the root; made if missing */
void     fat_put(uint32_t dir, const char *name83, const void *data, uint32_t len);
long     fat_get(uint32_t dir, const char *name83, void *buf, uint32_t cap);
void     fat_flush(void);
