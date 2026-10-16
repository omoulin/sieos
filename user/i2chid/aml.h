/*
 * aml.h - A small reader of the firmware's ACPI tables (aml.c): the DSDT
 * and SSDTs (SYS_ACPI), enough to find the I2C HID devices (touchpads) and
 * how to reach them. Also built on the host for its test (tests/aml).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#ifdef AML_HOST
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#else
#include "mk.h"
#define abort() sys_exit(1)
#endif

typedef struct {
    char path[48], hid[12];      /* "\_SB_.PC00.I2C5.TPD0", "ELAN06FA" */
    char bus[48], gpio[48];      /* its I2C controller, its GpioInt's controller (ACPI paths) */
    int addr, pin, irq, desc;    /* I2C address, GpioInt pin, Interrupt() line, HID descriptor register; -1: none */
    int pin_low, pin_edge;       /* the GpioInt: active low, edge-triggered */
    uint32_t speed;              /* the bus's speed, Hz */
} aml_hid_t;

long aml_load(const uint8_t *t, long n);    /* the tables, one after the other -> named objects (<0: none) */
int  aml_hid(aml_hid_t *d, int max);        /* the I2C HID devices present ("PNP0C50") -> how many */
/* An object (method: called with argc integers) as an integer: 0 if it worked. */
int  aml_int(const char *path, int argc, const int64_t *arg, int64_t *v);
int  aml_res(const char *path, aml_hid_t *d);   /* a resource template (a _CRS): its I2C, GpioInt, Interrupt */

/* Given by the program: a dword of physical memory (SystemMemory regions),
 * and the log. */
uint32_t aml_mem32(uint64_t pa);
void log_line(const char *fmt, ...);
