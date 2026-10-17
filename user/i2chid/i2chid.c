/*
 * i2chid - Touchpads and touch screens on I2C ("HID over I2C"), on x86-64
 * laptops: up to two devices (a 2-in-1 has both).
 *
 *   1. The firmware's ACPI tables (SYS_ACPI) say where each device is
 *      (aml.c): a device "PNP0C50", its I2C address and speed, the I2C
 *      controller (an ACPI path, whose _ADR is the PCI device), its
 *      interrupt pin (GpioInt) and its HID descriptor's register (_DSM).
 *   2. The controller: Intel's LPSS ("Serial IO") I2C, a Synopsys
 *      DesignWare core. Powered up (PCI PMCSR to D0), out of reset (LPSS
 *      private registers at BAR0 + 0x200), master at the bus's speed (the
 *      SCL counts the firmware gives, its FMCN/SSCN, else ours),
 *      transfers through its FIFO (DATA_CMD: restart, stop), status polled.
 *      (Two devices on one controller: only the first.)
 *   3. HID over I2C: the HID descriptor (30 bytes: where the report
 *      descriptor, input, command and data registers are), SET_POWER on,
 *      RESET, the report descriptor (user/lib/hidrep.c reads it); a
 *      precision touchpad is switched to its own mode (fingers) with a
 *      SET_REPORT on its "input mode" feature, else it stays a mouse; a
 *      touch screen to its multi-touch mode (2), its first finger then an
 *      absolute pointer (a touch: the left button where the finger is).
 *   4. Reports: when the device pulls its interrupt pin low, it has one;
 *      we read it (wMaxInputLength bytes: a length, then the report) and
 *      give it to hidrep.c, which sends moves and clicks to the console
 *      (CON_INJECT, like a USB mouse or tablet).
 *      The pin is a pad of the chipset's GPIO controller (Intel: the
 *      reference firmware's GINF/GADR methods say where its registers
 *      are); its interrupts reach us through the controller's line (its
 *      _CRS, level, active low): GPI_IE/GPI_IS. A watchdog thread checks
 *      once a second that a pending report was not missed; without the
 *      pin, or if interrupts do not come, we read every 8 ms.
 *
 * Everything is logged (the serial console, SYS_LOG), so a laptop's log
 * says what was found and where it stopped. Started by init at boot; with
 * no I2C HID device it ends at once (0) and is not started again.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "aml.h"
#include "hidrep.h"
#include "pci.h"

static struct { char b[199]; size_t n; } line;                 /* (what SYS_DEBUG takes) */
static void line_put(void *c, char ch) { (void)c; if (line.n < sizeof line.b) line.b[line.n++] = ch; }
void log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    line.n = 0;
    memcpy(line.b, "i2chid: ", 8); line.n = 8;
    vformat(line_put, 0, fmt, ap);
    sys_debug(line.b, line.n);
    va_end(ap);
}

/* AML's memory: pages mapped when first read (the firmware's variables, GPIO
 * registers); nothing else, and no writes (they stay in the interpreter). */
int aml_io(int space, uint64_t pa, int w, uint64_t *v, int write)
{
    static struct { uint64_t pa; volatile uint8_t *va; } m[16];
    static int n;
    int i = 0;
    if (space || write) return -1;
    while (i < n && m[i].pa != (pa & ~4095ULL)) i++;
    if (i == n) {
        volatile uint8_t *va = n < 16 ? map_phys(pa & ~4095ULL, 4096) : (void *)-1L;
        if ((long)va < 0) { log_line("cannot read memory at %lx\n", (long)pa); return -1; }
        m[n].pa = pa & ~4095ULL; m[n++].va = va;
    }
    volatile uint8_t *a = m[i].va + (pa & 4095);
    *v = w == 1 ? *a : w == 2 ? *(volatile uint16_t *)a : w == 4 ? *(volatile uint32_t *)a : *(volatile uint64_t *)a;
    return 0;
}

static const char *sub(const char *path, const char *seg)   /* "\\_SB.PC00.I2C5" + "_ADR" */
{
    static char p[64];
    size_t n = strlcpy(p, path, 56);
    p[n] = '.';
    strlcpy(p + n + 1, seg, 6);
    return p;
}

/* ---- The devices. */
typedef struct {
    aml_hid_t a;
    volatile uint8_t *ic;                            /* its controller's registers */
    int fifo;                                        /* their FIFOs' depth */
    uint32_t ns_byte;                                /* a byte's time on the bus (9 clocks) */
    uint8_t hd[30];                                  /* the HID descriptor */
    int maxin, girq, served;                         /* input report's length; interrupt line (-1: polled); reports read */
    hidrep_t rep;
    volatile uint32_t *gis, *gie;                    /* its GPIO group's interrupt status and enable */
    uint32_t gbit;
} hdev_t;
static hdev_t dv[2], *d;                             /* (d: the one spoken to) */
static int ndv;
static uint8_t in[512];                              /* an input report */

/* ---- The DesignWare I2C controller. */
enum { IC_CON = 0x00, IC_TAR = 0x04, IC_DATA_CMD = 0x10, IC_SS_HCNT = 0x14, IC_SS_LCNT = 0x18, IC_FS_HCNT = 0x1C,
       IC_FS_LCNT = 0x20, IC_INTR_MASK = 0x30, IC_RAW_INTR = 0x34, IC_CLR_INTR = 0x40, IC_CLR_TX_ABRT = 0x54, IC_ENABLE = 0x6C,
       IC_TXFLR = 0x74, IC_RXFLR = 0x78, IC_SDA_HOLD = 0x7C, IC_ABRT_SRC = 0x80,
       IC_EN_STATUS = 0x9C, IC_PARAM = 0xF4, IC_TYPE = 0xFC, LPSS_RESETS = 0x204 };
#define R(o) (*(volatile uint32_t *)(d->ic + (o)))

static int i2c_start(void)
{
    int64_t adr, cn[3];
    const aml_hid_t *a = &d->a;
    int fast = a->speed > 100000;
    if (d > dv && !strcmp(dv[0].a.bus, a->bus)) { log_line("%s: on the other device's controller: not supported\n", a->hid); return -1; }
    if (!a->bus[0] || aml_int(sub(a->bus, "_ADR"), 0, 0, &adr)) { log_line("no PCI address for %s\n", a->bus[0] ? a->bus : "its controller"); return -1; }
    pci_t c = { .at = (uint32_t)((adr >> 16 & 31) << 11 | (adr & 7) << 8) };   /* (bus 0) */
    uint32_t id = pci_cfg(&c, 0);
    log_line("controller %s: PCI 00:%02x.%x, id %04x:%04x\n", a->bus, (int)(adr >> 16 & 31), (int)(adr & 7), id & 0xFFFF, id >> 16);
    if (id == 0xFFFFFFFF) return -1;
    for (int cp = pci_cfg(&c, 0x34) & 0xFC, n = 0; cp && n < 16; cp = pci_cfg(&c, cp) >> 8 & 0xFC, n++)
        if ((pci_cfg(&c, cp) & 0xFF) == 1) {                     /* power management: to D0 */
            uint32_t pm = pci_cfg(&c, cp + 4);
            if (pm & 3) { log_line("powering it up from D%d\n", pm & 3); pci_cfgw(&c, cp + 4, pm & ~3u); sys_sleep(10000000); }
            break;
        }
    uint64_t len, pa = pci_bar(&c, 0, &len);
    if (!pa || (long)(d->ic = map_phys(pa, 4096)) < 0) { d->ic = 0; log_line("no registers (BAR0 %lx)\n", (long)pa); return -1; }
    R(LPSS_RESETS) = 0;                                          /* the LPSS block: reset, then out of it */
    sys_sleep(1000000);
    R(LPSS_RESETS) = 7;                                          /* (function and DMA) */
    sys_sleep(1000000);
    uint32_t type = R(IC_TYPE), par = R(IC_PARAM);
    d->fifo = (par >> 16 & 0xFF) + 1;
    if (d->fifo > (int)(par >> 8 & 0xFF) + 1) d->fifo = (par >> 8 & 0xFF) + 1;
    log_line("BAR0 %lx: DesignWare type %08x, FIFO %d, LPSS clock %08x\n", (long)pa, type, d->fifo, R(0x200));
    if (type != 0x44570140) { d->ic = 0; return -1; }
    R(IC_ENABLE) = 0;
    for (int i = 0; i < 100 && R(IC_EN_STATUS) & 1; i++) sys_sleep(100000);
    /* SCL counts (high, low, SDA hold): the firmware's for this board (FMCN,
     * SSCN, as Linux takes them), else ours for a 216 MHz clock (the fastest
     * LPSS has: 400 kHz there; Ice Lake to Raptor Lake run at 133 MHz: 250
     * kHz); within spec either way. */
    int fw = aml_eval(0, sub(a->bus, fast ? "FMCN" : "SSCN"), 0, 0, cn, 3) == 3 && cn[0] > 0 && cn[0] < 4096 && cn[1] > 0 && cn[1] < 4096;
    if (!fw) cn[0] = fast ? 180 : 870, cn[1] = fast ? 344 : 1020, cn[2] = 65;
    R(IC_CON) = 1 | (fast ? 2 : 1) << 1 | 1 << 5 | 1 << 6;      /* master, speed, restarts, no slave */
    R(fast ? IC_FS_HCNT : IC_SS_HCNT) = (uint32_t)cn[0]; R(fast ? IC_FS_LCNT : IC_SS_LCNT) = (uint32_t)cn[1];
    R(IC_SDA_HOLD) = cn[2] > 0 && cn[2] < 1024 ? (uint32_t)cn[2] : 65;
    log_line("SCL high %d, low %d, SDA hold %d: %s\n", (int)cn[0], (int)cn[1], (int)R(IC_SDA_HOLD), fw ? (fast ? "the firmware's FMCN" : "the firmware's SSCN") : "ours");
    R(IC_TAR) = a->addr & 0x3FF;
    R(IC_INTR_MASK) = 0;
    R(IC_ENABLE) = 1;
    d->ns_byte = fast ? 9 * 4000 : 9 * 10000;                    /* (at 250 and 100 kHz: the slowest) */
    return 0;
}

/* One transaction: write w[0..nw), then (restart) read r[0..nr). 0: done
 * (its stop seen); -1: aborted (no answer...), -2: too long. */
static int xfer(const uint8_t *w, int nw, uint8_t *r, int nr)
{
    int sent = 0, got = 0, all = nw + nr, slept = 0, fifo = d->fifo;
    int64_t end = sys_clock() + 10000000 + 2LL * all * d->ns_byte;
    (void)R(IC_CLR_INTR);
    for (;;) {
        uint32_t raw = R(IC_RAW_INTR);
        if (raw & 1 << 6) {                                      /* aborted */
            uint32_t src = R(IC_ABRT_SRC);
            (void)R(IC_CLR_TX_ABRT);
            if (src & ~1u) log_line("I2C abort, reasons %x\n", src);   /* (1: no answer to the address: quiet) */
            return -1;
        }
        while (sent < all && (int)R(IC_TXFLR) < fifo && (sent < nw || sent - nw - got < fifo)) {
            uint32_t c = sent < nw ? w[sent] : 1 << 8;           /* a byte to write, or one to read */
            if (sent == nw && nw && nr) c |= 1 << 10;            /* restart before the reads */
            if (sent == all - 1) c |= 1 << 9;                    /* stop after the last */
            R(IC_DATA_CMD) = c;
            sent++;
        }
        while (got < nr && R(IC_RXFLR)) r[got++] = (uint8_t)R(IC_DATA_CMD);
        if (sent == all && got == nr && raw & 1 << 9) return 0; /* its stop: done */
        if (sys_clock() > end) return -2;
        if (!slept++ && sent == all) sys_sleep((uint64_t)(all - got) * d->ns_byte);   /* the bytes take this long */
    }
}

/* ---- HID over I2C. */
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int command(int op, int low, const uint8_t *data, int n)   /* to the command register (+ the data register) */
{
    uint8_t *hd = d->hd, w[16] = { hd[16], hd[17], (uint8_t)low, (uint8_t)op, hd[18], hd[19] };
    int k = n ? 6 : 4;
    for (int i = 0; i < n && k < 16; i++) w[k++] = data[i];
    return xfer(w, k, 0, 0);
}

static void report(void)                             /* read one input report of d, if there is one */
{
    if (xfer(0, 0, in, d->maxin)) return;
    int len = le16(in);
    if (len > 2 && len <= d->maxin) hid_input(&d->rep, in + 2, len - 2);
}

static int hid_start(void)
{
    int r = -1, desc = d->a.desc;
    uint8_t *hd = d->hd;
    hidrep_t *rep = &d->rep;
    for (int k = 0, regs[3] = { desc, 1, 0x20 }; k < 3 && r; k++)   /* (_DSM's, else the usual ones) */
        if (regs[k] >= 0 && !(k && regs[k] == desc)) {
            uint8_t w[2] = { (uint8_t)regs[k], (uint8_t)(regs[k] >> 8) };
            r = xfer(w, 2, hd, 30) || le16(hd) != 30 || le16(hd + 2) != 0x100;
            if (!r && regs[k] != desc) log_line("HID descriptor at %d (not %d)\n", regs[k], desc);
        }
    if (r) { log_line("no HID descriptor at address 0x%x\n", d->a.addr); return -1; }
    int rlen = le16(hd + 4);
    d->maxin = le16(hd + 10) > (int)sizeof in ? (int)sizeof in : le16(hd + 10);
    log_line("HID %04x:%04x version %x: report descriptor %d bytes at %d, input %d bytes at %d, command %d, data %d\n",
             le16(hd + 20), le16(hd + 22), le16(hd + 24), rlen, le16(hd + 6), d->maxin, le16(hd + 8), le16(hd + 16), le16(hd + 18));
    command(8, 0, 0, 0);                                         /* SET_POWER on */
    sys_sleep(10000000);
    command(1, 0, 0, 0);                                         /* RESET: it answers with an empty report */
    sys_sleep(100000000);
    xfer(0, 0, in, 2);
    uint8_t *rd = malloc(rlen);
    uint8_t w[2] = { hd[6], hd[7] };
    if (!rd || xfer(w, 2, rd, rlen)) { log_line("cannot read the report descriptor\n"); return -1; }
    hid_parse(rep, rd, rlen);
    free(rd);
    log_line("%s: report %d, %d buttons, %s; touchpad: report %d, %d fingers, mode report %d\n", rep->screen ? "touch screen" : "mouse",
             rep->rid, rep->bn, rep->absolute ? "absolute" : "relative", rep->trid, rep->nf, rep->mrid);
    /* the device's own mode: SET_REPORT (feature) "input mode": 3 a touchpad's fingers, 2 a touch screen's; length, id, value */
    int mode = rep->trid ? 3 : rep->screen ? 2 : 0;
    uint8_t m[8] = { (uint8_t)(3 + (rep->msz + 7) / 8), 0, (uint8_t)rep->mrid, (uint8_t)mode };
    if (mode && (rep->mrid < 1 || rep->mrid > 14 || rep->msz > 32 || command(3, 0x30 | rep->mrid, m, m[0]))) {
        log_line("input mode %d not set%s\n", mode, rep->trid ? ": it stays a mouse" : "");
        rep->trid = 0;
    }
    if (!rep->trid && !rep->xsz) { log_line("neither a mouse, a touch screen nor a touchpad\n"); return -1; }
    return 0;
}

/* ---- The interrupt pin: a pad of Intel's GPIO controller, as its
 * reference firmware describes it (\_SB.GINF, \_SB.GADR: aml_gpio). */
static volatile int busy;

static int gpio_start(void)
{
    const aml_hid_t *dev = &d->a;
    aml_hid_t c;
    aml_gpio_t g;
    volatile uint8_t *cm;
    d->girq = -1;
    if (dev->pin < 0 || dev->pin_edge || aml_res(0, sub(dev->gpio, "_CRS"), &c) || c.irq < 0) {
        log_line("no GPIO interrupt (pin %d on %s)\n", dev->pin, dev->gpio);
        return -1;
    }
    if (!aml_gpio(dev->pin, &g) && (long)(cm = map_phys(g.base, 8192)) >= 0) {   /* its community's registers */
        volatile uint32_t *ho = (volatile uint32_t *)(cm + g.own), *pad = (volatile uint32_t *)(cm + g.pad);
        uint32_t gbit = d->gbit = 1u << g.bit;
        d->gis = (volatile uint32_t *)(cm + g.sts - 0x40);
        d->gie = (volatile uint32_t *)(cm + g.sts - 0x20);
        log_line("GPIO pin %d: group %d pad %d at %lx, DW0 %08x DW1 %08x, host owned %d; interrupt line %d\n",
                 dev->pin, g.group, g.bit, (long)(g.base + g.pad), pad[0], pad[1], !!(*ho & gbit), c.irq);
        *ho |= gbit;                                             /* ours (not ACPI's) */
        /* level, inverted if active low ("1": a report waits); not to the I/O APIC, SCI, SMI, NMI */
        pad[0] = (pad[0] & ~(3u << 25 | 1u << 23 | 0xFu << 17)) | (dev->pin_low ? 1u << 23 : 0);
        *d->gis = gbit;
        *d->gie |= gbit;
        d->girq = c.irq;
        return 0;
    }
    log_line("GPIO pin %d: its registers are unknown (no GINF/GADR)\n", dev->pin);
    return -1;
}

static void lock(void) { while (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) sys_yield(); }
static void unlock(void) { __atomic_store_n(&busy, 0, __ATOMIC_RELEASE); }

/* Once a second: a device's report waiting since the last look means its
 * interrupts do not arrive: from then on, this thread reads it every 8 ms. */
static void watchdog(void *arg)
{
    (void)arg;
    int seen[2] = { -1, -1 }, waited[2] = { 0, 0 };
    for (unsigned t = 0;; t++) {
        int poll = 0;
        for (int i = 0; i < ndv; i++) poll |= dv[i].girq < 0;
        sys_sleep(poll ? 8000000 : 1000000000);
        lock();
        for (int i = 0; i < ndv; i++) {
            d = &dv[i];
            if (d->girq >= 0) {
                if (poll && t % 125) continue;                   /* (looked at once a second) */
                waited[i] = (*d->gis & d->gbit) && seen[i] == d->served ? waited[i] + 1 : 0;
                seen[i] = d->served;
                if (waited[i] < 2) continue;
                log_line("%s: GPIO interrupts do not arrive: reading every 8 ms\n", d->a.hid);
                d->girq = -1;
            }
            report();
            if (d->gis) *d->gis = d->gbit;
        }
        unlock();
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    long n = sys_acpi(0, 0);
    uint8_t *t = n > 0 ? malloc(n) : 0;
    if (!t || sys_acpi(t, n) != n) { log_line("no ACPI tables\n"); return 0; }
    long objs = aml_load(t, n);
    log_line("ACPI tables: %ld bytes, %ld named objects\n", n, objs);
    aml_hid_t a[4];
    int na = aml_hid(a, 4);
    long port = 0;
    for (int k = 0; k < na && ndv < 2; k++) {
        d = &dv[ndv];
        d->a = a[k];
        if (d->a.addr < 0 || i2c_start() || hid_start()) { memset(d, 0, sizeof *d); continue; }
        if (!port) port = port_create("i2chid");
        if (!gpio_start() && irq_bind(d->girq | IRQ_LEVEL | IRQ_LOW, port)) { log_line("interrupt line %d refused\n", d->girq); d->girq = -1; }
        log_line("%s ready (%s), %s\n", d->a.hid, d->rep.trid ? "touchpad" : d->rep.screen ? "touch screen" : "mouse",
                 d->girq >= 0 ? "interrupts" : "read every 8 ms");
        ndv++;
    }
    if (!ndv) { log_line("no I2C touchpad or touch screen\n"); return 0; }
    hid_send(0, 0);                                              /* con: a pointer is here */
    thread_start(watchdog, 0, 8192);
    for (;;) {
        msg_t m = { 0 };
        if (ipc_recv(port, &m)) continue;                        /* (interrupts only) */
        lock();
        for (int i = 0; i < ndv; i++) {
            d = &dv[i];
            if (d->girq < 0 || !(*d->gis & d->gbit)) continue;
            report();
            *d->gis = d->gbit;
            d->served++;
        }
        unlock();
        for (int i = 0; i < ndv; i++) if (dv[i].girq >= 0) irq_ack(dv[i].girq);
    }
}
