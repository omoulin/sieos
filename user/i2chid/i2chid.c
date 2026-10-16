/*
 * i2chid - Touchpads on I2C ("HID over I2C"), on x86-64 laptops.
 *
 *   1. The firmware's ACPI tables (SYS_ACPI) say where the touchpad is
 *      (aml.c): a device "PNP0C50", its I2C address and speed, the I2C
 *      controller (an ACPI path, whose _ADR is the PCI device), its
 *      interrupt pin (GpioInt) and its HID descriptor's register (_DSM).
 *   2. The controller: Intel's LPSS ("Serial IO") I2C, a Synopsys
 *      DesignWare core. Powered up (PCI PMCSR to D0), out of reset (LPSS
 *      private registers at BAR0 + 0x200), master at the bus's speed,
 *      transfers through its FIFO (DATA_CMD: restart, stop), status polled.
 *   3. HID over I2C: the HID descriptor (30 bytes: where the report
 *      descriptor, input, command and data registers are), SET_POWER on,
 *      RESET, the report descriptor (user/lib/hidrep.c reads it); a
 *      precision touchpad is switched to its own mode (fingers) with a
 *      SET_REPORT on its "input mode" feature, else it stays a mouse.
 *   4. Reports: when the touchpad pulls its interrupt pin low, it has one;
 *      we read it (wMaxInputLength bytes: a length, then the report) and
 *      give it to hidrep.c, which sends moves and clicks to the console
 *      (CON_INJECT, like a USB mouse).
 *      The pin is a pad of the chipset's GPIO controller (Intel: the
 *      reference firmware's GINF/GADR methods say where its registers
 *      are); its interrupts reach us through the controller's line (its
 *      _CRS, level, active low): GPI_IE/GPI_IS. A watchdog thread checks
 *      once a second that a pending report was not missed; without the
 *      pin, or if interrupts do not come, we read every 8 ms.
 *
 * Everything is logged (the serial console, SYS_LOG), so a laptop's log
 * says what was found and where it stopped. Started by init at boot; with
 * no I2C touchpad it ends at once (0) and is not started again.
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

/* ---- The DesignWare I2C controller. */
enum { IC_CON = 0x00, IC_TAR = 0x04, IC_DATA_CMD = 0x10, IC_SS_HCNT = 0x14, IC_SS_LCNT = 0x18, IC_FS_HCNT = 0x1C,
       IC_FS_LCNT = 0x20, IC_INTR_MASK = 0x30, IC_RAW_INTR = 0x34, IC_CLR_INTR = 0x40, IC_CLR_TX_ABRT = 0x54, IC_ENABLE = 0x6C,
       IC_TXFLR = 0x74, IC_RXFLR = 0x78, IC_SDA_HOLD = 0x7C, IC_ABRT_SRC = 0x80,
       IC_EN_STATUS = 0x9C, IC_PARAM = 0xF4, IC_TYPE = 0xFC, LPSS_RESETS = 0x204 };
static volatile uint8_t *ic;
static int fifo;                                     /* its FIFOs' depth */
static uint32_t ns_byte;                             /* a byte's time on the bus (9 clocks) */
#define R(o) (*(volatile uint32_t *)(ic + (o)))

static int i2c_start(const aml_hid_t *d)
{
    int64_t adr;
    if (!d->bus[0] || aml_int(sub(d->bus, "_ADR"), 0, 0, &adr)) { log_line("no PCI address for %s\n", d->bus[0] ? d->bus : "its controller"); return -1; }
    pci_t c = { .at = (uint32_t)((adr >> 16 & 31) << 11 | (adr & 7) << 8) };   /* (bus 0) */
    uint32_t id = pci_cfg(&c, 0);
    log_line("controller %s: PCI 00:%02x.%x, id %04x:%04x\n", d->bus, (int)(adr >> 16 & 31), (int)(adr & 7), id & 0xFFFF, id >> 16);
    if (id == 0xFFFFFFFF) return -1;
    for (int cp = pci_cfg(&c, 0x34) & 0xFC, n = 0; cp && n < 16; cp = pci_cfg(&c, cp) >> 8 & 0xFC, n++)
        if ((pci_cfg(&c, cp) & 0xFF) == 1) {                     /* power management: to D0 */
            uint32_t pm = pci_cfg(&c, cp + 4);
            if (pm & 3) { log_line("powering it up from D%d\n", pm & 3); pci_cfgw(&c, cp + 4, pm & ~3u); sys_sleep(10000000); }
            break;
        }
    uint64_t len, pa = pci_bar(&c, 0, &len);
    if (!pa || (long)(ic = map_phys(pa, 4096)) < 0) { log_line("no registers (BAR0 %lx)\n", (long)pa); return -1; }
    R(LPSS_RESETS) = 0;                                          /* the LPSS block: reset, then out of it */
    sys_sleep(1000000);
    R(LPSS_RESETS) = 7;                                          /* (function and DMA) */
    sys_sleep(1000000);
    uint32_t type = R(IC_TYPE), par = R(IC_PARAM);
    fifo = (par >> 16 & 0xFF) + 1;
    if (fifo > (int)(par >> 8 & 0xFF) + 1) fifo = (par >> 8 & 0xFF) + 1;
    log_line("BAR0 %lx: DesignWare type %08x, FIFO %d, LPSS clock %08x\n", (long)pa, type, fifo, R(0x200));
    if (type != 0x44570140) return -1;
    R(IC_ENABLE) = 0;
    for (int i = 0; i < 100 && R(IC_EN_STATUS) & 1; i++) sys_sleep(100000);
    /* SCL counts for a 216 MHz clock (the fastest LPSS has): 400 kHz there,
     * slower if the clock is slower (133 MHz: 250 kHz); always within spec. */
    int fast = d->speed > 100000;
    R(IC_CON) = 1 | (fast ? 2 : 1) << 1 | 1 << 5 | 1 << 6;      /* master, speed, restarts, no slave */
    R(IC_SS_HCNT) = 870; R(IC_SS_LCNT) = 1020;
    R(IC_FS_HCNT) = 180; R(IC_FS_LCNT) = 344;
    R(IC_SDA_HOLD) = 65;
    R(IC_TAR) = d->addr & 0x3FF;
    R(IC_INTR_MASK) = 0;
    R(IC_ENABLE) = 1;
    ns_byte = fast ? 9 * 4000 : 9 * 10000;                       /* (at 250 and 100 kHz) */
    return 0;
}

/* One transaction: write w[0..nw), then (restart) read r[0..nr). 0: done
 * (its stop seen); -1: aborted (no answer...), -2: too long. */
static int xfer(const uint8_t *w, int nw, uint8_t *r, int nr)
{
    int sent = 0, got = 0, all = nw + nr, slept = 0;
    int64_t end = sys_clock() + 10000000 + 2LL * all * ns_byte;
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
        if (!slept++ && sent == all) sys_sleep((uint64_t)(all - got) * ns_byte);   /* the bytes take this long */
    }
}

/* ---- HID over I2C. */
static aml_hid_t dev;
static uint8_t hd[30], in[512];                      /* the HID descriptor; an input report */
static hidrep_t rep;
static int maxin;
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int command(int op, int low, const uint8_t *data, int n)   /* to the command register (+ the data register) */
{
    uint8_t w[16] = { hd[16], hd[17], (uint8_t)low, (uint8_t)op, hd[18], hd[19] };
    int k = n ? 6 : 4;
    for (int i = 0; i < n && k < 16; i++) w[k++] = data[i];
    return xfer(w, k, 0, 0);
}

static void report(void)                             /* read one input report, if there is one */
{
    if (xfer(0, 0, in, maxin)) return;
    int len = le16(in);
    if (len > 2 && len <= maxin) hid_input(&rep, in + 2, len - 2);
}

static int hid_start(void)
{
    int r = -1;
    for (int k = 0, regs[3] = { dev.desc, 1, 0x20 }; k < 3 && r; k++)   /* (_DSM's, else the usual ones) */
        if (regs[k] >= 0 && !(k && regs[k] == dev.desc)) {
            uint8_t w[2] = { (uint8_t)regs[k], (uint8_t)(regs[k] >> 8) };
            r = xfer(w, 2, hd, 30) || le16(hd) != 30 || le16(hd + 2) != 0x100;
            if (!r && regs[k] != dev.desc) log_line("HID descriptor at %d (not %d)\n", regs[k], dev.desc);
        }
    if (r) { log_line("no HID descriptor at address 0x%x\n", dev.addr); return -1; }
    int rlen = le16(hd + 4);
    maxin = le16(hd + 10) > (int)sizeof in ? (int)sizeof in : le16(hd + 10);
    log_line("HID %04x:%04x version %x: report descriptor %d bytes at %d, input %d bytes at %d, command %d, data %d\n",
             le16(hd + 20), le16(hd + 22), le16(hd + 24), rlen, le16(hd + 6), maxin, le16(hd + 8), le16(hd + 16), le16(hd + 18));
    command(8, 0, 0, 0);                                         /* SET_POWER on */
    sys_sleep(10000000);
    command(1, 0, 0, 0);                                         /* RESET: it answers with an empty report */
    sys_sleep(100000000);
    xfer(0, 0, in, 2);
    uint8_t *rd = malloc(rlen);
    uint8_t w[2] = { hd[6], hd[7] };
    if (!rd || xfer(w, 2, rd, rlen)) { log_line("cannot read the report descriptor\n"); return -1; }
    hid_parse(&rep, rd, rlen);
    free(rd);
    log_line("mouse: report %d, %d buttons, %s; touchpad: report %d, %d fingers, mode report %d\n", rep.rid, rep.bn,
             rep.absolute ? "absolute" : "relative", rep.trid, rep.nf, rep.mrid);
    /* the touchpad's own mode: SET_REPORT (feature) "input mode" = 3 (fingers): length, id, value */
    uint8_t m[8] = { (uint8_t)(3 + (rep.msz + 7) / 8), 0, (uint8_t)rep.mrid, 3 };
    if (rep.trid && (rep.mrid < 1 || rep.mrid > 14 || rep.msz > 32 || command(3, 0x30 | rep.mrid, m, m[0]))) {
        log_line("input mode not set: it stays a mouse\n");
        rep.trid = 0;
    }
    if (!rep.trid && !rep.xsz) { log_line("neither a mouse nor a touchpad\n"); return -1; }
    return 0;
}

/* ---- The interrupt pin: a pad of Intel's GPIO controller, as its
 * reference firmware describes it (\_SB.GINF (group, what): 0 its
 * community's offset, 1 pads, 2 their registers' offset, 3 host ownership,
 * 5 GPE status (interrupt status and enable are 0x40 and 0x20 before it),
 * 8 the group's first ACPI pin number; \_SB.GADR: with the base address). */
static volatile uint32_t *pad, *gis, *gie;
static uint32_t gbit;
static volatile int girq = -1;
static volatile int busy, served;

static int ginf(int64_t g, int64_t what, int64_t *v) { int64_t a[2] = { g, what }; return aml_int("\\_SB.GINF", 2, a, v); }

static int gpio_start(void)
{
    int64_t g, n, first, at, off, sts, own;
    aml_hid_t c;
    if (dev.pin < 0 || dev.pin_edge || aml_res(0, sub(dev.gpio, "_CRS"), &c) || c.irq < 0) {
        log_line("no GPIO interrupt (pin %d on %s)\n", dev.pin, dev.gpio);
        return -1;
    }
    for (g = 0; g < 32 && !ginf(g, 1, &n) && !ginf(g, 8, &first); g++) {
        if (dev.pin < first || dev.pin >= first + n) continue;
        int64_t a[2] = { g, 2 }, pin = dev.pin - first;
        if (aml_int("\\_SB.GADR", 2, a, &at) || ginf(g, 2, &off) || ginf(g, 5, &sts) || ginf(g, 3, &own)) break;
        volatile uint8_t *cm = map_phys(at - off, 8192);        /* the community's registers */
        if ((long)cm < 0) break;
        volatile uint32_t *ho = (volatile uint32_t *)(cm + own);
        pad = (volatile uint32_t *)(cm + off + pin * 16);
        gis = (volatile uint32_t *)(cm + sts - 0x40);
        gie = (volatile uint32_t *)(cm + sts - 0x20);
        gbit = 1u << pin;
        log_line("GPIO pin %d: group %d pad %d at %lx, DW0 %08x DW1 %08x, host owned %d; interrupt line %d\n",
                 dev.pin, (int)g, (int)pin, (long)(at + pin * 16), pad[0], pad[1], !!(*ho & gbit), c.irq);
        *ho |= gbit;                                             /* ours (not ACPI's) */
        /* level, inverted if active low ("1": a report waits); not to the I/O APIC, SCI, SMI, NMI */
        pad[0] = (pad[0] & ~(3u << 25 | 1u << 23 | 0xFu << 17)) | (dev.pin_low ? 1u << 23 : 0);
        *gis = gbit;
        *gie |= gbit;
        girq = c.irq;
        return 0;
    }
    log_line("GPIO pin %d: its registers are unknown (no GINF/GADR)\n", dev.pin);
    return -1;
}

static void lock(void) { while (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) sys_yield(); }
static void unlock(void) { __atomic_store_n(&busy, 0, __ATOMIC_RELEASE); }

/* Once a second: a report waiting since the last look means interrupts do
 * not arrive: from then on, this thread reads every 8 ms. */
static void watchdog(void *arg)
{
    (void)arg;
    for (int seen = -1, waited = 0;;) {
        sys_sleep(girq < 0 ? 8000000 : 1000000000);
        if (girq >= 0) {
            waited = (*gis & gbit) && seen == served ? waited + 1 : 0;
            seen = served;
            if (waited < 2) continue;
            log_line("GPIO interrupts do not arrive: reading every 8 ms\n");
            girq = -1;
        }
        lock();
        report();
        if (gis) *gis = gbit;
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
    aml_hid_t d[4];
    int nd = aml_hid(d, 4), k;
    for (k = 0; k < nd; k++) {
        dev = d[k];
        if (dev.addr >= 0 && !i2c_start(&dev) && !hid_start()) break;
    }
    if (k == nd) { log_line("no I2C touchpad\n"); return 0; }
    long port = port_create("i2chid");
    if (!gpio_start() && irq_bind(girq | IRQ_LEVEL | IRQ_LOW, port)) { log_line("interrupt line %d refused\n", girq); girq = -1; }
    log_line("%s ready, %s\n", dev.hid, girq >= 0 ? "interrupts" : "read every 8 ms");
    hid_send(0, 0);                                              /* con: a pointer is here */
    thread_start(watchdog, 0, 8192);
    for (;;) {
        msg_t m = { 0 };
        if (ipc_recv(port, &m) || girq < 0) continue;            /* (interrupts only) */
        if (*gis & gbit) {
            lock();
            report();
            *gis = gbit;
            served++;
            unlock();
        }
        irq_ack(girq);
    }
}
