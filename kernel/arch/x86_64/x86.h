/*
 * x86.h - What the x86-64 files share among themselves (not the core).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

/* cpu.c */
extern volatile int halting;
cpu_t *cpu_alloc(void);
void cpu_setup(cpu_t *c);
/* fpu.c */
void fpu_cpu_init(cpu_t *c);
uint32_t fpu_area_size(void);
/* smp.c */
void lapic_eoi(void);
void send_ipi(int apic_id, int vec);
void ipi_all_others(int mode);
/* platform.c: what a UEFI loader gave (0: none, booted by multiboot) */
extern uint64_t x86_rsdp;                       /* the ACPI tables' root pointer */
extern struct bootfb { uint64_t pa, size; uint32_t w, h, pitch, fmt; } x86_fb;   /* the firmware's screen */
