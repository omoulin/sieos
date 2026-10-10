/*
 * arm64.h - What the arm64 files share among themselves (not the core).
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

/* platform.c */
extern const void *fdt;               /* the device tree (in the direct map) */
extern uint64_t phys_base;            /* where the kernel image is, physically */
extern volatile int halting;          /* the machine is stopping: SGI_HALT halts the CPUs */
extern boot_info_t *boot_bi;         /* what arm64_main found (mmu.c maps its RAM) */
extern range_t forbid[];              /* the kernel's own devices (SYS_MAP_PHYS refuses them) */
extern range_t screen[];              /* the firmware's frame buffers (SYS_MAP_PHYS allows them) */
extern int nscreen;
extern int nforbid;
extern long (*psci)(uint64_t fn, uint64_t a, uint64_t b, uint64_t c);   /* 0 if no PSCI */
long psci_hvc(uint64_t fn, uint64_t a, uint64_t b, uint64_t c);          /* boot.S */
long psci_smc(uint64_t fn, uint64_t a, uint64_t b, uint64_t c);
/* syscall.c, called from trap_sync */
void syscall(regs_t *r);
/* cpu.c */
cpu_t *cpu_alloc(void);
void cpu_setup(cpu_t *c);
/* smp.c */
void gic_cpu_init(cpu_t *c);
void gic_sgi(cpu_t *c, int id);
int  trap_irq(regs_t *r);
/* fpu.c */
void fpu_cpu_init(cpu_t *c);
void fpu_save(void *area);            /* entry.S */
void fpu_load(void *area);

static inline uint64_t sysreg_esr(void) { uint64_t v; asm volatile("mrs %0, esr_el1" : "=r"(v)); return v; }
static inline uint64_t sysreg_far(void) { uint64_t v; asm volatile("mrs %0, far_el1" : "=r"(v)); return v; }

/* diag.c: the first boot steps on a Pi's screen (no serial cable needed) */
void diag_init(uint64_t dtb);
void diag_stage(int n);
void diag_high(uint64_t phys, uint64_t dtb);
void diag_fault(int kind, uint64_t esr, uint64_t elr, uint64_t far);
void diag_halt(void);
