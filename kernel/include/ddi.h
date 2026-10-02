/*
 * ddi.h - The driver interface: what a loadable driver declares, and the
 * kernel's services made for drivers.
 *
 * A driver is an ELF relocatable object, /drv/NAME.drv, linked against the
 * kernel when it is loaded (every global symbol of the kernel is its
 * library: kprintf, kmalloc, pci_*, mmio_map, blk_register, netif_register,
 * display_register, ...; the headers of <ddk/...> declare them).  It says:
 *
 *   DDI_DRIVER("name", DDI_PHASE_BOOT, "what it drives");
 *   DDI_ALIAS("pci8086,34f0");          the devices it drives, any number:
 *   DDI_ALIAS("pciclass,010802");       pciVVVV,DDDD  pciclass,CCSSPP  pciclass,CCSS
 *                                       platform,NAME (i8042)
 *   int _init(void) { ... }             attach: 0, or < 0 (nothing found, a failure)
 *
 * The kernel reads the name, the phase and the aliases from the file's
 * .drv_info and .drv_aliases sections without loading it.  At boot every
 * device found (the PCI functions, the platform's) is matched against the
 * aliases, the most specific first; the driver of a device is loaded (from
 * the boot archive the boot loader brought, else from /drv on the root)
 * and its _init called, once, in its phase:
 *   DDI_PHASE_DISPLAY  with the console (displays),
 *   DDI_PHASE_BOOT     before the root is mounted (input, disks, USB),
 *   DDI_PHASE_ROOT     after (network cards, what needs firmware files).
 * _init finds its devices itself (pci_find_all, pci_count/pci_at) and
 * registers them with their framework; it runs once, from the boot thread
 * (or modload, one load at a time).  The kernel runs on every processor at
 * once: a driver locks its own state (sync.h) where its entry points (its
 * interrupt handler, in the interrupt thread; its poll, in the clock
 * thread; its framework's calls) can meet.  Drivers are not unloaded.
 *
 * Build one on SIEOS:
 *   gcc -c -O2 -ffreestanding -fno-pic -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
 *       -fno-stack-protector -I/usr/include/ddk mydrv.c
 *   ld -r -o mydrv.drv mydrv.o          then modload mydrv.drv (or copy it to /drv)
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_DDI_H
#define SIEOS_DDI_H

#include "kernel.h"

#define DDI_MAGIC 0x56524453U           /* "SDRV" */
#define DDI_ABI   1

enum { DDI_PHASE_DISPLAY, DDI_PHASE_BOOT, DDI_PHASE_ROOT };

struct ddi_modinfo {
    uint32_t magic, abi, phase, flags;
    char name[32];
    char desc[96];
};

#define DDI_DRIVER(nm, ph, ds)                                                                              \
    const struct ddi_modinfo _ddi_modinfo __attribute__((section(".drv_info"), used)) = {                  \
        DDI_MAGIC, DDI_ABI, ph, 0, nm, ds }
#define DDI_CAT_(a, b) a##b
#define DDI_CAT(a, b)  DDI_CAT_(a, b)
#define DDI_ALIAS(s)                                                                                        \
    static const char DDI_CAT(_ddi_alias_, __COUNTER__)[]                                                   \
        __attribute__((section(".drv_aliases"), used, aligned(1))) = s

int _init(void);                        /* each driver's */

/* ---- the kernel's services for drivers */

/* A function called at every timer tick (100 Hz, the BSP, interrupts off): polled devices. */
void ddi_poll_register(void (*fn)(void));
/* A line of the boot's summary ("[ OK ] ..."). */
void ddi_report(const char *line);
/* The kernel's command line, and whether a word is on it ("nowifi", "usbdebug"). */
const char *ddi_cmdline(void);
bool boot_option(const char *cmdline, const char *opt);
/* The Wi-Fi driver's wifi() system call (iwlwifi). */
extern long (*ddi_wifi_op)(int op, void *buf, long n);

/* ---- the loader (modload.c), for the kernel */
void modules_init(uint64_t archive_pa, uint64_t archive_size);
void modules_attach(int phase);         /* the devices' drivers of this phase (and earlier ones not yet loaded) */
void modules_root(void);                /* after the root is mounted: /drv's drivers, then DDI_PHASE_ROOT */
void ddi_poll(void);                    /* the timer's: the drivers' poll functions */
int  modload_path(const char *path);    /* the modload system call's: a driver file, attached; 0 or -errno */

struct sieos_modinfo;
int  modinfo_get(int index, struct sieos_modinfo *mi);

#endif
