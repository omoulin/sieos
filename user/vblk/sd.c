/*
 * sd.c - The SD card backend of the disk server (arm64: the Raspberry Pi 4's
 * "EMMC2" and the Pi 5's SD controller; QEMU's raspi4b emulates the Pi 4's
 * older one, which holds the card there).
 *
 * These controllers follow the SD Host Controller standard ("SDHCI"): a page
 * of registers through which we send the card commands (CMD17: read one
 * block, CMD18: several...). The card speaks the SD protocol: before the
 * first read it is identified and given an address (CMD0, 8, 55+41, 2, 3, 9,
 * 7), switched to 4 data lines, then to its fastest common speed.
 *
 * Speed, three things:
 *   - ADMA2: the controller moves the data itself, following a small table
 *     of (address, length) descriptors we write, straight from or into the
 *     disk server's 128 KiB buffer (vblk serves that buffer to its clients,
 *     so a request's data is copied nowhere). 64-bit descriptors when the
 *     controller can, else 32-bit. No DMA support: the processor moves the
 *     words ("PIO"), as before.
 *   - Clock and mode: Default Speed (25 MHz), High Speed (50 MHz), and the
 *     "UHS-I" modes at 1.8 V signalling: SDR50 (100 MHz), DDR50 (50 MHz, both
 *     clock edges), SDR104 (up to 208 MHz, needs "tuning"). The best mode
 *     that the card, the controller and the board (device tree) allow; on
 *     errors, the next slower one.
 *   - Interrupts: the controller raises its line when a command or a
 *     transfer ends; we sleep in ipc_recv until then (no polling). A small
 *     watchdog thread wakes us at the deadline if an interrupt never comes;
 *     if that ever happens with the work done, interrupts are not trusted
 *     any more and we poll (with backoff) instead.
 *
 * The 1.8 V switch (UHS-I) is done only when it can be undone: the card's
 * signal voltage regulator ("vqmmc-supply") and its power ("vmmc-supply")
 * must both be GPIO-driven regulators we know how to drive: the Pi 4's
 * firmware GPIO expander (through the mailbox), or the Pi 5's always-on GPIO
 * block. A card that agreed to 1.8 V but does not come back after the
 * switch is powered off and on and used at 3.3 V (High Speed). On QEMU (no
 * regulators in its device tree) UHS is never tried.
 *
 * Caches: the real Pis' SD controllers do not see the processor's caches,
 * so the buffer and the descriptors are written back (dma_sync) before the
 * controller reads them and dropped after it wrote them.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "mk/fdt.h"
#include "mbox.h"
#include "sd.h"
#include "../usb/usb.h"                      /* pci_find (user/usb/pci.c): an SD controller on PCI */

enum { SDMA = 0x00, BLOCK = 0x04, ARG = 0x08, CMD = 0x0C, RESP = 0x10, DATA = 0x20, PRESENT = 0x24,
       CTRL = 0x28, CLOCK = 0x2C, STATUS = 0x30, STATUS_EN = 0x34, SIGNAL_EN = 0x38, CTRL2 = 0x3C,
       CAPS = 0x40, CAPS2 = 0x44, ADMA_ERR = 0x54, ADMA = 0x58, ADMA_HI = 0x5C, VERSION = 0xFC };
/* STATUS: low half "normal" events, bit 15 "error" (the high half says which) */
enum { S_CMD = 1, S_XFER = 2, S_WREADY = 1 << 4, S_RREADY = 1 << 5, S_ERR = 1 << 15 };
/* Command register flags (with the command's index << 8): response kind, checks, data. */
enum { R_NONE = 0, R_136 = 0x09, R_48 = 0x1A, R_48B = 0x1B, R_OCR = 0x02, F_DATA = 0x20 };
/* Transfer mode (CMD's low half): DMA, block count on, auto CMD12, read, several blocks */
enum { T_DMA = 1, T_COUNT = 2, T_AUTO12 = 4, T_READ = 16, T_MULTI = 32 };
/* CTRL2 (its high half: "host control 2"): UHS mode, 1.8 V, tuning */
enum { H2_18V = 1 << 19, H2_TUNE = 1 << 22, H2_TUNED = 1 << 23 };

/* The speeds: CMD6 function (group 1), UHS mode field, clock in kHz. */
enum { M_DS, M_HS, M_DDR50, M_SDR50, M_SDR104, NMODE };
static const struct { const char *name; int fn, uhs; uint32_t khz; } modes[NMODE] = {
    { "Default Speed", 0, 0, 25000 }, { "High Speed", 1, 1, 50000 }, { "DDR50", 4, 4, 50000 },
    { "SDR50", 2, 2, 100000 }, { "SDR104", 3, 3, 208000 },
};

static volatile uint32_t *sd;
static uint32_t rca, base_khz, clk_khz, caps, caps2, errs;
static int hc, wide, mode, v18;              /* high capacity; 4 data lines; current mode; 1.8 V */
static int dma;                              /* 0: PIO, 32 or 64: ADMA2 descriptor size */
static uint8_t *buf;                         /* the data buffer (DMA memory), DISK_MAX bytes */
static uint64_t buf_bus, tab_bus;            /* their bus addresses (what the controller sees) */
static uint32_t *tab;                        /* the ADMA2 descriptor table */
static int64_t pci_off;                      /* on PCI (QEMU's virt): bus address = physical + this */
static long irq_port;                        /* 0: polling */
static int irq_line, wd_tid;
static volatile uint64_t wd_at;              /* the watchdog's deadline (ns since boot), 0: none */
static uint32_t pend;                        /* STATUS events seen and not yet consumed */

static uint32_t rd(int r) { return sd[r / 4]; }
static void wr(int r, uint32_t v) { sd[r / 4] = v; }   /* (32-bit only: the Pi 4's older controller wants that) */

/* ---- Waiting for the controller ---------------------------------------- */

/* The watchdog: sleeps until wd_at, then sends the waiting thread a message
 * on the interrupt port so it looks at the controller again. */
static void watchdog(void *a)
{
    (void)a;
    for (;;) {
        uint64_t at = wd_at, now = sys_clock();
        if (!at) { sys_sleep(3600000000000UL); continue; }     /* nothing to watch: until woken */
        if (now < at) { sys_sleep(at - now); continue; }
        wd_at = 0;
        msg_t m = { 0 };
        ipc_call(irq_port, &m);
    }
}

static void take(void) { uint32_t s = rd(STATUS); if (s) { wr(STATUS, s); pend |= s; } }

/* Wait for any of `bits` (or an error), at most `ms` milliseconds: 0, or -1. */
static long wait_status(uint32_t bits, uint32_t ms)
{
    uint64_t end = sys_clock() + ms * 1000000UL, nap = 10000;
    for (int k = 0;; k++) {
        take();
        if (pend & S_ERR) { errs = pend >> 16; pend = 0; return -1; }
        if (pend & bits) { pend &= ~bits; return 0; }
        if ((uint64_t)sys_clock() >= end) return -1;
        if (irq_port) {                       /* sleep until the interrupt (or the watchdog) */
            wd_at = end;
            sys_wake(wd_tid);
            msg_t m = { 0 };
            long from = ipc_recv(irq_port, &m);
            if (from > 0) {                   /* the watchdog: was an interrupt lost? */
                reply_val(from, 0);
                take();
                if (pend & (bits | S_ERR)) {
                    printf("vblk: SD: an interrupt was lost, polling from now on\n");
                    wr(SIGNAL_EN, 0);
                    irq_port = 0;
                }
            } else if (from == 0) { take(); irq_ack(irq_line); }
        } else if (k < 16) sys_yield();
        else { sys_sleep(nap); if (nap < 1000000) nap *= 2; }   /* 10 us, doubling to 1 ms */
    }
}

/* Reset the command and/or data line after an error (CLOCK bits 25, 26). */
static void reset_lines(uint32_t which)
{
    wr(CLOCK, rd(CLOCK) | which);
    for (int k = 0; k < 10000 && rd(CLOCK) & which; k++) sys_yield();
    pend = 0;
}

/* One command: its index, argument, kind, transfer mode; the response's first word. */
static long command(int idx, uint32_t arg, int kind, uint32_t tmode, uint32_t *resp)
{
    uint32_t inhibit = 1 | (kind & F_DATA || kind == R_48B ? 2 : 0);   /* command (and data) lines free */
    for (int k = 0; rd(PRESENT) & inhibit; k++) { if (k > 20000) return -1; if (k > 64) sys_sleep(20000); else sys_yield(); }
    wr(STATUS, ~0u);
    pend = 0;
    wr(ARG, arg);
    wr(CMD, (uint32_t)(idx << 8 | kind) << 16 | tmode);
    if (wait_status(S_CMD, 500)) { reset_lines(1 << 25); return -1; }
    if (kind == R_48B && wait_status(S_XFER, 1000)) { reset_lines(3 << 25); return -1; }   /* busy until the card lets go */
    if (resp) *resp = rd(RESP);
    return 0;
}
static long app_command(int idx, uint32_t arg, int kind, uint32_t *resp)   /* CMD55 first: an "ACMD" */
{
    return command(55, rca << 16, R_48, 0, 0) ? -1 : command(idx, arg, kind, 0, resp);
}

/* A short read through the data port (CMD6's 64-byte status, the SCR's 8). */
static long pio_read(int idx, uint32_t arg, int app, void *dst, uint32_t len)
{
    wr(BLOCK, 1 << 16 | len);
    long e = app ? command(55, rca << 16, R_48, 0, 0) || command(idx, arg, R_48 | F_DATA, T_READ, 0)
                 : command(idx, arg, R_48 | F_DATA, T_READ, 0);
    if (e || wait_status(S_RREADY, 500)) { reset_lines(3 << 25); return -1; }
    for (uint32_t i = 0; i < len / 4; i++) ((uint32_t *)dst)[i] = rd(DATA);
    return wait_status(S_XFER, 500);
}

/* ---- Clock, voltage, power ----------------------------------------------- */

/* The card's clock: base / (2 N), N in a 10-bit field (0: the base itself). */
static void set_clock(uint32_t khz)
{
    uint32_t n = 0;
    while (n < 1023 && base_khz / (n ? 2 * n : 1) > khz) n++;
    clk_khz = base_khz / (n ? 2 * n : 1);
    wr(CLOCK, rd(CLOCK) & ~0xFFFFu);             /* card clock off */
    wr(CLOCK, (rd(CLOCK) & 0xFFFF0000u) | (n & 0xFF) << 8 | (n >> 8 & 3) << 6 | 1 | 0xE << 16);
    for (int k = 0; k < 10000 && !(rd(CLOCK) & 2); k++) sys_yield();   /* internal clock stable */
    wr(CLOCK, rd(CLOCK) | 4);                    /* card clock on */
    sys_sleep(1000000);
}

/* The controller's base clock in kHz: its capabilities, else the firmware
 * (mailbox: "get clock rate"), else the Pis' usual 100 MHz. */
static uint32_t base_clock(int emmc2)
{
    uint32_t mhz = caps >> 8 & 0xFF;
    if (mhz) return mhz * 1000;
    uint32_t m[8] = { 8 * 4, 0, 0x00030002, 8, 0, emmc2 ? 12 : 1, 0, 0 };
    if (!mbox_call(m, 8) && m[6]) return m[6] / 1000;
    return 100000;
}

/* A GPIO line that drives a regulator: the Pi 4's firmware expander (lines
 * 128 + n, through the mailbox), or a "brcmstb" GPIO block (the Pi 5's
 * always-on one: banks of 32 lines, 0x20 bytes each: DATA at +4, IODIR at +8,
 * a 1 there meaning input). kind 0: none we know. */
typedef struct { int kind, pin, high; volatile uint32_t *regs; } gpio_t;
static gpio_t vqmmc, vmmc;                   /* (high: the value that selects 1.8 V / turns power on) */

static int gpio_set(gpio_t *g, int on)
{
    int v = on ? g->high : !g->high;
    if (g->kind == 1) {
        uint32_t m[8] = { 8 * 4, 0, 0x00038041, 8, 8, 128 + (uint32_t)g->pin, (uint32_t)v, 0 };   /* SET_GPIO_STATE */
        if (mbox_call(m, 8)) return -1;
        uint32_t q[8] = { 8 * 4, 0, 0x00030041, 8, 4, 128 + (uint32_t)g->pin, 0, 0 };           /* GET_GPIO_STATE: check */
        return mbox_call(q, 8) || (int)q[6] != v ? -1 : 0;
    }
    if (g->kind == 2) {
        volatile uint32_t *b = g->regs + (g->pin / 32) * 8;
        uint32_t bit = 1u << (g->pin % 32);
        b[1] = v ? b[1] | bit : b[1] & ~bit;     /* DATA */
        b[2] &= ~bit;                            /* IODIR: output */
        return !!(b[1] & bit) == v ? 0 : -1;
    }
    return -1;
}

/* The node whose "phandle" is ph. */
static int by_phandle(const char *f, uint32_t ph)
{
    int d = 0, len;
    for (int n = fdt_next(f, -1, &d); n >= 0; n = fdt_next(f, n, &d)) {
        const void *p = fdt_prop(f, n, "phandle", &len);
        if (p && len == 4 && fdt_be32(p) == ph) return n;
    }
    return -1;
}

/* Learn how to drive the regulator that node's property `supply` names. */
static void find_regulator(const char *f, int node, const char *supply, gpio_t *g)
{
    int len, r, c;
    const void *p = fdt_prop(f, node, supply, &len);
    if (!p || len != 4 || (r = by_phandle(f, fdt_be32(p))) < 0) return;
    const uint8_t *gp = fdt_prop(f, r, "gpios", &len);
    if (!gp) gp = fdt_prop(f, r, "gpio", &len);
    if (!gp || len < 8 || (c = by_phandle(f, fdt_be32(gp))) < 0) return;
    g->pin = fdt_be32(gp + 4);
    g->high = 1;
    const uint8_t *st = fdt_prop(f, r, "states", &len);   /* {microvolts, value} pairs: 1.8 V's value */
    for (int i = 0; st && i + 8 <= len; i += 8)
        if (fdt_be32(st + i) == 1800000) g->high = fdt_be32(st + i + 4);
    uint64_t a, sz;
    if (fdt_compatible(f, c, "raspberrypi,firmware-gpio")) g->kind = 1;
    else if (fdt_compatible(f, c, "brcm,brcmstb-gpio") && !fdt_reg(f, c, 0, &a, &sz)) {
        void *m = map_phys(a, sz < 0x1000 ? 0x1000 : sz);
        if ((long)m >= 0) { g->regs = m; g->kind = 2; }
    }
}

/* ---- Identification --------------------------------------------------- */

/* Identify the card and get it to the transfer state: 0, and its size in
 * 512-byte sectors. try18: ask the card for 1.8 V signalling (UHS-I). */
static int card_init(uint64_t *sectors, int try18)
{
    wr(CTRL, (rd(CTRL) & ~0xFF1Eu) | 0x0F00);    /* bus power on, 3.3 V; 1 data line, no DMA, normal speed */
    wr(CTRL2, 0);                                /* 3.3 V signalling, no UHS mode */
    wr(STATUS_EN, 0xFFFF003Fu);                  /* see command, transfer, buffer and error events */
    wr(SIGNAL_EN, 0);                            /* (identification is polled) */
    v18 = 0;
    set_clock(400);                              /* identification: 400 kHz */

    uint32_t r;
    command(0, 0, R_NONE, 0, 0);                 /* go idle */
    int v2 = !command(8, 0x1AA, R_48, 0, &r) && (r & 0xFFF) == 0x1AA;   /* SD 2.0 or later */
    uint32_t want = 0x00FF8000 | (v2 ? 1u << 30 : 0) | (v2 && try18 ? 1u << 24 | 1u << 28 : 0);
    for (int k = 0;; k++) {                      /* until the card says it is powered up */
        if (app_command(41, want, R_OCR, &r)) return -1;   /* (not an SD card) */
        if (r & 1u << 31) break;
        if (k > 100) return -1;
        sys_sleep(10000000);
    }
    hc = !!(r & 1 << 30);
    if (try18 && r & 1 << 24) {                  /* the card accepts 1.8 V: CMD11, then switch */
        if (command(11, 0, R_48, 0, 0)) return -2;
        wr(CLOCK, rd(CLOCK) & ~4u);              /* card clock off */
        if (rd(PRESENT) >> 20 & 15) return -2;   /* the card should hold DAT0-3 low now */
        wr(CTRL2, H2_18V);
        if (gpio_set(&vqmmc, 1)) return -2;
        sys_sleep(5000000);                      /* the regulator settles (5 ms) */
        if (!(rd(CTRL2) & H2_18V)) return -2;
        wr(CLOCK, rd(CLOCK) | 4);
        sys_sleep(1000000);
        if ((rd(PRESENT) >> 20 & 15) != 15) return -2;   /* the card let DAT0-3 go: it switched */
        v18 = 1;
    }
    uint32_t csd[4];
    if (command(2, 0, R_136, 0, 0) || command(3, 0, R_48, 0, &r)) return -1;   /* its id; its address */
    rca = r >> 16;
    if (command(9, rca << 16, R_136, 0, 0)) return -1;                        /* its geometry ("CSD") */
    for (int i = 0; i < 4; i++) csd[i] = rd(RESP + 4 * i);
    /* (the registers hold the CSD's bits 127-8: CSD bit n is register bit n - 8) */
    if ((csd[3] >> 22 & 3) == 1) *sectors = (uint64_t)((csd[1] >> 8 & 0x3FFFFF) + 1) * 1024;      /* CSD 2.0 */
    else {
        uint32_t cs = (csd[2] & 3) << 10 | csd[1] >> 22, mult = csd[1] >> 7 & 7, bl = csd[2] >> 8 & 15;
        *sectors = ((uint64_t)(cs + 1) << (mult + 2) << bl) / 512;
    }
    if (command(7, rca << 16, R_48B, 0, 0)) return -1;                        /* select it */
    if (!hc) command(16, 512, R_48, 0, 0);                                    /* 512-byte blocks */
    set_clock(25000);
    wide = !app_command(6, 2, R_48, 0);                                       /* 4 data lines */
    if (wide) wr(CTRL, rd(CTRL) | 2);
    return 0;
}

/* Turn the card's power off and on (the way back from a failed 1.8 V switch). */
static void power_cycle(void)
{
    wr(CLOCK, rd(CLOCK) & ~4u);
    wr(CTRL, rd(CTRL) & ~0x100u);                /* bus power off */
    gpio_set(&vqmmc, 0);                         /* back to 3.3 V signalling */
    gpio_set(&vmmc, 0);
    sys_sleep(50000000);                         /* 50 ms without power */
    gpio_set(&vmmc, 1);
    sys_sleep(20000000);
}

/* ---- Speed --------------------------------------------------------------- */

/* SDR104 (and SDR50 when the controller asks for it) needs tuning: the
 * controller finds the right moment to sample the data, reading CMD19's
 * test pattern: 0 once it has. */
static int tune(void)
{
    wr(CTRL2, rd(CTRL2) | H2_TUNE);
    for (int k = 0; k < 40 && rd(CTRL2) & H2_TUNE; k++) {
        wr(BLOCK, 1 << 16 | 64);
        if (command(19, 0, R_48 | F_DATA, T_READ, 0)) break;
        wait_status(S_RREADY, 150);              /* (the controller keeps the pattern itself) */
    }
    int ok = !(rd(CTRL2) & H2_TUNE) && rd(CTRL2) & H2_TUNED;
    if (!ok) wr(CTRL2, rd(CTRL2) & ~(H2_TUNE | H2_TUNED));
    reset_lines(3 << 25);
    return ok ? 0 : -1;
}

/* Switch the card and the controller to mode m: 0, or -1 (nothing changed
 * on the card if CMD6 said no; the controller is back to Default Speed). */
static int set_mode(int m)
{
    uint32_t st[16];
    if (m != M_DS) {
        if (pio_read(6, 0x80FFFFF0u | modes[m].fn, 0, st, 64)) return -1;   /* CMD6 mode 1: switch */
        if ((st[4] & 15) != (uint32_t)modes[m].fn) return -1;               /* (byte 16, low nibble: selected) */
    }
    wr(CLOCK, rd(CLOCK) & ~4u);
    wr(CTRL, (rd(CTRL) & ~4u) | (m != M_DS ? 4 : 0));     /* "high speed" timing */
    wr(CTRL2, (rd(CTRL2) & ~(7u << 16 | H2_TUNE | H2_TUNED)) | (v18 ? (uint32_t)modes[m].uhs << 16 : 0));
    set_clock(modes[m].khz);
    mode = m;
    if (m == M_SDR104 || (m == M_SDR50 && caps2 & 1 << 13)) if (tune()) return -1;
    return 0;
}

/* The modes the card offers (CMD6 mode 0: "check"), one bit per function. */
static uint32_t card_modes(void)
{
    uint32_t scr[2], st[16];                    /* (both sent most significant byte first) */
    if (pio_read(51, 0, 1, scr, 8) || !(scr[0] & 0x0F)) return 0;  /* SD_SPEC 0: no CMD6 */
    if (pio_read(6, 0x00FFFFF0u, 0, st, 64)) return 0;
    return (st[3] & 0xFF) << 8 | (st[3] >> 8 & 0xFF);               /* bytes 12-13: group 1's functions */
}

/* ---- Data ------------------------------------------------------------------ */

/* Fill the ADMA2 table for `bytes` from the buffer: 64 KiB per descriptor. */
static void make_table(uint32_t bytes)
{
    int step = dma == 64 ? 3 : 2;                /* 12- or 8-byte descriptors (in 32-bit words) */
    uint32_t *d = tab;
    for (uint32_t off = 0; off < bytes; off += 0x10000, d += step) {
        uint32_t n = bytes - off < 0x10000 ? bytes - off : 0x10000;
        uint64_t a = buf_bus + off;
        d[0] = (n & 0xFFFF) << 16 | 0x21 | (off + n >= bytes ? 2 : 0);  /* length (0: 64 KiB), transfer, valid, end */
        d[1] = (uint32_t)a;
        if (step == 3) d[2] = (uint32_t)(a >> 32);
    }
    dma_sync(tab, 64);
}

/* One read or write of `count` blocks at `sector`, data in buf. */
static long xfer(int write, uint64_t sector, uint32_t count)
{
    uint32_t bytes = count * 512, tmode = T_COUNT | (count > 1 ? T_MULTI | T_AUTO12 : 0) | (write ? 0 : T_READ);
    wr(BLOCK, count << 16 | 512);
    if (dma) {
        make_table(bytes);
        dma_sync(buf, bytes);                    /* written back (write) / no stale lines left (read) */
        wr(ADMA, (uint32_t)tab_bus);
        wr(ADMA_HI, (uint32_t)(tab_bus >> 32));
        tmode |= T_DMA;
    }
    long e = command(write ? (count > 1 ? 25 : 24) : (count > 1 ? 18 : 17), (uint32_t)(hc ? sector : sector * 512),
                     R_48 | F_DATA, tmode, 0);
    uint32_t *w = (uint32_t *)buf;
    for (uint32_t b = 0; !e && !dma && b < count; b++) {
        if (wait_status(write ? S_WREADY : S_RREADY, 1000)) e = -1;
        else if (write) for (int i = 0; i < 128; i++) wr(DATA, *w++);
        else            for (int i = 0; i < 128; i++) *w++ = rd(DATA);
    }
    if (!e) e = wait_status(S_XFER, 2000 + count / 4);
    if (dma && !write) dma_sync(buf, bytes);     /* drop lines the processor may have fetched meanwhile */
    if (e) {
        reset_lines(3 << 25);
        if (count > 1) command(12, 0, R_48B, 0, 0);    /* stop the transfer if it is still on */
    }
    return e ? -1 : 0;
}

/* ---- Setup and requests ----------------------------------------------------- */

/* A property of node n (0 if none, or if the controller has no node: PCI). */
static const void *prop(const char *f, int n, const char *name, int *len) { return n < 0 ? 0 : fdt_prop(f, n, name, len); }

static int bus_addr(const char *f, int n, uint64_t pa, uint64_t *bus)
{
    if (n >= 0) return fdt_bus_addr(f, n, pa, bus);
    *bus = pa + pci_off;
    return 0;
}

static int try_controller(const char *f, int n, int emmc2, uint64_t a, uint64_t *sectors)
{
    wr(CLOCK, rd(CLOCK) | 1 << 24);              /* reset the controller */
    for (int k = 0; k < 10000 && rd(CLOCK) & 1 << 24; k++) sys_yield();
    caps = rd(CAPS); caps2 = rd(CAPS2);
    int len;                                     /* the board may correct them ("sdhci-caps(-mask)") */
    const uint8_t *cm = prop(f, n, "sdhci-caps-mask", &len);
    if (cm && len == 8) { caps2 &= ~fdt_be32(cm); caps &= ~fdt_be32(cm + 4); }
    const uint8_t *cs = prop(f, n, "sdhci-caps", &len);
    if (cs && len == 8) { caps2 |= fdt_be32(cs); caps |= fdt_be32(cs + 4); }
    base_khz = base_clock(emmc2);

    /* UHS-I: the controller offers a UHS mode and 1.8 V, the board allows it, and
     * both regulators can be driven (so a failed switch can be undone). */
    vqmmc = vmmc = (gpio_t){ 0 };
    if (n >= 0) {
        find_regulator(f, n, "vqmmc-supply", &vqmmc);
        find_regulator(f, n, "vmmc-supply", &vmmc);
    }
    int uhs = (caps2 & 7) && caps & 1 << 26 && !prop(f, n, "no-1-8-v", 0) && vqmmc.kind && vmmc.kind;
    int r = card_init(sectors, uhs);
    if (r == -2) {                               /* the 1.8 V switch failed: power-cycle, 3.3 V */
        printf("vblk: SD: the 1.8 V switch failed; the card is used at 3.3 V\n");
        power_cycle();
        uhs = 0;
        r = card_init(sectors, 0);
    }
    if (r) { printf("vblk: %s at %lx: no SD card\n", n >= 0 ? fdt_name(f, n) : "SDHCI", (unsigned long)a); return -1; }

    /* DMA: ADMA2 if the controller has it (capabilities bit 19), 64-bit
     * descriptors if it can (bit 28); the buffer and table where the device sees them. */
    uint64_t pa, tpa;                            /* (in the first GiB if there is RAM there: the Pis' */
    buf = dma_alloc(DISK_MAX | DMA_LOW, &pa);    /*  controllers see no more; else anywhere) */
    if ((long)buf < 0) buf = dma_alloc(DISK_MAX, &pa);
    tab = dma_alloc(4096 | DMA_LOW, &tpa);
    if ((long)tab < 0) tab = dma_alloc(4096, &tpa);
    if ((long)buf < 0 || (long)tab < 0 || bus_addr(f, n, pa, &buf_bus) || bus_addr(f, n, tpa, &tab_bus))
        return -1;
    dma = caps & 1 << 19 ? (caps & 1 << 28 ? 64 : 32) : 0;
    if (dma) wr(CTRL, (rd(CTRL) & ~0x18u) | (dma == 64 ? 0x18 : 0x10));

    /* The fastest mode both sides have: card (CMD6), controller (caps: SDR50,
     * SDR104, DDR50 bits 0-2; High Speed bit 21), board ("sd-uhs-..." adds). */
    uint32_t cm6 = wide ? card_modes() : 0;
    uint32_t host = 1 << M_DS | (caps & 1 << 21 ? 1 << M_HS : 0);
    if (v18) host |= (caps2 & 1 || prop(f, n, "sd-uhs-sdr50", 0) ? 1 << M_SDR50 : 0) |
                     (caps2 & 2 || prop(f, n, "sd-uhs-sdr104", 0) ? 1 << M_SDR104 : 0) |
                     (caps2 & 4 || prop(f, n, "sd-uhs-ddr50", 0) ? 1 << M_DDR50 : 0) | 1 << M_HS;
    for (int m = NMODE - 1; m > M_DS; m--) {
        if (!(host & 1 << m) || !(cm6 & 1u << modes[m].fn)) continue;
        if (!set_mode(m) && !xfer(0, 0, 1)) break;     /* switched, and a read works */
        set_mode(M_DS);
    }
    if (mode == M_DS) set_mode(M_DS);
    return 0;
}

static void report(const char *what, uint64_t a)
{
    printf("vblk: %s at %lx: SD card (%s), %d-bit, %s at %u kHz%s, %s, %s; caps %08x %08x\n",
           what, (unsigned long)a, hc ? "high capacity" : "standard capacity", wide ? 4 : 1,
           modes[mode].name, clk_khz, v18 ? " (1.8 V)" : "",
           dma == 64 ? "ADMA2 64-bit" : dma == 32 ? "ADMA2 32-bit" : "PIO",
           irq_port ? "interrupts" : "polled", caps, caps2);
}

/* pci.c (shared with the USB server) logs and names things with these two. */
void log_line(const char *fmt, ...) { (void)fmt; }
struct sbuf { char *o; int cap, n; };
static void sput(void *c, char ch) { struct sbuf *b = c; if (b->n < b->cap - 1) b->o[b->n++] = ch; }
void sfmt(char *out, int cap, const char *fmt, ...)
{
    struct sbuf b = { out, cap, 0 };
    va_list ap;
    va_start(ap, fmt);
    vformat(sput, &b, fmt, ap);
    va_end(ap);
    out[b.n] = 0;
}

/* Find an SD controller with a card (the device tree's enabled ones, in this
 * order; a Pi 4's second one serves the Wi-Fi chip, which is not an SD card
 * and fails identification): 0, and the card's size. */
int sd_setup(uint64_t *sectors)
{
    static const char *const compat[] = { "brcm,bcm2711-emmc2", "brcm,bcm2712-sdhci", "brcm,bcm2835-sdhci", "brcm,sdhci-brcmstb" };
    long size = sys_fdt(0, 0);
    char *f = size > 0 ? malloc(size) : 0;
    if (!f || sys_fdt(f, size) != size) { free(f); return -1; }
    int found = -1;
    for (unsigned c = 0; c < sizeof compat / sizeof *compat && found; c++)
        for (int n = -1; found && (n = fdt_find(f, n, compat[c])) >= 0; ) {
            const char *st = fdt_prop(f, n, "status", 0);
            uint64_t a, len;
            if ((st && strcmp(st, "okay") && strcmp(st, "ok")) || fdt_reg(f, n, 0, &a, &len)) continue;
            volatile uint32_t *m = map_phys(a, 0x100);
            if ((long)m < 0) continue;
            sd = m;
            /* no card in the slot (unless the slot cannot tell: "broken-cd") */
            if (!(rd(PRESENT) & 1 << 16) && !fdt_prop(f, n, "broken-cd", 0) && !fdt_prop(f, n, "non-removable", 0)) continue;
            if (try_controller(f, n, c == 0, a, sectors)) continue;
            /* Interrupts from now on (identification and the mode search were polled). */
            int spi, flags;
            if (!fdt_spi(f, n, 0, &spi, &flags) && (irq_port = port_create(0)) > 0) {
                irq_line = spi | ((flags & 4) ? IRQ_LEVEL : 0);
                if (irq_bind(irq_line, irq_port)) irq_port = 0;
                else {
                    irq_line &= ~IRQ_LEVEL;
                    wd_tid = thread_start(watchdog, 0, 4096);
                    wr(SIGNAL_EN, 0xFFFF0033u);  /* command and transfer done, buffer ready (PIO), errors */
                }
            } else irq_port = 0;
            report(compat[c], a);
            found = 0;
        }
    /* None: an SD controller on PCI (class 08/05: QEMU's virt with "-device
     * sdhci-pci", how the DMA, interrupt and speed paths are tested there). */
    hc_info_t h;
    for (int k = 0; found && !pci_find(0x080500, 0xFFFF00, &h, k); k++) {
        volatile uint32_t *m = map_phys(h.mmio, 0x1000);
        if ((long)m < 0) continue;
        sd = m;
        pci_off = h.bus_off;
        if (try_controller(f, -1, 0, h.mmio, sectors)) continue;
        if (h.irq >= 0 && (irq_port = port_create(0)) > 0 && !irq_bind(h.irq, irq_port)) {
            irq_line = h.irq & ~IRQ_LEVEL;
            wd_tid = thread_start(watchdog, 0, 4096);
            wr(SIGNAL_EN, 0xFFFF0033u);
        } else irq_port = 0;
        report(h.what, h.mmio);
        found = 0;
    }
    free(f);
    return found;
}

void *sd_buffer(void) { return buf; }

/* Read or write `count` 512-byte sectors at `sector`: data in b (if it is
 * not sd_buffer(), it goes through it). Two tries; after a second failure
 * the next slower mode, and a last try. */
long sd_io(int write, uint64_t sector, uint32_t count, void *b)
{
    if (count * 512 > DISK_MAX) return -EINVAL;
    if (b != buf && write) memcpy(buf, b, count * 512);
    long e = xfer(write, sector, count);
    if (e) e = xfer(write, sector, count);
    while (e && mode > M_DS) {
        int m = mode - 1;
        printf("vblk: SD errors (%x): %s -> %s\n", errs, modes[mode].name, modes[m].name);
        if (set_mode(m)) set_mode(M_DS);
        e = xfer(write, sector, count);
    }
    if (!e && b != buf && !write) memcpy(b, buf, count * 512);
    return e ? -EIO : 0;
}
