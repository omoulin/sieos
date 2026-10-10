/*
 * kprintf.c - The kernel's own messages. The architecture sends each byte
 * somewhere it can always reach (arch_putc: the first serial port on
 * x86-64): the kernel must be able to report a crash even when no driver
 * works.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"

static void put(void *ctx, char c)
{
    (void)ctx;
    if (c == '\n') arch_putc('\r');
    arch_putc(c);
}

/* A lock of its own, so lines from two CPUs never mix (it is also used
 * where the kernel lock is not held, e.g. while starting CPUs). */
static spinlock_t print_lock;

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
