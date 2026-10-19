/*
 * gfxhat.c - The console's log on a Pimoroni GFX HAT (Raspberry Pi 4): what
 * a serial wire would show, on the HAT's 128 x 64 screen, without a wire.
 *
 *   ┌─────────────────────┐  8 lines of 21 characters (a 5 x 7 font in
 *   │mk: 4 CPUs, 3840 MiB │  6 x 8 cells); long lines wrap. It follows
 *   │init: con started    │  the log as it grows; the touch buttons go
 *   │...                  │  back in it:
 *   │login:              ▐│    up / down     a line      minus / plus  a page
 *   └─────────────────────┘    back          the newest  select        the contrast
 * Select shows the contrast on the last row: minus / plus change it, back
 * (or 4 s untouched) keeps it (/etc/gfxhat.conf: these screens differ, and
 * one set wrong reads only from the side); select again: the backlight
 * off / on.
 * Scrolled back, a bar on the right says where; new lines then do not move
 * what is shown. The backlight is amber, like the desktop's accent.
 *
 * The HAT: an ST7567 screen on SPI0 (chip select 0; GPIO 6: data or
 * command, GPIO 5: reset), an SN3218 LED driver for the backlight (I2C1,
 * address 0x54), a CAP1166 for the six touch buttons (I2C1, 0x2c), which
 * also says whether a HAT is there at all. The log comes from the kernel
 * (SYS_LOG: its messages and the console server's output); it is read
 * every 100 ms, the buttons every 30 ms.
 *
 * Started by init (from the disk); with no Raspberry Pi GPIO (x86-64,
 * QEMU) or no HAT on it, it ends with 0 at once, and is not started again.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#ifdef GFXHAT_HOST                          /* (tests/gfxhat: the text part, on the development machine) */
#include <stdint.h>
#include <string.h>
#else
#include "mk.h"
#include <stdarg.h>
#ifdef __aarch64__
#include "mk/fdt.h"
#include "mbox.h"
#endif
#endif

#if !defined(GFXHAT_HOST) && !defined(__aarch64__)
int main(int argc, char **argv) { (void)argc; (void)argv; return 0; }   /* (no Raspberry Pi here) */
#else
/* ---- The text: the log cut into rows of 21 characters. */
#define COLS 21
#define ROWS 8
#define KEEP 1024                           /* rows kept (a power of two) */
static char rows[KEEP][COLS];
static uint64_t nrows = 1;                  /* rows so far (the last one is being written) */
static int col, esc;                        /* esc: 1 after ESC, 2 inside "ESC [ ... letter" */
static int back;                            /* rows scrolled back from the newest (0: following) */

static void new_row(void)
{
    memset(rows[nrows % KEEP], ' ', COLS);
    nrows++;
    col = 0;
    if (back) back = back + 1 < (int)(KEEP - ROWS) ? back + 1 : back;   /* (what is shown stays) */
}

static void text(const char *s, long n)
{
    for (long i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char *r = rows[(nrows - 1) % KEEP];
        if (esc == 1) { esc = c == '[' ? 2 : 0; continue; }
        if (esc == 2) { if (c >= 0x40 && c <= 0x7e) esc = 0; continue; }
        if (c == 27) esc = 1;
        else if (c == '\n') new_row();
        else if (c == '\b') { if (col) r[--col] = ' '; }
        else if (c == '\t') { do { if (col == COLS) new_row(); rows[(nrows - 1) % KEEP][col++] = ' '; } while (col % 4); }
        else if (c >= 32 && c < 127) {
            if (col == COLS) { new_row(); r = rows[(nrows - 1) % KEEP]; }
            r[col++] = (char)c;
        }                                   /* (\r and the rest: nothing) */
    }
}

/* ---- The picture: 8 pages of 128 columns, a byte = 8 pixels down (bit 0
 * on top), as the ST7567 takes them. */
static uint8_t pic[8][128];

static const uint8_t font5x7[95][5] = {     /* ' ' .. '~', a column per byte */
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x56,0x20,0x50},{0x00,0x08,0x07,0x03,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x2A,0x1C,0x7F,0x1C,0x2A},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x80,0x70,0x30,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x00,0x60,0x60,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x72,0x49,0x49,0x49,0x46},{0x21,0x41,0x49,0x4D,0x33},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x31},{0x41,0x21,0x11,0x09,0x07},
    {0x36,0x49,0x49,0x49,0x36},{0x46,0x49,0x49,0x29,0x1E},{0x00,0x00,0x14,0x00,0x00},{0x00,0x40,0x34,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x59,0x09,0x06},
    {0x3E,0x41,0x5D,0x59,0x4E},{0x7C,0x12,0x11,0x12,0x7C},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x41,0x3E},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x41,0x51,0x73},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x1C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x26,0x49,0x49,0x49,0x32},
    {0x03,0x01,0x7F,0x01,0x03},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},{0x61,0x59,0x49,0x4D,0x43},{0x00,0x7F,0x41,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x41,0x7F},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x03,0x07,0x08,0x00},{0x20,0x54,0x54,0x78,0x40},{0x7F,0x28,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x28},
    {0x38,0x44,0x44,0x28,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x00,0x08,0x7E,0x09,0x02},{0x18,0xA4,0xA4,0x9C,0x78},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x40,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x78,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0xFC,0x18,0x24,0x24,0x18},{0x18,0x24,0x24,0x18,0xFC},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x24},
    {0x04,0x04,0x3F,0x44,0x24},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x4C,0x90,0x90,0x90,0x7C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x77,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x02,0x01,0x02,0x04,0x02},
};

/* The rows on the screen: the newest ROWS, or as far back as `back` says. */
static void render(void)
{
    memset(pic, 0, sizeof pic);
    uint64_t kept = nrows < KEEP ? nrows : KEEP, last = nrows - 1 - (uint64_t)back;
    for (int i = 0; i < ROWS; i++) {
        uint64_t r = last - (ROWS - 1 - i);
        if (last < (uint64_t)(ROWS - 1 - i) || r + kept < nrows) continue;   /* (before the first, or forgotten) */
        for (int k = 0; k < COLS; k++) {
            unsigned c = (unsigned char)rows[r % KEEP][k];
            memcpy(&pic[i][k * 6], font5x7[c >= 32 && c < 127 ? c - 32 : 0], 5);   /* (0: an empty row's cell) */
        }
    }
    if (back) {                             /* where we are: a bar in the last two columns */
        int span = kept > ROWS ? (int)kept - ROWS : 1;
        int len = 64 * ROWS / (int)(kept > ROWS ? kept : ROWS), top = (64 - len) * (span - back) / span;
        if (len < 4) len = 4;
        if (top < 0) top = 0;
        if (top + len > 64) top = 64 - len;
        for (int y = top; y < top + len; y++) pic[y / 8][126] |= 1 << y % 8, pic[y / 8][127] |= 1 << y % 8;
    }
}

static void scroll(int by)                  /* + : back in the log */
{
    uint64_t kept = nrows < KEEP ? nrows : KEEP;
    int most = kept > ROWS ? (int)(kept - ROWS) : 0;
    back += by;
    if (back > most) back = most;
    if (back < 0) back = 0;
}

#ifndef GFXHAT_HOST
/* A line in the kernel log. */
static struct { char b[120]; size_t n; } line;
static void put(void *c, char ch) { (void)c; if (line.n < sizeof line.b) line.b[line.n++] = ch; }
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    line.n = 0;
    vformat(put, 0, fmt, ap);
    va_end(ap);
    sys_debug(line.b, line.n);
}

/* ---- The hardware: the GPIO block, I2C1 ("BSC1"), SPI0. */
static volatile uint32_t *gpio, *i2c, *spi;
static uint32_t core_hz = 500000000;        /* the clock I2C and SPI divide (the firmware says) */

static void pin_func(int pin, int f)        /* 0: input, 1: output, 4: ALT0 */
{
    volatile uint32_t *r = gpio + pin / 10;
    *r = (*r & ~(7u << pin % 10 * 3)) | (uint32_t)f << pin % 10 * 3;
}
static void pin_set(int pin, int on) { gpio[(on ? 0x1c : 0x28) / 4] = 1u << pin; }
static void ms(int n) { sys_sleep((uint64_t)n * 1000000); }

enum { I_C, I_S, I_DLEN, I_A, I_FIFO, I_DIV };                     /* (32-bit registers) */
#define I2CEN (1u << 15)
#define ST    (1u << 7)
#define CLEAR (3u << 4)
#define S_DONE 2u
#define S_TXD  0x10u
#define S_RXD  0x20u
#define S_ERR  0x100u
#define S_CLKT 0x200u

static int i2c_go(int addr, uint8_t *b, int n, int rd)   /* one transfer: 0, or -1 (no answer) */
{
    i2c[I_S] = S_DONE | S_ERR | S_CLKT;
    i2c[I_C] = I2CEN | CLEAR;
    i2c[I_A] = (uint32_t)addr;
    i2c[I_DLEN] = (uint32_t)n;
    int k = 0;
    if (!rd) while (k < n && (i2c[I_S] & S_TXD)) i2c[I_FIFO] = b[k++];
    i2c[I_C] = I2CEN | ST | (rd ? 1u : 0);
    uint64_t end = (uint64_t)sys_clock() + 20000000;   /* 20 ms */
    while (!(i2c[I_S] & S_DONE)) {
        if (rd) while (k < n && (i2c[I_S] & S_RXD)) b[k++] = (uint8_t)i2c[I_FIFO];
        else while (k < n && (i2c[I_S] & S_TXD)) i2c[I_FIFO] = b[k++];
        if (i2c[I_S] & (S_ERR | S_CLKT)) break;
        if ((uint64_t)sys_clock() > end) { i2c[I_C] = CLEAR; return -1; }
    }
    if (rd) while (k < n && (i2c[I_S] & S_RXD)) b[k++] = (uint8_t)i2c[I_FIFO];
    uint32_t s = i2c[I_S];
    i2c[I_S] = S_DONE | S_ERR | S_CLKT;
    return s & (S_ERR | S_CLKT) || k < n ? -1 : 0;
}
static int reg_w(int addr, int reg, int v) { uint8_t b[2] = { (uint8_t)reg, (uint8_t)v }; return i2c_go(addr, b, 2, 0); }
static int reg_r(int addr, int reg)
{
    uint8_t b = (uint8_t)reg;
    if (i2c_go(addr, &b, 1, 0) || i2c_go(addr, &b, 1, 1)) return -1;
    return b;
}

enum { P_CS, P_FIFO, P_CLK };
#define TA   (1u << 7)
#define DONE (1u << 16)
#define RXD  (1u << 17)
#define TXD  (1u << 18)
static void spi_send(const uint8_t *b, int n, int data)   /* to the screen (chip select 0) */
{
    pin_set(6, data);                       /* data or command */
    spi[P_CS] = CLEAR;
    spi[P_CS] = TA;
    for (int i = 0; i < n; i++) {
        while (!(spi[P_CS] & TXD)) ;
        spi[P_FIFO] = b[i];
        while (spi[P_CS] & RXD) (void)spi[P_FIFO];
    }
    while (!(spi[P_CS] & DONE)) while (spi[P_CS] & RXD) (void)spi[P_FIFO];
    spi[P_CS] = 0;
}

/* ---- The HAT's parts. */
#define CAP 0x2c                            /* the buttons */
#define LED 0x54                            /* the backlight */
static int light = 1, contrast = 58, adjusting;   /* (58: Pimoroni's) */
static uint64_t now, adjust_until;
#define CONF "/etc/gfxhat.conf"

static void backlight(int on)
{
    static const uint8_t zone[6] = { 2, 1, 0, 5, 4, 3 };   /* the six LEDs, left to right, as wired */
    uint8_t b[19] = { 0x01 };               /* the 18 channels' brightness, from register 1 */
    for (int i = 0; i < 6; i++) {
        uint8_t *c = &b[1 + zone[i] * 3];
        c[0] = on ? 12 : 0; c[1] = on ? 100 : 0; c[2] = on ? 170 : 0;   /* blue, green, red: the desktop's amber */
    }
    i2c_go(LED, b, sizeof b, 0);
    reg_w(LED, 0x16, 0xff);                 /* (shows them) */
}

static void screen_init(void)
{
    pin_set(5, 0); ms(10); pin_set(5, 1); ms(100);   /* reset */
    static const uint8_t init[] = {
        0xa3,                               /* bias 1/7 */
        0xa0,                               /* columns left to right */
        0xc8,                               /* rows bottom to top (the HAT's glass is upside down) */
        0xa6,                               /* not inverted */
        0x40,                               /* from line 0 */
        0x2f,                               /* booster, regulator, follower on */
        0x23,                               /* regulation ratio */
        0xaf,                               /* on */
    };
    spi_send(init, sizeof init, 0);
}
static void set_contrast(void) { uint8_t c[2] = { 0x81, (uint8_t)contrast }; spi_send(c, 2, 0); }
static void contrast_load(void)
{
    char b[32] = { 0 };
    long f = fs_open(CONF, FS_RDONLY, 0), n = f >= 0 ? fs_read(f, 0, b, sizeof b - 1) : -1;
    if (f >= 0) fs_close(f);
    if (n > 9 && !memcmp(b, "contrast=", 9)) {
        int v = 0;
        for (char *p = b + 9; *p >= '0' && *p <= '9'; p++) v = v * 10 + *p - '0';
        if (v <= 63) contrast = v;
    }
}
static void contrast_save(void)
{
    char b[32] = "contrast=";
    int n = 9;
    if (contrast >= 10) b[n++] = (char)('0' + contrast / 10);
    b[n++] = (char)('0' + contrast % 10);
    b[n++] = '\n';
    long f = fs_open(CONF, FS_WRONLY | FS_CREAT | FS_TRUNC, 0644);
    if (f >= 0) { fs_write(f, 0, b, (size_t)n); fs_close(f); }
    say("gfxhat: contrast %d (kept in " CONF ")\n", contrast);
}
/* The contrast on the last row, white on black: "contrast 58   - +". */
static void contrast_row(void)
{
    char t[COLS + 1] = "contrast     -  +    ";
    t[9] = (char)(contrast >= 10 ? '0' + contrast / 10 : ' ');
    t[10] = (char)('0' + contrast % 10);
    memset(pic[7], 0, 128);
    for (int k = 0; k < COLS; k++) memcpy(&pic[7][k * 6], font5x7[(unsigned char)t[k] - 32], 5);
    for (int x = 0; x < 128; x++) pic[7][x] = (uint8_t)~pic[7][x];
}
static void screen_show(void)
{
    for (int p = 0; p < 8; p++) {
        uint8_t at[3] = { (uint8_t)(0xb0 | p), 0x00, 0x10 };   /* page p, column 0 */
        spi_send(at, 3, 0);
        spi_send(pic[p], 128, 1);
    }
}

/* What the board has: the GPIO block, I2C1, SPI0 (by their place on the
 * Pis' peripheral bus: 0x7e200000, 0x7e804000, 0x7e204000). */
static volatile uint32_t *find(const void *f, const char *compat, uint64_t at)
{
    uint64_t a, size;
    for (int n = -1; (n = fdt_find(f, n, compat)) >= 0; )
        if (!fdt_reg(f, n, 0, &a, &size) && (a & 0xffffff) == at) {
            void *m = map_phys(a, 4096);
            return (long)m < 0 ? 0 : m;
        }
    return 0;
}
static int hardware(void)
{
    long size = sys_fdt(0, 0);
    char *f = size > 0 ? malloc(size) : 0;
    if (!f || sys_fdt(f, size) != size) { free(f); return -1; }
    gpio = find(f, "brcm,bcm2711-gpio", 0x200000);
    if (!gpio) gpio = find(f, "brcm,bcm2835-gpio", 0x200000);
    i2c = find(f, "brcm,bcm2711-i2c", 0x804000);
    if (!i2c) i2c = find(f, "brcm,bcm2835-i2c", 0x804000);
    spi = find(f, "brcm,bcm2835-spi", 0x204000);
    free(f);
    if (!gpio || !i2c || !spi) return -1;
    uint32_t __attribute__((aligned(16))) m[8] = { sizeof m, 0, 0x00030002, 8, 0, 4, 0, 0 };   /* the core clock's rate */
    if (!mbox_call(m, 8) && m[6]) core_hz = m[6];
    pin_func(2, 4); pin_func(3, 4);                          /* I2C1: SDA, SCL */
    for (int p = 7; p <= 11; p++) pin_func(p, 4);            /* SPI0: CE1, CE0, MISO, MOSI, SCLK */
    pin_func(5, 1); pin_func(6, 1);                          /* the screen's reset, data/command */
    pin_set(5, 1);
    i2c[I_DIV] = core_hz / 100000;                           /* 100 kHz */
    spi[P_CLK] = (core_hz / 1000000 + 1) & ~1u;              /* 1 MHz (an even divider) */
    return 0;
}

enum { B_UP, B_DOWN, B_BACK, B_MINUS, B_SELECT, B_PLUS };
static void press(int b)
{
    if (b == B_UP) scroll(1);
    else if (b == B_DOWN) scroll(-1);
    else if (b == B_MINUS) scroll(ROWS - 1);
    else if (b == B_PLUS) scroll(-(ROWS - 1));
    else if (b == B_BACK) back = 0;
    else if (b == B_SELECT) backlight(light = !light);
}
static void adjust(int b)                                    /* the contrast row is shown */
{
    adjust_until = now + 4000000000ULL;
    if (b == B_MINUS || b == B_DOWN) { if (contrast > 0) contrast--; set_contrast(); }
    else if (b == B_PLUS || b == B_UP) { if (contrast < 63) contrast++; set_contrast(); }
    else if (b == B_SELECT) backlight(light = !light);
    else if (b == B_BACK) { adjusting = 0; contrast_save(); }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (hardware()) return 0;                                /* not a Raspberry Pi 2..4 */
    if (reg_r(CAP, 0xfd) != 0x51) {                          /* the CAP1166's product id */
        say("gfxhat: no GFX HAT\n");
        return 0;
    }
    reg_w(CAP, 0x26, 0x3f);                                  /* calibrate the buttons */
    reg_w(CAP, 0x72, 0x00);                                  /* their LEDs: ours, not linked to touch */
    reg_w(LED, 0x00, 0x01);                                  /* the backlight: on, ... */
    reg_w(LED, 0x13, 0x3f); reg_w(LED, 0x14, 0x3f); reg_w(LED, 0x15, 0x3f);   /* ... all 18 channels */
    backlight(1);
    contrast_load();
    screen_init();
    set_contrast();
    say("gfxhat: the console's log on the GFX HAT (%u MHz core clock)\n", core_hz / 1000000);

    uint64_t pos = 0, next_log = 0, held_since = 0, next_repeat = 0;
    int held = 0, dirty = 1;
    static char buf[1024];
    for (;;) {
        now = (uint64_t)sys_clock();
        if (adjusting && now > adjust_until) { adjusting = 0; contrast_save(); dirty = 1; }
        if (now >= next_log) {                               /* the log */
            long n;
            while ((n = sys_log(buf, sizeof buf, &pos)) > 0) { text(buf, n); dirty = 1; }
            next_log = now + 100000000;
        }
        int main_c = reg_r(CAP, 0x00), st = reg_r(CAP, 0x03);   /* the buttons */
        if (main_c >= 0 && st >= 0) {
            if (main_c & 1) reg_w(CAP, 0x00, main_c & ~1);   /* (clears the touches no longer there) */
            st &= 0x3f;
            int down = st & ~held;
            for (int b = 0; b < 6; b++)
                if (down & 1 << b) {
                    if (adjusting) adjust(b);
                    else if (b == B_SELECT) { adjusting = 1; adjust_until = now + 4000000000ULL; }
                    else press(b);
                    dirty = 1;
                }
            if (down) { held_since = now; next_repeat = now + 400000000; }
            else if (st & (1 << B_UP | 1 << B_DOWN | 1 << B_MINUS | 1 << B_PLUS) && st == held && now >= next_repeat) {
                for (int b = 0; b < 6; b++) if (st & 1 << b && b != B_BACK && b != B_SELECT) { if (adjusting) adjust(b); else press(b); }
                next_repeat = now + (now - held_since > 2000000000 ? 30000000 : 80000000);   /* (faster after 2 s) */
                dirty = 1;
            }
            if (st != held) reg_w(CAP, 0x74, st);            /* a touched button lights */
            held = st;
        }
        if (dirty) { render(); if (adjusting) contrast_row(); screen_show(); dirty = 0; }
        ms(30);
    }
}
#endif
#endif
