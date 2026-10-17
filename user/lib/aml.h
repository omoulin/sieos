/*
 * aml.h - A small interpreter of the firmware's ACPI tables (aml.c): the
 * DSDT and SSDTs (SYS_ACPI), for the drivers that need them: the touchpad
 * (user/i2chid: where it is) and the ACPI server (user/acpid: sleep types,
 * battery, lid, buttons, the embedded controller). Also built on the host
 * for its test (tests/aml).
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
    int io[2];                   /* the first two I/O port ranges' bases (an embedded controller: data, command); -1: none */
} aml_hid_t;

long aml_load(const uint8_t *t, long n);    /* the tables, one after the other (others than DSDT/SSDT skipped) -> named objects */
int  aml_hid(aml_hid_t *d, int max);        /* the I2C HID devices present ("PNP0C50") -> how many */
int  aml_devs(const char *hid, int *dev, int max);   /* the devices with this _HID or _CID ("PNP0C0A") -> how many */
/* An object (path from object dev, 0: the root; a method: called with argc
 * integers): its integer, or its package's first max integers, in v -> how
 * many; -1: none, or it could not be evaluated. */
int  aml_eval(int dev, const char *path, int argc, const int64_t *arg, int64_t *v, int max);   /* (path 0: dev itself) */
int  aml_find(int dev, const char *path);   /* an object's number (for aml_eval), -1: none */
#define aml_int(path, argc, arg, v) (aml_eval(0, path, argc, arg, v, 1) < 0 ? -1 : 0)
int  aml_res(int dev, const char *path, aml_hid_t *d);   /* a resource template (a _CRS): I2C, GpioInt, Interrupt, I/O */
void aml_path(int dev, char *s, int n);     /* a device's path, "\_SB_.BAT0" */
int  aml_links(int back);                   /* PCI interrupt links: their settings kept (0) or given back (1) -> how many */

/* Given by the program: reading or writing an operation region (space: 0
 * memory, 1 I/O ports, 2 PCI configuration (addr as SYS_PCI's), 3 the
 * embedded controller; width 1, 2, 4 or 8 bytes) -> 0; else a read fails,
 * a write stays in the interpreter (no effect outside). And the
 * log. aml_notify (may be 0): a Notify (device, value) the AML made. */
int  aml_io(int space, uint64_t addr, int width, uint64_t *v, int write);
void log_line(const char *fmt, ...);
extern void (*aml_notify)(int dev, int64_t value);
