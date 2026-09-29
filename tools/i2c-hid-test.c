/*
 * i2c-hid-test.c - host test of the HID-over-I2C driver (kernel/i2c_hid.c)
 * on a simulated DesignWare controller (FIFOs, aborts, stop detection)
 * with a simulated ELAN-like touchpad at 0x15, a device at 0x2A that is not
 * HID, and nobody at 0x2C; the ACPI side is a DSDT with the touchpad's
 * Device () (_HID ELAN0129, _CID PNP0C50, an I2cSerialBusV2 in _CRS).
 * Checks the discovery, the start-up (SET_POWER, RESET, the report
 * descriptor read beyond the FIFO depth) and the polled input reads.
 */
#define I2C_HOST_TEST
#include "i2c_host_test.h"
#include "../kernel/i2c_hid.c"
#include "../kernel/hid.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---------------------------------------------------------------- the kernel around it */

static uint64_t clock_ns;
uint64_t hrtime(void) { return clock_ns += 1000; }
static struct pci_dev fake = { 0, 0x15, 0, 0x8086, 0x7A4C, 0x0C, 0x80, 0, 0, 0, { 0x1000 } };
int pci_count(void) { return 1; }
const struct pci_dev *pci_at(int i) { (void)i; return &fake; }
uint8_t pci_find_cap(const struct pci_dev *d, uint8_t id, uint8_t after) { (void)d; (void)id; (void)after; return 0; }
uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t o) { (void)b; (void)d; (void)f; (void)o; return 0; }
void pci_write32(uint8_t b, uint8_t d, uint8_t f, uint8_t o, uint32_t v) { (void)b; (void)d; (void)f; (void)o; (void)v; }
uint64_t pci_bar_addr(const struct pci_dev *d, int i, bool *io) { (void)d; (void)i; *io = false; return 0xFE000000; }
void pci_claim(const struct pci_dev *d, const char *driver) { (void)d; (void)driver; }
static uint8_t mmio_dummy[4096];
void *mmio_map(uint64_t pa, size_t size) { (void)pa; (void)size; return mmio_dummy; }
bool boot_option(const char *cmdline, const char *word) { return strstr(cmdline, word) != NULL; }

static int nkeys, mdx, mdy, mbtn, nmouse;
void kbd_key(uint16_t code, bool release) { (void)code; (void)release; nkeys++; }
void input_mouse(int dx, int dy, unsigned b) { mdx = dx; mdy = dy; mbtn = b; nmouse++; }
void input_mouse_abs(int x, int y, unsigned b) { (void)x; (void)y; (void)b; nmouse++; }

/* the DSDT: header + Device (TPD0) */
static uint8_t dsdt[512];
static uint32_t dsdt_len;
const void *acpi_table(const char *sig, int n, uint32_t *len)
{
    if (strcmp(sig, "DSDT") || n)
        return NULL;
    *len = dsdt_len;
    return dsdt;
}

static uint8_t *pkg(uint8_t *p, uint32_t len)   /* a 2-byte PkgLength (len counts itself) */
{
    len += 2;
    p[0] = 0x40 | (len & 0xF);
    p[1] = len >> 4;
    return p + 2;
}

static void make_dsdt(void)
{
    static const uint8_t res[] = {
        0x8E, 30, 0, 2, 0, 1, 0x02, 0, 0, 1, 6, 0, 0x80, 0x1A, 0x06, 0x00, 0x15, 0x00,
        '\\', '_', 'S', 'B', '.', 'P', 'C', '0', '0', '.', 'I', '2', 'C', '0', 0,
        0x79, 0x00,
    };
    uint8_t body[256], *b = body;
    memcpy(b, "\x08_HID\x0D" "ELAN0129", 14), b += 14, *b++ = 0;
    memcpy(b, "\x08_CID\x0C\x41\xD0\x0C\x50", 10), b += 10;
    uint8_t crs[128], *c = crs;                  /* Method (_CRS) { Return (Buffer () { res }) } */
    uint8_t buf[80], *u = buf;
    *u++ = 0x0A, *u++ = sizeof(res);             /* BytePrefix size */
    memcpy(u, res, sizeof(res)), u += sizeof(res);
    memcpy(c, "_CRS", 4), c += 4, *c++ = 0, *c++ = 0xA4, *c++ = 0x11;
    c = pkg(c, u - buf);
    memcpy(c, buf, u - buf), c += u - buf;
    *b++ = 0x14;
    b = pkg(b, c - crs);
    memcpy(b, crs, c - crs), b += c - crs;
    uint8_t *p = dsdt + 36;
    memcpy(p, "\x10\x0A\\_SB_", 7), p += 7;       /* (a Scope-like prefix: skipped by the scan) */
    *p++ = 0x5B, *p++ = 0x82;
    p = pkg(p, 4 + (b - body));
    memcpy(p, "TPD0", 4), p += 4;
    memcpy(p, body, b - body), p += b - body;
    dsdt_len = p - dsdt;
    memcpy(dsdt, "DSDT", 4);
    memcpy(dsdt + 4, &dsdt_len, 4);
}

/* ---------------------------------------------------------------- the controller and the devices */

static const uint8_t rdesc[] = {                /* the touchpad's mouse collection (ID 1), then its touchpad one */
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x95, 0x06, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xC0, 0xC0,
    0x05, 0x0D, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x03, 0x09, 0x22, 0xA1, 0x02,
    0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x26, 0xFF, 0x0F, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02, 0xC0, 0xC0,
    0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x0E, 0x09, 0xC5, 0x15, 0x00, 0x26, 0xFF, 0x00,
    0x75, 0x08, 0x96, 0x00, 0x01, 0xB1, 0x02, 0xC0,     /* (a vendor feature: makes it longer than the FIFO) */
};
static uint8_t hdesc[30];

static uint32_t en, tar, raw, tar_while_on;
static uint8_t rxq[256];
static int rxh, rxt;
static bool in_xfer, aborted, reading;
static uint8_t wbuf[16];
static int wn, rpos;
static const uint8_t *src;
static int srclen;
static uint8_t inrep[64];
static bool powered, reset_pending, reset_seen;
static int queued_len;
static uint8_t queued[16];

static bool present(int a) { return a == 0x15 || a == 0x2A; }

static void begin_read(void)
{
    static uint8_t ff[64];
    reading = true;
    rpos = 0;
    int reg = wn >= 2 ? wbuf[0] | wbuf[1] << 8 : -1;
    if (tar == 0x2A) {
        memset(ff, 0xFF, sizeof(ff));
        src = ff, srclen = sizeof(ff);
    } else if (reg == 0x0001) {
        src = hdesc, srclen = 30;
    } else if (reg == 0x0002) {
        src = rdesc, srclen = sizeof(rdesc);
    } else if (reg >= 0) {
        src = NULL, srclen = 0;                  /* (an unknown register: zeros) */
    } else {                                     /* the input register: a plain read */
        memset(inrep, 0, sizeof(inrep));
        if (reset_pending) {
            reset_pending = false;
        } else if (queued_len) {
            inrep[0] = queued_len + 2;
            memcpy(inrep + 2, queued, queued_len);
            queued_len = 0;
        }
        src = inrep, srclen = sizeof(inrep);
    }
}

static void end_xfer(void)
{
    if (!aborted && !reading && tar == 0x15 && wn == 4 && wbuf[0] == 0x05 && wbuf[1] == 0) {
        if (wbuf[3] == 0x08 && wbuf[2] == 0)
            powered = true;
        if (wbuf[3] == 0x01)
            reset_pending = reset_seen = true;
    }
    in_xfer = aborted = reading = false;
    wn = 0;
    raw |= INTR_STOP;
}

void sim_wr(uint32_t reg, uint32_t v)
{
    switch (reg) {
    case IC_ENABLE: en = v & 1; break;
    case IC_TAR: tar = v; tar_while_on += en; break;
    case IC_DATA_CMD:
        if (!en)
            break;
        if (!in_xfer) {
            in_xfer = true;
            if (!present(tar)) {
                aborted = true;
                raw |= INTR_TX_ABRT;
            }
        }
        if (!aborted) {
            if (v & CMD_READ) {
                if (!reading)
                    begin_read();
                rxq[rxt++ % 256] = src && rpos < srclen ? src[rpos] : 0;
                rpos++;
            } else if (wn < 16) {
                wbuf[wn++] = v & 0xFF;
            }
        }
        if (v & CMD_STOP)
            end_xfer();
        break;
    }
}

uint32_t sim_rd(uint32_t reg)
{
    switch (reg) {
    case IC_COMP_TYPE: return DW_COMP_TYPE;
    case IC_COMP_PARAM: return 63U << 8 | 63U << 16;
    case IC_ENABLE_STS: return en;
    case IC_TXFLR: return 0;
    case IC_RXFLR: return rxt - rxh;
    case IC_DATA_CMD: return rxh < rxt ? rxq[rxh++ % 256] : 0;
    case IC_RAW_INTR: return raw;
    case IC_CLR_TX_ABRT: raw &= ~INTR_TX_ABRT; return 0;
    case IC_CLR_STOP: raw &= ~INTR_STOP; return 0;
    case IC_CLR_INTR: raw = 0; return 0;
    }
    return 0;
}

int main(void)
{
    make_dsdt();
    uint8_t h[30] = { 30, 0, 0x00, 0x01, sizeof(rdesc), 0, 0x02, 0, 0x03, 0, 16, 0, 0x04, 0, 0, 0,
                      0x05, 0, 0x06, 0, 0xF3, 0x04, 0xF6, 0x32, 1, 0 };
    memcpy(hdesc, h, sizeof(h));
    CHECK(sizeof(rdesc) > 64);                   /* longer than the FIFOs */

    i2c_hid_init("i2cdebug");
    CHECK(nctrl == 1 && ndevs == 1);
    CHECK(devs[0].addr == 0x15 && devs[0].desc_reg == 1 && devs[0].vendor == 0x04F3 && devs[0].product == 0x32F6);
    CHECK(powered && reset_seen && !tar_while_on);
    CHECK(devs[0].hid.has_mouse && devs[0].hid.ids && devs[0].max_input == 16);
    char msg[80];
    CHECK(i2c_hid_summary(msg, sizeof(msg)));
    printf("%s\n", msg);

    /* nothing to report: zero lengths, no events */
    for (int i = 0; i < 5; i++)
        i2c_hid_poll();
    CHECK(nmouse == 0);
    /* a mouse report: button 1, 5 right, 3 up */
    const uint8_t r[] = { 0x01, 0x01, 0x05, 0xFD };
    memcpy(queued, r, sizeof(r));
    queued_len = sizeof(r);
    i2c_hid_poll();
    i2c_hid_poll();
    CHECK(nmouse == 1 && mdx == 5 && mdy == -3 && mbtn == 1);
    for (int i = 0; i < 5; i++)
        i2c_hid_poll();
    CHECK(nmouse == 1);                          /* (read once) */
    /* a touchpad-mode report (ID 3) is ignored */
    const uint8_t t[] = { 0x03, 0x01, 0x34, 0x12 };
    memcpy(queued, t, sizeof(t));
    queued_len = sizeof(t);
    i2c_hid_poll();
    i2c_hid_poll();
    CHECK(nmouse == 1);

    printf("i2c-hid-test: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
