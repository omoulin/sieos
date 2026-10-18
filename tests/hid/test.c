/*
 * tests/hid/test.c - hid-test: the USB server's HID driver (user/usb/hid.c)
 * and the report reader (user/lib/hidrep.c) on the host, as a Surface Pro
 * 7's Type Cover (045e:09c0) meets them: one interface, a boot keyboard by
 * its class, whose report descriptor (typecover.rd) also has a mouse,
 * consumer (volume) keys, a precision touchpad and Microsoft's own
 * collections. typecover.rd is the Surface Book 2's keyboard base, from
 * hid-tools' tests (same Microsoft keyboard firmware family; the Pro 7
 * cover's own descriptor was not available). Checked: the report protocol
 * chosen (not boot), the touchpad's input mode (3) and switches set by
 * SET_REPORT, then keys, a volume key, the mouse, finger moves, a tap.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include "usb.h"
#include "hidrep.h"

static int fails, checks;
static void check(const char *what, int ok) { checks++; if (!ok) fails++; printf("%s %s\n", ok ? "  ok" : "  FAILED", what); }

/* ---- What hid.c and hidrep.c call. */
static con_inject_t inj[64];
static int ninj, audio_key[8], naudio, protocol = -1, nset;
static uint8_t setrep[4][4];
static uint16_t setval[4];
static int64_t now;
static uint8_t rd[2048];
static int rdn;
long call_named(long *port, const char *name, msg_t *m, int repeat)
{
    (void)port; (void)repeat;
    if (!strcmp(name, "console")) for (size_t i = 0; i < m->slen / sizeof(con_inject_t) && ninj < 64; i++) inj[ninj++] = ((const con_inject_t *)m->sbuf)[i];
    if (!strcmp(name, "audio") && m->w[0] == AUDIO_KEY && naudio < 8) audio_key[naudio++] = (int)m->w[1];
    return 0;
}
long thread_start(void (*fn)(void *), void *arg, size_t stack) { (void)fn; (void)arg; (void)stack; return 99; }
long sys_clock(void) { return now; }
long sys_sleep(uint64_t ns) { (void)ns; return 0; }
long sys_wake(int tid) { (void)tid; return 0; }
long sys_yield(void) { return 0; }
void log_line(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
int control(usbdev_t *d, int type, int req, int value, int index, void *data, int len)
{
    (void)d; (void)index;
    if (type == 0x81 && req == 6 && value == 0x2200) { int n = len < rdn ? len : rdn; memcpy(data, rd, n); return n; }
    if (type == 0x21 && req == 0x0B) protocol = value;
    if (type == 0x21 && req == 0x09 && nset < 4) { setval[nset] = (uint16_t)value; memcpy(setrep[nset++], data, len < 4 ? len : 4); }
    return len;
}
int configure(usbdev_t *d, const uint8_t **eps, int neps) { (void)d; (void)eps; (void)neps; return 0; }
int dci_of(int ep_addr) { return (ep_addr & 15) * 2 + !!(ep_addr & 0x80); }
void *dmem(size_t n, uint64_t *bus) { *bus = 0x1000; return calloc(1, n); }
void dsync(volatile void *p, size_t n) { (void)p; (void)n; }
int xfer_async(usbdev_t *d, int dci, uint64_t bus, uint32_t len) { (void)d; (void)dci; (void)bus; (void)len; return 0; }

#include "hid.c"

/* A report from the interrupt endpoint (its transfer completes). */
static usbdev_t dev;
static void report(const uint8_t *r, int n)
{
    hid_t *h = dev.hid[0];
    memset(h->buf, 0, h->len);
    memcpy(h->buf, r, n);
    hid_async(&dev, h->dci, CC_SHORT, (uint32_t)(h->len - n));
}
/* The touchpad's report 4: finger 0 (confidence, tip) at x, y; the pad's button. */
static void finger(int down, int x, int y, int button)
{
    uint8_t r[30] = { 4, (uint8_t)(down ? 3 : 0), (uint8_t)x, (uint8_t)(x >> 8), (uint8_t)y, (uint8_t)(y >> 8) };
    r[28] = (uint8_t)!!down;                                       /* contact count */
    r[29] = (uint8_t)button;
    report(r, 30);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "tests/hid";
    char p[512];
    snprintf(p, sizeof p, "%s/typecover.rd", dir);
    FILE *f = fopen(p, "rb");
    if (!f) { printf("hid-test: cannot read %s\n", p); return 1; }
    rdn = (int)fread(rd, 1, sizeof rd, f);
    fclose(f);

    /* the interface as the Type Cover's configuration gives it: class 3, boot subclass, keyboard protocol */
    const uint8_t iface[] = { 9, 4, 0, 0, 1, 3, 1, 1, 0,  9, 0x21, 0x11, 0x01, 0, 1, 0x22, (uint8_t)rdn, (uint8_t)(rdn >> 8),  7, 5, 0x81, 3, 64, 0, 1 };
    dev.vendor = 0x045e; dev.product = 0x09c0;
    printf("hid-test: a Surface Type Cover's interface (%d-byte report descriptor)\n", rdn);
    check("attached", !hid_attach(&dev, iface, sizeof iface) && dev.hid[0]);
    hid_t *h = dev.hid[0];
    hidrep_t *r = &h->r;
    check("read through its report descriptor, not the boot protocol", h && !h->kbd && protocol == 1);
    check("keyboard report 1, 10 keys; mouse report 2, 5 buttons; volume keys report 3", r->krid == 1 && r->kn == 10 && r->rid == 2 && r->bn == 5 && !r->absolute && r->crid == 3 && r->csz == 16);
    check("touchpad report 4, 5 fingers, X up to 2020; input mode report 7, switches report 8", r->trid == 4 && r->nf == 5 && r->txmax == 2020 && r->mrid == 7 && r->srid == 8);
    check("SET_REPORT feature 7 = 3 (fingers), then feature 8 = 3 (surface and button)", nset == 2 && setval[0] == 0x307 && setrep[0][0] == 7 && setrep[0][1] == 3 && setval[1] == 0x308 && setrep[1][1] == 3);

    ninj = 0;
    report((const uint8_t[]){ 1, 0x02, 0x04 }, 3);                 /* left shift + a */
    report((const uint8_t[]){ 1, 0x00, 0x00 }, 3);
    check("keys: shift, a pressed, both released", ninj == 4 && inj[0].sc == 0x2A && inj[1].sc == 0x1E && inj[2].sc == 0xAA && inj[3].sc == 0x9E);
    ninj = 0;
    report((const uint8_t[]){ 3, 0xE9, 0x00 }, 3);                 /* volume up, held, then let go */
    report((const uint8_t[]){ 3, 0xE9, 0x00 }, 3);
    report((const uint8_t[]){ 3, 0x00, 0x00 }, 3);
    report((const uint8_t[]){ 3, 0xEA, 0x00 }, 3);
    check("volume up, down: to the audio server, once each; nothing to the console", naudio == 2 && audio_key[0] == MK_VOLUP && audio_key[1] == MK_VOLDOWN && !ninj);
    report((const uint8_t[]){ 2, 0x01, 5, (uint8_t)-3 }, 4);       /* the cover's mouse report (before the mode is set) */
    check("mouse: left button, moved 5, -3", ninj == 1 && inj[0].kind == INJ_REL && inj[0].buttons == MB_LEFT && inj[0].dx == 5 && inj[0].dy == -3);

    ninj = 0; now = 1000000000;
    finger(1, 1000, 500, 0);
    finger(1, 1101, 500, 0); now += 20000000;
    finger(1, 1202, 540, 0); now += 20000000;
    finger(0, 0, 0, 0); now += 300000000;
    int dx = 0, dy = 0, down = 0;
    for (int i = 0; i < ninj; i++) dx += inj[i].dx, dy += inj[i].dy, down |= inj[i].buttons;
    check("one finger moved right and down: the pointer moved so (relative), no click", ninj == 2 && dx > 200 && dy > 0 && !down);
    check("the pad's size from its descriptor: 101 x 66 mm (X 0-2020, Y 0-1321)", r->tw == 101000 && r->th / 1000 == 66 && r->tymax == 1321);
    /* acceleration: 1 cm slowly (1 mm per 40 ms: 25 mm/s), then 5 cm quickly (1 cm per 20 ms: 500 mm/s) */
    int slow = 0, fast = 0;
    ninj = 0; now += 1000000000;
    for (int i = 0; i <= 10; i++, now += 40000000) finger(1, 100 + 20 * i, 500, 0);
    for (int i = 0; i < ninj; i++) slow += inj[i].dx;
    finger(0, 0, 0, 0); now += 1000000000;
    ninj = 0;
    for (int i = 0; i <= 5; i++, now += 20000000) finger(1, 100 + 202 * i, 500, 0);
    for (int i = 0; i < ninj; i++) fast += inj[i].dx;
    finger(0, 0, 0, 0); now += 1000000000;
    printf("  (1 cm slowly: %d counts; 5 cm quickly: %d)\n", slow, fast);
    check("slow moves 8 counts a mm, quick ones 32 (accelerated): a 2736-pixel screen in one quick 9 cm stroke", slow >= 75 && slow <= 85 && fast >= 1550 && fast <= 1650);
    ninj = 0;
    finger(1, 600, 600, 0); now += 50000000;
    finger(0, 0, 0, 0);
    check("a short touch: a click (press, release)", ninj == 2 && inj[0].buttons == MB_LEFT && inj[1].buttons == 0);
    ninj = 0; now += 500000000;
    finger(1, 600, 600, 1);
    finger(1, 600, 600, 0);
    finger(0, 0, 0, 0);
    check("pressing the pad: the left button down, then up", ninj == 2 && inj[0].buttons == MB_LEFT && inj[1].buttons == 0);

    printf(fails ? "hid-test: %d of %d checks FAILED\n" : "hid-test: all %d checks passed\n", fails ? fails : checks, checks);
    return fails != 0;
}
