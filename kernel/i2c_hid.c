/*
 * i2c_hid.c - HID over I2C (touchpads, some keyboards and touchscreens) on
 * the Intel LPSS I2C controllers (Synopsys DesignWare): Raptor Lake-S PCH
 * (8086:7A4C-7A4F, 7A7C, 7A7D), Alder Lake, Ice Lake-LP (8086:34E8-34EB,
 * 34C5, 34C6), Tiger Lake, Meteor Lake, Arrow Lake, ...
 *
 * Finding the devices: the ACPI tables (DSDT, SSDTs) are searched for the
 * HID-over-I2C devices (_HID/_CID PNP0C50 or ACPI0C50) and, in the body of
 * each, its I2cSerialBus resource: the device's address.  The AML is not
 * run, only read, and an address may be filled in at run time (a template
 * with address 0), so the usual touchpad addresses (0x15 ELAN, 0x2C
 * Synaptics and others, 0x2A) are tried as well.  An address is first checked with a
 * one-byte read (harmless for any device); the HID descriptor register,
 * which comes from a _DSM, is found by trying the usual ones (1, 0x20, 0,
 * 0x30) for a valid 30-byte HID descriptor.
 *
 * The controller: taken out of reset (the LPSS private registers at
 * 0x200), master mode, standard speed (100 kHz).  The input clock differs
 * between chipsets (100 to 216 MHz); the SCL counts are set for 216 MHz,
 * so a slower clock gives a slower bus, never a faster one.  No interrupts.
 *
 * The device: SET_POWER on, RESET, the report descriptor into hid.c.  Its
 * interrupt line (a GPIO) is not used: the input register is read from the
 * timer tick instead.  A device with nothing to report answers with a
 * zero length.  The reads are started in one tick and collected in the
 * next (the controller's FIFOs hold them), so the tick never waits for the
 * bus.  A precision touchpad reports as a mouse until the host switches it
 * to touchpad mode, which is not done: it moves the pointer and clicks.
 *
 * "noi2c" on the boot command line leaves the controllers alone.
 */
#ifdef I2C_HOST_TEST
#include "i2c_host_test.h"
#else
#include "hid.h"
#include "pci.h"
#include "mm.h"
#include "smp.h"
#endif

/* DesignWare I2C registers */
#define IC_CON          0x00
#define IC_TAR          0x04
#define IC_DATA_CMD     0x10
#define IC_SS_SCL_HCNT  0x14
#define IC_SS_SCL_LCNT  0x18
#define IC_INTR_MASK    0x30
#define IC_RAW_INTR     0x34
#define IC_RX_TL        0x38
#define IC_TX_TL        0x3C
#define IC_CLR_INTR     0x40
#define IC_CLR_TX_ABRT  0x54
#define IC_CLR_STOP     0x60
#define IC_ENABLE       0x6C
#define IC_STATUS       0x70
#define IC_TXFLR        0x74
#define IC_RXFLR        0x78
#define IC_SDA_HOLD     0x7C
#define IC_ABRT_SOURCE  0x80
#define IC_ENABLE_STS   0x9C
#define IC_COMP_PARAM   0xF4
#define IC_COMP_TYPE    0xFC
#define DW_COMP_TYPE    0x44570140
#define CMD_READ        (1U << 8)
#define CMD_STOP        (1U << 9)
#define CMD_RESTART     (1U << 10)
#define INTR_TX_ABRT    (1U << 6)
#define INTR_STOP       (1U << 9)
#define LPSS_RESETS     0x204
#define LPSS_REMAP_LO   0x240
#define LPSS_REMAP_HI   0x244

#define MAX_CTRL  8
#define MAX_DEVS  4
#define MAX_ADDRS 16

static const uint16_t lpss_i2c_ids[] = {
    0x7A4C, 0x7A4D, 0x7A4E, 0x7A4F, 0x7A7C, 0x7A7D,                 /* Raptor Lake-S PCH */
    0x7ACC, 0x7ACD, 0x7ACE, 0x7ACF, 0x7AFC, 0x7AFD,                 /* Alder Lake-S PCH */
    0x51E8, 0x51E9, 0x51EA, 0x51EB, 0x51C5, 0x51C6, 0x51D8, 0x51D9, /* Alder/Raptor Lake-P */
    0x54E8, 0x54E9, 0x54EA, 0x54EB, 0x54C5, 0x54C6,                 /* Alder Lake-N */
    0x34E8, 0x34E9, 0x34EA, 0x34EB, 0x34C5, 0x34C6,                 /* Ice Lake-LP */
    0xA0E8, 0xA0E9, 0xA0EA, 0xA0EB, 0xA0C5, 0xA0C6, 0xA0D8, 0xA0D9, /* Tiger Lake-LP */
    0x43E8, 0x43E9, 0x43EA, 0x43EB, 0x43AD, 0x43AE, 0x43D8, 0x43D9, /* Tiger Lake-H */
    0x02E8, 0x02E9, 0x02EA, 0x02EB, 0x02C5, 0x02C6,                 /* Comet Lake-LP */
    0x7E78, 0x7E79, 0x7E7A, 0x7E7B, 0x7E50, 0x7E51,                 /* Meteor Lake-P */
    0x7778, 0x7779, 0x777A, 0x777B, 0x7750, 0x7751,                 /* Arrow Lake-H */
};

struct i2c_ctrl {
    struct pci_dev pci;
    volatile uint8_t *regs;
    int index;
    int tx_depth, rx_depth;
    int tar;                        /* the target address set, -1: none */
    /* the read started by the tick, collected by the next */
    struct i2c_hid *pending;
    int pending_len;
    uint64_t started;
    int next;                       /* round robin over its devices */
};

struct i2c_hid {
    struct i2c_ctrl *c;
    int addr;
    uint16_t desc_reg, input_reg, cmd_reg, report_reg, report_len, max_input;
    uint16_t vendor, product;
    struct hid hid;
    uint8_t buf[256];
};

static struct i2c_ctrl ctrls[MAX_CTRL];
static int nctrl;
static struct i2c_hid devs[MAX_DEVS];
static int ndevs;
static bool debug, ready;

#ifdef I2C_HOST_TEST
#define rd(c, r)    ((void)(c), sim_rd(r))
#define wr(c, r, v) ((void)(c), sim_wr(r, v))
#else
static inline uint32_t rd(struct i2c_ctrl *c, uint32_t r) { return *(volatile uint32_t *)(c->regs + r); }
static inline void wr(struct i2c_ctrl *c, uint32_t r, uint32_t v) { *(volatile uint32_t *)(c->regs + r) = v; }
#endif

static void udelay(unsigned us)
{
    uint64_t end = hrtime() + (uint64_t)us * 1000;
    while (hrtime() < end)
        __asm__ volatile("pause");
}

/* ---------------------------------------------------------------- the controller */

static bool dw_enable(struct i2c_ctrl *c, bool on)
{
    wr(c, IC_ENABLE, on);
    for (int t = 0; t < 1000; t++) {
        if ((rd(c, IC_ENABLE_STS) & 1) == on)
            return true;
        udelay(25);
    }
    return false;
}

static bool dw_target(struct i2c_ctrl *c, int addr)
{
    if (c->tar == addr)
        return true;
    if (!dw_enable(c, false))
        return false;
    wr(c, IC_TAR, addr & 0x7F);
    c->tar = addr;
    return dw_enable(c, true);
}

static bool dw_init(struct i2c_ctrl *c)
{
    const struct pci_dev *pd = &c->pci;
    uint8_t pm = pci_find_cap(pd, 1, 0);
    if (pm) {
        uint32_t pmcsr = pci_read32(pd->bus, pd->dev, pd->func, pm + 4);
        if (pmcsr & 3) {                         /* D3: to D0 */
            pci_write32(pd->bus, pd->dev, pd->func, pm + 4, pmcsr & ~3U);
            udelay(10000);
        }
    }
    uint32_t cmd = pci_read32(pd->bus, pd->dev, pd->func, 4);
    pci_write32(pd->bus, pd->dev, pd->func, 4, cmd | 0x6);
    bool io;
    uint64_t bar = pci_bar_addr(pd, 0, &io);
    if (!bar || io)
        return false;
    c->regs = mmio_map(bar, 0x1000);
    if (!c->regs)
        return false;
    wr(c, LPSS_RESETS, 0);                       /* the LPSS function out of reset */
    udelay(100);
    wr(c, LPSS_RESETS, 7);
    wr(c, LPSS_REMAP_LO, (uint32_t)bar);
    wr(c, LPSS_REMAP_HI, (uint32_t)(bar >> 32));
    udelay(100);
    if (rd(c, IC_COMP_TYPE) != DW_COMP_TYPE)
        return false;
    uint32_t param = rd(c, IC_COMP_PARAM);
    c->rx_depth = ((param >> 8) & 0xFF) + 1;
    c->tx_depth = ((param >> 16) & 0xFF) + 1;
    if (c->rx_depth < 8 || c->tx_depth < 8)
        c->rx_depth = c->tx_depth = 32;
    if (!dw_enable(c, false))
        return false;
    wr(c, IC_CON, 1 | 1U << 1 | 1U << 5 | 1U << 6);   /* master, standard speed, restart, no slave */
    wr(c, IC_SS_SCL_HCNT, 920);                  /* 4.3 us high, 5.1 us low at 216 MHz (longer below) */
    wr(c, IC_SS_SCL_LCNT, 1100);
    wr(c, IC_SDA_HOLD, 65);
    wr(c, IC_INTR_MASK, 0);
    wr(c, IC_RX_TL, 0);
    wr(c, IC_TX_TL, 0);
    rd(c, IC_CLR_INTR);
    c->tar = -1;
    return true;
}

/*
 * A write of wlen bytes and/or a read of rlen bytes (with a repeated start
 * between), synchronously: 0, or < 0 (no answer, timeout).
 */
static int dw_xfer(struct i2c_ctrl *c, int addr, const uint8_t *w, int wlen, uint8_t *r, int rlen)
{
    if (!dw_target(c, addr))
        return -1;
    rd(c, IC_CLR_TX_ABRT);
    rd(c, IC_CLR_STOP);
    while (rd(c, IC_RXFLR))
        rd(c, IC_DATA_CMD);
    int total = wlen + rlen, sent = 0, got = 0;
    uint64_t end = hrtime() + 100UL * 1000000 + (uint64_t)total * 400000;   /* (40 us a byte at 25 kHz) */
    while (got < rlen || sent < total) {
        while (sent < total && (int)rd(c, IC_TXFLR) < c->tx_depth && sent - wlen - got < c->rx_depth - 1) {
            uint32_t v;
            if (sent < wlen)
                v = w[sent];
            else
                v = CMD_READ | (sent == wlen && wlen ? CMD_RESTART : 0);
            if (sent == total - 1)
                v |= CMD_STOP;
            wr(c, IC_DATA_CMD, v);
            sent++;
        }
        while (got < rlen && rd(c, IC_RXFLR))
            r[got++] = rd(c, IC_DATA_CMD);
        if (rd(c, IC_RAW_INTR) & INTR_TX_ABRT) {
            rd(c, IC_CLR_TX_ABRT);
            return -1;                           /* no acknowledge */
        }
        if (hrtime() > end)
            return -2;
    }
    for (int t = 0; t < 2000 && !(rd(c, IC_RAW_INTR) & INTR_STOP); t++)
        udelay(10);
    rd(c, IC_CLR_STOP);
    if (rd(c, IC_RAW_INTR) & INTR_TX_ABRT) {    /* (a write not acknowledged: seen at the end) */
        rd(c, IC_CLR_TX_ABRT);
        return -1;
    }
    return 0;
}

/* Start reading n bytes (n <= the FIFO depth) without waiting. */
static bool dw_read_start(struct i2c_ctrl *c, int addr, int n)
{
    if (!dw_target(c, addr))
        return false;
    rd(c, IC_CLR_TX_ABRT);
    rd(c, IC_CLR_STOP);
    while (rd(c, IC_RXFLR))
        rd(c, IC_DATA_CMD);
    for (int i = 0; i < n; i++)
        wr(c, IC_DATA_CMD, CMD_READ | (i == n - 1 ? CMD_STOP : 0));
    return true;
}

/* The read started: 1 done (into r), 0 not yet, < 0 failed. */
static int dw_read_done(struct i2c_ctrl *c, uint8_t *r, int n)
{
    uint32_t raw = rd(c, IC_RAW_INTR);
    if (raw & INTR_TX_ABRT) {
        rd(c, IC_CLR_TX_ABRT);
        return -1;
    }
    if (!(raw & INTR_STOP) || (int)rd(c, IC_RXFLR) < n)
        return 0;
    for (int i = 0; i < n; i++)
        r[i] = rd(c, IC_DATA_CMD);
    rd(c, IC_CLR_STOP);
    return 1;
}

/* ---------------------------------------------------------------- ACPI */

static int pkg_length(const uint8_t *p, const uint8_t *end, uint32_t *len)
{
    int follow = p[0] >> 6;
    if (p + 1 + follow > end)
        return 0;
    if (!follow) {
        *len = p[0] & 0x3F;
        return 1;
    }
    uint32_t v = p[0] & 0x0F;
    for (int i = 0; i < follow; i++)
        v |= (uint32_t)p[1 + i] << (4 + 8 * i);
    *len = v;
    return 1 + follow;
}

static bool name_char(uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* The HID-over-I2C marker: EisaId ("PNP0C50"), or the strings "PNP0C50", "ACPI0C50". */
static int hid_marker(const uint8_t *p, const uint8_t *end)
{
    static const uint8_t eisa[] = { 0x0C, 0x41, 0xD0, 0x0C, 0x50 };
    if (p + 5 <= end && !memcmp(p, eisa, 5))
        return 5;
    if (p + 9 <= end && !memcmp(p, "\x0DPNP0C50", 9))
        return 9;
    if (p + 10 <= end && !memcmp(p, "\x0D" "ACPI0C50", 10))
        return 10;
    return 0;
}

static void add_addr(int *addrs, int *n, int a)
{
    for (int i = 0; i < *n; i++)
        if (addrs[i] == a)
            return;
    if (*n < MAX_ADDRS)
        addrs[(*n)++] = a;
}

/*
 * The I2C addresses of the HID-over-I2C devices in one AML table; *found
 * is set when there is such a device at all (its address may be patched in
 * at run time, and show as 0 here).
 */
static void scan_aml(const uint8_t *aml, uint32_t len, int *addrs, int *n, bool *found)
{
    const uint8_t *end = aml + len;
    for (const uint8_t *m = aml; m < end; m++) {
        int ml = hid_marker(m, end);
        if (!ml)
            continue;
        /* the innermost Device () around the marker */
        const uint8_t *body = NULL, *bend = NULL;
        for (const uint8_t *p = m - 2; p >= aml && m - p < 0x20000; p--) {
            if (p[0] != 0x5B || p[1] != 0x82)
                continue;
            uint32_t pl;
            int k = pkg_length(p + 2, end, &pl);
            if (!k || p + 2 + pl < m || p + 2 + pl > end || p + 2 + k + 4 > end)
                continue;
            const uint8_t *name = p + 2 + k;
            if (!name_char(name[0]) || !name_char(name[1]) || !name_char(name[2]) || !name_char(name[3]))
                continue;
            body = name + 4;
            bend = p + 2 + pl;
            if (debug)
                kprintf("i2c-hid: ACPI device %.4s is HID over I2C\n", (const char *)name);
            break;
        }
        if (!body)
            continue;
        *found = true;
        for (const uint8_t *r = body; r + 18 <= bend; r++) {   /* I2cSerialBus descriptors */
            if (r[0] != 0x8E || r[5] != 1)
                continue;
            uint16_t dlen = r[1] | r[2] << 8, tlen = r[10] | r[11] << 8;
            if (dlen < 15 || tlen < 6 || r + 3 + dlen > bend || (r[7] & 1))
                continue;                        /* (10-bit addresses: not used) */
            int a = r[16] | r[17] << 8;
            if (a > 0 && a < 0x78)
                add_addr(addrs, n, a);
        }
        m += ml - 1;
    }
}

/* ---------------------------------------------------------------- HID over I2C */

static int reg_read(struct i2c_hid *d, uint16_t reg, uint8_t *buf, int n)
{
    uint8_t w[2] = { reg & 0xFF, reg >> 8 };
    return dw_xfer(d->c, d->addr, w, 2, buf, n);
}

static int command(struct i2c_hid *d, uint8_t low, uint8_t opcode)
{
    uint8_t w[4] = { d->cmd_reg & 0xFF, d->cmd_reg >> 8, low, opcode };
    return dw_xfer(d->c, d->addr, w, 4, NULL, 0);
}

/* A HID device at addr on c: its descriptor register, found by trying the usual ones. */
static bool hid_probe(struct i2c_hid *d)
{
    static const uint16_t regs[] = { 0x0001, 0x0020, 0x0000, 0x0030 };
    uint8_t b[30];
    for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
        memset(b, 0, sizeof(b));
        if (reg_read(d, regs[i], b, 30) < 0)
            continue;
        uint16_t dlen = b[0] | b[1] << 8, ver = b[2] | b[3] << 8;
        if (dlen != 30 || ver != 0x0100)
            continue;
        d->desc_reg = regs[i];
        d->report_len = b[4] | b[5] << 8;
        d->report_reg = b[6] | b[7] << 8;
        d->input_reg = b[8] | b[9] << 8;
        d->max_input = b[10] | b[11] << 8;
        d->cmd_reg = b[16] | b[17] << 8;
        d->vendor = b[20] | b[21] << 8;
        d->product = b[22] | b[23] << 8;
        return d->report_len > 0 && d->report_len <= 4096 && d->max_input >= 2;
    }
    return false;
}

static bool hid_start(struct i2c_hid *d)
{
    static uint8_t rdesc[4096];
    command(d, 0x00, 0x08);                      /* SET_POWER ON */
    udelay(1000);
    command(d, 0x00, 0x01);                      /* RESET */
    for (int t = 0; t < 100; t++) {              /* the device answers with a zero-length report */
        uint8_t b[2] = { 0xFF, 0xFF };
        udelay(10000);
        if (dw_xfer(d->c, d->addr, NULL, 0, b, 2) == 0 && b[0] == 0 && b[1] == 0)
            break;
    }
    if (reg_read(d, d->report_reg, rdesc, d->report_len) < 0)
        return false;
    snprintf(d->hid.name, sizeof(d->hid.name), "i2c %d-%02x", d->c->index, d->addr);
    d->hid.debug = debug;
    if (hid_parse(&d->hid, rdesc, d->report_len) < 0) {
        kprintf("i2c-hid %d-%02x: %04x:%04x, no keyboard or pointer\n", d->c->index, d->addr, d->vendor,
                d->product);
        return false;
    }
    int fifo = d->c->rx_depth < d->c->tx_depth ? d->c->rx_depth : d->c->tx_depth;
    int n = d->max_input > sizeof(d->buf) ? (int)sizeof(d->buf) : d->max_input;
    d->max_input = n > fifo ? fifo : n;          /* (a longer report is cut: the mouse ones are short) */
    kprintf("i2c-hid %d-%02x: %04x:%04x %s\n", d->c->index, d->addr, d->vendor, d->product,
            d->hid.has_kbd && d->hid.has_mouse ? "keyboard and pointer" : d->hid.has_kbd ? "keyboard" : "pointer");
    return true;
}

void i2c_hid_init(const char *cmdline)
{
    if (boot_option(cmdline, "noi2c"))
        return;
    debug = boot_option(cmdline, "i2cdebug") || boot_option(cmdline, "usbdebug");
    for (int i = 0; i < pci_count() && nctrl < MAX_CTRL; i++) {
        const struct pci_dev *pd = pci_at(i);
        if (pd->vendor != 0x8086)
            continue;
        bool known = false;
        for (size_t k = 0; k < ARRAY_SIZE(lpss_i2c_ids); k++)
            known |= pd->device == lpss_i2c_ids[k];
        if (!known)
            continue;
        struct i2c_ctrl *c = &ctrls[nctrl];
        c->pci = *pd;
        c->index = nctrl;
        if (!dw_init(c)) {
            kprintf("i2c: %04x:%04x at %02x:%02x.%x did not start\n", pd->vendor, pd->device, pd->bus, pd->dev,
                    pd->func);
            continue;
        }
        pci_claim(pd, "i2c-designware");
        if (debug)
            kprintf("i2c %d: %04x:%04x, FIFOs %d/%d\n", nctrl, pd->vendor, pd->device, c->tx_depth, c->rx_depth);
        nctrl++;
    }
    if (!nctrl)
        return;

    int addrs[MAX_ADDRS], n = 0;
    bool found = false;
    uint32_t len;
    const uint8_t *t = acpi_table("DSDT", 0, &len);
    if (t)
        scan_aml(t + 36, len - 36, addrs, &n, &found);
    for (int k = 0; (t = acpi_table("SSDT", k, &len)); k++)
        scan_aml(t + 36, len - 36, addrs, &n, &found);
    if (found) {                                 /* (addresses set at run time): the usual touchpad ones too */
        add_addr(addrs, &n, 0x15);
        add_addr(addrs, &n, 0x2C);
        add_addr(addrs, &n, 0x2A);
    }
    if (debug || !found)
        kprintf("i2c-hid: %s in the ACPI tables, %d address%s to try\n", found ? "devices" : "no device", n,
                n == 1 ? "" : "es");

    for (int a = 0; a < n && ndevs < MAX_DEVS; a++)
        for (int k = 0; k < nctrl && ndevs < MAX_DEVS; k++) {
            uint8_t b;
            if (dw_xfer(&ctrls[k], addrs[a], NULL, 0, &b, 1) < 0)
                continue;                        /* nobody there */
            struct i2c_hid *d = &devs[ndevs];
            memset(d, 0, sizeof(*d));
            d->c = &ctrls[k];
            d->addr = addrs[a];
            if (!hid_probe(d)) {
                if (debug)
                    kprintf("i2c %d-%02x: answers, no HID descriptor\n", k, addrs[a]);
                continue;
            }
            if (hid_start(d))
                ndevs++;
            break;
        }
    ready = true;
}

bool i2c_hid_summary(char *buf, size_t n)
{
    if (!ndevs)
        return false;
    int kbd = 0, ptr = 0;
    for (int i = 0; i < ndevs; i++) {
        kbd += devs[i].hid.has_kbd;
        ptr += devs[i].hid.has_mouse;
    }
    snprintf(buf, n, "I2C HID: %d device%s (%d keyboard%s, %d pointer%s)", ndevs, ndevs > 1 ? "s" : "", kbd,
             kbd == 1 ? "" : "s", ptr, ptr == 1 ? "" : "s");
    return true;
}

/* From the timer tick: collect the read started last time, start the next. */
void i2c_hid_poll(void)
{
    if (!ready || !ndevs)
        return;
    for (int k = 0; k < nctrl; k++) {
        struct i2c_ctrl *c = &ctrls[k];
        if (c->pending) {
            struct i2c_hid *d = c->pending;
            int r = dw_read_done(c, d->buf, c->pending_len);
            if (r == 0 && hrtime() - c->started < 50UL * 1000000)
                continue;                        /* still on the bus */
            c->pending = NULL;
            if (r > 0) {
                int len = d->buf[0] | d->buf[1] << 8;
                if (len > 2 && len <= c->pending_len && len != 0xFFFF)
                    hid_input(&d->hid, d->buf + 2, len - 2);
            } else if (r == 0) {
                dw_enable(c, false);             /* stuck: start over */
                c->tar = -1;
            }
        }
        for (int i = 0; i < ndevs; i++) {
            struct i2c_hid *d = &devs[(c->next + i) % ndevs];
            if (d->c != c)
                continue;
            c->next = (d - devs + 1) % ndevs;
            if (dw_read_start(c, d->addr, d->max_input)) {
                c->pending = d;
                c->pending_len = d->max_input;
                c->started = hrtime();
            }
            break;
        }
    }
    for (int i = 0; i < ndevs; i++)
        hid_tick(&devs[i].hid);
}
