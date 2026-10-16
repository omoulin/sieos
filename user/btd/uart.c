/*
 * uart.c - The Raspberry Pis' Bluetooth line: the CYW43455's HCI over a
 * UART with RTS/CTS flow control, H4 framing (a type byte, then the packet).
 *
 *   Pi 4  the mini UART (UART1, "brcm,bcm2835-aux-uart") on GPIO 30-33
 *         (ALT5: CTS, RTS, TXD, RXD). Linux puts Bluetooth on the PL011,
 *         but the PL011 is SIEOS's serial console (config.txt: disable-bt),
 *         so Bluetooth takes the other UART (as Linux's "miniuart-bt").
 *         Its clock is the VPU's: config.txt pins it (core_freq_min=500).
 *         BT_ON: line 0 of the firmware's GPIO expander (mailbox).
 *   Pi 5  the BCM2712's UART at 0x107d50c000 ("brcm,bcm7271-uart", a 16550
 *         with automatic flow control, 96 MHz) on GPIO 24-27 (the pin
 *         controller's "uart0" function); BT_ON: GPIO 29 of the always-on
 *         block (as wifid's WL_ON, 28).
 * Both are 16550-like, registers 4 bytes apart. Receiving: the line's
 * interrupt when the device tree gives one, else polled every millisecond
 * (no byte is lost: when we are slow, RTS stops the chip).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "btd.h"
#include "mk/fdt.h"
#include "mbox.h"

enum { DATA = 0, IER = 1, FCR = 2, LCR = 3, MCR = 4, LSR = 5, CNTL = 8, BAUD = 10 };   /* (CNTL, BAUD: the mini UART's) */
static volatile uint32_t *u;
static int mini, irq = -1;
static uint32_t clk;
static long irq_port;

int uart_max;                                    /* the line's top speed (the device tree's max-speed) */

void uart_baud(int baud)
{
    if (mini) { u[BAUD] = clk / (8 * (uint32_t)baud) - 1; return; }
    uint32_t div = (clk + 8 * (uint32_t)baud) / (16 * (uint32_t)baud);
    u[LCR] = 0x83; u[DATA] = div & 0xFF; u[IER] = div >> 8; u[LCR] = 0x03;   /* (the divisor latch) */
}

void uart_write(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++) { while (!(u[LSR] & 0x20)) ; u[DATA] = p[i]; }
}

/* Wait for bytes; -> how many (into b). */
int uart_read(uint8_t *b, int cap)
{
    int n = 0;
    while (!n) {
        if (!(u[LSR] & 1)) {
            if (irq_port) { msg_t m = { 0 }; u[IER] = 1; ipc_recv(irq_port, &m); u[IER] = 0; irq_ack(irq); }
            else sys_sleep(1000000);
        }
        while (n < cap && (u[LSR] & 1)) b[n++] = (uint8_t)u[DATA];
    }
    return n;
}

static volatile uint32_t *map(const void *f, int node)
{
    uint64_t a, sz;
    if (node < 0 || fdt_reg(f, node, 0, &a, &sz)) return 0;
    volatile uint32_t *p = map_phys(a & ~0xFFFUL, 0x1000);
    return (long)p < 0 ? 0 : (volatile uint32_t *)((volatile uint8_t *)p + (a & 0xFFF));
}

/* Power the chip's Bluetooth side, set the pins and the line: 115200, 8N1, flow control. */
int uart_open(const void *f, char *what, int cap)
{
    int pi5 = fdt_find(f, -1, "brcm,bcm2712") >= 0;
    int n = fdt_find(f, -1, pi5 ? "brcm,bcm7271-uart" : "brcm,bcm2835-aux-uart");
    if (!(u = map(f, n))) { strlcpy(what, "no Bluetooth UART in the device tree", cap); return -1; }
    uart_max = 115200;
    int c = fdt_find(f, n, "brcm,bcm43438-bt");          /* its child: the next such node */
    const void *v = c >= 0 ? fdt_prop(f, c, "max-speed", 0) : 0;
    if (v) uart_max = (int)fdt_be32(v);
    if (pi5) {
        v = fdt_prop(f, n, "clock-frequency", 0);
        clk = v ? fdt_be32(v) : 96000000;
        volatile uint32_t *pc = map(f, fdt_find(f, -1, "brcm,bcm2712c0-pinctrl")), *g = 0;
        if (pc) pc[3] = (pc[3] & ~0xF0FFFFu) | 3 | 4 << 4 | 4 << 8 | 4 << 12;   /* GPIO 24-27: uart0 (fsel 3, 4, 4, 4); 29: GPIO */
        else bt_log("bt: no BCM2712 c0 pin controller: the UART's pins are left as they are\n");
        for (int p = -1; (p = fdt_find(f, p, "brcm,brcmstb-gpio")) >= 0; ) {
            uint64_t a, sz;
            if (!fdt_reg(f, p, 0, &a, &sz) && a == 0x107d508500ULL) { g = map(f, p); break; }
        }
        if (!g) { strlcpy(what, "no GPIO block for BT_ON", cap); return -1; }
        g[1] &= ~(1u << 29); g[2] &= ~(1u << 29);         /* BT_ON low, an output (a read-modify-write wifid also does, once) */
        sys_sleep(50000000);
        g[1] |= 1u << 29;
        u[MCR] = 0x22;                                    /* RTS, automatic flow control */
        strlcpy(what, "Pi 5 UART", cap);
    } else {
        uint32_t __attribute__((aligned(16))) m[8] = { sizeof m, 0, 0x00030002, 8, 0, 4, 0, 0 };   /* the core clock */
        if (mbox_call(m, 8) || !m[6]) { strlcpy(what, "the firmware gives no core clock", cap); return -1; }
        clk = m[6];
        volatile uint32_t *aux = map(f, fdt_find(f, -1, "brcm,bcm2835-aux")), *gp = map(f, fdt_find(f, -1, "brcm,bcm2711-gpio"));
        if (!aux || !gp) { strlcpy(what, "no AUX or GPIO block in the device tree", cap); return -1; }
        uint32_t s = gp[3];                               /* GPFSEL3: pins 30-39; ALT5 is 2 */
        for (int p = 30; p <= 33; p++) s = (s & ~(7u << 3 * (p - 30))) | 2u << 3 * (p - 30);
        gp[3] = s;                                        /* (wifid sets 34-39 in the same register, once) */
        aux[1] |= 1;                                      /* AUX_ENABLES: the mini UART */
        mini = 1;
        u[CNTL] = 0; u[IER] = 0; u[LCR] = 3; u[MCR] = 0; u[FCR] = 6;
        uint32_t bt_on[8] = { sizeof bt_on, 0, 0x00038041, 8, 8, 128, 0, 0 };   /* SET_GPIO_STATE BT_ON (expander line 0) */
        mbox_call(bt_on, 8);
        sys_sleep(50000000);
        bt_on[1] = 0; bt_on[3] = 8; bt_on[4] = 8; bt_on[5] = 128; bt_on[6] = 1;
        mbox_call(bt_on, 8);
        u[CNTL] = 0xCF;                                   /* receive, send; RTS and CTS automatic, active low */
        strlcpy(what, "Pi 4 mini UART", cap);
    }
    if (!pi5) uart_baud(115200);
    else { u[FCR] = 0x87; u[LCR] = 3; uart_baud(115200); }
    sys_sleep(200000000);                                 /* the chip starts */
    while (u[LSR] & 1) (void)u[DATA];
    int spi, fl;
    if (!fdt_spi(f, n, 0, &spi, &fl) && (irq_port = port_create(0)) > 0) {
        irq = spi | (fl & 4 ? IRQ_LEVEL : 0);
        if (irq_bind(irq, irq_port)) irq_port = 0; else irq &= ~IRQ_LEVEL;
    }
    bt_log("bt: %s at %u Hz, up to %d bit/s, %s\n", what, clk, uart_max, irq_port ? "interrupts" : "polled");
    return 0;
}
