/*
 * power.h - Power management (power.c): power-off, restart, idle, the
 * processors' frequencies and temperatures, the thermal policy, /dev/power.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_POWER_H
#define SIEOS_POWER_H

#include "kernel.h"

struct trapframe;

void power_init(const char *cmdline);    /* after smp_boot: the ACPI tables, the processors */
const char *power_summary(char *buf, size_t n);
void power_off(void);                    /* returns only if the machine did not turn off */
void power_reset(void);
void power_idle(void);                   /* the idle loop's wait: HLT or MWAIT */
void power_cpu_tick(void);               /* every local timer tick, on every processor */
bool msr_fixup(struct trapframe *tf);    /* #GP in kernel mode on a probed MSR: skipped */
long power_ioctl(unsigned long cmd, void *arg);

#endif
