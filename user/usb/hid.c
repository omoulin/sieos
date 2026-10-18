/*
 * hid.c - USB keyboards, mice and tablets ("human interface devices").
 * Their input arrives on an interrupt endpoint as small "reports"; we keep
 * one transfer queued on it, and each completion (an async event, on the
 * interrupt thread) is turned into what the console server (con) knows:
 * PS/2 set-1 scancodes for keys, moves and buttons for pointers (CON_INJECT).
 *
 * Keyboards use the "boot protocol": 8 bytes, the modifier keys as bits,
 * then up to 6 keys held. Mice, tablets, touchpads, and keyboards that
 * are more than that on one interface (a Surface Type Cover: keyboard,
 * mouse, volume keys and a precision touchpad, switched to its fingers'
 * mode) are read with their own "report descriptor", which says where the
 * buttons, X, Y, the wheel and the fingers are in a report. Both in
 * user/lib/hidrep.c (shared with I2C and Bluetooth devices).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "usb.h"
#include "hidrep.h"

typedef struct {
    usbdev_t *d;
    int dci, len, kbd;
    uint8_t *buf, prev[8];
    uint64_t bus;
    hidrep_t r;                              /* a pointer's report layout (user/lib/hidrep.c) */
} hid_t;
static int nhid;

/* A report arrived: use it, and queue the next transfer. */
static void hid_async(usbdev_t *d, int dci, int code, uint32_t left)
{
    for (int k = 0; k < 4; k++) {
        hid_t *h = d->hid[k];
        if (!h || h->dci != dci) continue;
        if (code == CC_SUCCESS || code == CC_SHORT) {
            dsync(h->buf, h->len);
            int got = h->len - (int)left;
            if (h->kbd) { if (got >= 8) hid_keys(h->prev, h->buf); }
            else hid_input(&h->r, h->buf, got);
        } else if (code == CC_STALL) return;                 /* (left alone: rare) */
        xfer_async(d, dci, h->bus, h->len);
    }
}

/* ---- A HID interface: a keyboard (boot protocol), or what its report
 * descriptor says: pointer, touchpad, keyboard, volume keys. */
int hid_attach(usbdev_t *d, const uint8_t *f, int len)
{
    int slot = 0;
    while (slot < 4 && d->hid[slot]) slot++;
    const uint8_t *ep = 0;
    int rlen = 0;
    for (int i = f[0]; i + 1 < len; i += f[i]) {
        if (f[i + 1] == 5 && (f[i + 2] & 0x80) && (f[i + 3] & 3) == 3 && !ep) ep = f + i;
        if (f[i + 1] == 0x21) rlen = f[i + 7] | f[i + 8] << 8;    /* the HID descriptor: report descriptor size */
    }
    if (slot == 4 || !ep || configure(d, &ep, 1)) return -1;
    hid_t *h = malloc(sizeof *h);
    if (!h) return -1;
    memset(h, 0, sizeof *h);
    h->d = d;
    h->dci = dci_of(ep[2]);
    h->len = (ep[4] | ep[5] << 8) & 0x7FF;
    if (h->len > 64) h->len = 64;
    if (!(h->buf = dmem(64, &h->bus))) { free(h); return -1; }
    static uint8_t rd[2048];                                     /* (a Type Cover's: 1.1 KB) */
    hidrep_t *r = &h->r;
    if (rlen > (int)sizeof rd) rlen = sizeof rd;
    if (rlen && control(d, 0x81, 6, 0x2200, f[2], rd, rlen) > 0) hid_parse(r, rd, rlen);
    /* A boot keyboard alone keeps the boot protocol; one that is also a
     * pointer or a touchpad (a Surface Type Cover: keyboard, mouse,
     * consumer keys and touchpad on one interface) is read in the report
     * protocol, all its reports through its descriptor. */
    h->kbd = f[6] == 1 && f[7] == 1 && !r->xsz && !r->trid && !r->csz;
    if (h->kbd) control(d, 0x21, 0x0B, 0, f[2], 0, 0);           /* SET_PROTOCOL(boot) */
    else if (f[6] == 1) control(d, 0x21, 0x0B, 1, f[2], 0, 0);   /* SET_PROTOCOL(report) */
    if (h->kbd || r->kn) control(d, 0x21, 0x0A, 0, f[2], 0, 0);  /* SET_IDLE(0): report changes only */
    if (!h->kbd && !r->xsz && !r->trid && !r->kn && !r->csz && f[6] == 1 && f[7] == 2) {   /* no layout found: a boot mouse */
        control(d, 0x21, 0x0B, 0, f[2], 0, 0);
        *r = (hidrep_t){ .bn = 3, .xpos = 8, .xsz = 8, .ypos = 16, .ysz = 8 };
    }
    if (r->trid) {                     /* a precision touchpad: its fingers (input mode 3), all of them and its button */
        uint8_t m[2] = { (uint8_t)r->mrid, 3 }, s[2] = { (uint8_t)r->srid, 3 };
        if (r->mrid < 1 || r->msz > 8 || control(d, 0x21, 0x09, 0x300 | r->mrid, f[2], m, 2) < 0) r->trid = 0;
        else if (r->srid) control(d, 0x21, 0x09, 0x300 | r->srid, f[2], s, 2);
    }
    if (!h->kbd && !r->xsz && !r->trid && !r->kn && !r->csz) { free(h); return -1; }
    log_line("usb: %04x:%04x interface %d: %s%s%s%s%s\n", d->vendor, d->product, f[2], h->kbd ? "boot keyboard" : r->kn ? "keyboard, " : "",
             r->trid ? "touchpad (fingers), " : r->mrid ? "touchpad (as a mouse), " : "", r->screen ? "touch screen, " : r->xsz ? (r->absolute ? "tablet, " : "mouse, ") : "",
             r->csz ? "volume keys, " : "", h->kbd ? "" : "its reports");
    int w, hh, ok = hid_pad_size(r, &w, &hh);
    if (r->trid) log_line("usb: %04x:%04x touchpad: " HID_PAD_FMT "\n", d->vendor, d->product, HID_PAD_ARGS(r, w, hh, ok));
    d->hid[slot] = h;
    d->async[h->dci] = hid_async;
    if (!nhid++) hid_send(0, 0);                                 /* con: a keyboard or pointer is here */
    xfer_async(d, h->dci, h->bus, h->len);
    return 0;
}

void hid_detach(usbdev_t *d)
{
    static const uint8_t none[8];
    for (int k = 0; k < 4; k++) {                                /* its keys let go */
        hid_t *h = d->hid[k];
        if (h && h->kbd) hid_keys(h->prev, none);
        else if (h && h->r.kn) hid_keys(h->r.kprev, none);
    }
}
int hid_count(void) { return nhid; }
