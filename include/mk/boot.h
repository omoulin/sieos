/*
 * mk/boot.h - What SIEOS's UEFI loader (boot/uefi) hands the x86-64 kernel.
 *
 * The loader enters the kernel at its 64-bit entry point (boot.S,
 * _start64; the "SIE6" header after the multiboot one tells where) with
 *   rdi = SIEOS_BOOT_MAGIC, rsi = the physical address of a sieos_boot_t,
 * paging on (the firmware's identity map), interrupts off, after
 * ExitBootServices. Everything it points to is below 4 GiB.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

#define SIEOS_BOOT_MAGIC 0x55454953u           /* "SIEU" */
#define SIEOS_ENTRY64    0x36454953u           /* "SIE6": the header word before the 64-bit entry */
#define SIEOS_BOOT_MODS  16

typedef struct {
    uint32_t magic, version;                   /* SIEOS_BOOT_MAGIC, 1 */
    uint64_t mmap, mmap_size, desc_size;       /* the firmware's memory map: its descriptors (UEFI
                                                  layout: type at 0, start at 8, pages at 24) */
    uint64_t rsdp;                             /* the ACPI root pointer (0: none) */
    struct {                                   /* the screen the firmware set up (pa 0: none) */
        uint64_t pa, size;
        uint32_t width, height, pitch, format; /* pitch: pixels per row; format: 0 = bytes R,G,B,x,
                                                  1 = bytes B,G,R,x (our 0x00RRGGBB words) */
    } fb;
    uint64_t self;                             /* this structure's own pages: pa, then length below */
    uint64_t self_len;
    uint32_t nmod, pad;
    struct { uint64_t pa, len; char cmdline[64]; } mod[SIEOS_BOOT_MODS];   /* init first */
} sieos_boot_t;
