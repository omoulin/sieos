/*
 * sdio.c - The SDIO host for the Wi-Fi chip (wifi.h). An SDHCI controller
 * (the same register layout as the disk server's sd.c) driven as an SDIO
 * host: CMD5 brings an SDIO card up, CMD52 reads or writes one register,
 * CMD53 moves a block or a byte stream to or from a function.
 *
 *   The Pi 4's chip is on the legacy "emmc" controller (mmcnr@7e300000),
 *   shared with nothing here; its six data/clock/command pins (GPIO 34-39)
 *   are switched to ALT3 and the chip powered by WL_ON on the firmware's
 *   GPIO expander (mailbox).
 *   The Pi 5's chip is on the BCM2712's second SDHCI (mmc@1001100000,
 *   "sdio2"); its pins (GPIO 30-35) are switched to the "sd2" function and
 *   the chip powered by WL_ON (GPIO 28 on the always-on GPIO block).
 *
 * We find the controller by its "brcm,bcm4329-fmac" child in the device
 * tree. The chip is non-removable and needs no 1.8 V switch (it runs at
 * 3.3 V signalling, High Speed), so this is much simpler than sd.c.
 *
 * UNTESTED ON HARDWARE (QEMU emulates no SDIO card): docs/raspberrypi.md.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "wifi.h"
#include "mk/fdt.h"
#include "mbox.h"

enum { BLOCK = 0x04, ARG = 0x08, CMD = 0x0C, RESP = 0x10, DATA = 0x20, PRESENT = 0x24,
       CTRL = 0x28, CLOCK = 0x2C, STATUS = 0x30, STATUS_EN = 0x34, SIGNAL_EN = 0x38, CAPS = 0x40 };
enum { S_CMD = 1, S_XFER = 2, S_WREADY = 1 << 4, S_RREADY = 1 << 5, S_CARD = 1 << 8, S_ERR = 1 << 15 };
enum { R_NONE = 0, R_48 = 0x1A, R_48B = 0x1B, R_OCR = 0x02, F_DATA = 0x20 };
enum { T_DMA = 1, T_COUNT = 2, T_READ = 16, T_MULTI = 32 };

/* SDIO's own commands and CCCR registers (function 0). */
#define CMD_IO_OP_COND  5                    /* CMD5: the card's voltage, powered-up bit */
#define CMD_RW_DIRECT   52
#define CMD_RW_EXTENDED 53
enum { CCCR_IOEN = 0x02, CCCR_IORDY = 0x03, CCCR_INTEN = 0x04, CCCR_BUSIFC = 0x07,
       CCCR_CAP = 0x08, CCCR_HISPEED = 0x13, FBR1_BLKSIZE = 0x110, FBR2_BLKSIZE = 0x210 };

static volatile uint32_t *sd;
static uint32_t rca, base_khz;
static long irq_port;
static int irq_line, acked;

static uint32_t rd(int r) { return sd[r / 4]; }
static void wr(int r, uint32_t v) { sd[r / 4] = v; }

static void udelay(uint64_t us) { long end = sys_clock() + (long)us * 1000; while (sys_clock() < end) ; }

/* Wait for any of `bits` (or an error), at most `ms` ms: 0, or -1. */
static int wait_status(uint32_t bits, uint32_t ms)
{
    long end = sys_clock() + (long)ms * 1000000L;
    for (;;) {
        uint32_t s = rd(STATUS);
        if (s & S_ERR) { wr(STATUS, s); return -1; }
        if (s & bits) { wr(STATUS, s & bits); return 0; }
        if (sys_clock() >= end) return -1;
        sys_yield();
    }
}

/* One command: index, argument, response kind, transfer mode; resp[0..3]. */
static int command(int idx, uint32_t arg, int kind, uint32_t tmode, uint32_t *resp)
{
    uint32_t inhibit = 1 | (kind & F_DATA || kind == R_48B ? 2 : 0);
    for (int k = 0; rd(PRESENT) & inhibit; k++) { if (k > 100000) return -1; udelay(1); }
    wr(STATUS, ~0u);
    wr(ARG, arg);
    wr(CMD, (uint32_t)(idx << 8 | kind) << 16 | tmode);
    if (wait_status(S_CMD, 500)) return -1;
    if (kind == R_48B && wait_status(S_XFER, 1000)) return -1;
    if (resp) for (int i = 0; i < 4; i++) resp[i] = rd(RESP + 4 * i);
    return 0;
}

/* The card's clock: base / (2 N), N a 10-bit field (0: the base). */
static void set_clock(uint32_t khz)
{
    uint32_t n = 0;
    while (n < 1023 && base_khz / (n ? 2 * n : 1) > khz) n++;
    wr(CLOCK, rd(CLOCK) & ~0xFFFFu);
    wr(CLOCK, (n & 0xFF) << 8 | (n >> 8 & 3) << 6 | 1);
    for (int k = 0; k < 10000 && !(rd(CLOCK) & 2); k++) udelay(10);
    wr(CLOCK, rd(CLOCK) | 4);
    udelay(1000);
}

/* ---- CMD52 / CMD53 (wifi.h). The R5 response is in resp[0]: its low byte
 * is the data (CMD52) or a status, bits 8-15 the card's status flags. */
int sdio_rb(int fn, uint32_t addr)
{
    uint32_t r, a = (uint32_t)(fn & 7) << 28 | (addr & 0x1FFFF) << 9;
    if (command(CMD_RW_DIRECT, a, R_48, 0, &r) || r & 0xCF00) return -1;
    return r & 0xFF;
}
int sdio_wb(int fn, uint32_t addr, int val)
{
    uint32_t r, a = 1u << 31 | (uint32_t)(fn & 7) << 28 | (addr & 0x1FFFF) << 9 | (val & 0xFF);
    for (int k = 0; k < 10; k++) if (!command(CMD_RW_DIRECT, a, R_48, 0, &r) && !(r & 0xCF00)) return 0;
    return -1;
}

/* One CMD53: `blocks` blocks of `bsize` (block mode), or `bytes` bytes (byte
 * mode, blocks 0). The controller moves the words by PIO (the frames are
 * small and this is never a fast path; a 32-bit word at a time). */
static int cmd53(int fn, int write, uint32_t addr, uint8_t *buf, int blocks, int bytes, int bsize, int incr)
{
    uint32_t tot = blocks ? (uint32_t)blocks * bsize : (uint32_t)bytes, count = blocks ? (uint32_t)blocks : tot;
    uint32_t a = (uint32_t)write << 31 | (uint32_t)(fn & 7) << 28 | (blocks ? 1u << 27 : 0) |
                 (incr ? 1u << 26 : 0) | (addr & 0x1FFFF) << 9 | (count & 0x1FF);
    uint32_t tmode = (write ? 0 : T_READ) | (blocks ? T_COUNT | T_MULTI : 0);
    wr(BLOCK, (blocks ? (uint32_t)blocks : 1) << 16 | (blocks ? (uint32_t)bsize : tot));
    if (command(CMD_RW_EXTENDED, a, R_48 | F_DATA, tmode, 0)) return -1;
    uint32_t chunk = blocks ? (uint32_t)bsize : tot;      /* a FIFO fill/drain is ready per block */
    for (uint32_t done = 0; done < tot; ) {
        if (wait_status(write ? S_WREADY : S_RREADY, 1000)) return -1;
        for (uint32_t i = 0; i < chunk; i += 4, done += 4) {
            if (write) { uint32_t v = 0; for (int j = 0; j < 4 && done + j < tot; j++) v |= (uint32_t)buf[done + j] << (8 * j); wr(DATA, v); }
            else { uint32_t v = rd(DATA); for (int j = 0; j < 4 && done + j < tot; j++) buf[done + j] = (uint8_t)(v >> (8 * j)); }
        }
    }
    return wait_status(S_XFER, 1000);
}

int sdio_rw(int fn, int write, uint32_t addr, void *buf, int len, int incr)
{
    int bsize = fn == 1 ? 64 : 256;              /* the block sizes set in sdio_up */
    uint8_t *p = buf;
    while (len > 0) {
        int m;
        if (len >= bsize) { int nb = len / bsize; if (nb > 511) nb = 511; m = nb * bsize; if (cmd53(fn, write, addr, p, nb, 0, bsize, incr)) return -1; }
        else { m = len; if (cmd53(fn, write, addr, p, 0, len, bsize, incr)) return -1; }
        p += m; len -= m;
        if (incr) addr += (uint32_t)m;
    }
    return 0;
}

/* ---- The pins and the chip's power, per board. */

/* The Pi 4: GPIO 34-39 to ALT3 (the chip's SDIO), WL_ON high on the
 * firmware's GPIO expander (mailbox, line 128 + 1). gpio@7e200000's
 * GPFSEL registers: 10 pins each, 3 bits a pin; ALT3 is 7. */
static int pi4_pins(const void *f)
{
    uint32_t m[8] = { 8 * 4, 0, 0x00038041, 8, 8, 128 + 1, 1, 0 };   /* SET_GPIO_STATE WL_ON = 1 */
    if (mbox_call(m, 8)) return -1;
    int n; uint64_t a, sz;
    if ((n = fdt_find(f, -1, "brcm,bcm2711-gpio")) < 0 && (n = fdt_find(f, -1, "brcm,bcm2835-gpio")) < 0) return -1;
    if (fdt_reg(f, n, 0, &a, &sz)) return -1;
    volatile uint32_t *g = map_phys(a, 0x1000);
    if ((long)g < 0) return -1;
    uint32_t sel = g[3];                         /* GPFSEL3: pins 30-39 */
    for (int p = 34; p <= 39; p++) sel = (sel & ~(7u << (3 * (p - 30)))) | 7u << (3 * (p - 30));
    g[3] = sel;
    return 0;
}

/* The Pi 5: GPIO 30-35 to "sd2" through the BCM2712 pin controller
 * (pinctrl@7d504100), and WL_ON (GPIO 28) high on the always-on GPIO block
 * (brcm,brcmstb-gpio at 0x107d508500: a bank's DATA at +4, IODIR at +8). */
static int pi5_pins(const void *f)
{
    int n; uint64_t a, sz;
    if ((n = fdt_find(f, -1, "brcm,bcm2712c0-pinctrl")) < 0 && (n = fdt_find(f, -1, "brcm,bcm2712-pinctrl")) < 0)
        { printf("wifi: no BCM2712 pin controller (a d-stepping Pi 5?): the SDIO pins are not set\n"); }
    else if (!fdt_reg(f, n, 0, &a, &sz)) {
        volatile uint32_t *pc = map_phys(a, 0x1000);
        if ((long)pc >= 0) {                     /* pin -> {register, shift, fsel for "sd2"} (c0 stepping) */
            static const struct { uint8_t pin, reg, shift, fsel; } mux[6] = {
                { 30, 12, 24, 4 }, { 31, 12, 28, 4 }, { 32, 16, 0, 4 }, { 33, 16, 4, 3 }, { 34, 16, 8, 4 }, { 35, 16, 12, 3 } };
            for (int i = 0; i < 6; i++) pc[mux[i].reg / 4] = (pc[mux[i].reg / 4] & ~(0xFu << mux[i].shift)) | (uint32_t)mux[i].fsel << mux[i].shift;
        }
    }
    for (int p = -1; (p = fdt_find(f, p, "brcm,brcmstb-gpio")) >= 0; ) {
        if (fdt_reg(f, p, 0, &a, &sz) || a != 0x107d508500ULL) continue;   /* the always-on block */
        volatile uint32_t *g = map_phys(a, 0x1000);
        if ((long)g < 0) return -1;
        g[1] |= 1u << 28;                        /* bank 0 DATA: WL_ON high */
        g[2] &= ~(1u << 28);                     /* IODIR: output */
        return 0;
    }
    return -1;
}

/* ---- Bring the controller and the SDIO card up. */
int sdio_up(char *what, size_t cap)
{
    const void *f = wifi_fdt();
    if (!f) return -1;
    int node = -1, len, pi5 = fdt_find(f, -1, "brcm,bcm2712") >= 0 || fdt_find(f, -1, "brcm,bcm2712-pcie") >= 0;
    uint64_t a = 0, sz;
    for (int n = -1; (n = fdt_next(f, n, &len)) >= 0; ) {         /* the SDHCI node with a wifi@ child */
        for (int c = -1, d = 0; (c = fdt_next(f, c, &d)) >= 0; )
            if (c > n && d == len + 1 && fdt_compatible(f, c, "brcm,bcm4329-fmac")) { node = n; break; }
        if (node >= 0) break;
    }
    if (node < 0 || fdt_reg(f, node, 0, &a, &sz)) { printf("wifi: no Wi-Fi SDIO controller in the device tree\n"); return -1; }
    sd = map_phys(a, 0x100);
    if ((long)sd < 0) return -1;

    if (pi5 ? pi5_pins(f) : pi4_pins(f)) { printf("wifi: could not set the SDIO pins or power the chip (WL_ON)\n"); return -1; }
    udelay(10000);                               /* the chip's own power-on */

    /* The base clock: the controller's capabilities, else the firmware. */
    base_khz = rd(CAPS) >> 8 & 0xFF ? (rd(CAPS) >> 8 & 0xFF) * 1000 : 50000;
    uint32_t m[8] = { 8 * 4, 0, 0x00030002, 8, 0, pi5 ? 0u : 1u, 0, 0 };   /* "get clock rate" */
    if (!mbox_call(m, 8) && m[6]) base_khz = m[6] / 1000;

    wr(CLOCK, 0); wr(CTRL, 0);
    wr(CTRL, 0x0F00);                            /* bus power on, 3.3 V */
    wr(STATUS_EN, 0xFFFFFFFFu);
    wr(SIGNAL_EN, 0);
    set_clock(400);

    uint32_t r[4];
    command(0, 0, R_NONE, 0, 0);                 /* GO_IDLE (harmless to an SDIO card) */
    command(CMD_IO_OP_COND, 0, R_OCR, 0, 0);     /* ask once, then with the voltage we can give */
    int up = 0;
    for (int k = 0; k < 50; k++) {
        if (command(CMD_IO_OP_COND, 0x00300000, R_OCR, 0, r)) return -1;   /* 3.2-3.4 V */
        if (r[0] & 1u << 31) { up = 1; break; }
        udelay(100000);
    }
    if (!up) { printf("wifi: the chip did not answer CMD5 (no SDIO card)\n"); return -1; }
    if (command(3, 0, R_48, 0, r)) return -1;    /* a relative address */
    rca = r[0] >> 16;
    if (command(7, rca << 16, R_48B, 0, 0)) return -1;   /* select it */

    sdio_wb(0, CCCR_HISPEED, 2);                 /* high speed */
    sdio_wb(0, CCCR_BUSIFC, 2);                  /* 4-bit bus */
    wr(CTRL, rd(CTRL) | 2 | 4);                  /* controller: 4-bit, high-speed timing */
    set_clock(50000);
    sdio_wb(0, FBR1_BLKSIZE, 64); sdio_wb(0, FBR1_BLKSIZE + 1, 0);       /* F1 blocks: 64 bytes */
    sdio_wb(0, FBR2_BLKSIZE, 0); sdio_wb(0, FBR2_BLKSIZE + 1, 1);        /* F2 blocks: 256 bytes (the 43455's) */
    sdio_wb(0, CCCR_IOEN, 1 << 1);               /* enable function 1 */
    for (int k = 0; k < 50; k++) { if (sdio_rb(0, CCCR_IORDY) & 1 << 1) break; udelay(10000); }
    if (!(sdio_rb(0, CCCR_IORDY) & 1 << 1)) { printf("wifi: SDIO function 1 did not become ready\n"); return -1; }
    sdio_wb(0, CCCR_INTEN, 0);

    int spi, flags;
    if (!fdt_spi(f, node, 0, &spi, &flags) && (irq_port = port_create(0)) > 0) {
        irq_line = spi | ((flags & 4) ? IRQ_LEVEL : 0);
        if (irq_bind(irq_line, irq_port)) irq_port = 0; else irq_line &= ~IRQ_LEVEL;
    }
    wifi_fmt(what, cap, "%s at %lx, SDIO 4-bit high speed %u kHz%s", fdt_name(f, node), (unsigned long)a,
            base_khz, irq_port ? ", card interrupt" : ", polled");
    return 0;
}

int sdio_enable_f2(void)
{
    sdio_wb(0, CCCR_IOEN, 1 << 1 | 1 << 2);      /* enable function 2 as well */
    for (int k = 0; k < 50; k++) { if (sdio_rb(0, CCCR_IORDY) & 1 << 2) return 0; udelay(10000); }
    return -1;
}

/* ---- The card's interrupt: the chip raises it when it has a frame. */
void sdio_irq_wait(void)
{
    if (!irq_port) { sys_sleep(acked ? 5000000 : 1000000); acked = 1; return; }   /* polled */
    wr(SIGNAL_EN, rd(SIGNAL_EN) | S_CARD); wr(STATUS_EN, rd(STATUS_EN) | S_CARD);
    if (rd(STATUS) & S_CARD) return;             /* already pending */
    msg_t m = { 0 };
    ipc_recv(irq_port, &m);
    acked = 0;
}
void sdio_irq_ack(void)
{
    if (!irq_port) { acked = 0; return; }
    wr(STATUS, S_CARD);
    irq_ack(irq_line);
}
