/*
 * smp.h - Per-CPU state, spinlocks, the big kernel lock and the local APIC.
 */
#ifndef SIEOS_SMP_H
#define SIEOS_SMP_H

#include "kernel.h"

#define NCPU 16

#define T_LAPIC_TIMER 0x40
#define T_IPI_RESCHED 0x41
#define T_IPI_TLB     0x42           /* reload CR3 (another CPU changed our page tables) */
#define T_SPURIOUS    0xFF

struct proc;
struct lwp;

struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct cpu {
    struct cpu *self;              /* must be first: read through %gs:0 */
    uint64_t syscall_scratch;      /* %gs:8  user rsp while entering via syscall */
    uint64_t syscall_rsp0;         /* %gs:16 kernel stack top of the current process */
    int id;                        /* logical CPU number */
    int apic_id;
    volatile int online;
    struct lwp *lwp;               /* LWP running on this CPU */
    struct lwp *idle;              /* this CPU's idle LWP */
    uint64_t slice_start;          /* tick at which the current slice began */
    bool slice_expired;            /* the running LWP used up its quantum */
    int rr;                        /* round-robin scan position */
    uint64_t busy_ticks, idle_ticks;
    bool offline;               /* p_online(P_OFFLINE): runs only its idle LWP */
    volatile int tlb_flush;        /* page tables changed: reload CR3 */
    volatile int bkl_waiting;      /* spinning for the big kernel lock */
    uint64_t kstack_top;           /* idle / boot stack */
    int64_t tsc_off;               /* added to this CPU's TSC to match the boot CPU's */
    uint64_t next_tick_ns;         /* hrtime of this CPU's next scheduling tick */
    uint64_t armed_ns;             /* hrtime the local timer is set to fire at */
    volatile bool need_resched;    /* a higher-priority LWP is runnable: reschedule before user mode */
    uint64_t gdt[7];
    struct tss tss;
};

extern struct cpu cpus[NCPU];

/* timer.c: align an application processor's TSC with the boot CPU's */
void tsc_sync_master(void);
void tsc_sync_slave(struct cpu *c);
extern int ncpu;
extern bool lapic_ok;

static inline struct cpu *mycpu(void)
{
    struct cpu *c;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(c));
    return c;
}

/* spinlocks (callers run with interrupts disabled) */
struct spinlock {
    volatile uint32_t locked;
    volatile int cpu;
};

static inline void spin_lock(struct spinlock *l)
{
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
        while (l->locked)
            __asm__ volatile("pause");
}

static inline void spin_unlock(struct spinlock *l)
{
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

/* The big kernel lock: held by a CPU whenever it executes kernel code
 * (other than the trap entry/exit paths and the idle loop's hlt). */
void bkl_lock(void);
void bkl_unlock(void);
bool bkl_held(void);

/* MADT: I/O APICs and interrupt source overrides (smp.c, used by ioapic.c) */
#define MADT_MAX_IOAPIC 4
#define MADT_MAX_ISO    16
struct madt_ioapic { uint8_t id; uint32_t addr, gsi_base; };
struct madt_iso { uint8_t irq; uint32_t gsi; uint16_t flags; };   /* flags: polarity bits 0-1, trigger 2-3 */
extern struct madt_ioapic madt_ioapics[MADT_MAX_IOAPIC];
extern int madt_nioapic;
extern struct madt_iso madt_isos[MADT_MAX_ISO];
extern int madt_niso;

/* ioapic.c */
bool ioapic_init(void);             /* route device interrupts through the I/O APIC (PIC masked) */
extern bool ioapic_ok;
void ioapic_route(int irq, int dest_apic);     /* ISA/PCI line irq -> vector IRQ_BASE + irq */
void ioapic_mask(int irq);

/* smp.c */
void cpu_early_init(void);          /* BSP per-CPU data (before anything else) */
void acpi_init(uint64_t mb_info_phys);
/* The n-th ACPI table with this signature ("DSDT", "SSDT", ...), header included; NULL if none. */
const void *acpi_table(const char *sig, int n, uint32_t *len);
void lapic_init(void);
void lapic_timer_start(void);
struct trapframe;
void lapic_timer_irq(struct trapframe *tf);  /* the local timer fired */
void lapic_timer_hint(uint64_t when_ns);     /* fire no later than when_ns (an hrtime) */
bool hr_timers(void);                        /* the local timers wake sleepers at ns precision */
void lapic_eoi(void);
void smp_boot(void);
void smp_kick_idle(void);
void smp_resched(struct cpu *c);             /* have c reschedule */           /* wake idle CPUs to look for work */
void tlb_shootdown(uint64_t pml4);   /* every CPU using pml4 drops its TLB entries */
void ap_main(struct cpu *c) __attribute__((noreturn));

#define MSR_FS_BASE        0xC0000100
#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

#endif
