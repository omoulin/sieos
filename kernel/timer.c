/*
 * timer.c - PIT system timer and CMOS real-time clock.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "hid.h"
#include "arch.h"
#include "proc.h"
#include "smp.h"
#include "net.h"
#include "ddi.h"
#include "mm.h"
#include "power.h"

volatile uint64_t ticks;
static uint64_t boot_time;

/*
 * High-resolution time: the CPU timestamp counter, calibrated against PIT
 * channel 2 at boot.  hrtime is nanoseconds since boot (monotonic across
 * CPUs: each application processor measures its TSC's offset from the boot
 * CPU's when it starts, see tsc_sync_*, as a hypervisor may reset the TSC
 * of a processor it starts); the real-time clock is boot time + hrtime + an offset changed by
 * clock_settime/stime and slewed by adjtime.
 */
uint64_t tsc_hz;
static uint64_t tsc0, tsc_mult;             /* ns = (tsc - tsc0) * tsc_mult >> 32 */
static volatile uint64_t hr_last;
static int64_t rt_offset_ns;                 /* realtime = boot_time + hrtime + offset */
static int64_t adj_pending_ns;               /* adjtime: still to slew */
#define ADJ_SLEW_PER_TICK (500000L / TIMER_HZ)   /* 500 us per second */
static struct spinlock clock_lock;           /* rt_offset_ns, adj_pending_ns (the tick, clock_settime, adjtime) */

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * Whether the PIT counts at all: recent Intel platforms (Ice Lake and
 * later: the Surface Pro 7, ...) may gate its clock off, and then it
 * neither counts nor interrupts.  Channel 0 (programmed by timer_init) is
 * latched and read twice, 100 port reads (about 100 us) apart.
 */
bool pit_ok;

static uint16_t pit_count(void)
{
    outb(0x43, 0x00);                        /* latch channel 0 */
    uint16_t lo = inb(0x40);
    return lo | (uint16_t)inb(0x40) << 8;
}

static bool pit_running(void)
{
    for (int round = 0; round < 3; round++) {
        uint16_t a = pit_count();
        for (int i = 0; i < 100; i++)
            inb(0x61);
        uint16_t b = pit_count();
        if (a != b && a != 0xFFFF)
            return true;
    }
    return false;
}

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

/* The TSC frequency the CPU states (Intel CPUID 15h: crystal ratio; else 16h: base MHz); 0 if none. */
static uint64_t tsc_cpuid_hz(void)
{
    uint32_t max, a, b, c, d;
    cpuid(0, &max, &b, &c, &d);
    if (max < 0x15)
        return 0;
    cpuid(0x15, &a, &b, &c, &d);
    if (a && b && c)
        return (uint64_t)c * b / a;
    if (max >= 0x16) {
        cpuid(0x16, &a, &b, &c, &d);
        if (a & 0xFFFF)
            return (uint64_t)(a & 0xFFFF) * 1000000;
    }
    return 0;
}

/* Against PIT channel 2 (50 ms); 0 if its output never rises (a dead PIT). */
static uint64_t tsc_pit_hz(void)
{
    const uint16_t count = 59659;            /* 50 ms of the 1.193182 MHz PIT */
    uint8_t v = inb(0x61);
    outb(0x61, (v & ~0x02) | 0x01);          /* gate channel 2 on, speaker off */
    outb(0x43, 0xB0);                        /* channel 2, lo/hi byte, mode 0 */
    outb(0x42, count & 0xFF);
    outb(0x42, count >> 8);
    uint64_t t0 = rdtsc(), t1;
    bool done = false;
    while (!(done = inb(0x61) & 0x20) && (t1 = rdtsc()) - t0 < 4000000000UL)   /* (0.4 s at 10 GHz) */
        ;
    t1 = rdtsc();
    outb(0x61, v);
    return done ? (t1 - t0) * 1193182UL / count : 0;
}

static inline uint32_t inl_(uint16_t port)
{
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* Against the ACPI PM timer (3.579545 MHz, 24 or 32 bits), 50 ms. */
static uint64_t tsc_pmtimer_hz(void)
{
    uint32_t port = acpi_pm_timer_port, mask = acpi_pm_timer_32 ? 0xFFFFFFFF : 0xFFFFFF;
    if (!port || port > 0xFFFF)
        return 0;
    uint32_t start = inl_(port) & mask, now = start;
    uint64_t t0 = rdtsc(), t1;
    while (((now = inl_(port) & mask) - start) % (mask + 1UL) < 178977) {   /* 50 ms */
        if (rdtsc() - t0 > 4000000000UL)
            return 0;                        /* not counting */
    }
    t1 = rdtsc();
    return (t1 - t0) * 3579545UL / ((now - start) & mask);
}

/* Against the HPET's main counter, 50 ms. */
static uint64_t tsc_hpet_hz(void)
{
    if (!acpi_hpet_base)
        return 0;
    volatile uint64_t *h = mmio_map(acpi_hpet_base, 0x400);
    if (!h)
        return 0;
    uint64_t period_fs = h[0] >> 32;         /* GCAP_ID: femtoseconds per count */
    if (!period_fs || period_fs > 100000000UL)
        return 0;
    if (!(h[2] & 1))
        h[2] |= 1;                           /* GEN_CONF: the counter on */
    uint64_t want = 50000000000000UL / period_fs;   /* 50 ms in counts */
    uint64_t c0 = h[30], c, t0 = rdtsc(), t1;
    while ((c = h[30]) - c0 < want)
        if (rdtsc() - t0 > 4000000000UL)
            return 0;
    t1 = rdtsc();
    return (t1 - t0) * (1000000000000000UL / period_fs) / (c - c0);    /* (counts a second) */
}

static const char *tsc_source = "PIT";

/* A plausible TSC frequency: 100 MHz to 10 GHz. */
static bool tsc_plausible(uint64_t hz) { return hz >= 100000000UL && hz <= 10000000000UL; }

static void tsc_calibrate(void)
{
    static const struct { const char *name; uint64_t (*measure)(void); } refs[] = {
        { "PIT", tsc_pit_hz }, { "ACPI PM timer", tsc_pmtimer_hz }, { "HPET", tsc_hpet_hz }, { "CPUID", tsc_cpuid_hz },
    };
    pit_ok = pit_running();
    tsc_hz = 0;
    for (size_t i = pit_ok ? 0 : 1; i < ARRAY_SIZE(refs) && !tsc_hz; i++) {
        uint64_t hz = refs[i].measure();
        if (tsc_plausible(hz)) {
            tsc_hz = hz;
            tsc_source = refs[i].name;
        }
    }
    if (tsc_hz)
        tsc_mult = (uint64_t)(((unsigned __int128)1000000000UL << 32) / tsc_hz);
    else
        tsc_source = "none";                 /* unusable: fall back to the tick counter */
    tsc0 = rdtsc();
}

static void timer_irq(struct trapframe *tf);

const char *timer_tsc_source(void) { return tsc_source; }

/* The boot CPU's local timer drives the tick (from lapic_timer_irq). */
void timer_lapic_tick(struct trapframe *tf)
{
    timer_irq(tf);           /* always: a PIT may count without its IRQ 0 reaching us (laptops); */
}                            /* the tick follows hrtime, so two sources count it once */

static volatile bool tsc_offsets;            /* an application processor has an offset */

/* This CPU's TSC in the boot CPU's time base. */
static inline uint64_t tsc_now(void)
{
    uint64_t t = rdtsc();
    return tsc_offsets ? t + mycpu()->tsc_off : t;
}

/*
 * Measure an application processor's TSC offset: the boot CPU publishes its
 * TSC and the AP reads its own right after seeing it, so boot - AP is a lower
 * bound of the offset, closest to it in the round with the least latency.
 */
#define TSC_SYNC_ROUNDS 16
static volatile uint64_t sync_tsc;
static volatile int sync_state;              /* 0 idle, 1 AP ready, 2 published, 3 read */

bool tsc_sync_master(void)
{
    uint64_t limit = rdtsc() + (tsc_hz ? tsc_hz : 4000000000UL);   /* a second: the processor may be stuck */
    for (int i = 0; i < TSC_SYNC_ROUNDS; i++) {
        while (__atomic_load_n(&sync_state, __ATOMIC_ACQUIRE) != 1)
            if (rdtsc() > limit)
                return false;
            else
                __builtin_ia32_pause();
        sync_tsc = rdtsc();
        __atomic_store_n(&sync_state, 2, __ATOMIC_RELEASE);
        while (__atomic_load_n(&sync_state, __ATOMIC_ACQUIRE) != 3)
            if (rdtsc() > limit)
                return false;
            else
                __builtin_ia32_pause();
        __atomic_store_n(&sync_state, 0, __ATOMIC_RELEASE);
    }
    tsc_offsets = true;
    return true;
}

void tsc_sync_slave(struct cpu *c)
{
    int64_t best = -(1LL << 62);
    for (int i = 0; i < TSC_SYNC_ROUNDS; i++) {
        while (__atomic_load_n(&sync_state, __ATOMIC_ACQUIRE) != 0)
            __builtin_ia32_pause();
        __atomic_store_n(&sync_state, 1, __ATOMIC_RELEASE);
        while (__atomic_load_n(&sync_state, __ATOMIC_ACQUIRE) != 2)
            __builtin_ia32_pause();
        int64_t off = (int64_t)(sync_tsc - rdtsc());
        if (off > best)
            best = off;
        __atomic_store_n(&sync_state, 3, __ATOMIC_RELEASE);
    }
    c->tsc_off = best;
}

uint64_t hrtime(void)
{
    uint64_t ns;
    if (!tsc_hz)
        ns = ticks * (1000000000UL / TIMER_HZ);
    else
        ns = (uint64_t)(((unsigned __int128)(tsc_now() - tsc0) * tsc_mult) >> 32);
    /* never go backwards, even if the CPUs' counters differ slightly */
    uint64_t last = hr_last;
    while (ns > last && !__atomic_compare_exchange_n(&hr_last, &last, ns, false, __ATOMIC_RELAXED,
                                                     __ATOMIC_RELAXED))
        ;
    return ns > last ? ns : last;
}

int64_t realtime_ns(void)
{
    return (int64_t)boot_time * 1000000000L + (int64_t)hrtime() + rt_offset_ns;
}

void realtime_set(int64_t ns)
{
    spin_lock(&clock_lock);
    rt_offset_ns = ns - (int64_t)boot_time * 1000000000L - (int64_t)hrtime();
    adj_pending_ns = 0;
    spin_unlock(&clock_lock);
}

/* adjtime: slew by delta; returns the adjustment still pending before. */
int64_t realtime_adjust(int64_t delta_ns, bool set)
{
    spin_lock(&clock_lock);
    int64_t old = adj_pending_ns;
    if (set)
        adj_pending_ns = delta_ns;
    spin_unlock(&clock_lock);
    return old;
}

/*
 * The tick count follows hrtime (with the TSC): a hypervisor may deliver
 * missed timer interrupts in a burst after a stall, and counting them
 * would make every tick-based timeout expire early.
 */
static void timer_irq(struct trapframe *tf)
{
    uint64_t now = tsc_hz ? hrtime() / (1000000000UL / TIMER_HZ) : ticks + 1;
    if (now <= ticks)
        return;                                  /* a late interrupt for a tick already counted */
    uint64_t n = now - ticks;
    ticks = now;
    spin_lock(&clock_lock);
    for (uint64_t i = 0; i < n && adj_pending_ns; i++) {
        int64_t step = adj_pending_ns > 0 ? MIN(adj_pending_ns, ADJ_SLEW_PER_TICK)
                                          : -MIN(-adj_pending_ns, ADJ_SLEW_PER_TICK);
        rt_offset_ns += step;
        adj_pending_ns -= step;
    }
    spin_unlock(&clock_lock);
    clock_wake_sleepers();
    if (kthreads_running()) {
        clock_thread_kick();       /* the rest: the clock thread's */
    } else {                       /* (the boot) */
        clock_tick(n);
        net_poll();
        ddi_poll();                /* the drivers' polled devices (USB, I2C HID, Wi-Fi) */
        power_tick();
    }
    if (!lapic_ok)
        sched_tick(tf);            /* no local APIC timer: the PIT schedules */
}

static uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static int bcd(int v) { return (v & 0x0F) + (v >> 4) * 10; }

uint64_t rtc_unix_time(void)
{
    for (int i = 0; i < 1000000 && (cmos_read(0x0A) & 0x80); i++)   /* update in progress */
        ;
    int sec = cmos_read(0x00), min = cmos_read(0x02), hour = cmos_read(0x04);
    int day = cmos_read(0x07), mon = cmos_read(0x08), year = cmos_read(0x09);
    uint8_t regb = cmos_read(0x0B);
    if (!(regb & 0x04)) {
        sec = bcd(sec);
        min = bcd(min);
        hour = bcd(hour & 0x7F) | (hour & 0x80);
        day = bcd(day);
        mon = bcd(mon);
        year = bcd(year);
    }
    if (!(regb & 0x02) && (hour & 0x80))
        hour = ((hour & 0x7F) + 12) % 24;
    year += 2000;

    static const int mdays[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    uint64_t days = 0;
    for (int y = 1970; y < year; y++)
        days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    days += mdays[mon - 1] + day - 1;
    if (mon > 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
        days++;
    return days * 86400 + hour * 3600 + min * 60 + sec;
}

uint64_t kernel_time(void)
{
    return realtime_ns() / 1000000000L;
}

void timer_init(void)
{
    uint32_t divisor = 1193182 / TIMER_HZ;
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, divisor >> 8);
    boot_time = rtc_unix_time();
    tsc_calibrate();
    irq_register(IRQ_TIMER, timer_irq);
}
