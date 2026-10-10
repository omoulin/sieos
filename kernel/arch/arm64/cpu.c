/*
 * cpu.c (arm64) - Setting up each processor, a new thread's first frame,
 * and the synchronous exceptions: system calls ("svc"), faults, and the
 * first use of the FP/SIMD registers. (Interrupts: smp.c, trap_irq.)
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"
#include "arm64.h"

/* Exception classes (ESR_EL1 bits 31-26) we act on. */
enum { EC_FP = 0x07, EC_SVC = 0x15, EC_IABT_LOW = 0x20, EC_DABT_LOW = 0x24 };

/* A CPU's cpu_t: one page of the direct map (it needs no special mapping:
 * the processor finds it through TPIDR_EL1). */
cpu_t *cpu_alloc(void)
{
    uint64_t pa = page_alloc(1);
    return pa ? P2V(pa) : 0;
}

/* Once, on the first CPU: its cpu_t (cpus[0]). */
void cpu_init(void)
{
    cpu_t *c = cpu_alloc();
    if (!c) panic("no memory for the first CPU\n");
    c->id = 0;
    cpus[ncpu++] = c;
    cpu_setup(c);
#ifdef STACKCHECK
    extern uint64_t boot_stack[], boot_stack_top[];   /* this stack, CPU 0's idle stack later */
    c->stack_lo = boot_stack;
    c->stack_hi = boot_stack_top;
#endif
}

/* On every CPU: TPIDR_EL1 = its cpu_t, its MPIDR, the FP/SIMD registers. */
void cpu_setup(cpu_t *c)
{
    asm volatile("msr tpidr_el1, %0" : : "r"(c));
    asm volatile("mrs %0, mpidr_el1" : "=r"(c->a.mpidr));
    c->a.mpidr &= 0xFF00FFFFFFUL;          /* the affinity fields only */
    c->a.tls_loaded = 0;
    asm volatile("msr tpidr_el0, xzr");
    /* the cycle counter (random.c's timing samples): PMCR_EL0 = E (on) | C
     * (reset it) | LC (64 bits); PMCNTENSET_EL0 bit 31: count cycles */
    uint64_t pmcr = 1 | 4 | 64, cyc = 1UL << 31;
    asm volatile("msr s3_3_c9_c12_0, %0; msr s3_3_c9_c12_1, %1; isb" : : "r"(pmcr), "r"(cyc));
    fpu_cpu_init(c);
}

/* The running thread's kernel stack: nothing to do here. SP_EL1 is
 * already its stack's top whenever it runs in user mode (its frame was
 * popped on the way out), and I/O ports do not exist on arm64. */
void arch_set_kstack(cpu_t *c, uint64_t top, int io) { (void)c; (void)top; (void)io; }

/* A new thread's kernel stack: a regs_t that enters user mode (EL0t, with
 * interrupts enabled) at pc with the stack sp and arg in x0, under the
 * registers switch_to pops, with trap_return as switch_to's return address. */
void arch_thread_start(task_t *t, uint64_t pc, uint64_t sp, uint64_t arg)
{
    extern void trap_return(void);         /* entry.S */
    regs_t *r = (regs_t *)(t->kstack + KSTACK) - 1;
    memset(r, 0, sizeof *r);
    r->pc = pc; r->sp = sp; r->pstate = 0; r->x[0] = arg;
    uint64_t *k = (uint64_t *)r - 12;      /* x19-x30 for switch_to: zero, but x30 */
    memset(k, 0, 12 * 8);
    k[11] = (uint64_t)trap_return;
    t->ksp = (uint64_t)k;
}

/* The running thread's TLS pointer: TPIDR_EL0. */
void arch_set_tls(cpu_t *c, uint64_t base)
{
    asm volatile("msr tpidr_el0, %0" : : "r"(c->a.tls_loaded = base));
}

/* Stop every other CPU (power off, panic): a "halt" interrupt. (GICv2 has
 * no interrupt that gets through masking, unlike x86's NMI: a CPU spinning
 * for the kernel lock never gets it, but it then waits forever anyway.) */
volatile int halting;
void halt_others(void)
{
    halting = 1;
    cpu_t *me = this_cpu();
    for (int i = 0; i < ncpu; i++) if (cpus[i] != me) gic_sgi(cpus[i], SGI_HALT);
}

/* A synchronous exception from a program: a system call, its first FP/SIMD
 * instruction, or a fault (its process dies). Returns 1: the lock is held. */
int trap_sync(regs_t *r)
{
    uint64_t esr = sysreg_esr(), far = sysreg_far();
    int ec = esr >> 26 & 0x3F;
    if (ec == EC_SVC) { diag_stage(12); syscall(r); return 1; }   /* step 12: a program runs (once) */
    klock();
    rand_event(esr ^ ticks());
    if (ec == EC_FP && fpu_first_use(cur)) return 1;   /* the instruction runs again */
    diag_fault(4, esr, r->pc, far);                    /* (on a Pi's screen, before step 12: init itself) */
    kprintf("mk: %s (pid %d) killed: %s at %lx (syndrome %lx, address %lx)\n",
            cur->proc->name, cur->proc->pid,
            ec == EC_DABT_LOW ? "bad memory access" : ec == EC_IABT_LOW ? "bad jump" : "exception",
            r->pc, esr, far);
    proc_exit(-1);
}

/* A synchronous exception in the kernel itself: a bug. */
int trap_kernel(regs_t *r)
{
    diag_fault(1, sysreg_esr(), r->pc, sysreg_far());
    panic("exception in the kernel at %lx (syndrome %lx, address %lx, lr %lx)\n",
          r->pc, sysreg_esr(), sysreg_far(), r->x[30]);
}

int trap_bad(regs_t *r)
{
    diag_fault(1, sysreg_esr(), r->pc, sysreg_far());
    panic("unexpected exception at %lx (syndrome %lx)\n", r->pc, sysreg_esr());
}
