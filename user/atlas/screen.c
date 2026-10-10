/*
 * screen.c - Where the desktop's picture goes, and the time of day: the
 * only machine-dependent part of Atlas.
 *
 *   x86-64          started by the UEFI loader (boot/uefi): the frame buffer the
 *                   firmware set up, at its size, write-combining;
 *                   else QEMU's standard VGA (PCI 1234:1111), set to 1920 x 1080
 *                   x 32 through its "DISPI" registers; video memory, not RAM.
 *   arm64, QEMU virt  "ramfb": a frame buffer in our own memory (8 MiB), whose
 *                   address we give QEMU through its fw_cfg interface.
 *   Raspberry Pi 4  the firmware's mailbox: "allocate a 1920 x 1080 x 32
 *                   frame buffer" (in the GPU's memory, not ours).
 *   Raspberry Pi 5  the frame buffer the firmware set up and describes in the
 *                   device tree ("simple-framebuffer"), at the size it chose.
 *
 * Atlas draws a SW x SH picture: 1920 x 1080 except on a UEFI firmware's
 * screen (its own size); a bigger screen shows it centred, a smaller one
 * is not used. The clock: the CMOS clock (x86), the PL031 (QEMU
 * virt); the Pis have none (uptime, until the network gives the time).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "draw.h"
#include "atlas.h"

#if defined(__x86_64__)
static uint32_t pci(int dev, int reg) { return pci_read(dev << 11 | reg); }   /* bus 0 (SYS_PCI: shared with the drivers) */
static void dispi(int i, int v) { outw(0x1CE, i); outw(0x1CF, v); }

int screen_init(screen_t *s)
{
    /* Started by SIEOS's UEFI loader (a USB key): the screen the firmware
     * set up, at its size (2560 x 1600, 1920 x 1200...: Atlas lays out to
     * any size from 1024 x 600; above 3840 x 2400 it uses the top-left part). */
    mk_screen_t g;
    if (!sys_screen(&g) && g.width >= 1024 && g.height >= 600 && g.pitch >= g.width) {
        SW = g.width < 3840 ? g.width : 3840;
        SH = g.height < 2400 ? g.height : 2400;
        *s = (screen_t){ map_phys(g.pa, ((uint64_t)g.pitch * g.height * 4) | MAP_WC), g.pitch, g.format == 0, "firmware screen (UEFI)" };
        return (long)s->fb > 0 ? 0 : -1;
    }
    for (int d = 0; d < 32; d++) {
        if (pci(d, 0) != 0x11111234) continue;
        uint64_t bar = pci(d, 0x10) & ~0xFUL;
        dispi(4, 0);                                     /* off while changing */
        dispi(1, SW); dispi(2, SH); dispi(3, 32);        /* width, height, bits per pixel */
        dispi(6, SW); dispi(7, SH); dispi(8, 0); dispi(9, 0);   /* virtual size, offset */
        dispi(4, 0x41);                                  /* on, linear video memory */
        *s = (screen_t){ map_phys(bar, (uint64_t)SW * SH * 4), SW, 0, "standard VGA" };
        return (long)s->fb > 0 ? 0 : -1;
    }
    return -1;
}

/* The CMOS clock (ports 0x70/0x71), as it keeps the time (QEMU: the host's, UTC). */
static int cmos(int r) { outb(0x70, r); return inb(0x71); }
long rtc_seconds(void)
{
    int s, m, h, b, k = 0;
    do {                                                 /* the same twice: not mid-update, and */
        while (cmos(0x0A) & 0x80) ;                      /* not disturbed by another reader */
        s = cmos(0); m = cmos(2); h = cmos(4); b = cmos(0x0B);
    } while ((s != cmos(0) || m != cmos(2) || h != cmos(4)) && ++k < 100);
    int pm = h & 0x80;
    h &= 0x7F;
    if (!(b & 4)) { s = (s & 15) + (s >> 4) * 10; m = (m & 15) + (m >> 4) * 10; h = (h & 15) + (h >> 4) * 10; }
    if (!(b & 2) && pm) h = (h % 12) + 12;               /* 12-hour mode */
    return h * 3600 + m * 60 + s;
}

#elif defined(__aarch64__)
#include "mk/fdt.h"
#include "mbox.h"

static char *dt;                                         /* the device tree (a copy) */

/* A picture of at least 1920 x 1080 at fb (pitch pixels per row): ours is
 * centred in it. */
static int use(screen_t *s, uint32_t *fb, int w, int h, int pitch, int swap, const char *name)
{
    if ((long)fb <= 0 || w < SW || h < SH) return -1;
    *s = (screen_t){ fb + (h - SH) / 2 * pitch + (w - SW) / 2, pitch, swap, name };
    return 0;
}

/* ---- QEMU's "ramfb": the fw_cfg device ("qemu,fw-cfg-mmio") is a selector
 * register, a data register and a DMA register; files are found in its
 * directory (item 0x19), then written by DMA. All big-endian. */
static uint32_t be32(uint32_t v) { return v >> 24 | (v >> 8 & 0xFF00) | (v << 8 & 0xFF0000) | v << 24; }
static uint64_t be64(uint64_t v) { return (uint64_t)be32((uint32_t)v) << 32 | be32((uint32_t)(v >> 32)); }

static int ramfb(screen_t *s)
{
    int n = fdt_find(dt, -1, "qemu,fw-cfg-mmio");
    uint64_t a, len, pa, fbpa;
    if (n < 0 || fdt_reg(dt, n, 0, &a, &len)) return -1;
    volatile uint8_t *fc = map_phys(a, 0x18);
    if ((long)fc < 0) return -1;
    volatile uint16_t *sel = (volatile uint16_t *)(fc + 8);
    *sel = 0x1900;                                       /* the directory (0x19, big-endian) */
    uint32_t count = 0, item = 0;
    for (int i = 0; i < 4; i++) count = count << 8 | fc[0];
    for (uint32_t i = 0; i < count && !item; i++) {      /* entries: size, select, reserved, name[56] */
        uint8_t e[64];
        for (int k = 0; k < 64; k++) e[k] = fc[0];
        if (!strcmp((char *)e + 8, "etc/ramfb")) item = e[4] << 8 | e[5];
    }
    if (!item) return -1;                                /* (QEMU without "-device ramfb") */
    uint32_t *fb = dma_alloc((uint64_t)SW * SH * 4, &fbpa);
    struct { uint32_t control, length; uint64_t address; uint64_t addr; uint32_t fourcc, flags, w, h, stride; }
        __attribute__((packed)) *d = dma_alloc(4096, &pa);
    if ((long)fb <= 0 || (long)d <= 0) return -1;
    d->addr = be64(fbpa);
    d->fourcc = be32(0x34325258);                        /* "XR24": 32 bits, x r g b (ours: 0x00RRGGBB) */
    d->flags = 0;
    d->w = be32(SW); d->h = be32(SH); d->stride = be32(SW * 4);
    d->control = be32(item << 16 | 0x08 | 0x10);         /* select this item, write */
    d->length = be32(28);
    d->address = be64(pa + 16);                          /* (the configuration follows the request) */
    dma_sync(d, 64);
    *(volatile uint32_t *)(fc + 16) = be32((uint32_t)(pa >> 32));
    *(volatile uint32_t *)(fc + 20) = be32((uint32_t)pa);   /* (this half starts it) */
    for (int k = 0; k < 100000 && be32(*(volatile uint32_t *)&d->control) & ~1u; k++) { dma_sync(d, 4); sys_yield(); }
    return use(s, fb, SW, SH, SW, 0, "QEMU ramfb");
}

/* ---- The Pi 4: ask the firmware for a frame buffer (mailbox tags: size,
 * virtual size, depth, pixel order, allocate, pitch). Its address is a
 * VideoCore bus address: the low 30 bits are the ARM's. */
static int pi_mailbox(screen_t *s)
{
    if (fdt_find(dt, -1, "brcm,bcm2835-mbox") < 0) return -1;
    uint32_t m[] = { 0, 0,
        0x48003, 8, 0, SW, SH,          /* physical size */
        0x48004, 8, 0, SW, SH,          /* virtual size */
        0x48005, 4, 0, 32,              /* depth */
        0x48006, 4, 0, 0,               /* pixel order: BGR in memory, i.e. our 0x00RRGGBB words */
        0x40001, 8, 0, 4096, 0,         /* allocate: alignment -> address, size */
        0x40008, 4, 0, 0,               /* pitch, bytes per row */
        0 };
    int n = sizeof m / 4;
    m[0] = n * 4;
    /* (answers: the virtual size in words 10-11, the buffer's address and
     * size in 23-24, the pitch in 28) */
    if (mbox_call(m, n) || !m[23] || m[28] < SW * 4) return -1;
    uint64_t pa = m[23] & 0x3FFFFFFF, size = m[24];
    return use(s, map_phys(pa, size | MAP_WC), m[10], m[11], m[28] / 4, 0, "Raspberry Pi firmware");
}

/* ---- The Pi 5 (and any firmware that does it): the frame buffer it set up. */
static int simplefb(screen_t *s)
{
    int n = fdt_find(dt, -1, "simple-framebuffer");
    uint64_t a, len;
    if (n < 0 || fdt_reg(dt, n, 0, &a, &len)) return -1;
    const void *w = fdt_prop(dt, n, "width", 0), *h = fdt_prop(dt, n, "height", 0), *st = fdt_prop(dt, n, "stride", 0);
    const char *fmt = fdt_prop(dt, n, "format", 0);
    if (!w || !h || !st || !fmt) return -1;
    int swap = !strcmp(fmt, "a8b8g8r8") || !strcmp(fmt, "x8b8g8r8");
    if (!swap && strcmp(fmt, "a8r8g8b8") && strcmp(fmt, "x8r8g8b8")) return -1;   /* 32-bit formats only */
    return use(s, map_phys(a, len | MAP_WC), fdt_be32(w), fdt_be32(h), fdt_be32(st) / 4, swap, "firmware frame buffer");
}

int screen_init(screen_t *s)
{
    long size = sys_fdt(0, 0);
    if (size <= 0 || !(dt = malloc(size)) || sys_fdt(dt, size) != size) return -1;
    return simplefb(s) && pi_mailbox(s) && ramfb(s) ? -1 : 0;
}

/* The PL031 real-time clock (QEMU's virt): seconds since 1970. */
long rtc_seconds(void)
{
    int n = dt ? fdt_find(dt, -1, "arm,pl031") : -1;
    uint64_t a, len;
    if (n < 0 || fdt_reg(dt, n, 0, &a, &len)) return -1;
    volatile uint32_t *r = map_phys(a, 0x1000);
    return (long)r < 0 ? -1 : (long)(*r % 86400);
}
#endif
