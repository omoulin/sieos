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
void fpu_flush(cpu_t *c, task_t *t);           /* t's registers saved, none loaded (TS set) */
uint32_t fpu_area_size(void);
/* smp.c */
void lapic_eoi(void);
void send_ipi(int apic_id, int vec);
void ipi_all_others(int mode);
typedef struct __attribute__((packed)) { char sig[4]; uint32_t len; uint8_t rev, sum; char oem[6], table[8];
                                         uint32_t oemrev, creator, crev; uint8_t data[]; } sdt_t;
sdt_t *acpi_find(const char *sig, int nth);     /* the nth ACPI table with this signature, 0: none */
/* platform.c */
long acpi_sleep(int s, uint32_t vector, int check);   /* enter ACPI sleep state 3 or 5 */
void serial_init(void);                         /* COM1, for the log (again after a suspend) */
/* platform.c: what a UEFI loader gave (0: none, booted by multiboot) */
extern uint64_t x86_rsdp;                       /* the ACPI tables' root pointer */
extern struct bootfb { uint64_t pa, size; uint32_t w, h, pitch, fmt; } x86_fb;   /* the firmware's screen */
/* fbcon.c: the log on the firmware's screen */
void fbcon_start(void);
void fbcon_map(void);
void fbcon_putc(char c);
void fbcon_mapped(uint64_t pa, uint64_t size);
