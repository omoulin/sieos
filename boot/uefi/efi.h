/*
 * efi.h - The few pieces of the UEFI firmware interface the loader uses,
 * written from the UEFI specification: tables of function pointers, with
 * the index of each function we call. The firmware's functions use the
 * Microsoft x64 calling convention; efi_call (start.S) bridges to it.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

typedef uint64_t efi_status_t;               /* 0: success; top bit set: an error */
#define EFI_ERR(s) ((int64_t)(s) < 0)
#define EFI_BUFFER_TOO_SMALL (0x8000000000000005UL)
#define EFI_LOAD_ERROR       (0x8000000000000001UL)

typedef struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } efi_guid_t;

typedef struct { uint64_t signature; uint32_t revision, header_size, crc32, reserved; } efi_header_t;

/* Boot services: function pointers after the header, by index. */
typedef struct { efi_header_t h; void *fn[44]; } efi_boot_t;
enum { BS_ALLOCATE_PAGES = 2, BS_FREE_PAGES = 3, BS_GET_MEMORY_MAP = 4, BS_HANDLE_PROTOCOL = 16,
       BS_EXIT_BOOT_SERVICES = 26, BS_SET_WATCHDOG = 29, BS_LOCATE_PROTOCOL = 37 };
enum { ALLOC_ANY = 0, ALLOC_MAX_ADDRESS = 1, ALLOC_ADDRESS = 2 };   /* AllocatePages types */
#define LOADER_DATA 2                        /* the memory type of what we allocate */

typedef struct { efi_guid_t guid; void *table; } efi_config_t;

typedef struct {
    efi_header_t h;
    uint16_t *vendor; uint32_t vendor_rev, pad;
    void *con_in_handle, *con_in, *con_out_handle;
    struct { void *reset, *output_string; } *con_out;   /* text output: OutputString(this, UCS-2) */
    void *err_handle, *err, *runtime;
    efi_boot_t *boot;
    uint64_t ntables;
    efi_config_t *tables;
} efi_system_t;

/* The loaded image (our own): where we were read from. */
typedef struct { uint32_t revision, pad; void *parent, *system, *device, *file_path; } efi_loaded_image_t;

/* A FAT volume and its files: OpenVolume(this, &root); then Open(this, &file, UCS-2 path, mode, attributes),
 * Close, Read(this, &bytes, buffer), GetPosition/SetPosition(this, position). */
typedef struct { uint64_t revision; void *open_volume; } efi_fs_t;
typedef struct { uint64_t revision; void *open, *close, *del, *read, *write, *get_pos, *set_pos; } efi_file_t;

/* The screen ("graphics output"): QueryMode(this, n, &size, &info), SetMode(this, n), its current mode. */
typedef struct {
    uint32_t version, width, height, format;   /* format: 0 = R,G,B,x bytes; 1 = B,G,R,x; 2 = masks; 3 = no frame buffer */
    uint32_t masks[4], pitch;                   /* pitch: pixels per row */
} efi_mode_info_t;
typedef struct { uint32_t max_mode, mode; efi_mode_info_t *info; uint64_t info_size, fb_base, fb_size; } efi_gop_mode_t;
typedef struct { void *query_mode, *set_mode, *blt; efi_gop_mode_t *mode; } efi_gop_t;

/* start.S: call a firmware function with up to 6 arguments. */
efi_status_t efi_call(void *fn, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);
#define CALL(fn, ...) efi_call((void *)(fn), __VA_ARGS__)
/* start.S: leave for the kernel's 64-bit entry, interrupts off (never returns). */
void efi_jump(uint64_t entry, uint64_t magic, uint64_t info) __attribute__((noreturn));
