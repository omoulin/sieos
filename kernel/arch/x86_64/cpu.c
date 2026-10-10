/*
 * cpu.c (x86-64) - Setting up each processor: segments, the TSS, interrupts
 * and the SYSCALL instruction; a new thread's first frame. Also trap(),
 * where every interrupt and exception arrives.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"
#include "x86.h"

extern void syscall_entry(void);
#define NSTUB 129
extern const uint64_t isr_table[NSTUB];   /* entry.S: vectors 0-127, then 255 */

volatile int halting;                     /* the machine is stopping: NMIs halt the CPUs */

/* Stop every other CPU (power off, restart, panic): an NMI reaches them
 * even with interrupts off, and the NMI handler halts when `halting` is set. */
void halt_others(void)
{
    halting = 1;
    if (ncpu > 1) ipi_all_others(0x400);
}

/* ---- The GDT (segment descriptors), one per CPU because each has its own
 * TSS. In 64-bit mode segments barely matter: they only say "kernel"
 * (privilege 0) or "user" (privilege 3). SYSCALL and SYSRET require this
 * order: kernel code, kernel data, user data, user code. Selectors are
 * offsets in this table, +3 for user ones (0x1B, 0x23). */
static const uint64_t gdt_template[5] = {
    0,
    0x00AF9A000000FFFF,   /* 0x08 kernel code */
    0x00CF92000000FFFF,   /* 0x10 kernel data */
    0x00CFF2000000FFFF,   /* 0x18 user data */
    0x00AFFA000000FFFF,   /* 0x20 user code */
};                        /* 0x28: the TSS (a 16-byte descriptor) */

/* ---- The IDT, shared by all CPUs: for each interrupt vector, the code to run. */
static struct __attribute__((packed)) {
    uint16_t off0, sel;
    uint8_t  ist, type;               /* type 0x8E: present, kernel-only, interrupt gate */
    uint16_t off1;
    uint32_t off2, zero;
} idt[256];

/* Once, on the first CPU: the IDT, the 8259 PICs moved out of the way
 * (to vectors 32-47; smp.c then masks them for good: the I/O APICs
 * replace them), then its own cpu_t. */
void cpu_init(void)
{
    for (int i = 0; i < NSTUB; i++) {
        int v = i < NSTUB - 1 ? i : VEC_SPURIOUS;
        idt[v] = (typeof(idt[0])){ isr_table[i] & 0xFFFF, 0x08, 0, 0x8E,
                                   isr_table[i] >> 16 & 0xFFFF, isr_table[i] >> 32, 0 };
    }
    outb(0x20, 0x11); outb(0xA0, 0x11);     /* initialise both PICs ... */
    outb(0x21, 32);   outb(0xA1, 40);       /* ... at vectors 32 and 40 ... */
    outb(0x21, 4);    outb(0xA1, 2);        /* ... chained on IRQ 2 */
    outb(0x21, 1);    outb(0xA1, 1);
    outb(0x21, 0xFF); outb(0xA1, 0xFF);

    cpu_t *c = cpu_alloc();
    c->id = 0;
    cpus[ncpu++] = c;
    cpu_setup(c);
#ifdef STACKCHECK
    extern uint64_t boot_stack[], boot_stack_top[];   /* boot.S painted it: this stack, CPU 0's idle stack later */
    c->stack_lo = boot_stack;
    c->stack_hi = boot_stack_top;
#endif
}

/* A CPU's cpu_t: one page, with its TSS at the very end. In the kernel's
 * address space, that page is followed by the I/O permission bitmap: two
 * pages of zeros (every port allowed), shared by all CPUs. The processor
 * looks for the bitmap at TSS + iomap, so for a driver iomap points just
 * past the TSS, into the shared pages; for anyone else it points beyond
 * the TSS's limit: no bitmap, no port. (The byte the processor wants after
 * the bitmap is left out: it only matters for ports 0xFFF8-0xFFFF, which
 * are then refused to drivers too; no device uses them.) 8 KiB saved per CPU. */
_Static_assert(sizeof(cpu_t) + sizeof(tss_t) <= PAGE, "cpu_t and its TSS must share one page");

cpu_t *cpu_alloc(void)
{
    static uint64_t bitmap;
    uint64_t pa = page_alloc(1);
    if (!pa || (!bitmap && !(bitmap = page_alloc(2)))) return 0;
    cpu_t *c = kmap(pa, PAGE, PTE_NX);
    kmap(bitmap, 2 * PAGE, PTE_NX);      /* right after it */
    c->a.tss = (tss_t *)((char *)c + PAGE - sizeof(tss_t));
    return c;
}

/* On every CPU (smp.c calls it on the others): load its GDT, TSS and the
 * IDT, point GS at its cpu_t, and set up SYSCALL. */
void cpu_setup(cpu_t *c)
{
    c->a.self = c;
    memcpy(c->a.gdt, gdt_template, sizeof gdt_template);
    uint64_t t = (uint64_t)c->a.tss, lim = sizeof(tss_t) + 65536 / 8 - 1;   /* the TSS and the bitmap */
    c->a.gdt[5] = (lim & 0xFFFF) | (t & 0xFFFFFF) << 16 | 0x89UL << 40 | (lim >> 16 & 0xF) << 48 | (t >> 24 & 0xFF) << 56;
    c->a.gdt[6] = t >> 32;
    c->a.tss->iomap = 0xFFFF;
    struct __attribute__((packed)) { uint16_t lim; uint64_t base; }
        gdtr = { sizeof c->a.gdt - 1, (uint64_t)c->a.gdt }, idtr = { sizeof idt - 1, (uint64_t)idt };
    asm volatile("lgdt %0; ltr %w1; lidt %2" : : "m"(gdtr), "r"(0x28), "m"(idtr));  /* same 0x08/0x10 as boot: no reload */

    /* The PAT: the memory types the page entries' PWT/PCD bits select.
     * As the power-on default, but entry 1 (PWT alone) = write-combining
     * instead of write-through: frame buffers (VM_WC). Entries: WB, WC,
     * UC-, UC, then the same again. */
    wrmsr(0x277, 0x0007040600070106UL);
    wrmsr(0xC0000101, (uint64_t)c);          /* GS base: this cpu_t */
    wrmsr(0xC0000102, 0);                    /* the user's GS base, swapped in by SWAPGS */
    /* SYSCALL: STAR gives the kernel (0x08) and user (0x10 + 16 = 0x20)
     * segments, LSTAR the entry point, FMASK the flags to clear on entry:
     * IF (interrupts), TF (single-step), DF (direction), AC. */
    wrmsr(0xC0000081, 0x10UL << 48 | 0x08UL << 32);
    wrmsr(0xC0000082, (uint64_t)syscall_entry);
    wrmsr(0xC0000084, 0x40700);
    fpu_cpu_init(c);                         /* floating point and vectors for user mode */
}

/* Called when switching to a thread: its kernel stack, and its I/O rights
 * (drivers get the all-zero bitmap: every port; others none). */
void arch_set_kstack(cpu_t *c, uint64_t top, int io)
{
    c->a.tss->rsp0 = c->a.kstack_top = top;
    c->a.tss->iomap = io ? sizeof(tss_t) : 0xFFFF;
}

/* A new thread's kernel stack: a trap frame that enters user mode at pc
 * with the stack sp and arg in rdi (its first argument), under the
 * registers switch_to pops, then trap_return as switch_to's return address. */
void arch_thread_start(task_t *t, uint64_t pc, uint64_t sp, uint64_t arg)
{
    extern void trap_return(void);       /* entry.S */
    regs_t *r = (regs_t *)(t->kstack + KSTACK) - 1;
    r->rip = pc; r->cs = 0x23; r->rflags = 0x202;  /* 0x202: interrupts on */
    r->rsp = sp; r->ss = 0x1B; r->rdi = arg;
    uint64_t *k = (uint64_t *)r;
    *--k = (uint64_t)trap_return;
    t->ksp = (uint64_t)(k - 6);          /* rbp, rbx, r12-r15: zero */
}

/* The running thread's TLS pointer: the FS base (an MSR). */
void arch_set_tls(cpu_t *c, uint64_t base) { wrmsr(0xC0000100, c->a.fs_loaded = base); }

/* Every interrupt and exception arrives here (from entry.S). The kernel
 * runs with interrupts off, so this only interrupts user mode or idle()
 * (except NMIs, which arrive anywhere). Returns 1 if it took the kernel
 * lock: entry.S then leaves through kernel_exit, which releases it. */
int trap(regs_t *r)
{
    cpu_t *c = this_cpu();
    if (r->vec == 2) {                      /* NMI: a stop request, or hardware trouble */
        if (halting) arch_halt_forever();
        kprintf("mk: NMI on CPU %d\n", c->id);
        return 0;
    }
    if (r->vec == VEC_TLB) { tlb_ack(c); lapic_eoi(); return 0; }   /* no lock: see smp.c */
    uint64_t cr2 = 0;                       /* for a page fault: the faulting address */
    if (r->vec < 32) asm volatile("mov %%cr2, %0" : "=r"(cr2));
    if (r->vec < 32 && !(r->cs & 3))        /* a bug in the kernel (it may hold the lock: don't take it) */
        panic("exception %lu in the kernel at %lx (error %lx, address %lx)\n", r->vec, r->rip, r->err, cr2);
    klock();
    rand_event(r->vec);                     /* the time of every interrupt feeds the random pool */
    if (r->vec == 7 && fpu_first_use(cur)) return 1;   /* #NM: its first floating-point instruction */
    if (r->vec < 32) {                      /* a user program did something wrong: its process dies */
        kprintf("mk: %s (pid %d) killed: exception %lu at %lx (error %lx, address %lx)\n",
                cur->proc->name, cur->proc->pid, r->vec, r->rip, r->err, cr2);
        proc_exit(-1);
    }
    if (r->vec == VEC_SPURIOUS || r->vec < VEC_TIMER) return 1;   /* nothing to acknowledge */
    if (r->vec >= VEC_IRQ0) {               /* a device: masked until its driver has handled it */
        int irq = r->vec - VEC_IRQ0;
        irq_mask(irq);
        lapic_eoi();
        irq_raise(irq);                     /* becomes a message to the driver (ipc.c) */
        return 1;
    }
    lapic_eoi();                            /* the timer, or "reschedule" */
    core_tick(c, r->vec == VEC_TIMER);
    return 1;
}
