/*
 * cpu.c (arm64) - Setting up each processor, a new thread's first frame,
 * and the synchronous exceptions: system calls ("svc"), faults, and the
 * first use of the FP/SIMD registers. (Interrupts: smp.c, trap_irq.)
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"
#include "arm64.h"

/* Exception classes (ESR_EL1 bits 31-26) we act on. */
enum { EC_FP = 0x07, EC_SVC = 0x15, EC_IABT_LOW = 0x20, EC_DABT_LOW = 0x24, EC_SERROR = 0x2F };

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
    c->cur_as = kernel_as;                 /* (arch_mem_init loaded it) */
    cpus[ncpu++] = c;
    cpu_setup(c);
#ifdef STACKCHECK
    extern uint64_t boot_stack[], boot_stack_top[];   /* this stack, CPU 0's idle stack later */
    c->stack_lo = boot_stack;
    c->stack_hi = boot_stack_top;
#endif
}

/* This CPU's kind, from MIDR_EL1 (implementer 0x41: ARM; the part number,
 * bits 15-4): the efficiency cores of big.LITTLE designs are Cortex-A32,
 * A34, A35, A53, A55, A510 and A520; the others (A57, A72, A76, A78, X1...)
 * are performance cores. A machine whose cores are all alike (the Pi 4's
 * A72s, the Pi 5's A76s) is not hybrid: task.c treats them all as P-cores. */
static int core_kind(void)
{
    uint64_t midr;
    asm volatile("mrs %0, midr_el1" : "=r"(midr));
    if ((midr >> 24 & 0xFF) != 0x41) return CPU_P;
    switch (midr >> 4 & 0xFFF) {
    case 0xD01: case 0xD02: case 0xD03: case 0xD04: case 0xD05: case 0xD46: case 0xD80: return CPU_E;
    }
    return CPU_P;
}

/* On every CPU: TPIDR_EL1 = its cpu_t, its MPIDR, its kind, the FP/SIMD registers. */
void cpu_setup(cpu_t *c)
{
    c->type = core_kind();
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

/* A signal's function (task.c): pc(arg) on the thread's stack, 128 bytes
 * below sp and 16-aligned, in EL0 with interrupts on. It never returns:
 * it ends with SYS_SIGRETURN, which puts every register back (eret
 * always does). */
void arch_sig_enter(regs_t *r, uint64_t pc, long arg)
{
    r->sp = (r->sp - 128) & ~15UL;
    r->pc = pc; r->x[0] = arg; r->x[30] = 0; r->pstate = 0;
}
void arch_sig_return(regs_t *r) { (void)r; }

/* The running thread's TLS pointer: TPIDR_EL0. */
void arch_set_tls(cpu_t *c, uint64_t base)
{
    asm volatile("msr tpidr_el0, %0" : : "r"(c->a.tls_loaded = base));
}

/* Stop every other CPU (power off, panic): a "halt" interrupt. (GICv2 has
 * no interrupt that gets through masking, unlike x86's NMI: a CPU spinning
 * in the kernel never gets it, but its spin then lasts forever anyway.) */
volatile int halting;
void halt_others(void)
{
    halting = 1;
    cpu_t *me = this_cpu();
    for (int i = 0; i < ncpu; i++) if (cpus[i] != me) gic_sgi(cpus[i], SGI_HALT);
}

/* A synchronous exception from a program: a system call, its first FP/SIMD
 * instruction, or a fault (its process dies). Also an SError taken while a
 * program ran: the bus refused an access (on a Pi, a driver touching a
 * device that is off or held in reset); it arrives a few instructions
 * late, but a driver's own registers are the likely cause: that process
 * dies too, not the whole system. Returns 1: leave through kernel_exit. */
int trap_sync(regs_t *r)
{
    uint64_t esr = sysreg_esr(), far = sysreg_far();
    int ec = esr >> 26 & 0x3F;
    if (ec == EC_SVC) { diag_stage(12); syscall(r); return 1; }   /* step 12: a program runs (once) */
    rand_event(esr ^ ticks());
    if (ec == EC_FP && fpu_first_use(cur)) return 1;   /* the instruction runs again */
    int fsc = esr & 0x3C;                              /* translation (4) or permission (0xC) fault, any level */
    if ((ec == EC_DABT_LOW || ec == EC_IABT_LOW) && (fsc == 4 || fsc == 0xC) &&
        vm_fault(far, ec == EC_IABT_LOW ? PROT_EXEC : esr & (1 << 6) ? PROT_WRITE : PROT_READ))
        return 1;                                      /* memory given on first touch (vm.c) */
    diag_fault(4, esr, r->pc, far);                    /* (on a Pi's screen, before step 12: init itself) */
    kprintf("mk: %s (pid %d) killed: %s at %lx (syndrome %lx, address %lx)\n",
            cur->proc->name, cur->proc->pid,
            ec == EC_DABT_LOW ? "bad memory access" : ec == EC_IABT_LOW ? "bad jump" :
            ec == EC_SERROR ? "bus error (SError)" : "exception",
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
