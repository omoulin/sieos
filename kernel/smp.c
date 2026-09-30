/*
 * smp.c - Multiprocessor support.
 *
 * CPUs are discovered from the ACPI MADT (GRUB passes the RSDP in the
 * multiboot2 information).  Each CPU runs its local APIC in periodic
 * timer mode for scheduling; legacy device interrupts keep going through
 * the 8259 PIC to the bootstrap processor (virtual wire mode).
 *
 * Application processors are started with the INIT-SIPI-SIPI sequence
 * into a real-mode trampoline (ap_boot.S) copied to AP_TRAMPOLINE.
 *
 * The local APICs are used in the mode the firmware left: xAPIC (memory
 * mapped) or x2APIC (MSRs 0x800 + register / 16), which recent firmware
 * turns on (the Surface Pro 7, most Ice Lake and later laptops); the MADT's
 * x2APIC entries are read too.
 *
 * Concurrency: a big kernel lock serialises all kernel code, so user
 * programs run in parallel on every CPU while the kernel itself keeps
 * its simple uniprocessor structure.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "power.h"
#include "smp.h"
#include "proc.h"
#include "cpu.h"
#include "arch.h"
#include "mm.h"
#include "proc.h"

#define AP_TRAMPOLINE 0x8000

/* Local APIC registers */
#define LAPIC_ID       0x020
#define LAPIC_TPR      0x080
#define LAPIC_EOI      0x0B0
#define LAPIC_SVR      0x0F0
#define LAPIC_ESR      0x280
#define LAPIC_ICR_LO   0x300
#define LAPIC_ICR_HI   0x310
#define LAPIC_LVT_TMR  0x320
#define LAPIC_LINT0    0x350
#define LAPIC_LINT1    0x360
#define LAPIC_LVT_ERR  0x370
#define LAPIC_TMR_INIT 0x380
#define LAPIC_TMR_CUR  0x390
#define LAPIC_TMR_DIV  0x3E0

#define MSR_APIC_BASE 0x1B

struct cpu cpus[NCPU];
int ncpu = 1;
bool lapic_ok;

static volatile uint32_t *lapic;
bool x2apic;                                 /* the firmware left the local APICs in x2APIC mode */
bool smp_want_x2apic;                        /* "x2apic": turn it on when the processor has it */
static uint64_t lapic_phys;
static int apic_ids[NCPU];
static int napic;
static uint32_t lapic_timer_count;
static volatile uint64_t lapic_irqs[NCPU];   /* local timer interrupts per CPU */

static struct spinlock bkl;
static volatile int bkl_owner = -1;

extern char kernel_stack_top[];
extern char ap_trampoline[], ap_trampoline_end[];
extern uint64_t ap_param_cr3, ap_param_stack, ap_param_entry, ap_param_cpu;

/* ------------------------------------------------------------------ */
/* Big kernel lock                                                     */
/* ------------------------------------------------------------------ */

void bkl_lock(void)
{
    struct cpu *c = mycpu();
    c->bkl_waiting = 1;
    spin_lock(&bkl);
    c->bkl_waiting = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* against tlb_shootdown's tlb_flush/bkl_waiting */
    if (c->tlb_flush) {                 /* a shootdown arrived while we waited */
        c->tlb_flush = 0;
        write_cr3(read_cr3());
    }
    bkl_owner = c->id;
}

void bkl_unlock(void)
{
    bkl_owner = -1;
    spin_unlock(&bkl);
}

bool bkl_held(void)
{
    return bkl.locked && bkl_owner == mycpu()->id;
}

/* ------------------------------------------------------------------ */
/* Local APIC                                                          */
/* ------------------------------------------------------------------ */

static inline uint32_t lapic_read(uint32_t reg)
{
    if (x2apic)
        return (uint32_t)rdmsr(0x800 + reg / 16);
    return lapic[reg / 4];
}

static inline void lapic_write(uint32_t reg, uint32_t v)
{
    if (x2apic) {
        wrmsr(0x800 + reg / 16, v);
        return;
    }
    lapic[reg / 4] = v;
    (void)lapic[LAPIC_ID / 4];         /* wait for the write to complete */
}

void lapic_eoi(void)
{
    if (lapic_ok)
        lapic_write(LAPIC_EOI, 0);
}

static int lapic_id(void)
{
    return x2apic ? (int)lapic_read(LAPIC_ID) : (int)(lapic_read(LAPIC_ID) >> 24);
}

void lapic_init(void)
{
    if (!lapic_ok)
        return;
    uint64_t base = rdmsr(MSR_APIC_BASE) | (1 << 11);
    wrmsr(MSR_APIC_BASE, base);
    if (x2apic && !(base & (1 << 10)))
        wrmsr(MSR_APIC_BASE, base | (1 << 10));          /* (an application processor: x2APIC like the boot one) */
    lapic_write(LAPIC_SVR, 0x100 | T_SPURIOUS);
    lapic_write(LAPIC_TPR, 0);
    bool bsp = mycpu()->id == 0;
    lapic_write(LAPIC_LINT0, bsp ? 0x700 : 0x10000);     /* ExtINT (PIC) on the BSP only */
    lapic_write(LAPIC_LINT1, bsp ? 0x400 : 0x10000);     /* NMI */
    lapic_write(LAPIC_LVT_ERR, 0x10000);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_LVT_TMR, 0x10000);
    lapic_eoi();
}

static void wait_ticks(uint64_t n)
{
    if (tsc_hz) {                            /* by the clock: no timer interrupt is needed (boot) */
        uint64_t end = hrtime() + n * (1000000000UL / TIMER_HZ);
        while (hrtime() < end) {
            sti();
            for (int i = 0; i < 100; i++)
                __asm__ volatile("pause");
            cli();
        }
        return;
    }
    uint64_t end = ticks + n;
    while (ticks < end) {
        sti();
        hlt();
        cli();
    }
}

static void udelay(int us)
{
    for (int i = 0; i < us; i++)
        io_wait();
}

/* Measure the APIC timer against the TSC, else the PIT ticks (must run on the BSP). */
static void lapic_timer_calibrate(void)
{
    lapic_write(LAPIC_TMR_DIV, 0x3);                    /* divide by 16 */
    lapic_write(LAPIC_LVT_TMR, 0x10000);
    if (tsc_hz) {                                        /* (there may be no PIT at all) */
        uint64_t t0 = hrtime();
        lapic_write(LAPIC_TMR_INIT, 0xFFFFFFFF);
        uint64_t t1;
        while ((t1 = hrtime()) - t0 < 100000000UL)
            __asm__ volatile("pause");
        uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TMR_CUR);
        lapic_write(LAPIC_TMR_INIT, 0);
        lapic_timer_count = (uint64_t)elapsed * (1000000000UL / TIMER_HZ) / (t1 - t0);
        if (lapic_timer_count < 1000)
            lapic_timer_count = 1000;
        return;
    }
    wait_ticks(1);
    lapic_write(LAPIC_TMR_INIT, 0xFFFFFFFF);
    wait_ticks(10);
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TMR_CUR);
    lapic_write(LAPIC_TMR_INIT, 0);
    lapic_timer_count = elapsed / 10;                   /* per 1/TIMER_HZ second */
    if (lapic_timer_count < 1000)
        lapic_timer_count = 1000;
}

/*
 * The local timer runs one-shot: after each interrupt it is set for the
 * earlier of this CPU's next scheduling tick and the first high-resolution
 * sleeper's deadline (proc.c: hr_wake), so nanosleep does not wait for a
 * tick boundary.
 */
#define TICK_NS (1000000000UL / TIMER_HZ)

bool hr_timers(void)
{
    return lapic_ok && tsc_hz;
}

static void lapic_arm(struct cpu *c, uint64_t now, uint64_t when)
{
    uint64_t ns = when > now ? when - now : 0;
    uint64_t counts = ns * lapic_timer_count / TICK_NS;
    if (counts < 16)
        counts = 16;
    if (counts > 0xFFFFFFFFUL)
        counts = 0xFFFFFFFFUL;
    c->armed_ns = when;
    lapic_write(LAPIC_TMR_INIT, (uint32_t)counts);
}

void lapic_timer_start(void)
{
    if (!lapic_ok)
        return;
    if (!lapic_timer_count)
        lapic_timer_calibrate();
    struct cpu *c = mycpu();
    lapic_write(LAPIC_TMR_DIV, 0x3);
    lapic_write(LAPIC_LVT_TMR, T_LAPIC_TIMER);             /* one-shot */
    uint64_t now = hrtime();
    c->next_tick_ns = now + TICK_NS;
    lapic_arm(c, now, c->next_tick_ns);
    if (c->id == 0 && tsc_hz) {                          /* does it interrupt? (the boot CPU, once) */
        uint64_t end = hrtime() + 50000000UL;
        while (!lapic_irqs[0] && hrtime() < end) {
            sti();
            __asm__ volatile("pause");
            cli();
        }
        if (!lapic_irqs[0])
            kprintf("smp: the local %sAPIC timer does not interrupt (count %u)\n", x2apic ? "x2" : "",
                    lapic_timer_count);
    }
}

void lapic_timer_irq(struct trapframe *tf)
{
    struct cpu *c = mycpu();
    lapic_irqs[c->id]++;
    uint64_t now = hrtime();
    uint64_t hr = hr_wake(now);
    bool tick = now >= c->next_tick_ns;
    if (tick || c->next_tick_ns > now + 2 * TICK_NS)       /* also after the TSC offset was set */
        c->next_tick_ns = now + TICK_NS;
    lapic_arm(c, now, MIN(c->next_tick_ns, hr));
    if (c->id == 0)
        timer_lapic_tick(tf);                           /* the clock and the polls, when there is no PIT */
    power_cpu_tick();                                   /* temperatures, frequencies, the thermal policy */
    if (tick)
        sched_tick(tf);
}

void lapic_timer_hint(uint64_t when)
{
    if (!lapic_ok)
        return;
    struct cpu *c = mycpu();
    if (when < c->armed_ns)
        lapic_arm(c, hrtime(), when);
}

static void lapic_send_ipi(int apic_id, uint32_t low)
{
    if (x2apic) {                                        /* one 64-bit ICR, a 32-bit destination */
        wrmsr(0x830, (uint64_t)(uint32_t)apic_id << 32 | low);
        return;
    }
    lapic_write(LAPIC_ICR_HI, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_ICR_LO, low);
    for (int i = 0; i < 100000 && (lapic_read(LAPIC_ICR_LO) & (1 << 12)); i++)
        __asm__ volatile("pause");
}

void smp_resched(struct cpu *c)
{
    if (lapic_ok && c->online)
        lapic_send_ipi(c->apic_id, 0x4000 | T_IPI_RESCHED);
}

void smp_kick_idle(void)
{
    if (!lapic_ok || ncpu < 2)
        return;
    struct cpu *me = mycpu();
    for (int i = 0; i < ncpu; i++) {
        struct cpu *c = &cpus[i];
        if (c != me && c->online && c->lwp == c->idle)
            lapic_send_ipi(c->apic_id, 0x4000 | T_IPI_RESCHED);
    }
}

/*
 * Page tables of pml4 changed (copy-on-write, munmap, mprotect): flush this
 * CPU and every other CPU running an LWP of that address space.  A CPU in
 * user mode reloads CR3 from the IPI; one waiting for the big kernel lock
 * does it when it gets the lock.  Called with the lock held.
 */
void tlb_shootdown(uint64_t pml4)
{
    if (read_cr3() == pml4)
        write_cr3(pml4);
    if (!lapic_ok || ncpu < 2)
        return;
    struct cpu *me = mycpu();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* the page-table stores before the CPU scan */
    for (int i = 0; i < ncpu; i++) {
        struct cpu *c = &cpus[i];
        struct lwp *l = c->lwp;
        if (c == me || !c->online || !l || l->is_idle || l->proc->pml4 != pml4)
            continue;
        c->tlb_flush = 1;
        lapic_send_ipi(c->apic_id, 0x4000 | T_IPI_TLB);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        while (c->tlb_flush && !c->bkl_waiting)
            __asm__ volatile("pause");
    }
}

/* ------------------------------------------------------------------ */
/* ACPI                                                                */
/* ------------------------------------------------------------------ */

struct acpi_sdt {
    char sig[4];
    uint32_t length;
    uint8_t revision, checksum;
    char oem[6], oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
} __attribute__((packed));

struct acpi_rsdp {
    char sig[8];
    uint8_t checksum;
    char oem[6];
    uint8_t revision;
    uint32_t rsdt;
    uint32_t length;
    uint64_t xsdt;
    uint8_t xchecksum, reserved[3];
} __attribute__((packed));

/* I/O APICs and ISA interrupt overrides from the MADT (for ioapic.c) */
struct madt_ioapic madt_ioapics[MADT_MAX_IOAPIC];
int madt_nioapic;
struct madt_iso madt_isos[MADT_MAX_ISO];
int madt_niso;

static void parse_madt(struct acpi_sdt *madt)
{
    uint8_t *p = (uint8_t *)madt + sizeof(*madt);
    lapic_phys = *(uint32_t *)p;
    uint8_t *end = (uint8_t *)madt + madt->length;
    p += 8;                                              /* lapic addr + flags */
    while (p + 2 <= end && p[1] >= 2) {
        if ((p[0] == 0 || (p[0] == 9 && p[1] >= 16)) && napic < NCPU) {   /* processor local (x2)APIC */
            uint32_t flags = *(uint32_t *)(p + (p[0] == 0 ? 4 : 8));
            int id = p[0] == 0 ? p[3] : (int)*(uint32_t *)(p + 4);
            bool dup = false;
            for (int k = 0; k < napic; k++)
                dup |= apic_ids[k] == id;
            if ((flags & 3) && !dup && !(p[0] == 0 && id == 0xFF))
                apic_ids[napic++] = id;
        } else if (p[0] == 5) {                          /* 64-bit LAPIC override */
            lapic_phys = *(uint64_t *)(p + 4);
        } else if (p[0] == 1 && madt_nioapic < MADT_MAX_IOAPIC) {   /* I/O APIC */
            madt_ioapics[madt_nioapic].id = p[2];
            madt_ioapics[madt_nioapic].addr = *(uint32_t *)(p + 4);
            madt_ioapics[madt_nioapic++].gsi_base = *(uint32_t *)(p + 8);
        } else if (p[0] == 2 && madt_niso < MADT_MAX_ISO) {          /* interrupt source override */
            madt_isos[madt_niso].irq = p[3];
            madt_isos[madt_niso].gsi = *(uint32_t *)(p + 4);
            madt_isos[madt_niso++].flags = *(uint16_t *)(p + 8);
        }
        p += p[1];
    }
}

uint32_t acpi_pm_timer_port;
bool acpi_pm_timer_32;
uint64_t acpi_hpet_base;

/* The tables of the RSDT/XSDT, and the DSDT (for acpi_table). */
#define ACPI_MAX_TABLES 64
static struct acpi_sdt *acpi_tables[ACPI_MAX_TABLES];
static int acpi_ntables;

static void acpi_add(uint64_t pa)
{
    if (!pa || pa >= DIRECT_MAP_SIZE || acpi_ntables == ACPI_MAX_TABLES)
        return;
    struct acpi_sdt *t = P2V(pa);
    if (t->length < sizeof(*t) || pa + t->length > DIRECT_MAP_SIZE)
        return;
    acpi_tables[acpi_ntables++] = t;
}

const void *acpi_table(const char *sig, int n, uint32_t *len)
{
    for (int i = 0; i < acpi_ntables; i++)
        if (!memcmp(acpi_tables[i]->sig, sig, 4) && n-- == 0) {
            *len = acpi_tables[i]->length;
            return acpi_tables[i];
        }
    return NULL;
}

void acpi_init(uint64_t mb_info_phys)
{
    struct acpi_rsdp *rsdp = NULL;
    uint32_t total = *(uint32_t *)P2V(mb_info_phys);
    uint8_t *p = (uint8_t *)P2V(mb_info_phys) + 8, *end = (uint8_t *)P2V(mb_info_phys) + total;
    while (p < end) {
        uint32_t type = ((uint32_t *)p)[0], size = ((uint32_t *)p)[1];
        if (type == 0)
            break;
        if ((type == 14 || type == 15) && (!rsdp || type == 15))
            rsdp = (struct acpi_rsdp *)(p + 8);
        p += (size + 7) & ~7;
    }
    if (!rsdp || memcmp(rsdp->sig, "RSD PTR ", 8) != 0)
        return;

    bool xsdt = rsdp->revision >= 2 && rsdp->xsdt;
    uint64_t root_pa = xsdt ? rsdp->xsdt : rsdp->rsdt;
    if (root_pa >= DIRECT_MAP_SIZE)
        return;
    struct acpi_sdt *root = P2V(root_pa);
    int n = (root->length - sizeof(*root)) / (xsdt ? 8 : 4);
    for (int i = 0; i < n; i++) {
        uint64_t pa = xsdt ? ((uint64_t *)(root + 1))[i] : ((uint32_t *)(root + 1))[i];
        if (pa >= DIRECT_MAP_SIZE)
            continue;
        struct acpi_sdt *t = P2V(pa);
        acpi_add(pa);
        if (memcmp(t->sig, "APIC", 4) == 0)
            parse_madt(t);
        if (memcmp(t->sig, "FACP", 4) == 0 && t->length >= 116) {        /* the PM timer */
            uint8_t *f = (uint8_t *)t;
            acpi_pm_timer_port = *(uint32_t *)(f + 76);
            acpi_pm_timer_32 = *(uint32_t *)(f + 112) & (1U << 8);
            if (t->length >= 220 && f[208] == 1 && *(uint64_t *)(f + 212))  /* X_PM_TMR_BLK, in I/O space */
                acpi_pm_timer_port = (uint32_t)*(uint64_t *)(f + 212);
        }
        if (memcmp(t->sig, "HPET", 4) == 0 && t->length >= 56 && ((uint8_t *)t)[40] == 0)
            acpi_hpet_base = *(uint64_t *)((uint8_t *)t + 44);
        if (memcmp(t->sig, "FACP", 4) == 0 && t->length >= 44) {         /* the DSDT: X_DSDT, else DSDT */
            uint8_t *f = (uint8_t *)t;
            uint64_t dsdt = t->length >= 148 ? *(uint64_t *)(f + 140) : 0;
            acpi_add(dsdt ? dsdt : *(uint32_t *)(f + 40));
        }
    }
    x2apic = napic > 0 && (rdmsr(MSR_APIC_BASE) & (1 << 10));   /* EXTD: the firmware chose x2APIC */
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (napic > 0 && !x2apic && smp_want_x2apic && (c & (1U << 21))) {   /* "x2apic" on the command line */
        wrmsr(MSR_APIC_BASE, rdmsr(MSR_APIC_BASE) | (1 << 11) | (1 << 10));
        x2apic = true;
    }
    if (napic > 0 && lapic_phys && lapic_phys < DIRECT_MAP_SIZE) {
        vmm_set_uncached(lapic_phys);
        lapic = P2V(lapic_phys);
        lapic_ok = true;
    } else if (x2apic) {
        lapic_ok = true;                                 /* (no MMIO needed) */
    }
}

/* ------------------------------------------------------------------ */
/* CPU bring-up                                                        */
/* ------------------------------------------------------------------ */

void cpu_early_init(void)
{
    struct cpu *c = &cpus[0];
    memset(c, 0, sizeof(*c));
    c->id = 0;
    c->online = 1;
    c->kstack_top = (uint64_t)kernel_stack_top;
    gdt_init(c);
    cpu_features_init(true);
}

void ap_main(struct cpu *c)
{
    gdt_init(c);
    cpu_features_init(false);
    idt_load();
    write_cr3(kernel_pml4_phys);
    lapic_init();
    lapic_timer_start();
    proc_init_cpu(c);
    c->online = 1;
    tsc_sync_slave(c);                              /* with the boot CPU, which waits for us */
    bkl_lock();
    cpu_idle();
}

void smp_boot(void)
{
    if (!lapic_ok)
        return;
    cpus[0].apic_id = lapic_id();
    if (napic < 2)
        return;

    kprintf("smp: starting %d more CPU%s (%sAPIC)\n", napic - 1, napic > 2 ? "s" : "", x2apic ? "x2" : "x");
    size_t len = ap_trampoline_end - ap_trampoline;
    memcpy(P2V(AP_TRAMPOLINE), ap_trampoline, len);
    uint8_t *tramp = P2V(AP_TRAMPOLINE);
#define PARAM(sym) (*(uint64_t *)(tramp + ((char *)&sym - ap_trampoline)))

    vmm_identity_map(true);
    for (int i = 0; i < napic && ncpu < NCPU; i++) {
        if (apic_ids[i] == cpus[0].apic_id)
            continue;
        struct cpu *c = &cpus[ncpu];
        memset(c, 0, sizeof(*c));
        c->id = ncpu;
        c->apic_id = apic_ids[i];
        uint64_t stack = pmm_alloc_contig(KSTACK_SIZE / PAGE_SIZE);
        if (!stack)
            break;
        c->kstack_top = (uint64_t)P2V(stack) + KSTACK_SIZE;

        PARAM(ap_param_cr3) = kernel_pml4_phys;
        PARAM(ap_param_stack) = c->kstack_top;
        PARAM(ap_param_entry) = (uint64_t)ap_main;
        PARAM(ap_param_cpu) = (uint64_t)c;

        lapic_send_ipi(c->apic_id, 0x4500);              /* INIT */
        wait_ticks(1);
        for (int k = 0; k < 2 && !c->online; k++) {
            lapic_send_ipi(c->apic_id, 0x4600 | (AP_TRAMPOLINE >> 12));   /* STARTUP */
            udelay(200);
        }
        for (int k = 0; k < 20 && !c->online; k++)
            wait_ticks(1);
        if (c->online) {
            if (!tsc_sync_master())                      /* (it runs: it keeps its slot, without an offset) */
                kprintf("smp: CPU with APIC id %d did not answer the clock synchronisation\n", c->apic_id);
            ncpu++;
        }
        else
            kprintf("smp: CPU with APIC id %d did not start\n", c->apic_id);
    }
    vmm_identity_map(false);
#undef PARAM
}
