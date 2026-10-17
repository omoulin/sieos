/*
 * fbcon.c (x86-64) - The kernel log on the firmware's screen, for machines
 * with no serial port (most laptops and tablets): a phone photo of the
 * screen then says where the boot stopped.
 *
 * Started by SIEOS's UEFI loader, the kernel finds the frame buffer the
 * firmware set up (GOP). Every byte of the log (arch_putc: the kernel's
 * lines and the programs', SYS_DEBUG) is kept in a small text buffer and
 * drawn there in atlas's font (5 x 9 pixels), scaled to the screen, until
 * a driver maps that frame buffer (the desktop takes the screen: SYS_MAP_PHYS,
 * arch_phys_mapped). A panic takes it back and shows the last lines.
 *
 * Before the page allocator works, the screen is reached through boot.S's
 * direct map of the first 4 GiB (if it is there); once kmap works
 * (arch_mem_init), through a write-combining mapping of its own, wherever
 * it is. What was printed before is drawn then.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"
#include "x86.h"
#include "../../../user/atlas/font.c"

#define LOGN 128                        /* lines kept (more than any screen shows) */
#define LOGW 200                        /* characters per line at most */
static char lines[LOGN][LOGW + 1];
static uint8_t drawn[LOGN];             /* per screen row: characters on it now */
static int nlines, col;                 /* lines begun; the column in the last one */
static int on;                          /* 1: we draw (the screen is not a driver's) */
static uint8_t *fb;                     /* the frame buffer, mapped (0: not yet) */
static uint32_t sc, cw, ch, rows, cols, fg, bg;   /* scale, cell size, text size, colours */

static void cell(uint32_t r, uint32_t c, char k)
{
    const uint8_t *g = font[k > 32 && k < 127 ? k - 32 : 0];
    for (uint32_t j = 0; j < ch; j++) {
        volatile uint32_t *px = (volatile uint32_t *)(fb + (uint64_t)(r * ch + j + sc) * x86_fb.pitch * 4) + (c + 1) * cw;
        uint32_t gy = j / sc, bits = gy < 9 ? g[gy] : 0;
        for (uint32_t i = 0; i < cw; i++) px[i] = i / sc < 5 && bits >> (4 - i / sc) & 1 ? fg : bg;
    }
}

/* Screen row r shows line l (or nothing: l < 0). */
static void row(uint32_t r, int l)
{
    const char *s = l >= 0 ? lines[l % LOGN] : "";
    uint32_t n = 0;
    for (; s[n] && n < cols; n++) cell(r, n, s[n]);
    for (uint32_t i = n; i < drawn[r]; i++) cell(r, i, ' ');
    drawn[r] = (uint8_t)n;
}

/* Every row: lines top, top + 1, ... */
static int top;                         /* the line on the first row */
static void redraw(void)
{
    for (uint32_t r = 0; r < rows; r++) row(r, top + (int)r < nlines ? top + (int)r : -1);
}

static void clear(void)
{
    for (uint32_t y = 0; y < x86_fb.h; y++) {
        volatile uint32_t *px = (volatile uint32_t *)(fb + (uint64_t)y * x86_fb.pitch * 4);
        for (uint32_t x = 0; x < x86_fb.w; x++) px[x] = bg;
    }
    for (int r = 0; r < LOGN; r++) drawn[r] = 0;
}

/* The frame buffer is reachable at p: clear it, draw what was printed. */
static void attach(uint8_t *p)
{
    fb = p;
    clear();
    redraw();
}

/* platform.c: the UEFI loader gave a screen (x86_fb). */
void fbcon_start(void)
{
    if (!x86_fb.pa || x86_fb.w < 320 || x86_fb.h < 200 || x86_fb.pitch < x86_fb.w) return;
    sc = (x86_fb.h + 300) / 600;            /* 800 lines: 1; 1080: 2; 1824: 3; 2160: 4 */
    cw = 6 * sc; ch = 11 * sc;
    rows = x86_fb.h / ch - 1;
    cols = x86_fb.w / cw - 2;
    if (rows > LOGN) rows = LOGN;
    if (cols > LOGW) cols = LOGW;
    fg = 0xD0D0D0; bg = 0x101018;
    if (x86_fb.fmt == 0) bg = 0x181010;     /* bytes R, G, B: (blue in the low byte) */
    on = 1;
    if (x86_fb.pa + x86_fb.size <= 4UL << 30) attach(P2V(x86_fb.pa));   /* boot.S's direct map */
}

/* mmu.c, once kmap works: a mapping of its own (write-combining once cpu.c
 * has set the PAT: PWT alone). */
void fbcon_map(void)
{
    if (on) attach(kmap(x86_fb.pa, x86_fb.size, PTE_PWT | PTE_NX));
}

/* arch_putc: one byte of the log. A full screen scrolls by a quarter at
 * once (each scroll redraws it all: a few ms). */
void fbcon_putc(char k)
{
    if (!on || k == '\r') return;
    if (!nlines) nlines = 1;
    if (k == '\n' || col >= (int)cols) {
        lines[nlines++ % LOGN][0] = 0;
        col = 0;
        if (nlines - top > (int)rows) {
            top = nlines - (int)rows + (int)rows / 4;
            if (fb) redraw();
        }
        if (k == '\n') return;
    }
    char *l = lines[(nlines - 1) % LOGN];
    l[col++] = k == '\t' ? ' ' : k;
    l[col] = 0;
    uint32_t r = (uint32_t)(nlines - 1 - top);
    if (fb && (uint32_t)col <= cols) { cell(r, (uint32_t)col - 1, l[col - 1]); if (drawn[r] < col) drawn[r] = (uint8_t)col; }
}

/* SYS_MAP_PHYS gave a driver [pa, pa + size): if that is our screen, it is
 * the driver's now (the desktop). */
void fbcon_mapped(uint64_t pa, uint64_t size)
{
    if (on && pa < x86_fb.pa + x86_fb.size && pa + size > x86_fb.pa) on = 0;
}

/* A panic: the screen is ours again, the last lines on it. */
void arch_panic_screen(void)
{
    if (!fb || on) return;
    on = 1;
    clear();
    redraw();
}
