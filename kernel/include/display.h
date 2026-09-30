/*
 * display.h - Displays: the drivers behind /dev/fb0, /dev/fb1, ...
 *
 * A display driver registers each output it drives with its operations:
 * the modes it can show and how to set one (both optional).  The
 * framebuffer the firmware set up (GRUB, UEFI GOP) is registered first; a
 * native driver for the same device takes it over (display_takeover)
 * instead of adding another display.  Once every driver has probed, the
 * displays are numbered by kind: the physical cards' first, then virtual
 * ones (QEMU's), then a firmware framebuffer no driver took; the text
 * console moves to display 0 (/dev/fb0, the desktop's).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_DISPLAY_H
#define SIEOS_DISPLAY_H

#include "kernel.h"
#include "sieos/sysinfo.h"

#define DISPLAY_MAX 4

struct display;
struct file;

struct display_mode {
    uint32_t width, height, pitch;          /* pitch: bytes per line (32 bits per pixel) */
    uint32_t refresh;                       /* Hz, 0 if unknown */
    bool preferred;
};

enum display_kind { DISPLAY_PHYSICAL, DISPLAY_VIRTUAL, DISPLAY_FIRMWARE };

struct display_ops {
    const char *name;
    enum display_kind kind;                 /* the order of /dev/fbN */
    /* The modes the display can show (NULL: only the current one). */
    int  (*modes)(struct display *d, struct display_mode *out, int max);
    /* Set one of them; fill d->mode on success (NULL: the mode is fixed). */
    int  (*set_mode)(struct display *d, const struct display_mode *m);
};

struct display {
    int index;
    bool present;
    const struct display_ops *ops;
    void *drv;
    char desc[48];
    struct display_mode mode;
    uint8_t red_pos, green_pos, blue_pos;
    uint64_t fb_phys, fb_size;              /* what can be scanned out (and mapped by fbmap) */
    void *fb;                               /* the kernel's mapping (write-combining) */
    bool console;                           /* the text console draws here */
    int owner;                              /* the process that has it mapped, 0 none */
};

extern struct display displays[DISPLAY_MAX];
extern int ndisplays;

void display_init(void);                    /* the firmware display, the native drivers, the order */
struct display *display_register(const struct display_ops *ops, void *drv, const char *desc,
                                 const struct display_mode *m, uint64_t fb_phys, uint64_t fb_size);
/* A native driver for the device whose memory is [lo, hi): it takes over the
 * firmware display there if there is one (keeping its index and the
 * console), else registers a new display. */
struct display *display_takeover(uint64_t lo, uint64_t hi, const struct display_ops *ops, void *drv,
                                 const char *desc, const struct display_mode *m, uint64_t fb_phys,
                                 uint64_t fb_size);
void display_mode_changed(struct display *d);   /* after set_mode: the console follows */

int  fb_open(int minor);
long fb_ioctl(struct file *f, unsigned long cmd, uint64_t arg);
long sys_fbmap(int fd);
void fb_release_owner(int pid);

/* drivers */
void display_order(void);               /* after the displays' drivers: the console's display chosen */

#endif
