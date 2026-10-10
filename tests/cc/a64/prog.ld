/* prog.ld - an AArch64 test program at 4 MiB, for the user-mode emulator; the
 * start-up code (libc.c) finds the thread-local template through these symbols.
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
ENTRY(_start)
SECTIONS {
  . = 0x400000;
  .text : { *(.text .text.*) }
  . = ALIGN(4096);
  .rodata : { *(.rodata .rodata.*) }
  . = ALIGN(4096);
  .data : { *(.data .data.*) }
  .tdata : { __tdata_start = .; *(.tdata .tdata.*) __tdata_end = .; }
  .tbss : { *(.tbss .tbss.*) }
  __tbss_size = SIZEOF(.tbss);
  __tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss));
  .bss : { *(.bss .bss.*) }
}
