/*
 * mk/gpt.h - The GUID partition table (GPT) as SIEOS uses it: a USB key or
 * a disk carries an EFI system partition (FAT, with the boot loader) and a
 * SieFS partition. The disk server (vblk) finds the SieFS one by its type
 * GUID below; the image builder writes it with that type and the name
 * "SIEOS". One constant, shared by both.
 *
 * Layout on the disk (512-byte sectors): sector 0 a protective MBR, sector
 * 1 the header ("EFI PART"), then the entries (128 bytes each); with 4 KiB
 * sectors, the header is in sector 1 as well (byte 4096).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

/* The SieFS partition type, 5e1e0500-51ef-4153-9e05-534945465331 (the
 * same as tools/mkusb.py's SIEFS_TYPE), as stored on disk (the first three
 * fields little-endian). */
#define GPT_SIEFS_TYPE { 0x00, 0x05, 0x1e, 0x5e, 0xef, 0x51, 0x53, 0x41, \
                         0x9e, 0x05, 0x53, 0x49, 0x45, 0x46, 0x53, 0x31 }
#define GPT_SIEFS_NAME "SIEOS"           /* the partition's name (UTF-16 on disk) */

typedef struct {                         /* the header, at the start of sector 1 */
    char sig[8];                         /* "EFI PART" */
    uint32_t rev, hsize, hcrc, zero;
    uint64_t self, alt, first_usable, last_usable;
    uint8_t guid[16];
    uint64_t entries_lba;
    uint32_t nentries, entsize, entcrc;
} __attribute__((packed)) gpt_header_t;

typedef struct {                         /* one partition */
    uint8_t type[16], guid[16];
    uint64_t first, last, attr;          /* first and last sectors, inclusive */
    uint16_t name[36];                   /* UTF-16 */
} __attribute__((packed)) gpt_entry_t;
