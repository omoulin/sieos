/*
 * kprintf.c - The kernel's own messages. The architecture sends each byte
 * somewhere it can always reach (arch_putc: the first serial port on
 * x86-64): the kernel must be able to report a crash even when no driver
 * works.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"

/* The log: the last LOG_KEEP bytes printed, for SYS_LOG (a small screen
 * that shows the console, like the Pi's GFX HAT). */
static char ring[LOG_KEEP];
static uint64_t ring_end;                    /* bytes logged since boot */
static void keep(char c) { ring[ring_end++ % LOG_KEEP] = c; }

static void put(void *ctx, char c)
{
    (void)ctx;
    keep(c);
    if (c == '\n') arch_putc('\r');
    arch_putc(c);
}

/* A lock of its own, so lines from two CPUs never mix (it is also used
 * where the kernel lock is not held, e.g. while starting CPUs). */
static spinlock_t print_lock;

/* SYS_LOG. */
long log_read(char *to, size_t size, uint64_t *pos)       /* (kernel memory: the caller copies out) */
{
    spin_lock(&print_lock);
    uint64_t p = *pos, oldest = ring_end > LOG_KEEP ? ring_end - LOG_KEEP : 0;
    if (p < oldest || p > ring_end) p = p > ring_end ? ring_end : oldest;
    size_t n = 0;
    for (; n < size && p < ring_end; n++, p++) to[n] = ring[p % LOG_KEEP];
    *pos = p;
    spin_unlock(&print_lock);
    return (long)n;
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    spin_lock(&print_lock);
    vformat(put, 0, fmt, ap);
    spin_unlock(&print_lock);
    va_end(ap);
}

void panic(const char *fmt, ...)   /* (no lock: it may be held; the message matters more) */
{
    halt_others();                  /* stop the other CPUs first */
    va_list ap;
    va_start(ap, fmt);
    for (const char *s = "\nmk: PANIC: "; *s; s++) put(0, *s);
    vformat(put, 0, fmt, ap);
    va_end(ap);
    arch_halt_forever();
}
