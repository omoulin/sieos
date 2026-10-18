/*
 * smp.c (x86-64) - Processors, interrupt controllers and time.
 *
 *  - The ACPI tables, left in memory by the firmware, list the processors
 *    and the I/O APICs (the "MADT" table, signature "APIC").
 *  - Each processor has a "local APIC": its own interrupt controller, with
 *    a timer and a way to send interrupts to the other processors ("IPIs").
 *  - The "I/O APICs" collect the devices' interrupt lines and send each one,
 *    as a vector, to the processor we choose. (The old 8259 PICs, which can
 *    only reach the first processor, are switched off.)
 *  - Time comes from the TSC, a counter in every processor that ticks at a
 *    fixed rate (measured once against the PIT, the PC's old timer chip).
 *  - Tickless: a processor's timer is set only when something must happen
 *    at a given time: the end of the running thread's time slice, when
 *    other threads wait for a processor, or a sleeping thread's wake-up.
 *    An idle processor with nothing to wait for sleeps until an interrupt,
 *    however long that is: no periodic tick wakes it.
 *  - At power-on only the first processor runs. Each other one is woken
 *    with the INIT-SIPI-SIPI sequence and starts in 16-bit real mode at a
 *    page we choose below 1 MiB (0x8000), where we copy ap.S's
 *    "trampoline": it climbs to 64-bit mode and calls ap_main().
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"
#include "x86.h"

static volatile uint32_t *lapic;   /* the local APIC's registers (the same address on every CPU) */
static int x2;                     /* x2APIC mode: the registers are MSRs (0x800 + offset / 16), ids 32 bits */
static uint32_t apic_ids[MAXCPU];
static int napic;

enum { L_ID = 0x20, L_EOI = 0xB0, L_SVR = 0xF0, L_ICRLO = 0x300, L_ICRHI = 0x310,
       L_TIMER = 0x320, L_LINT0 = 0x350, L_LINT1 = 0x360, L_TINIT = 0x380, L_TCUR = 0x390, L_TDIV = 0x3E0 };
static uint32_t lr(int reg)             { return x2 ? (uint32_t)rdmsr(0x800 + reg / 16) : lapic[reg / 4]; }
static void     lw(int reg, uint32_t v) { if (x2) wrmsr(0x800 + reg / 16, v); else lapic[reg / 4] = v; }

/* x2APIC: on this CPU (each has its own IA32_APIC_BASE, MSR 0x1B: bit 11
 * enabled, bit 10 x2APIC; the firmware may have set both already, and
 * then the memory-mapped registers do not answer at all). Recent
 * processors all have it (CPUID 1: ECX bit 21): used whenever there. */
static void x2_on(void)
{
    uint64_t v = rdmsr(0x1B);
    if (!x2 || v & 0x400) return;
    if (!(v & 0x800)) wrmsr(0x1B, v |= 0x800);   /* (disabled -> x2APIC directly is not allowed) */
    wrmsr(0x1B, v | 0x400);
}

void lapic_eoi(void) { lw(L_EOI, 0); }

/* In x2APIC mode the command register is one 64-bit MSR (the destination
 * in the high half) and there is no "still sending" bit; its WRMSR does not
 * wait for earlier stores: a fence first, so the target sees them. */
static void x2_icr(uint32_t dest, uint32_t cmd)
{
    asm volatile("mfence; lfence" : : : "memory");
    wrmsr(0x830, (uint64_t)dest << 32 | cmd);
}

void send_ipi(int apic_id, int vec)
{
    if (x2) { x2_icr((uint32_t)apic_id, (uint32_t)vec); return; }
    lw(L_ICRHI, (uint32_t)apic_id << 24);
    lw(L_ICRLO, vec);                    /* writing the low half sends it */
    while (lr(L_ICRLO) & 1 << 12) asm volatile("pause");   /* until delivered */
}

/* To every other CPU at once ("all excluding self"); mode 0x400 = an NMI,
 * which arrives even when interrupts are off. */
void ipi_all_others(int mode)
{
    if (x2) { x2_icr(0, 3 << 18 | (uint32_t)mode); return; }
    lw(L_ICRHI, 0);
    lw(L_ICRLO, 3 << 18 | mode);
    while (lr(L_ICRLO) & 1 << 12) asm volatile("pause");
}

/* Busy-wait with the PIT's channel 2 (its counter runs at 1.193182 MHz;
 * at most 54 ms), to measure the TSC. Recent PCs may have the PIT switched
 * off (its clock gated): give up after 600 million TSC ticks (0.1-0.6 s
 * whatever the TSC's rate) and say so: -1. */
static int pit_wait(uint32_t us)
{
    uint32_t n = (uint64_t)us * 1193182 / 1000000;
    outb(0x61, inb(0x61) & ~3);          /* channel 2 gate off, speaker off */
    outb(0x43, 0xB0);                    /* channel 2, count once (mode 0) */
    outb(0x42, n); outb(0x42, n >> 8);
    outb(0x61, (inb(0x61) & ~2) | 1);    /* gate on: count down */
    if (inb(0x61) & 0x20) return -1;     /* already high: no PIT there (the port reads 0xFF) */
    uint64_t t = rdtsc();
    while (!(inb(0x61) & 0x20))          /* its output goes high at 0 */
        if (rdtsc() - t > 600000000) return -1;
    return 0;
}

/* Without a PIT: the TSC's rate as the CPU reports it (CPUID 0x15: the
 * crystal's frequency and the ratio; else 0x16: the base frequency, in MHz).
 * 0: it does not say. */
static uint64_t cpuid_tsc_hz(void)
{
    uint32_t r[4], max;
    cpuid(0, r); max = r[0];
    if (max >= 0x15) { cpuid(0x15, r); if (r[0] && r[1] && r[2]) return (uint64_t)r[2] * r[1] / r[0]; }
    if (max >= 0x16) { cpuid(0x16, r); if (r[0] & 0xFFFF) return (uint64_t)(r[0] & 0xFFFF) * 1000000; }
    return 0;
}

/* Else the ACPI power-management timer (3.579545 MHz, at a port the FADT
 * gives; hardware-reduced machines have none), over 10 ms. 0: none. */
static uint64_t pmtimer_tsc_hz(void)
{
    const uint8_t *f = (const uint8_t *)acpi_find("FACP", 0);
    uint32_t len = f ? ((const sdt_t *)f)->len : 0, port = len >= 80 ? *(const uint32_t *)(f + 76) : 0, a, b, l;
    if (!port && len >= 220 && f[208] == 1) port = (uint32_t)*(const uint64_t *)(f + 212);   /* X_PM_TMR_BLK, I/O */
    if (!port || port > 0xFFFF) return 0;
    uint32_t mask = len >= 116 && *(const uint32_t *)(f + 112) >> 8 & 1 ? ~0u : 0xFFFFFF;   /* TMR_VAL_EXT: 32 bits */
    asm volatile("inl %w1, %0" : "=a"(a) : "Nd"((uint16_t)port));
    uint64_t n = 0, t = rdtsc();
    for (l = a; n < 35795; l = b) {
        if (rdtsc() - t > 4000000000UL) return 0;     /* not counting */
        asm volatile("inl %w1, %0" : "=a"(b) : "Nd"((uint16_t)port));
        n += (b - l) & mask;
    }
    return (rdtsc() - t) * 3579545 / n;
}

/* ---- Time. tsc_hz: TSC ticks per second. Conversions use fixed-point
 * multipliers (no 128-bit division in the kernel): ns = tsc * ns_mul >> 32. */
static uint64_t tsc_hz, tsc0, ns_mul, tsc_mul, lapic_mul;
static int tsc_deadline;             /* the timer can be set to a TSC value directly */

uint64_t now_ns(void)       { return (unsigned __int128)(rdtsc() - tsc0) * ns_mul >> 32; }
uint64_t ns_to_ticks(uint64_t ns) { return (unsigned __int128)ns * tsc_mul >> 24; }

/* Set this CPU's timer to fire at TSC value d (NEVER: switch it off). With
 * "TSC deadline" (most CPUs since 2012) the timer takes the TSC value
 * itself; otherwise its count-down is computed from the time left. */
void arch_timer_set(uint64_t d)
{
    if (tsc_deadline) { wrmsr(0x6E0, d == NEVER ? 0 : d); return; }
    if (d == NEVER) { lw(L_TINIT, 0); return; }
    uint64_t now = rdtsc(), n = d > now ? (unsigned __int128)(d - now) * lapic_mul >> 32 : 1;
    lw(L_TINIT, n < 1 ? 1 : n > 0xFFFFFFFF ? 0xFFFFFFFF : n);
}

/* Wake CPU c (halted in idle) or make it reschedule: an inter-processor
 * interrupt, "reschedule". */
void arch_kick(cpu_t *c) { send_ipi(c->a.apic_id, VEC_RESCHED); }

/* ---- TLB shootdown. Threads of one process may run on several CPUs at
 * once, each with copies of the process's page translations in its TLB;
 * and any CPU may be copying a message into or out of its pages (vm_copy).
 * After removing mappings (vm_unmap), every other CPU must flush its TLB
 * and be done with such a copy before the pages are freed. We ask each
 * one (tlb_want), send it an interrupt and wait for its answer (tlb_done).
 * It answers from its interrupt handler, which only runs in user mode or
 * idle, or from a spin loop in the kernel (arch_lock_wait): never in the
 * middle of a copy. Counters rather than a flag: several CPUs may ask at
 * once; each answer covers every request made before its flush. (A
 * process ending uses it too, to reach every CPU: proc_stop.) */
void tlb_ack(cpu_t *c)
{
    uint32_t w = __atomic_load_n(&c->tlb_want, __ATOMIC_ACQUIRE);
    write_cr3(c->cur_as);                /* reloading CR3 flushes the non-global entries */
    __atomic_store_n(&c->tlb_done, w, __ATOMIC_RELEASE);
}

void tlb_shootdown(proc_t *p)
{
    (void)p;
    cpu_t *me = this_cpu();
    int n = ncpus();
    for (int i = 0; i < n; i++) {
        cpu_t *c = cpus[i];
        if (c == me) continue;
        __atomic_add_fetch(&c->tlb_want, 1, __ATOMIC_SEQ_CST);   /* (after the page tables changed) */
        send_ipi(c->a.apic_id, VEC_TLB);
    }
    for (int i = 0; i < n; i++)
        while (cpus[i] != me && __atomic_load_n(&cpus[i]->tlb_done, __ATOMIC_ACQUIRE) != cpus[i]->tlb_want)
            arch_lock_wait(me);          /* (answering others meanwhile: they may wait for us) */
}

/* ---- ACPI: the RSDP ("RSD PTR ") is in the first KiB of the EBDA or in
 * the BIOS area 0xE0000-0xFFFFF; it points to the RSDT (32-bit pointers)
 * or XSDT (64-bit), which list the other tables. */
static uint8_t *rsdp_scan(uint64_t pa, uint64_t len)
{
    for (uint8_t *p = P2V(pa), *e = p + len; p < e; p += 16) {
        uint8_t sum = 0;
        for (int i = 0; i < 20; i++) sum += p[i];
        if (!memcmp(p, "RSD PTR ", 8) && !sum) return p;
    }
    return 0;
}

sdt_t *acpi_find(const char *sig, int nth)          /* the nth table with this signature */
{
    uint8_t *rsdp = x86_rsdp ? P2V(x86_rsdp) : 0;     /* (given by the UEFI loader: no BIOS areas then) */
    if (!rsdp) rsdp = rsdp_scan((uint64_t)*(uint16_t *)P2V(0x40E) << 4, 1024);
    if (!rsdp) rsdp = rsdp_scan(0xE0000, 0x20000);
    if (!rsdp) return 0;
    int x = rsdp[15] >= 2 && *(uint64_t *)(rsdp + 24);  /* ACPI 2+: the XSDT */
    sdt_t *root = P2V(x ? *(uint64_t *)(rsdp + 24) : *(uint32_t *)(rsdp + 16));
    for (uint32_t i = 0; i < (root->len - sizeof *root) / (x ? 8 : 4); i++) {
        sdt_t *t = P2V(x ? ((uint64_t *)root->data)[i] : ((uint32_t *)root->data)[i]);
        if (!memcmp(t->sig, sig, 4) && !nth--) return t;
    }
    return 0;
}

/* SYS_ACPI: the FADT, the DSDT (the FADT says where) then every SSDT, one
 * after the other, for drivers' AML interpreter (user/lib/aml.c: the
 * touchpad, the ACPI server). -> their size (a buffer too small gets nothing). */
long arch_acpi(uint64_t ubuf, uint64_t size)
{
    sdt_t *f = acpi_find("FACP", 0), *t[64];
    uint64_t d = 0, len = 0;
    int n = 0, k = 0;
    if (f) t[n++] = f, d = f->len >= 148 && *(uint64_t *)((uint8_t *)f + 140) ? *(uint64_t *)((uint8_t *)f + 140) : *(uint32_t *)((uint8_t *)f + 40);
    if (d) t[n++] = P2V(d);
    while (n < 64 && (t[n] = acpi_find("SSDT", k++))) n++;
    for (int i = 0; i < n; i++) len += t[i]->len;
    if (size < len) return len;
    for (int i = 0; i < n; ubuf += t[i++]->len)
        if (vm_copy(cur->proc->as, (void *)ubuf, 0, t[i], t[i]->len)) return -EFAULT;
    return len;
}

/* ---- I/O APICs. Each has a window of registers (select, then read or
 * write) and a "redirection entry" per input line: the vector, the target
 * CPU, the line's polarity and trigger mode, and a mask bit. Lines are
 * numbered globally ("GSI"); ISA interrupts 0-15 map to GSIs 0-15 unless
 * the MADT says otherwise ("interrupt source overrides"). */
static struct { volatile uint32_t *r; int base, count; } ioapic[8];
static int nioapic;
static spinlock_t io_lock;               /* (select, then read or write: two accesses) */
static int irq_gsi[16];                  /* ISA IRQ -> GSI */
static uint16_t irq_flags[MAXIRQ];       /* per API irq: polarity (low = 1) | trigger (level = 2) */

static uint32_t io_read(int a, int reg)          { ioapic[a].r[0] = reg; return ioapic[a].r[4]; }
static void     io_write(int a, int reg, uint32_t v) { ioapic[a].r[0] = reg; ioapic[a].r[4] = v; }

/* The I/O APIC and input of an irq ("irq" in the API: an ISA IRQ 0-15, or a GSI 16+). */
static int io_find(int irq, int *pin)
{
    int gsi = irq < 16 ? irq_gsi[irq] : irq;
    for (int a = 0; a < nioapic; a++)
        if (gsi >= ioapic[a].base && gsi < ioapic[a].base + ioapic[a].count) { *pin = gsi - ioapic[a].base; return a; }
    return -1;
}

/* Send irq to a CPU (masked until irq_unmask). PCI lines (GSI 16+) are
 * level-triggered, active low; ISA ones edge, active high, unless overridden. */
int irq_route(int irq, int cpu)
{
    int pin, a = io_find(irq, &pin);
    if (a < 0 || irq >= MAXIRQ) return -EINVAL;
    uint32_t lo = (VEC_IRQ0 + irq) | 1 << 16 | (irq_flags[irq] & 1 ? 1 << 13 : 0) | (irq_flags[irq] & 2 ? 1 << 15 : 0);
    spin_lock(&io_lock);
    uint32_t id = (uint32_t)cpus[cpu]->a.apic_id;
    io_write(a, 0x11 + 2 * pin, (id < 255 ? id : (uint32_t)cpus[0]->a.apic_id) << 24);   /* (8 bits without remapping) */
    io_write(a, 0x10 + 2 * pin, lo);
    spin_unlock(&io_lock);
    return 0;
}
/* A PCI device's line routed by the chipset to an ISA IRQ (IRQ_LEVEL):
 * level-triggered, active high unless an override or the driver (IRQ_LOW:
 * the ACPI tables said so) says low. */
void irq_set_level(int irq, int low) { irq_flags[irq] |= 2 | (low ? 1 : 0); }

static void irq_set_mask(int irq, int m)
{
    int pin, a = io_find(irq, &pin);
    if (a < 0) return;
    spin_lock(&io_lock);
    uint32_t lo = io_read(a, 0x10 + 2 * pin);
    io_write(a, 0x10 + 2 * pin, m ? lo | 1 << 16 : lo & ~(1u << 16));
    spin_unlock(&io_lock);
}
void irq_mask(int irq)   { irq_set_mask(irq, 1); }
void irq_unmask(int irq) { irq_set_mask(irq, 0); }

/* This CPU's local APIC: on, legacy pins off (LINT0: the 8259s, unused
 * now; LINT1: NMIs), and its timer in one-shot or TSC-deadline mode. */
static void lapic_setup(void)
{
    x2_on();
    lw(L_SVR, 0x100 | VEC_SPURIOUS);     /* enable */
    lw(L_LINT0, 0x10000);
    lw(L_LINT1, 0x400);
    lw(L_TDIV, 3);                       /* timer clock = bus clock / 16 */
    lw(L_TIMER, VEC_TIMER | (tsc_deadline ? 2 << 17 : 0));
    this_cpu()->armed = NEVER;
}

/* On the first CPU: find the processors and I/O APICs, measure time, and
 * start its APIC. */
void smp_init(void)
{
    uint64_t base = 0xFEE00000;
    for (int i = 0; i < 16; i++) irq_gsi[i] = i;
    for (int i = 16; i < MAXIRQ; i++) irq_flags[i] = 3;           /* PCI: level, active low */
    sdt_t *madt = acpi_find("APIC", 0);
    if (!madt) panic("no ACPI MADT: cannot find the processors and I/O APICs\n");
    uint32_t r[4];
    cpuid(1, r);
    x2 = rdmsr(0x1B) >> 10 & 1 || r[2] >> 21 & 1;   /* (already on, or there) */
    /* after the header: the APIC address (32 bits), flags (32), then
     * entries {type, length, ...}: 0 = a processor, 1 = an I/O APIC,
     * 2 = an interrupt source override, 5 = a 64-bit APIC address,
     * 9 = a processor with a 32-bit (x2APIC) id. A processor counts if
     * "enabled" (flags bit 0; bit 1 "online capable": one that may be
     * added later, not now); firmware may list one twice (types 0 and 9). */
    base = *(uint32_t *)madt->data;
    for (uint8_t *e = madt->data + 8; e + 2 <= (uint8_t *)madt + madt->len && e[1] >= 2; e += e[1]) {
        uint32_t id = e[0] == 0 ? e[3] : e[0] == 9 ? *(uint32_t *)(e + 4) : ~0u, on = *(uint32_t *)(e + (e[0] ? 8 : 4)) & 1;
        if (id != ~0u && on && (id < 255 || (x2 && e[0] == 9)) && napic < MAXCPU) {
            int k = 0;
            while (k < napic && apic_ids[k] != id) k++;
            if (k == napic) apic_ids[napic++] = id;
        }
        if (e[0] == 1 && nioapic < 8) {
            ioapic[nioapic].r = kmap_dev(*(uint32_t *)(e + 4), 0x20);
            ioapic[nioapic].base = *(uint32_t *)(e + 8);
            ioapic[nioapic].count = (io_read(nioapic, 1) >> 16 & 0xFF) + 1;
            nioapic++;
        }
        if (e[0] == 2 && e[3] < 16) {
            uint16_t f = *(uint16_t *)(e + 8);
            irq_gsi[e[3]] = *(uint32_t *)(e + 4);
            irq_flags[e[3]] = ((f & 3) == 3 ? 1 : 0) | ((f >> 2 & 3) == 3 ? 2 : 0);
        }
        if (e[0] == 5) base = *(uint64_t *)(e + 4);
    }
    /* The ACPI interrupt (SCI: user/acpid): level, active low, unless the MADT said otherwise. */
    sdt_t *fa = acpi_find("FACP", 0);
    int sci = fa ? *(uint16_t *)((uint8_t *)fa + 46) : 0;
    if (sci && sci < 16 && irq_gsi[sci] == sci && !irq_flags[sci]) irq_flags[sci] = 3;
    if (!nioapic) panic("no I/O APIC\n");
    for (int a = 0; a < nioapic; a++)    /* every line masked to start with */
        for (int p = 0; p < ioapic[a].count; p++) io_write(a, 0x10 + 2 * p, 1 << 16);
    outb(0x21, 0xFF); outb(0xA1, 0xFF);  /* the 8259s: all masked, for good */

    if (!x2) lapic = kmap_dev(base, PAGE);
    x2_on();
    cpus[0]->a.apic_id = x2 ? (int)lr(L_ID) : (int)(lr(L_ID) >> 24);
    uint8_t *fx = (uint8_t *)fa;          /* (FADT: IAPC_BOOT_ARCH at 109, flags at 112) */
    kprintf("mk: ACPI: %d CPUs, %d I/O APIC%s, local APIC %s (id %d); FADT boot flags %x, flags %x%s\n",
            napic, nioapic, nioapic > 1 ? "s" : "", x2 ? "x2APIC" : "xAPIC", cpus[0]->a.apic_id,
            fx && fa->len >= 111 ? *(uint16_t *)(fx + 109) : 0, fx && fa->len >= 116 ? *(uint32_t *)(fx + 112) : 0,
            fx && fa->len >= 116 && *(uint32_t *)(fx + 112) >> 20 & 1 ? " (hardware-reduced)" : "");

    /* Measure the TSC (and the APIC timer, for CPUs without TSC deadline)
     * against the PIT over 10 ms (one PIT tick, 0.84 us, is 0.01% of it). */
    cpuid(1, r);
    tsc_deadline = r[2] >> 24 & 1;
    cpuid(0x80000007, r);
    int invariant = r[3] >> 8 & 1;
    lw(L_TDIV, 3);
    lw(L_TIMER, 1 << 16);                /* masked, one-shot */
    lw(L_TINIT, 0xFFFFFFFF);
    uint64_t t = rdtsc();
    if (!pit_wait(10000) && (tsc_hz = (rdtsc() - t) * 100) > 100000000 && tsc_hz < 20000000000UL) ;
    else {                               /* no PIT: the CPU's word or ACPI's timer, and the APIC timer over 10 ms of TSC */
        const char *by = "CPUID";
        if (!(tsc_hz = cpuid_tsc_hz()) && (by = "the ACPI PM timer", !(tsc_hz = pmtimer_tsc_hz())))
            panic("no way to measure time (no PIT, no TSC rate in CPUID, no ACPI PM timer)\n");
        lw(L_TINIT, 0xFFFFFFFF);
        for (t = rdtsc(); rdtsc() - t < tsc_hz / 100; ) asm volatile("pause");
        kprintf("mk: no PIT: TSC rate from %s\n", by);
    }
    uint64_t lapic_hz = (uint64_t)(0xFFFFFFFF - lr(L_TCUR)) * 100;
    lw(L_TINIT, 0);
    tsc0 = rdtsc();
    ns_mul = (1000000000UL << 32) / tsc_hz;
    tsc_mul = (tsc_hz << 24) / 1000000000UL;
    lapic_mul = (lapic_hz << 32) / tsc_hz;
    kprintf("mk: TSC %lu MHz%s, timer: %s, %d I/O APIC%s\n", tsc_hz / 1000000, invariant ? " (invariant)" : "",
            tsc_deadline ? "TSC deadline" : "APIC one-shot", nioapic, nioapic > 1 ? "s" : "");
    lapic_setup();
}

/* ---- Starting the other CPUs. */
extern char ap_start[], ap_end[], ap_cr3[], ap_stack[], ap_cpu[];   /* ap.S */
extern uint64_t boot_pml4[512], pdpt_low[512];                      /* boot.S */
int s3_save(uint64_t *ctx);                                         /* ap.S */
void s3_jump(uint64_t *ctx) __attribute__((noreturn));
static uint64_t tramp_pml4;              /* the trampoline's page table (kept, as its page: for waking up) */
static uint64_t wake_ctx[8], wake_as, wake_tsc;   /* suspend: s3_save's registers, CR3, the TSC */
static int64_t tsc_fix;                  /* after waking: added to each CPU's TSC (it started again from 0) */
static void s3_wake(void) __attribute__((noreturn));

/* Where each new CPU arrives in C, in 64-bit mode, on its own stack (CPU
 * 0 too, waking from a suspend). */
void ap_main(cpu_t *c)
{
    if (c == cpus[0]) s3_wake();
    write_cr3(c->cur_as = kernel_as);    /* leave the trampoline's page table */
    uint64_t cr4;                        /* then keep kernel translations across CR3 changes */
    asm volatile("mov %%cr4, %0; or $0x80, %0; mov %0, %%cr4" : "=r"(cr4));
    if (tsc_fix) wrmsr(0x10, rdtsc() + tsc_fix);
    cpu_setup(c);
    lapic_setup();
    c->online = 1;
    idle();
}

/* The trampoline at TRAMP, for CPU c on that stack. */
static void tramp(cpu_t *c, uint64_t stack)
{
    memcpy(P2V(TRAMP), ap_start, ap_end - ap_start);
    *(uint32_t *)P2V(TRAMP + (ap_cr3 - ap_start)) = tramp_pml4;
    *(uint64_t *)P2V(TRAMP + (ap_stack - ap_start)) = stack;
    *(uint64_t *)P2V(TRAMP + (ap_cpu - ap_start)) = (uint64_t)c;
}
/* INIT, 10 ms, then SIPI: c starts at 0x8000 -> 0: it runs idle(). One CPU
 * at a time: a CPU waiting for its SIPI cannot enter SMM, and the firmware's
 * SMIs then wait for it (up to a second each, on the Surface Pro 7). */
static int ap_boot(cpu_t *c)
{
    tramp(c, c->a.idle_stack);
    send_ipi(c->a.apic_id, 0x4500);
    for (uint64_t t = rdtsc(); rdtsc() - t < tsc_hz / 100; ) asm volatile("pause");   /* (TSC: the PIT may be off) */
    /* A second SIPI if it has not answered after 1 ms (the standard asks
     * for two; one is enough on most machines), then up to 1 s: a real
     * machine may take far longer than QEMU (a CPU that answers after we
     * gave up would run on a trampoline already set up for the next one). */
    for (int s = 0; s < 2 && !c->online; s++) {
        send_ipi(c->a.apic_id, 0x4600 | TRAMP >> 12);
        for (uint64_t end = rdtsc() + ns_to_ticks(s ? 1000000000 : 1000000); !c->online && rdtsc() < end; )
            arch_lock_wait(cpus[0]);    /* (the CPUs already started may ask us for a shootdown) */
    }
    if (!c->online) kprintf("mk: CPU with APIC id %d did not start\n", c->a.apic_id);
    return c->online ? 0 : -1;
}

void smp_start(void)
{
    /* The trampoline needs its own page to be mapped where it runs (at
     * 0x8000, "identity") when it turns paging on: a page table with the
     * kernel's upper half and boot.S's first-GiB identity map. */
    tramp_pml4 = page_alloc(1);
    memcpy((uint64_t *)P2V(tramp_pml4) + 256, boot_pml4 + 256, 256 * 8);
    ((uint64_t *)P2V(tramp_pml4))[0] = ((uint64_t)pdpt_low - KVMA) | PTE_P | PTE_W;
    if (napic > 1) kprintf("mk: starting %d more CPUs\n", napic - 1);
    for (int i = 0; i < napic; i++) {
        if ((int)apic_ids[i] == cpus[0]->a.apic_id) continue;
        if (opt_cpus && ncpu >= opt_cpus) break;
        cpu_t *c = cpu_alloc();
        uint64_t stack = page_alloc(1);  /* its idle stack, 4 KiB (deepest use measured: 600 bytes) */
        if (!c || !stack) break;
#ifdef STACKCHECK
        stack_paint(c->stack_lo = P2V(stack), c->stack_hi = (uint64_t *)P2V(stack) + PAGE / 8);
#endif
        c->id = ncpu;
        c->a.apic_id = (int)apic_ids[i];
        c->a.idle_stack = (uint64_t)P2V(stack) + PAGE;
        if (ap_boot(c)) continue;
        kprintf("mk: CPU %d (APIC id %d) runs\n", ncpu, c->a.apic_id);
        cpus[ncpu] = c;                  /* (it already runs idle(): the others see it from now on) */
        __atomic_store_n(&ncpu, ncpu + 1, __ATOMIC_RELEASE);
    }
}

/* ---- Suspend to RAM (ACPI S3; task.c has stopped the other CPUs). What
 * the sleep loses on CPU 0 is saved: its registers (s3_save), the I/O
 * APICs' entries, its FPU registers (into the thread's area). On waking the
 * firmware jumps to the trampoline in real mode, which climbs to ap_main and
 * s3_wake: the address space, the TSC carried on from where it was (time
 * does not count the sleep), GDT/IDT/TSS/MSRs/PAT/FPU (cpu_setup), the
 * local APIC; then back in arch_suspend, the other CPUs start as at boot,
 * and the I/O APICs' entries come back. */
__attribute__((cold)) static void s3_wake(void)   /* (CPU 0, the trampoline's stack and page table) */
{
    write_cr3(wake_as);
    serial_init();
    uint64_t cr4;
    asm volatile("mov %%cr4, %0; or $0x80, %0; mov %0, %%cr4" : "=r"(cr4));
    int64_t d = (int64_t)(wake_tsc - rdtsc());
    tsc_fix = d > 0 ? d : 0;
    if (tsc_fix) wrmsr(0x10, rdtsc() + tsc_fix);
    uint16_t io = cpus[0]->a.tss->iomap;  /* (the thread's I/O rights: cpu_setup clears them) */
    cpu_setup(cpus[0]);
    cpus[0]->a.tss->iomap = io;
    wrmsr(0xC0000100, cpus[0]->a.fs_loaded);
    lapic_setup();
    s3_jump(wake_ctx);
}
void arch_park(volatile int *park)       /* (another CPU, CPU 0 about to sleep: its caches, then stopped) */
{
    asm volatile("wbinvd" : : : "memory");
    *park = 3;
    arch_halt_forever();
}
__attribute__((cold)) long arch_suspend(int n)
{
    static uint64_t saved[MAXIRQ];
    long r = acpi_sleep(3, TRAMP, 1);
    if (r || !n) return r;
    cpu_t *c = cpus[0];
    spin_lock(&io_lock);
    for (int irq = 1, pin, a; irq < MAXIRQ; irq++)        /* the I/O APICs' entries saved, then all masked */
        if ((a = io_find(irq, &pin)) >= 0) {
            saved[irq] = io_read(a, 0x10 + 2 * pin) | (uint64_t)io_read(a, 0x11 + 2 * pin) << 32;
            io_write(a, 0x10 + 2 * pin, 1 << 16);
        }
    spin_unlock(&io_lock);
    fpu_flush(c, cur);
    uint64_t mtrr = rdmsr(0x2FF);                          /* (the firmware restores the memory types: checked) */
    wake_as = c->cur_as;
    tramp(c, (uint64_t)P2V(TRAMP + PAGE));                /* (its stack: the top of its own page) */
    tsc_fix = 0;
    if (!s3_save(wake_ctx)) {
        wake_tsc = rdtsc();
        r = acpi_sleep(3, TRAMP, 0);                      /* (on waking: s3_save returns 1) */
    }
    outb(0x21, 0xFF); outb(0xA1, 0xFF);                  /* the 8259s: masked again */
    kprintf("mk: %s; TSC %s\n", r ? "suspend failed: still awake" : "awake", tsc_fix ? "carried on" : "kept running");
    if (rdmsr(0x2FF) != mtrr) kprintf("mk: the firmware did not restore the MTRRs (%lx, was %lx): memory may be uncached\n", rdmsr(0x2FF), mtrr);
    for (int i = 1; i < n; i++) { cpus[i]->online = 0; ap_boot(cpus[i]); }
    spin_lock(&io_lock);
    for (int irq = 1, pin, a; irq < MAXIRQ; irq++)
        if ((a = io_find(irq, &pin)) >= 0) { io_write(a, 0x11 + 2 * pin, saved[irq] >> 32); io_write(a, 0x10 + 2 * pin, (uint32_t)saved[irq]); }
    spin_unlock(&io_lock);
    return r;
}
