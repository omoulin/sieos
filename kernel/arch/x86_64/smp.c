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
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"
#include "x86.h"

static volatile uint32_t *lapic;   /* the local APIC's registers (the same address on every CPU) */
static uint8_t apic_ids[MAXCPU];
static int napic;

enum { L_ID = 0x20, L_EOI = 0xB0, L_SVR = 0xF0, L_ICRLO = 0x300, L_ICRHI = 0x310,
       L_TIMER = 0x320, L_LINT0 = 0x350, L_LINT1 = 0x360, L_TINIT = 0x380, L_TCUR = 0x390, L_TDIV = 0x3E0 };
static uint32_t lr(int reg)             { return lapic[reg / 4]; }
static void     lw(int reg, uint32_t v) { lapic[reg / 4] = v; }

void lapic_eoi(void) { lw(L_EOI, 0); }

void send_ipi(int apic_id, int vec)
{
    lw(L_ICRHI, (uint32_t)apic_id << 24);
    lw(L_ICRLO, vec);                    /* writing the low half sends it */
    while (lr(L_ICRLO) & 1 << 12) asm volatile("pause");   /* until delivered */
}

/* To every other CPU at once ("all excluding self"); mode 0x400 = an NMI,
 * which arrives even when interrupts are off. */
void ipi_all_others(int mode)
{
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
 * once, each with copies of the process's page translations in its TLB.
 * After removing mappings, every other CPU running a thread of the process
 * must flush its TLB before the freed pages can be reused. We set its
 * tlb_req, send it an interrupt and wait. It answers at once: from user
 * mode, the interrupt handler flushes without needing the kernel lock (we
 * hold it); a CPU waiting for the kernel lock answers from klock's loop. */
void tlb_ack(cpu_t *c)
{
    write_cr3(c->cur_as);                /* reloading CR3 flushes the non-global entries */
    __atomic_store_n(&c->tlb_req, 0, __ATOMIC_RELEASE);
}

void tlb_shootdown(proc_t *p)
{
    cpu_t *me = this_cpu();
    for (int i = 0; i < ncpu; i++) {
        cpu_t *c = cpus[i];
        if (c == me || !c->running || c->running->proc != p) continue;
        c->tlb_req = 1;
        send_ipi(c->a.apic_id, VEC_TLB);
    }
    for (int i = 0; i < ncpu; i++)
        while (__atomic_load_n(&cpus[i]->tlb_req, __ATOMIC_ACQUIRE)) asm volatile("pause");
}

/* ---- ACPI: the RSDP ("RSD PTR ") is in the first KiB of the EBDA or in
 * the BIOS area 0xE0000-0xFFFFF; it points to the RSDT (32-bit pointers)
 * or XSDT (64-bit), which list the other tables. */
typedef struct __attribute__((packed)) { char sig[4]; uint32_t len; uint8_t rev, sum; char oem[6], table[8];
                                         uint32_t oemrev, creator, crev; uint8_t data[]; } sdt_t;

static uint8_t *rsdp_scan(uint64_t pa, uint64_t len)
{
    for (uint8_t *p = P2V(pa), *e = p + len; p < e; p += 16) {
        uint8_t sum = 0;
        for (int i = 0; i < 20; i++) sum += p[i];
        if (!memcmp(p, "RSD PTR ", 8) && !sum) return p;
    }
    return 0;
}

static sdt_t *acpi_find(const char *sig)
{
    uint8_t *rsdp = x86_rsdp ? P2V(x86_rsdp) : 0;     /* (given by the UEFI loader: no BIOS areas then) */
    if (!rsdp) rsdp = rsdp_scan((uint64_t)*(uint16_t *)P2V(0x40E) << 4, 1024);
    if (!rsdp) rsdp = rsdp_scan(0xE0000, 0x20000);
    if (!rsdp) return 0;
    int x = rsdp[15] >= 2 && *(uint64_t *)(rsdp + 24);  /* ACPI 2+: the XSDT */
    sdt_t *root = P2V(x ? *(uint64_t *)(rsdp + 24) : *(uint32_t *)(rsdp + 16));
    for (uint32_t i = 0; i < (root->len - sizeof *root) / (x ? 8 : 4); i++) {
        sdt_t *t = P2V(x ? ((uint64_t *)root->data)[i] : ((uint32_t *)root->data)[i]);
        if (!memcmp(t->sig, sig, 4)) return t;
    }
    return 0;
}

/* ---- I/O APICs. Each has a window of registers (select, then read or
 * write) and a "redirection entry" per input line: the vector, the target
 * CPU, the line's polarity and trigger mode, and a mask bit. Lines are
 * numbered globally ("GSI"); ISA interrupts 0-15 map to GSIs 0-15 unless
 * the MADT says otherwise ("interrupt source overrides"). */
static struct { volatile uint32_t *r; int base, count; } ioapic[8];
static int nioapic;
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
    io_write(a, 0x11 + 2 * pin, (uint32_t)cpus[cpu]->a.apic_id << 24);
    io_write(a, 0x10 + 2 * pin, lo);
    return 0;
}
/* A PCI device's line routed by the chipset to an ISA IRQ (IRQ_LEVEL):
 * level-triggered, active high (unless an override says otherwise). */
void irq_set_level(int irq) { irq_flags[irq] |= 2; }

static void irq_set_mask(int irq, int m)
{
    int pin, a = io_find(irq, &pin);
    if (a < 0) return;
    uint32_t lo = io_read(a, 0x10 + 2 * pin);
    io_write(a, 0x10 + 2 * pin, m ? lo | 1 << 16 : lo & ~(1u << 16));
}
void irq_mask(int irq)   { irq_set_mask(irq, 1); }
void irq_unmask(int irq) { irq_set_mask(irq, 0); }

/* This CPU's local APIC: on, legacy pins off (LINT0: the 8259s, unused
 * now; LINT1: NMIs), and its timer in one-shot or TSC-deadline mode. */
static void lapic_setup(void)
{
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
    sdt_t *madt = acpi_find("APIC");
    if (!madt) panic("no ACPI MADT: cannot find the processors and I/O APICs\n");
    /* after the header: the APIC address (32 bits), flags (32), then
     * entries {type, length, ...}: 0 = a processor, 1 = an I/O APIC,
     * 2 = an interrupt source override, 5 = a 64-bit APIC address */
    base = *(uint32_t *)madt->data;
    for (uint8_t *e = madt->data + 8; e < (uint8_t *)madt + madt->len; e += e[1]) {
        if (e[0] == 0 && (*(uint32_t *)(e + 4) & 1) && napic < MAXCPU) apic_ids[napic++] = e[3];
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
    if (!nioapic) panic("no I/O APIC\n");
    for (int a = 0; a < nioapic; a++)    /* every line masked to start with */
        for (int p = 0; p < ioapic[a].count; p++) io_write(a, 0x10 + 2 * p, 1 << 16);
    outb(0x21, 0xFF); outb(0xA1, 0xFF);  /* the 8259s: all masked, for good */

    lapic = kmap_dev(base, PAGE);
    cpus[0]->a.apic_id = lr(L_ID) >> 24;

    /* Measure the TSC (and the APIC timer, for CPUs without TSC deadline)
     * against the PIT over 10 ms (one PIT tick, 0.84 us, is 0.01% of it). */
    uint32_t r[4];
    cpuid(1, r);
    tsc_deadline = r[2] >> 24 & 1;
    cpuid(0x80000007, r);
    int invariant = r[3] >> 8 & 1;
    lw(L_TDIV, 3);
    lw(L_TIMER, 1 << 16);                /* masked, one-shot */
    lw(L_TINIT, 0xFFFFFFFF);
    uint64_t t = rdtsc();
    if (!pit_wait(10000)) tsc_hz = (rdtsc() - t) * 100;
    else {                               /* no PIT: the CPU's word, and the APIC timer over 10 ms of TSC */
        if (!(tsc_hz = cpuid_tsc_hz())) panic("no way to measure time (no PIT, no TSC rate in CPUID)\n");
        lw(L_TINIT, 0xFFFFFFFF);
        for (t = rdtsc(); rdtsc() - t < tsc_hz / 100; ) asm volatile("pause");
        kprintf("mk: no PIT: TSC rate from CPUID\n");
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

/* Where each new CPU arrives in C, in 64-bit mode, on its own stack. */
void ap_main(cpu_t *c)
{
    write_cr3(c->cur_as = kernel_as);    /* leave the trampoline's page table */
    uint64_t cr4;                        /* then keep kernel translations across CR3 changes */
    asm volatile("mov %%cr4, %0; or $0x80, %0; mov %0, %%cr4" : "=r"(cr4));
    cpu_setup(c);
    lapic_setup();
    c->online = 1;
    klock();
    idle();
}

void smp_start(void)
{
    /* The trampoline needs its own page to be mapped where it runs (at
     * 0x8000, "identity") when it turns paging on: a temporary page table
     * with the kernel's upper half and boot.S's first-GiB identity map. */
    uint64_t pml4 = page_alloc(1);
    memcpy((uint64_t *)P2V(pml4) + 256, boot_pml4 + 256, 256 * 8);
    ((uint64_t *)P2V(pml4))[0] = ((uint64_t)pdpt_low - KVMA) | PTE_P | PTE_W;
    memcpy(P2V(TRAMP), ap_start, ap_end - ap_start);
    *(uint32_t *)P2V(TRAMP + (ap_cr3 - ap_start)) = pml4;

    /* INIT resets the other CPUs, all at once; they need 10 ms before the
     * start-up message (SIPI), then each starts in microseconds. */
    if (napic > 1) {                    /* (10 ms of TSC: the PIT may be off) */
        ipi_all_others(0x4500);
        for (uint64_t t = rdtsc(); rdtsc() - t < tsc_hz / 100; ) asm volatile("pause");
    }
    for (int i = 0; i < napic; i++) {
        if (apic_ids[i] == cpus[0]->a.apic_id) continue;
        cpu_t *c = cpu_alloc();
        uint64_t stack = page_alloc(1);  /* its idle stack, 4 KiB (deepest use measured: 600 bytes) */
        if (!c || !stack) break;
#ifdef STACKCHECK
        stack_paint(c->stack_lo = P2V(stack), c->stack_hi = (uint64_t *)P2V(stack) + PAGE / 8);
#endif
        c->id = ncpu;
        c->a.apic_id = apic_ids[i];
        *(uint64_t *)P2V(TRAMP + (ap_stack - ap_start)) = (uint64_t)P2V(stack) + PAGE;
        *(uint64_t *)P2V(TRAMP + (ap_cpu - ap_start)) = (uint64_t)c;

        /* SIPI: start at 0x8000. A second one if it has not answered after
         * 1 ms (the standard asks for two; one is enough on most machines). */
        for (int s = 0; s < 2 && !c->online; s++) {
            send_ipi(c->a.apic_id, 0x4600 | TRAMP >> 12);
            for (uint64_t end = rdtsc() + ns_to_ticks(s ? 100000000 : 1000000); !c->online && rdtsc() < end; )
                asm volatile("pause");
        }
        if (!c->online) { kprintf("mk: CPU with APIC id %d did not start\n", c->a.apic_id); continue; }
        cpus[ncpu++] = c;                /* (its idle() will take the kernel lock after us) */
    }
    page_free(pml4, 1);
    page_free(TRAMP, 1);                 /* the trampoline is not needed any more */
    kprintf("mk: %d CPU%s running\n", ncpu, ncpu > 1 ? "s" : "");
}
