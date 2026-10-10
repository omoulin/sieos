/*
 * hid.c - USB keyboards, mice and tablets ("human interface devices").
 * Their input arrives on an interrupt endpoint as small "reports"; we keep
 * one transfer queued on it, and each completion (an async event, on the
 * interrupt thread) is turned into what the console server (con) knows:
 * PS/2 set-1 scancodes for keys, moves and buttons for pointers (CON_INJECT).
 *
 * Keyboards use the "boot protocol": 8 bytes, the modifier keys as bits,
 * then up to 6 keys held. We compare with the previous report to find the
 * presses and releases, and repeat a held key (USB keyboards do not).
 * Mice and tablets are read with their own "report descriptor", which
 * says where the buttons, X, Y and the wheel are in a report.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "usb.h"

typedef struct {
    usbdev_t *d;
    int dci, len, kbd, rid;                  /* rid: the report id carrying X/Y (0: none) */
    uint8_t *buf, prev[8];
    uint64_t bus;
    int bpos, bn, xpos, xsz, ypos, ysz, wpos, wsz, absolute;
    int32_t xmax, ymax;
} hid_t;
static int nhid;
static long con;

/* Usage (the key's USB number, 4..0x52) -> set-1 scancode; 0x100: an
 * extended one (0xE0 first). */
static const uint16_t usage_sc[0x53] = {
    [0x04] = 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,
    0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,                      /* a..z */
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,                      /* 1..0 */
    0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35,
    0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,   /* caps, F1..F12 */
    [0x4A] = 0x147, 0x149, 0x153, 0x14F, 0x151, 0x14D, 0x14B, 0x150, 0x148,           /* home pgup del end pgdn → ← ↓ ↑ */
};
/* The modifier bits: left ctrl, shift, alt, gui; right ctrl, shift, alt, gui. */
static const uint16_t mod_sc[8] = { 0x1D, 0x2A, 0x38, 0, 0x11D, 0x36, 0x138, 0 };

static void send(const con_inject_t *v, int n)
{
    msg_t m = { .w = { CON_INJECT }, .sbuf = v, .slen = n * sizeof *v };
    call_named(&con, "console", &m, 1);
}
static int key(con_inject_t *v, int n, int sc, int up)
{
    if (!sc) return n;
    if (sc & 0x100) v[n++] = (con_inject_t){ .kind = INJ_KEY, .sc = 0xE0 };
    v[n++] = (con_inject_t){ .kind = INJ_KEY, .sc = (uint8_t)((sc & 0x7F) | (up ? 0x80 : 0)) };
    return n;
}

/* ---- Key repeat: a key held 0.5 s repeats 30 times a second. */
static volatile int rep_usage;
static volatile int64_t rep_at;
static volatile long rep_tid;
static void repeat_thread(void *a)
{
    (void)a;
    while (!rep_tid) sys_yield();
    for (;;) {
        int u = rep_usage;
        if (!u) { sys_sleep(3600000000000L); continue; }
        int64_t wait = rep_at - sys_clock();
        if (wait > 0) { sys_sleep(wait); continue; }
        if (u != rep_usage) continue;
        con_inject_t v[2];
        send(v, key(v, 0, usage_sc[u], 0));
        rep_at = sys_clock() + 33000000;
    }
}

static int held(const uint8_t *r, int u) { for (int i = 2; i < 8; i++) if (r[i] == u) return 1; return 0; }

static void keyboard(hid_t *h, const uint8_t *r)
{
    con_inject_t v[40];
    int n = 0;
    if (r[2] == 1) return;                                   /* "too many keys": ignore this report */
    for (int b = 0; b < 8; b++)
        if ((r[0] ^ h->prev[0]) & 1 << b) n = key(v, n, mod_sc[b], !(r[0] & 1 << b));
    for (int i = 2; i < 8; i++)
        if (h->prev[i] && !held(r, h->prev[i]) && h->prev[i] < 0x53) {
            n = key(v, n, usage_sc[h->prev[i]], 1);
            if (rep_usage == h->prev[i]) rep_usage = 0;
        }
    for (int i = 2; i < 8; i++)
        if (r[i] && !held(h->prev, r[i]) && r[i] < 0x53 && usage_sc[r[i]]) {
            n = key(v, n, usage_sc[r[i]], 0);
            rep_usage = r[i];
            rep_at = sys_clock() + 500000000;
            if (rep_tid) sys_wake(rep_tid);
        }
    memcpy(h->prev, r, 8);
    if (n) send(v, n);
}

static int32_t bits(const uint8_t *r, int len, int pos, int sz, int sign)
{
    uint32_t v = 0;
    for (int i = 0; i < sz && i < 32; i++) {
        int b = pos + i;
        if (b / 8 < len && r[b / 8] >> (b % 8) & 1) v |= 1u << i;
    }
    if (sign && sz < 32 && v & 1u << (sz - 1)) v |= ~0u << sz;
    return (int32_t)v;
}

static void pointer(hid_t *h, const uint8_t *r, int len)
{
    if (h->rid) { if (r[0] != h->rid) return; r++; len--; }
    con_inject_t v = { .kind = h->absolute ? INJ_ABS : INJ_REL };
    int b = h->bn ? bits(r, len, h->bpos, h->bn > 3 ? 3 : h->bn, 0) : 0;
    v.buttons = (b & 1 ? MB_LEFT : 0) | (b & 2 ? MB_RIGHT : 0) | (b & 4 ? MB_MIDDLE : 0);
    int32_t x = bits(r, len, h->xpos, h->xsz, !h->absolute), y = bits(r, len, h->ypos, h->ysz, !h->absolute);
    if (h->absolute) {
        v.x = (uint16_t)((int64_t)(x < 0 ? 0 : x) * 65535 / (h->xmax > 0 ? h->xmax : 32767));
        v.y = (uint16_t)((int64_t)(y < 0 ? 0 : y) * 65535 / (h->ymax > 0 ? h->ymax : 32767));
    } else { v.dx = (int16_t)x; v.dy = (int16_t)y; }
    if (h->wsz) v.wheel = (int8_t)bits(r, len, h->wpos, h->wsz, 1);
    send(&v, 1);
}

/* A report arrived: use it, and queue the next transfer. */
static void hid_async(usbdev_t *d, int dci, int code, uint32_t left)
{
    for (int k = 0; k < 4; k++) {
        hid_t *h = d->hid[k];
        if (!h || h->dci != dci) continue;
        if (code == CC_SUCCESS || code == CC_SHORT) {
            dsync(h->buf, h->len);
            int got = h->len - (int)left;
            if (h->kbd) { if (got >= 8) keyboard(h, h->buf); }
            else pointer(h, h->buf, got);
        } else if (code == CC_STALL) return;                 /* (left alone: rare) */
        xfer_async(d, dci, h->bus, h->len);
    }
}

/* Where the buttons, X, Y and wheel are, from the report descriptor. */
static void parse(hid_t *h, const uint8_t *p, int n)
{
    int page = 0, size = 0, count = 0, rid = 0, pos = 0, umin = 0, umax = 0, nu = 0, usages[16];
    int32_t lmax = 0;
    for (int i = 0; i < n; ) {
        int b = p[i], sz = (b & 3) == 3 ? 4 : b & 3, type = b >> 2 & 3, tag = b >> 4;
        uint32_t v = 0;
        for (int k = 0; k < sz && i + 1 + k < n; k++) v |= (uint32_t)p[i + 1 + k] << 8 * k;
        i += 1 + sz;
        if (type == 1) {                                     /* global */
            if (tag == 0) page = v;
            else if (tag == 2) lmax = sz == 1 ? (int8_t)v : sz == 2 ? (int16_t)v : (int32_t)v;
            else if (tag == 7) size = v;
            else if (tag == 8) { rid = v; pos = 0; }
            else if (tag == 9) count = v;
        } else if (type == 2) {                              /* local */
            if (tag == 0 && nu < 16) usages[nu++] = v & 0xFFFF;
            else if (tag == 1) umin = v;
            else if (tag == 2) umax = v;
        } else if (type == 0) {                              /* main */
            if (tag == 8 && !(v & 1)) {                      /* an input, data (not padding) */
                for (int k = 0; k < count; k++) {
                    int u = k < nu ? usages[k] : nu ? usages[nu - 1] : umin + k;
                    int at = pos + k * size;
                    if (page == 9 && !h->bn) { h->bpos = at; h->bn = count; h->rid = rid; }
                    if (page == 1 && u == 0x30) { h->xpos = at; h->xsz = size; h->absolute = !(v & 4); h->xmax = lmax; h->rid = rid; }
                    if (page == 1 && u == 0x31) { h->ypos = at; h->ysz = size; h->ymax = lmax; }
                    if (page == 1 && u == 0x38) { h->wpos = at; h->wsz = size; }
                }
            }
            if (tag == 8) pos += size * count;
            nu = 0; umin = umax = 0;
        }
    }
    (void)umax;
}

/* ---- A HID interface: a keyboard (boot protocol), or a pointer. */
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
    h->kbd = f[6] == 1 && f[7] == 1;                             /* boot interface, keyboard */
    h->len = (ep[4] | ep[5] << 8) & 0x7FF;
    if (h->len > 64) h->len = 64;
    if (!(h->buf = dmem(64, &h->bus))) return -1;
    if (h->kbd) {
        control(d, 0x21, 0x0B, 0, f[2], 0, 0);                   /* SET_PROTOCOL(boot) */
        control(d, 0x21, 0x0A, 0, f[2], 0, 0);                   /* SET_IDLE(0): report changes only */
    } else {
        static uint8_t rd[1024];
        if (rlen > (int)sizeof rd) rlen = sizeof rd;
        if (rlen && control(d, 0x81, 6, 0x2200, f[2], rd, rlen) > 0) parse(h, rd, rlen);
        if (!h->xsz && f[6] == 1 && f[7] == 2) {                 /* no layout found: a boot mouse */
            control(d, 0x21, 0x0B, 0, f[2], 0, 0);
            h->bpos = 0; h->bn = 3; h->xpos = 8; h->xsz = 8; h->ypos = 16; h->ysz = 8; h->rid = 0;
        }
        if (!h->xsz) { free(h); return -1; }
    }
    d->hid[slot] = h;
    d->async[h->dci] = hid_async;
    if (!nhid++) {
        rep_tid = thread_start(repeat_thread, 0, 8192);
        send(0, 0);                                              /* con: a keyboard or pointer is here */
    }
    xfer_async(d, h->dci, h->bus, h->len);
    return 0;
}

void hid_detach(usbdev_t *d)
{
    if (d->hid[0]) rep_usage = 0;                                /* stop repeating its key */
}
int hid_count(void) { return nhid; }
