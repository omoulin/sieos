/*
 * efi.ld - The loader's layout, linked twice at two different addresses
 * (BASE: 0x10000000 and 0x20000000); tools/elf2efi.py compares the two to
 * find every 64-bit address in it (the firmware may load us anywhere and
 * must then fix those), and writes the PE file UEFI wants.
 * The first page is left for the PE headers.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
ENTRY(efi_start)
SECTIONS
{
    . = BASE + 0x1000;
    .text : { *(.text .text.*) }
    . = ALIGN(4096);
    .data : { *(.rodata .rodata.*) *(.data .data.*) *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
