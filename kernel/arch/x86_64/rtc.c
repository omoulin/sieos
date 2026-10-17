/*
 * rtc.c (x86-64) - The PC's real-time clock (the CMOS clock, ports 0x70 and
 * 0x71; UTC as QEMU and most setups keep it), read once for the wall clock
 * (SYS_TIME, syscall.c).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"
#include "x86.h"

static int cmos(int r) { outb(0x70, r); return inb(0x71); }

/* Days from 1970-01-01 to y-m-d (the civil calendar). */
static int64_t days(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = y / 400, yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t arch_rtc(void)
{
    int v[6], w[6], b, k = 0;
    static const int reg[6] = { 0, 2, 4, 7, 8, 9 };          /* seconds, minutes, hours, day, month, year */
    do {                                                      /* the same twice: not mid-update */
        while (cmos(0x0A) & 0x80 && ++k < 1000000) ;
        for (int i = 0; i < 6; i++) v[i] = cmos(reg[i]);
        for (int i = 0; i < 6; i++) w[i] = cmos(reg[i]);
    } while (memcmp(v, w, sizeof v) && ++k < 2000000);
    b = cmos(0x0B);
    int pm = v[2] & 0x80;
    v[2] &= 0x7F;
    if (!(b & 4)) for (int i = 0; i < 6; i++) v[i] = (v[i] & 15) + (v[i] >> 4) * 10;   /* BCD */
    if (!(b & 2) && pm) v[2] = v[2] % 12 + 12;               /* 12-hour mode */
    if (v[4] < 1 || v[4] > 12 || v[3] < 1) return 0;
    return days(2000 + v[5], v[4], v[3]) * 86400 + v[2] * 3600 + v[1] * 60 + v[0];
}
