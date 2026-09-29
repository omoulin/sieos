/*
 * timer.c - PIT system timer and CMOS real-time clock.
 */
#include "hid.h"
#include "arch.h"
#include "proc.h"
#include "smp.h"
#include "net.h"

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

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void tsc_calibrate(void)
{
    const uint16_t count = 59659;            /* 50 ms of the 1.193182 MHz PIT */
    uint8_t v = inb(0x61);
    outb(0x61, (v & ~0x02) | 0x01);          /* gate channel 2 on, speaker off */
    outb(0x43, 0xB0);                        /* channel 2, lo/hi byte, mode 0 */
    outb(0x42, count & 0xFF);
    outb(0x42, count >> 8);
    uint64_t t0 = rdtsc();
    for (uint64_t spin = 0; !(inb(0x61) & 0x20) && spin < 100000000UL; spin++)
        ;
    uint64_t t1 = rdtsc();
    outb(0x61, v);
    tsc_hz = (t1 - t0) * 1193182UL / count;
    if (tsc_hz < 10000000UL)
        tsc_hz = 0;                          /* unusable: fall back to the tick counter */
    else
        tsc_mult = (uint64_t)(((unsigned __int128)1000000000UL << 32) / tsc_hz);
    tsc0 = rdtsc();
}

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

void tsc_sync_master(void)
{
    for (int i = 0; i < TSC_SYNC_ROUNDS; i++) {
        while (__atomic_load_n(&sync_state, __ATOMIC_ACQUIRE) != 1)
            __builtin_ia32_pause();
        sync_tsc = rdtsc();
        __atomic_store_n(&sync_state, 2, __ATOMIC_RELEASE);
        while (__atomic_load_n(&sync_state, __ATOMIC_ACQUIRE) != 3)
            __builtin_ia32_pause();
        __atomic_store_n(&sync_state, 0, __ATOMIC_RELEASE);
    }
    tsc_offsets = true;
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
    rt_offset_ns = ns - (int64_t)boot_time * 1000000000L - (int64_t)hrtime();
    adj_pending_ns = 0;
}

/* adjtime: slew by delta; returns the adjustment still pending before. */
int64_t realtime_adjust(int64_t delta_ns, bool set)
{
    int64_t old = adj_pending_ns;
    if (set)
        adj_pending_ns = delta_ns;
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
    for (uint64_t i = 0; i < n && adj_pending_ns; i++) {
        int64_t step = adj_pending_ns > 0 ? MIN(adj_pending_ns, ADJ_SLEW_PER_TICK)
                                          : -MIN(-adj_pending_ns, ADJ_SLEW_PER_TICK);
        rt_offset_ns += step;
        adj_pending_ns -= step;
    }
    clock_tick(n);
    net_poll();
    usb_poll();
    i2c_hid_poll();
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
    while (cmos_read(0x0A) & 0x80)      /* update in progress */
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
