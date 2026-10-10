/* tls.ld - a program at 4 MiB whose start-up code finds the thread-local template.
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
  .bss : { *(.bss .bss.*) }
}
