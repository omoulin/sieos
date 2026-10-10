/*
 * user.ld - The memory layout of a user program: code at 4 MiB, then
 * read-only data, then data. Each kind gets its own pages and segment, so
 * the kernel can map code read+execute, constants read-only and data
 * read+write but never executable ("W^X").
 *
 * Thread-local variables (_Thread_local) have a template here: one
 * section with their initial values (and the zero ones, so the template is
 * one simple block), described by a PT_TLS header; each thread gets its own
 * copy (user/lib/lib.c). It starts on a 64-byte boundary and its size is a
 * multiple of 64, so the thread pointer (template start + size) is where
 * the compilers' offsets assume, whatever alignment the linker records.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); rodata PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); tls PT_TLS; }
SECTIONS
{
    . = 0x400000;
    .text : { *(.text .text.*) } :text
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) } :rodata
    . = ALIGN(4096);
    .data : { *(.data .data.*) } :data
    .tdata ALIGN(64) : { __tls_start = .; *(.tdata .tdata.*) *(.tbss .tbss.*) *(.tcommon) . = ALIGN(64); __tls_end = .; } :data :tls
    .bss  : { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
