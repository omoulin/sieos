/*
 * smp.c (arm64) - Processors, the interrupt controller and time.
 *
 *  - The GIC (version 2, "GIC-400" on the Raspberry Pi 4 and 5; QEMU's virt
 *    with gic-version=2): a "distributor" collects the devices' interrupt
 *    lines and sends each to the CPU we choose; each CPU's "CPU interface"
 *    hands it the interrupt (read IAR: which one) and is told when it is
 *    handled (write EOIR). CPUs interrupt each other with "software
 *    generated interrupts" (SGIs): our kick and halt.
 *  - Time: the generic timer, the same on every ARMv8 CPU: a counter
 *    (CNTVCT_EL0) at a fixed rate (CNTFRQ_EL0), and per CPU a comparator
 *    that interrupts when the counter reaches it (CNTV_CVAL_EL0): exactly
 *    a one-shot timer. Tickless: armed only when something must happen.
 *  - The other CPUs: started with PSCI CPU_ON (QEMU, the Pi 5) or the
 *    "spin table" (the Pi 4's firmware: write the entry address where the
 *    device tree says, then "sev"); each enters boot.S's secondary_start.
 *  - TLB: invalidations are broadcast by the hardware ("tlbi ...is", mmu.c):
 *    no shootdown interrupts.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"
#include "arm64.h"
#include "mk/fdt.h"

/* ---- The GIC's registers (offsets in 32-bit words). */
static volatile uint32_t *gicd, *gicc;
enum { D_CTLR = 0, D_TYPER = 1, D_ISENABLER = 0x100 / 4, D_ICENABLER = 0x180 / 4, D_ICPENDR = 0x280 / 4,
       D_IPRIORITYR = 0x400 / 4, D_ITARGETSR = 0x800 / 4, D_ICFGR = 0xC00 / 4, D_SGIR = 0xF00 / 4 };
enum { C_CTLR = 0, C_PMR = 1, C_BPR = 2, C_IAR = 3, C_EOIR = 4 };
static int nlines;                         /* interrupt ids 0 .. nlines-1 */

void gic_sgi(cpu_t *c, int id)
{
    asm volatile("dsb ishst" : : : "memory");    /* what we wrote first is visible to it */
    gicd[D_SGIR] = (uint32_t)c->a.gic_mask << 16 | id;
}

/* Send SPI irq to a CPU (masked until irq_unmask). */
int irq_route(int irq, int cpu)
{
    int id = GIC_SPI0 + irq;
    if (id >= nlines) return -EINVAL;
    volatile uint8_t *t = (volatile uint8_t *)&gicd[D_ITARGETSR];
    t[id] = cpus[cpu]->a.gic_mask;
    return 0;
}
/* Lines are edge-triggered unless a driver says level (IRQ_LEVEL): a
 * device that keeps its line up until handled (virtio, the PL011). */
void irq_set_level(int irq)
{
    int id = GIC_SPI0 + irq;
    if (id < nlines) gicd[D_ICFGR + id / 16] &= ~(2u << (id % 16 * 2));
}
void irq_mask(int irq)   { int id = GIC_SPI0 + irq; if (id < nlines) gicd[D_ICENABLER + id / 32] = 1u << id % 32; }
void irq_unmask(int irq) { int id = GIC_SPI0 + irq; if (id < nlines) gicd[D_ISENABLER + id / 32] = 1u << id % 32; }

/* This CPU's part: its own lines (SGIs, its timer: "banked" registers),
 * its interface on, and its bit for targets (read back from ITARGETSR0). */
void gic_cpu_init(cpu_t *c)
{
    gicd[D_ICENABLER] = 0xFFFF0000;               /* its per-CPU lines off ... */
    for (int i = 0; i < 8; i++) gicd[D_IPRIORITYR + i] = 0xA0A0A0A0;
    gicd[D_ISENABLER] = 1u << SGI_KICK | 1u << SGI_HALT | 1u << PPI_VTIMER;   /* ... but these */
    c->a.gic_mask = *(volatile uint8_t *)&gicd[D_ITARGETSR];
    gicc[C_PMR] = 0xF0;                           /* every priority we use gets through */
    gicc[C_BPR] = 0;
    gicc[C_CTLR] = 1;
    c->armed = NEVER;
}

/* ---- Time: ticks() is the counter; conversions use fixed-point multipliers. */
static uint64_t freq, cnt0, ns_mul, tk_mul;
uint64_t now_ns(void)             { return (unsigned __int128)(ticks() - cnt0) * ns_mul >> 32; }
uint64_t ns_to_ticks(uint64_t ns) { return (unsigned __int128)ns * tk_mul >> 32; }

/* This CPU's timer: fire at counter value d (NEVER: off). */
void arch_timer_set(uint64_t d)
{
    if (d == NEVER) { asm volatile("msr cntv_ctl_el0, xzr; isb"); return; }
    uint64_t on = 1;
    asm volatile("msr cntv_cval_el0, %0; msr cntv_ctl_el0, %1; isb" : : "r"(d), "r"(on));
}

/* Wake CPU c (halted in idle) or make it reschedule. */
void arch_kick(cpu_t *c) { gic_sgi(c, SGI_KICK); }

/* The hardware broadcasts TLB invalidations (mmu.c): only wait for them. */
void tlb_shootdown(proc_t *p) { (void)p; asm volatile("dsb ish" : : : "memory"); }
void tlb_ack(cpu_t *c) { (void)c; }

/* ---- Every interrupt, from a program or from idle (entry.S). Returns 1
 * if it took the kernel lock (then kernel_exit releases it). */
int trap_irq(regs_t *r)
{
    (void)r;
    cpu_t *c = this_cpu();
    uint32_t iar = gicc[C_IAR], id = iar & 0x3FF;
    if (id >= 1020) return 0;                     /* spurious: nothing pending any more */
    if (id == SGI_HALT) {
        gicc[C_EOIR] = iar;
        if (halting) arch_halt_forever();
        return 0;
    }
    klock();
    rand_event(iar ^ ticks());                    /* every interrupt's time feeds the random pool */
    if (id == PPI_VTIMER) {
        asm volatile("msr cntv_ctl_el0, xzr; isb");   /* it stays up until changed: off; kernel_exit re-arms */
        gicc[C_EOIR] = iar;
        core_tick(c, 1);
    } else if (id >= GIC_SPI0) {                  /* a device: masked until its drivers have handled it */
        int irq = id - GIC_SPI0;
        irq_mask(irq);
        gicc[C_EOIR] = iar;
        irq_raise(irq);
    } else {                                      /* SGI_KICK: "reschedule" */
        gicc[C_EOIR] = iar;
        core_tick(c, 0);
    }
    return 1;
}

/* On the first CPU: the GIC (from the device tree) and time. */
void smp_init(void)
{
    int n = fdt_find(fdt, -1, "arm,gic-400");
    if (n < 0) n = fdt_find(fdt, -1, "arm,cortex-a15-gic");
    uint64_t da, ds, ca, cs;
    if (n < 0 || fdt_reg(fdt, n, 0, &da, &ds) || fdt_reg(fdt, n, 1, &ca, &cs))
        panic("no GICv2 interrupt controller in the device tree\n");
    gicd = kmap_dev(da, 0x1000);
    gicc = kmap_dev(ca, 0x2000);
    forbid[nforbid++] = (range_t){ da, da + 0x1000 };
    forbid[nforbid++] = (range_t){ ca, ca + 0x2000 };
    nlines = ((gicd[D_TYPER] & 0x1F) + 1) * 32;
    if (nlines > GIC_SPI0 + MAXIRQ) nlines = GIC_SPI0 + MAXIRQ;
    gicd[D_CTLR] = 0;
    for (int i = GIC_SPI0; i < nlines; i += 32) { gicd[D_ICENABLER + i / 32] = ~0u; gicd[D_ICPENDR + i / 32] = ~0u; }
    for (int i = GIC_SPI0; i < nlines; i += 4) gicd[D_IPRIORITYR + i / 4] = 0xA0A0A0A0;
    for (int i = GIC_SPI0; i < nlines; i += 16) gicd[D_ICFGR + i / 16] = 0xAAAAAAAA;   /* edge */
    gicd[D_CTLR] = 1;
    gic_cpu_init(cpus[0]);
    diag_stage(9);                                /* step 9: interrupt controller */

    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    cnt0 = ticks();
    ns_mul = (1000000000UL << 32) / freq;
    tk_mul = (freq << 32) / 1000000000UL;
    diag_stage(10);                               /* step 10: timer */
    kprintf("mk: generic timer %lu.%lu MHz, GICv2 with %d lines\n", freq / 1000000, freq / 100000 % 10, nlines);
}

/* ---- Starting the other CPUs. One at a time, each with this start
 * block, which boot.S's secondary_start reads (MMU off: physically). */
extern char secondary_start[], secondary_spin[];   /* boot.S */
struct { uint64_t stack_top; cpu_t *cpu; uint64_t phys_base; } ap_boot __attribute__((aligned(64)));
static uint64_t kphys(void *v) { return (uint64_t)v - KVMA + phys_base; }   /* the image's addresses */

/* Where each new CPU arrives in C, at its high address, on its own stack. */
void ap_main(cpu_t *c)
{
    cpu_setup(c);
    arch_as_load(c->cur_as = kernel_as);   /* leave boot.S's identity map */
    gic_cpu_init(c);
    c->online = 1;
    klock();
    idle();
}

void smp_start(void)
{
    int cpus_node = fdt_path(fdt, "/cpus");
    for (int d = 1, n = cpus_node; cpus_node >= 0 && (n = fdt_next(fdt, n, &d)) >= 0 && d > 1; ) {
        const char *t = fdt_prop(fdt, n, "device_type", 0);
        uint64_t mpidr, sz;
        if (d != 2 || !t || strcmp(t, "cpu") || fdt_reg(fdt, n, 0, &mpidr, &sz)) continue;
        if (mpidr == cpus[0]->a.mpidr || ncpu >= MAXCPU) continue;
        const char *how = fdt_prop(fdt, n, "enable-method", 0);
        cpu_t *c = cpu_alloc();
        uint64_t stack = page_alloc(KSTACK / PAGE);      /* its idle stack */
        if (!c || !stack) break;
#ifdef STACKCHECK
        stack_paint(c->stack_lo = P2V(stack), c->stack_hi = (uint64_t *)P2V(stack) + KSTACK / 8);
#endif
        c->id = ncpu;
        c->a.mpidr = mpidr;
        ap_boot.stack_top = (uint64_t)P2V(stack) + KSTACK;
        ap_boot.cpu = c;
        ap_boot.phys_base = phys_base;
        /* the new CPU reads the block with its caches off: write it to memory */
        asm volatile("dc civac, %0; dsb sy" : : "r"(&ap_boot) : "memory");
        if (how && !strcmp(how, "spin-table")) {
            int len;
            const void *ra = fdt_prop(fdt, n, "cpu-release-addr", &len);
            if (!ra) { kprintf("mk: CPU %lx: no release address\n", mpidr); continue; }
            volatile uint64_t *rel = kmap_dev(fdt_cells(ra, len / 4) & ~7UL, 8);
            *rel = kphys(secondary_spin);        /* (the spin table sets no x0: this entry finds ap_boot) */
            asm volatile("dsb sy; sev");
        } else if (!psci || psci(0xC4000003, mpidr, kphys(secondary_start), kphys(&ap_boot)) != 0) {
            kprintf("mk: CPU %lx did not accept to start\n", mpidr);
            continue;
        }
        for (uint64_t end = ticks() + ns_to_ticks(2000000000UL); !c->online && ticks() < end; ) arch_relax();
        if (!c->online) { kprintf("mk: CPU %lx did not start\n", mpidr); continue; }
        cpus[ncpu++] = c;                /* (its idle() takes the kernel lock after us) */
    }
    kprintf("mk: %d CPU%s running\n", ncpu, ncpu > 1 ? "s" : "");
    diag_stage(11);                              /* step 11: the other CPUs started (or given up) */
}
