/*
 * rtc.c (arm64) - The real-time clock, read once for the wall clock
 * (SYS_TIME, syscall.c): an ARM PL031 (QEMU's virt: seconds since 1970)
 * if the device tree has one. The Raspberry Pis have none the kernel can
 * read (the Pi 4 has no clock; the Pi 5's is the firmware's): 0, and the
 * clock is set by root (SYS_TIME) instead.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"
#include "mk/fdt.h"

extern const void *fdt;                 /* (platform.c) */

int64_t arch_rtc(void)
{
    uint64_t a, s;
    int n = fdt ? fdt_find(fdt, -1, "arm,pl031") : -1;
    if (n < 0 || fdt_reg(fdt, n, 0, &a, &s)) return 0;
    volatile uint32_t *r = kmap_dev(a, PAGE);
    return r[0];
}
