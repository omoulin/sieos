/*
 * linker.ld (arm64) - The kernel image: linked at KVMA (0xFFFFFFFF80000000),
 * wherever the boot loader puts it physically (boot.S maps it there).
 * The flat file (objcopy -O binary) starts with the "Image" header of
 * boot.S; .bss is not in the file (the header gives the size in memory).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
ENTRY(_start)
SECTIONS
{
    . = 0xFFFFFFFF80000000;
    .text : { *(.text.boot) *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .data : { *(.data .data.*) }
    . = ALIGN(4096);
    .bss : { _bss_start = .; *(.bss .bss.*) *(COMMON) . = ALIGN(4096); _bss_end = .; }
    _kernel_end = .;
    _image_size = _kernel_end - _start;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
