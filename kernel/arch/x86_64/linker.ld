/*
 * linker.ld - Where each part of the kernel goes in memory.
 *
 * The boot loader puts the kernel file at physical address 1 MiB (0x100000)
 * and starts it in 32-bit mode without paging. So the first part (.boot)
 * is linked at 1 MiB. Everything else is linked "in the higher half", at
 * 0xFFFFFFFF80000000 + its physical address, where boot.S maps it, so that
 * the lower half of every address space is free for user programs.
 * AT(...) gives the physical (load) address of each section.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
ENTRY(_start)
KVMA = 0xFFFFFFFF80000000;

SECTIONS
{
    . = 0x100000;
    .boot : { KEEP(*(.multiboot)) *(.boot) }

    . = ALIGN(4096) + KVMA;
    .text   : AT(ADDR(.text) - KVMA)   { *(.text .text.*) }
    .rodata : AT(ADDR(.rodata) - KVMA) { *(.rodata .rodata.*) }
    .data   : AT(ADDR(.data) - KVMA)   { *(.data .data.*) }
    _load_end = . - KVMA;               /* the file ends here (physical) */

    .bss ALIGN(4096) : AT(ADDR(.bss) - KVMA) {
        _bss_start = . - KVMA;
        *(.bss .bss.*) *(COMMON)
    }
    . = ALIGN(4096);
    _bss_end = . - KVMA;                /* the kernel's memory ends here (physical) */
    _kernel_end = .;                    /* the same, as a virtual address */

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
