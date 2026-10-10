/*
 * arch.h - What each processor architecture gives the portable core.
 *
 * The core (the .c files of kernel/: processes, threads, scheduling, messages, memory
 * policy, quotas, random mixing, the log) never touches a register or a
 * device itself. Each architecture (kernel/arch/NAME/) provides:
 *
 *  - arch_defs.h, included first by kernel.h: the memory layout (P2V,
 *    USER_TOP...), regs_t (an interrupted program's registers) with the
 *    SC_* accessors for system call arguments, arch_cpu_t (its part of the
 *    per-CPU data, first in cpu_t), the VM_* mapping flags, and inline
 *    helpers: this_cpu, ticks (its fast clock), arch_relax, arch_wait_irq,
 *    arch_halt_forever, arch_as_load, arch_tls_switch, arch_lock_wait;
 *  - the functions declared below, in its own .c and .S files;
 *  - its boot code, which fills a boot_info_t and calls kernel_main();
 *  - its linker script.
 *
 * Today: arch/x86_64 and arch/arm64 (docs/arch.md).
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

struct cpu;
struct task;
struct proc;

/* ---- Boot: what the boot code found, handed to kernel_main (main.c). */
#define MAXMOD   16
#define MAXRANGE 64
typedef struct { uint64_t start, end; } range_t;          /* physical, [start, end) */
typedef struct {
    range_t ram[MAXRANGE];  int nram;      /* usable RAM */
    range_t keep[MAXRANGE]; int nkeep;     /* the boot loader's data: kept until mem_boot_done */
    range_t rsvd[8];        int nrsvd;     /* never allocated (firmware data, start-up pages) */
    uint64_t kernel_start, kernel_end;     /* the kernel image, physical */
    uint64_t dmap_boot;                    /* RAM already in the direct map at boot (bytes) */
    struct { uint64_t pa, len; const char *cmdline; } mod[MAXMOD];
    int nmod;                              /* the boot modules: init first */
} boot_info_t;
void kernel_main(boot_info_t *bi) __attribute__((noreturn));

/* ---- Processors (cpu.c, smp.c on x86-64). */
void cpu_init(void);                       /* the first CPU: tables, its cpu_t (cpus[0]) */
void smp_init(void);                       /* interrupt controllers, time, CPU discovery */
void smp_start(void);                      /* start the other CPUs (each ends in idle()) */
void arch_set_kstack(struct cpu *c, uint64_t top, int io);  /* the running thread's kernel stack;
                                              io: it may use I/O ports (drivers) */
void arch_kick(struct cpu *c);             /* wake c (idle) or make it reschedule */
void halt_others(void);                    /* stop every other CPU (power off, panic) */
void tlb_shootdown(struct proc *p);        /* after unmapping p's pages: every CPU forgets them */
void tlb_ack(struct cpu *c);               /* answer a shootdown request */

/* ---- Threads (cpu.c, entry.S). */
void switch_to(uint64_t *save_ksp, uint64_t ksp);   /* save this kernel stack, resume another */
void arch_thread_start(struct task *t, uint64_t pc, uint64_t sp, uint64_t arg);
                                           /* t's first switch_to enters user mode at pc */
void arch_set_tls(struct cpu *c, uint64_t base);    /* the running thread's TLS register, now */

/* ---- Floating-point and vector registers of user threads (fpu.c). */
int  fpu_first_use(struct task *t);
void fpu_switch(struct cpu *c, struct task *prev, struct task *next);
void fpu_free(struct task *t);

/* ---- Interrupt lines of devices: an "irq" is a line number (ISA 0-15 or
 * a global line 16+ on x86-64). Masked until the drivers have handled it. */
int  irq_route(int irq, int cpu);
void irq_set_level(int irq);
void irq_mask(int irq);
void irq_unmask(int irq);

/* ---- Time (smp.c): ns since boot; ticks() (arch_defs.h) is the fast clock. */
uint64_t now_ns(void);
uint64_t ns_to_ticks(uint64_t ns);
void arch_timer_set(uint64_t deadline);    /* this CPU's one-shot timer, in ticks (NEVER: off) */

/* ---- Memory (mmu.c). Address spaces are named by a physical address
 * (the top page table). Flags are the VM_* of arch_defs.h. */
extern uint64_t kernel_as;                 /* the kernel's own address space */
void  arch_mem_init(uint64_t top);         /* direct-map all RAM below top, switch to kernel_as */
void *kmap(uint64_t pa, uint64_t size, uint64_t flags);   /* the kernel's own device mappings */
uint64_t vm_new(void);
int   vm_map(uint64_t as, uint64_t va, uint64_t pa, uint64_t flags);
uint64_t arch_vm_unmap(uint64_t as, uint64_t va, uint64_t *flags);  /* -> the page's pa (0: none) */
void  arch_vm_free(uint64_t as, void (*leaf)(uint64_t pa, uint64_t flags));
char *arch_uaddr(uint64_t as, uint64_t va, int write);   /* a user address, for the kernel */
void  arch_sync_code(void *p, uint64_t n);  /* code just written at p (kernel address): make it runnable */
int   arch_phys_forbidden(uint64_t pa, uint64_t size);   /* the kernel's own devices (SYS_MAP_PHYS) */
int   arch_phys_screen(uint64_t pa, uint64_t size);      /* the firmware's frame buffer, inside RAM (SYS_MAP_PHYS) */

/* ---- The platform (platform.c): log output, power, hardware randomness. */
void arch_putc(char c);                    /* one byte of the kernel log */
void arch_power(int restart) __attribute__((noreturn));
const char *arch_hw_random_init(void);     /* the CPU's generator's name, 0 if none */
int  arch_hw_random(uint64_t *v);          /* 1 if v got 64 random bits */
uint32_t arch_hwcap(void);                 /* HWCAP_* (mk/abi.h): features programs may use */
long arch_fdt(uint64_t ubuf, uint64_t size);   /* SYS_FDT: copy the device tree to the caller (-ENOSYS: none) */
long arch_screen(uint64_t ubuf);               /* SYS_SCREEN: the firmware's frame buffer (-ENOSYS: none) */
