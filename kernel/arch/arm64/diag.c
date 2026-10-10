/*
 * diag.c (arm64) - The first boot steps, shown on the screen of a Raspberry
 * Pi 4 or 5, for when no serial cable is connected.
 *
 * Right after the firmware starts the kernel, we ask it for a frame buffer
 * (the "mailbox", as the desktop does later) and paint one coloured block
 * per boot step reached, left to right, each with as many white dots as
 * its number. If the kernel faults or stops before a program runs, a red
 * band at the bottom shows the step and the exception as rows of squares
 * (white = 1). A photo of the screen then says where it stopped.
 *
 * This code also runs before the MMU is on (boot.S calls it from the first
 * instruction on), so it follows the rules of that world: memory behaves as
 * a device then (aligned accesses only: the device tree is read byte by
 * byte), no absolute addresses (no switch, no pointer tables: everything is
 * reached relative to the program counter), and its state lives in .data,
 * which boot.S does not clear (it clears .bss). Other machines (QEMU's virt)
 * have no such firmware: there it does nothing.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"
#include "arm64.h"

#define NSTAGE 12

static struct {
    uint64_t fb;            /* the frame buffer, physical (0: none: do nothing) */
    uint64_t base;          /* added to fb to reach it: 0 before the MMU, DMAP after */
    uint32_t w, h, pitch;   /* pixels, pixels, bytes per row */
    int on;                 /* 1 until the first program runs (then the screen is its) */
    int faulted;
} D __attribute__((section(".data")));

static uint32_t msg[36] __attribute__((aligned(16), section(".data")));

/* ---- The device tree, byte by byte (aligned reads only before the MMU). */
static uint32_t be32(const volatile uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
/* Does the root node's "compatible" name this chip ("brcm,bcm2711")? */
static int root_is(const volatile uint8_t *f, const char *chip)
{
    if (be32(f) != 0xD00DFEED) return 0;
    const volatile uint8_t *st = f + be32(f + 8), *strs = f + be32(f + 12);
    if (be32(st) != 1) return 0;                     /* FDT_BEGIN_NODE: the root, name "" */
    uint32_t o = 8;                                  /* token + empty name, padded */
    while (be32(st + o) == 3) {                      /* FDT_PROP: len, name offset, value */
        uint32_t len = be32(st + o + 4), no = be32(st + o + 8);
        const volatile uint8_t *v = st + o + 12, *name = strs + no;
        const char *c = "compatible";
        int k = 0;
        while (c[k] && name[k] == (uint8_t)c[k]) k++;
        if (!c[k] && !name[k])
            for (uint32_t i = 0; i < len; i++) {     /* any of the strings contains chip */
                int j = 0;
                while (chip[j] && i + j < len && v[i + j] == (uint8_t)chip[j]) j++;
                if (!chip[j]) return 1;
            }
        o += 12 + ((len + 3) & ~3u);
    }
    return 0;
}

/* ---- The firmware's mailbox: (buffer's bus address | channel 8). */
static int mailbox(uint64_t mb, uint32_t bus_or)
{
    volatile uint32_t *m = (volatile uint32_t *)mb;
    uint32_t a = (uint32_t)((uint64_t)msg - D.base) | bus_or;    /* (before the MMU: physical) */
    for (int k = 0; k < 1000000 && (m[0x38 / 4] & 0x80000000u); k++) ;   /* STATUS1: full */
    m[0x20 / 4] = a | 8;
    for (int k = 0; k < 10000000; k++) {
        if (m[0x18 / 4] & 0x40000000u) continue;                       /* STATUS0: empty */
        if (m[0] == (a | 8)) return msg[1] == 0x80000000u ? 0 : -1;
    }
    return -1;
}

/* ---- Drawing: solid rectangles, each row cleaned to memory (after the MMU
 * the frame buffer is reached through the caches; the display reads RAM). */
static void fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    if (!D.fb || x >= D.w || y >= D.h) return;
    if (x + w > D.w) w = D.w - x;
    if (y + h > D.h) h = D.h - y;
    for (uint32_t j = 0; j < h; j++) {
        volatile uint32_t *row = (volatile uint32_t *)(D.fb + D.base + (uint64_t)(y + j) * D.pitch) + x;
        for (uint32_t i = 0; i < w; i++) row[i] = rgb;
        for (uint64_t a = (uint64_t)row & ~63UL; a < (uint64_t)(row + w); a += 64)
            asm volatile("dc cvac, %0" : : "r"(a) : "memory");
    }
    asm volatile("dsb sy" : : : "memory");
}

/* n white dots, 4 per row, inside the box (x, y, size s). */
static void dots(uint32_t x, uint32_t y, uint32_t s, int n)
{
    uint32_t d = s / 10;
    for (int i = 0; i < n; i++) fill(x + d + (uint32_t)(i % 4) * 2 * d, y + d + (uint32_t)(i / 4) * 2 * d, d, d, 0xFFFFFF);
}

/* A row of squares for the low `bits` bits of v, most significant first,
 * a gap every 4 (one hexadecimal digit per group). */
static void bits_row(uint32_t y, uint32_t sq, uint64_t v, int bits, uint32_t on)
{
    uint32_t x = sq;
    for (int b = bits - 1; b >= 0; b--) {
        fill(x, y, sq, sq, v >> b & 1 ? on : 0x400000);
        x += sq + sq / 3 + (b % 4 == 0 ? sq : 0);
    }
}

static const uint32_t colour[NSTAGE] = {
    0x2E86DE, 0x10AC84, 0xF368E0, 0xFF9F43, 0x54A0FF, 0x1DD1A1,
    0xEE5253, 0xFECA57, 0x5F27CD, 0x00D2D3, 0xC8D6E5, 0x4BE3C1,
};
static uint32_t bsize(void) { return D.w / 16; }
static uint32_t bx(int n)   { return D.w / 2 - (NSTAGE * bsize() + (NSTAGE - 1) * bsize() / 4) / 2 + (uint32_t)(n - 1) * (bsize() + bsize() / 4); }
static uint32_t by(void)    { return D.h / 4; }

/* ---- boot.S: the first thing, with the device tree's physical address. */
void diag_init(uint64_t dtb)
{
    const volatile uint8_t *f = (const volatile uint8_t *)dtb;
    uint64_t mb;
    uint32_t bus;
    if (root_is(f, "bcm2711"))      { mb = 0xFE00B880UL;   bus = 0xC0000000u; }   /* Pi 4, peripherals low */
    else if (root_is(f, "bcm2712")) { mb = 0x107C013880UL; bus = 0; }             /* Pi 5 */
    else return;
    /* the screen's size as the firmware set it up (else 1920 x 1080), */
    uint32_t q[] = { 0, 0, 0x40003, 8, 0, 0, 0, 0 };
    for (int i = 0; i < 8; i++) msg[i] = q[i];
    msg[0] = 8 * 4;
    uint32_t w = 1920, h = 1080;
    if (!mailbox(mb, bus) && msg[5] >= 640 && msg[6] >= 480 && msg[5] <= 7680 && msg[6] <= 4320) { w = msg[5]; h = msg[6]; }
    /* then that size, 32 bits, our 0x00RRGGBB words, allocate, pitch: as the
     * desktop asks later */
    uint32_t t[] = { 0, 0, 0x48003, 8, 0, w, h, 0x48004, 8, 0, w, h,
                     0x48005, 4, 0, 32, 0x48006, 4, 0, 0, 0x40001, 8, 0, 4096, 0,
                     0x40008, 4, 0, 0, 0 };
    int n = sizeof t / 4;
    for (int i = 0; i < n; i++) msg[i] = t[i];
    msg[0] = n * 4;
    if (mailbox(mb, bus) || !msg[23] || !msg[10] || msg[28] < msg[10] * 4) return;
    D.fb = msg[23] & 0x3FFFFFFF;
    D.w = msg[10]; D.h = msg[11]; D.pitch = msg[28];
    D.on = 1;
    fill(0, 0, D.w, D.h, 0x0A0F1C);                  /* the background */
    for (int i = 1; i <= NSTAGE; i++) fill(bx(i), by() + bsize() + bsize() / 4, bsize(), bsize() / 10, 0x2A3550);
    diag_stage(1);
}

/* A boot step reached: its block. */
void diag_stage(int n)
{
    if (!D.on || n < 1 || n > NSTAGE) return;
    fill(bx(n), by(), bsize(), bsize(), colour[n - 1]);
    dots(bx(n), by(), bsize(), n);
    if (n == NSTAGE) D.on = 0;                       /* a program runs: the screen is the desktop's now */
#ifdef DIAG_TEST
    /* make ARCH=arm64 DIAG_TEST=n: a fault on purpose right after step n, to
     * see the red band (an address no RAM and no table maps, MMU on or off) */
    if (n == DIAG_TEST) *(volatile uint32_t *)0xFFFF7F0000000000UL = 1;
#endif
}

/* boot.S, at the kernel's high address: reach the frame buffer through the
 * direct map, if its GiB is mapped there yet (boot.S maps the kernel's and
 * the device tree's; the Pi 4's frame buffer is in the kernel's). */
void diag_high(uint64_t phys, uint64_t dtb)
{
    if (D.fb && (D.fb >> 30 == phys >> 30 || D.fb >> 30 == dtb >> 30)) D.base = DMAP;
    else D.on = 0;
    diag_stage(6);
}

static int last_stage(void)
{
    int n = 0;
    for (int i = 1; i <= NSTAGE; i++) {               /* the blocks painted: read back their first pixel */
        volatile uint32_t *p = (volatile uint32_t *)(D.fb + D.base + (uint64_t)by() * D.pitch) + bx(i);
        if (*p == colour[i - 1]) n = i;
    }
    return n;
}

/* A failure before any program runs: the red band. kind: 1 exception,
 * 2 the boot modules are missing or damaged, 3 the kernel stopped (panic),
 * 4 init, the first program, faulted before its first system call. */
void diag_fault(int kind, uint64_t esr, uint64_t elr, uint64_t far)
{
    if (!D.on || D.faulted) return;
    D.faulted = 1;
    uint32_t y = D.h * 5 / 8, sq = D.w / 80;
    fill(0, y, D.w, D.h - y, 0x8B0000);
    dots(sq, y + sq, 8 * sq, last_stage());          /* the last step reached */
    dots(D.w - 9 * sq, y + sq, 8 * sq, kind);        /* what happened */
    bits_row(y + 10 * sq, sq, esr >> 26 & 0x3F, 6, 0xFFFFFF);   /* exception class */
    bits_row(y + 12 * sq, sq, elr, 24, 0xFFFFFF);    /* where: the low 24 bits of the address */
    bits_row(y + 14 * sq, sq, far, 24, 0xFFE066);    /* the address it touched (yellow) */
}

/* arch_halt_forever: the kernel stops (a panic) before any program ran. */
void diag_halt(void) { diag_fault(3, 0, 0, 0); }
