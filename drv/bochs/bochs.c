/*
 * bochs.c - The Bochs/QEMU display: QEMU's standard VGA (-vga std) and
 * bochs-display, PCI 1234:1111, one display each.
 *
 * The VBE "DISPI" registers set the mode: through the MMIO window in BAR2
 * (offset 0x500) when the card has one, else the I/O ports 0x1CE/0x1CF
 * (only one card can use those).  The frame buffer is BAR0.  The modes
 * offered are the usual sizes up to the card's maximum (DISPI "get caps")
 * that fit in its memory; the monitor's preferred mode comes from the EDID
 * QEMU puts at the start of BAR2.  The standard VGA is usually the one
 * GRUB set a mode on: its driver takes over display 0 and the console.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "display.h"
#include "ddi.h"
#include "pci.h"
#include "arch.h"
#include "mm.h"

#define DISPI_ID        0
#define DISPI_XRES      1
#define DISPI_YRES      2
#define DISPI_BPP       3
#define DISPI_ENABLE    4
#define DISPI_BANK      5
#define DISPI_VIRT_W    6
#define DISPI_VIRT_H    7
#define DISPI_X_OFF     8
#define DISPI_Y_OFF     9
#define DISPI_VRAM_64K  10

#define EN_ENABLED  0x01
#define EN_GETCAPS  0x02
#define EN_LFB      0x40

struct bochs {
    volatile uint16_t *dispi;                    /* MMIO registers, or NULL: I/O ports */
    volatile uint8_t *edid;
    uint64_t fb_phys, vram;
    uint32_t max_w, max_h;
    uint32_t pref_w, pref_h, pref_hz;
};

#define MAX_CARDS DISPLAY_MAX
static struct bochs cards[MAX_CARDS];
static int ncards;
static bool ports_used;

static uint16_t rd(struct bochs *b, int reg)
{
    if (b->dispi)
        return b->dispi[reg];
    outw(0x1CE, reg);
    return inw(0x1CF);
}

static void wr(struct bochs *b, int reg, uint16_t v)
{
    if (b->dispi) {
        b->dispi[reg] = v;
        return;
    }
    outw(0x1CE, reg);
    outw(0x1CF, v);
}

static const uint16_t sizes[][2] = {
    { 640, 480 },   { 800, 600 },   { 1024, 768 },  { 1152, 864 },  { 1280, 720 },  { 1280, 800 },
    { 1280, 1024 }, { 1366, 768 },  { 1440, 900 },  { 1600, 900 },  { 1600, 1200 }, { 1680, 1050 },
    { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 }, { 2560, 1600 }, { 3840, 2160 },
};

static bool fits(struct bochs *b, uint32_t w, uint32_t h)
{
    return w <= b->max_w && h <= b->max_h && (uint64_t)w * h * 4 <= b->vram;
}

static int bochs_modes(struct display *d, struct display_mode *out, int max)
{
    struct bochs *b = d->drv;
    int n = 0;
    bool pref_listed = false;
    for (size_t i = 0; i < ARRAY_SIZE(sizes) && n < max; i++) {
        if (!fits(b, sizes[i][0], sizes[i][1]))
            continue;
        bool pref = sizes[i][0] == b->pref_w && sizes[i][1] == b->pref_h;
        pref_listed |= pref;
        out[n++] = (struct display_mode){ sizes[i][0], sizes[i][1], sizes[i][0] * 4, pref ? b->pref_hz : 60, pref };
    }
    if (!pref_listed && b->pref_w && n < max && fits(b, b->pref_w, b->pref_h))
        out[n++] = (struct display_mode){ b->pref_w, b->pref_h, b->pref_w * 4, b->pref_hz, true };
    return n;
}

static void program(struct bochs *b, uint32_t w, uint32_t h)
{
    wr(b, DISPI_ENABLE, 0);
    wr(b, DISPI_BPP, 32);
    wr(b, DISPI_XRES, w);
    wr(b, DISPI_YRES, h);
    wr(b, DISPI_BANK, 0);
    wr(b, DISPI_VIRT_W, w);
    wr(b, DISPI_VIRT_H, h);
    wr(b, DISPI_X_OFF, 0);
    wr(b, DISPI_Y_OFF, 0);
    wr(b, DISPI_ENABLE, EN_ENABLED | EN_LFB);
}

static int bochs_set_mode(struct display *d, const struct display_mode *m)
{
    struct bochs *b = d->drv;
    if (!fits(b, m->width, m->height))
        return -EINVAL;
    program(b, m->width, m->height);
    if (rd(b, DISPI_XRES) != m->width || rd(b, DISPI_YRES) != m->height)
        return -EIO;
    d->mode = *m;
    d->mode.pitch = m->width * 4;
    memset(d->fb, 0, (size_t)d->mode.pitch * m->height);
    return 0;
}

static const struct display_ops bochs_ops = { "bochs-vbe", DISPLAY_VIRTUAL, bochs_modes, bochs_set_mode };

/* The preferred mode from an EDID block (the first detailed timing). */
static void read_edid(struct bochs *b)
{
    static const uint8_t magic[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (!b->edid)
        return;
    uint8_t e[128];
    for (int i = 0; i < 128; i++)
        e[i] = b->edid[i];
    if (memcmp(e, magic, 8))
        return;
    uint8_t sum = 0;
    for (int i = 0; i < 128; i++)
        sum += e[i];
    const uint8_t *t = e + 54;
    uint32_t clock = (t[0] | t[1] << 8) * 10000U;
    if (sum || !clock)
        return;
    uint32_t hact = t[2] | (t[4] & 0xF0) << 4, hbl = t[3] | (t[4] & 0x0F) << 8;
    uint32_t vact = t[5] | (t[7] & 0xF0) << 4, vbl = t[6] | (t[7] & 0x0F) << 8;
    b->pref_w = hact;
    b->pref_h = vact;
    b->pref_hz = (hact + hbl) && (vact + vbl) ? (clock + (hact + hbl) * (vact + vbl) / 2) / ((hact + hbl) * (vact + vbl)) : 60;
}

static bool bochs_start(struct bochs *b, const struct pci_dev *pd)
{
    b->fb_phys = pci_bar_addr(pd, 0, NULL);
    uint64_t size = pci_bar_size(pd, 0);
    if (!b->fb_phys || !size)
        return false;
    pci_enable_bus_master(pd);
    bool io;
    uint64_t mmio = pci_bar_addr(pd, 2, &io);
    if (mmio && !io && pci_bar_size(pd, 2) >= 0x1000) {
        volatile uint8_t *regs = mmio_map(mmio, 0x1000);
        if (regs) {
            b->dispi = (volatile uint16_t *)(regs + 0x500);
            b->edid = regs;
        }
    }
    if (!b->dispi) {
        if (ports_used)
            return false;                        /* (one card can use the ports) */
        ports_used = true;
    }
    uint16_t id = rd(b, DISPI_ID);
    if (id < 0xB0C0 || id > 0xB0CF)
        return false;
    b->vram = rd(b, DISPI_VRAM_64K) * 65536UL;
    if (!b->vram || b->vram > size)
        b->vram = size;
    /* the largest mode the card takes */
    uint16_t en = rd(b, DISPI_ENABLE);
    wr(b, DISPI_ENABLE, en | EN_GETCAPS);
    b->max_w = rd(b, DISPI_XRES);
    b->max_h = rd(b, DISPI_YRES);
    wr(b, DISPI_ENABLE, en);
    if (b->max_w < 640 || b->max_h < 480)
        b->max_w = 1920, b->max_h = 1200;
    read_edid(b);

    char desc[48];
    snprintf(desc, sizeof(desc), "QEMU %s, %lu MiB", pd->class_code == 3 && pd->subclass == 0 ? "standard VGA"
             : "bochs-display", (unsigned long)(b->vram >> 20));
    struct display_mode m;
    if (en & EN_ENABLED) {                       /* already set up (by GRUB): keep that mode */
        uint32_t w = rd(b, DISPI_XRES), h = rd(b, DISPI_YRES), virt = rd(b, DISPI_VIRT_W);
        m = (struct display_mode){ w, h, (virt ? virt : w) * 4, 60, w == b->pref_w && h == b->pref_h };
        if (rd(b, DISPI_BPP) != 32) {
            program(b, w, h);
            m.pitch = w * 4;
        }
    } else {
        uint32_t w = b->pref_w && fits(b, b->pref_w, b->pref_h) ? b->pref_w : 1024;
        uint32_t h = b->pref_w && fits(b, b->pref_w, b->pref_h) ? b->pref_h : 768;
        program(b, w, h);
        m = (struct display_mode){ w, h, w * 4, 60, w == b->pref_w && h == b->pref_h };
    }
    return display_takeover(b->fb_phys, b->fb_phys + size, &bochs_ops, b, desc, &m, b->fb_phys, b->vram) != NULL;
}

void bochs_probe(void)
{
    static const uint16_t ids[] = { 0x1111 };
    struct pci_dev pd[MAX_CARDS];
    int n = pci_find_all(0x1234, ids, 1, pd, MAX_CARDS);
    for (int i = 0; i < n && ncards < MAX_CARDS; i++)
        if (bochs_start(&cards[ncards], &pd[i])) {
            pci_claim(&pd[i], "bochs");
            ncards++;
        }
}

DDI_DRIVER("bochs", DDI_PHASE_DISPLAY, "QEMU/Bochs standard VGA (the DISPI mode registers)");
DDI_ALIAS("pci1234,1111");

int _init(void)
{
    bochs_probe();
    return 0;
}
