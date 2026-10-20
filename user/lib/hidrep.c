/*
 * hidrep.c - HID input reports, for USB and I2C mice, tablets and
 * touchpads: the report descriptor says where things are, each report
 * becomes CON_INJECT events (moves and buttons) for the console.
 *
 * A precision touchpad ("PTP", digitizer page, Windows' touchpads) reports
 * its fingers, in absolute positions, once it is told to (input mode 3;
 * until then it acts as a mouse). Here: one finger moves the pointer
 * (relative, like a mouse; by millimetres on the pad, from the descriptor's
 * physical size, faster the faster the finger goes), two fingers scroll, a short touch is a click
 * (two fingers: the right button), pressing the pad is a click (with two
 * fingers down: the right button). Contacts the pad thinks are a palm
 * (confidence 0) are left out.
 *
 * A touch screen (digitizer "touch screen", unless it describes a mouse
 * first: that one is used) is an absolute pointer: its first finger's
 * place, the left button while it touches.
 *
 * Keyboards: each report (the boot layout) is compared with the previous
 * one for the presses and releases (PS/2 set-1 scancodes); a key held 0.5 s
 * repeats (keyboards do not). A keyboard described by its report
 * descriptor is read into that layout.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "hidrep.h"

static long con;
void hid_send(const con_inject_t *v, int n)
{
    msg_t m = { .w = { CON_INJECT }, .sbuf = v, .slen = n * sizeof *v };
    call_named(&con, "console", &m, 1);
}

/* Usage (the key's USB number, 4..0x52) -> set-1 scancode; 0x100: an
 * extended one (0xE0 first). */
static const uint16_t usage_sc[0x53] = {
    [0x04] = 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,
    0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,                      /* a..z */
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,                      /* 1..0 */
    0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35,
    0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,   /* caps, F1..F12 */
    [0x49] = 0x152, 0x147, 0x149, 0x153, 0x14F, 0x151, 0x14D, 0x14B, 0x150, 0x148,    /* insert home pgup del end pgdn → ← ↓ ↑ */
};
/* The modifier bits: left ctrl, shift, alt, gui; right ctrl, shift, alt, gui. */
static const uint16_t mod_sc[8] = { 0x1D, 0x2A, 0x38, 0x15B, 0x11D, 0x36, 0x138, 0x15C };

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
        hid_send(v, key(v, 0, usage_sc[u], 0));
        rep_at = sys_clock() + 33000000;
    }
}

static int held(const uint8_t *r, int u) { for (int i = 2; i < 8; i++) if (r[i] == u) return 1; return 0; }

void hid_keys(uint8_t prev[8], const uint8_t r[8])
{
    con_inject_t v[40];
    int n = 0;
    if (!rep_tid) rep_tid = thread_start(repeat_thread, 0, 8192);
    if (r[2] == 1) return;                                   /* "too many keys": ignore this report */
    for (int b = 0; b < 8; b++)
        if ((r[0] ^ prev[0]) & 1 << b) n = key(v, n, mod_sc[b], !(r[0] & 1 << b));
    for (int i = 2; i < 8; i++)
        if (prev[i] && !held(r, prev[i]) && prev[i] < 0x53) {
            n = key(v, n, usage_sc[prev[i]], 1);
            if (rep_usage == prev[i]) rep_usage = 0;
        }
    for (int i = 2; i < 8; i++)
        if (r[i] && !held(prev, r[i]) && r[i] < 0x53 && usage_sc[r[i]]) {
            n = key(v, n, usage_sc[r[i]], 0);
            rep_usage = r[i];
            rep_at = sys_clock() + 500000000;
            if (rep_tid) sys_wake(rep_tid);
        }
    memcpy(prev, r, 8);
    if (n) hid_send(v, n);
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

/* A physical extent in µm: units centimetres (0x11) or inches (0x13),
 * times 10^exponent. 0: no length given. */
static int phys_um(int32_t pmin, int32_t pmax, int exp, uint32_t unit)
{
    int64_t v = (int64_t)(pmax - pmin) * ((unit & 0xFF) == 0x11 ? 10000 : (unit & 0xFF) == 0x13 ? 25400 : 0);
    for (; exp > 0 && v < 10000000; exp--) v *= 10;
    for (; exp < 0; exp++) v /= 10;
    return v > 0 && v < 1000000 ? (int)v : 0;
}

/* Where the buttons, X, Y, wheel and fingers are. */
void hid_parse(hidrep_t *h, const uint8_t *p, int n)
{
    int page = 0, size = 0, count = 0, rid = 0, pos = 0, umin = 0, nu = 0, usages[16], depth = 0, app = 0, finger = -1, fdepth = 0, uexp = 0;
    int32_t lmin = 0, lmax = 0, pmin = 0, pmax = 0;
    uint32_t unit = 0;
    for (int i = 0; i < n; ) {
        int b = p[i], sz = (b & 3) == 3 ? 4 : b & 3, type = b >> 2 & 3, tag = b >> 4;
        uint32_t v = 0;
        for (int k = 0; k < sz && i + 1 + k < n; k++) v |= (uint32_t)p[i + 1 + k] << 8 * k;
        i += 1 + sz;
        if (type == 1) {                                     /* global */
            int32_t s = sz == 1 ? (int8_t)v : sz == 2 ? (int16_t)v : (int32_t)v;
            if (tag == 0) page = v;
            else if (tag == 1) lmin = s;
            else if (tag == 2) lmax = s;
            else if (tag == 3) pmin = s;
            else if (tag == 4) pmax = s;
            else if (tag == 5) uexp = v & 8 ? (int)(v & 0xF) - 16 : (int)(v & 0xF);
            else if (tag == 6) unit = v;
            else if (tag == 7) size = v;
            else if (tag == 8) { rid = v; pos = 0; }
            else if (tag == 9) count = v;
        } else if (type == 2) {                              /* local */
            if (tag == 0 && nu < 16) usages[nu++] = v & 0xFFFF;
            else if (tag == 1) umin = v;
        } else if (type == 0) {                              /* main */
            int u0 = nu ? usages[0] : umin;
            if (tag == 10) {                                 /* a collection: the application's, a finger's */
                if (!depth) app = page << 16 | u0;
                if (page == 0x0D && u0 == 0x22 && (app == 0x0D0005 || app == 0x0D0004) && h->nf < HID_FINGERS) finger = h->nf++, fdepth = depth;
                depth++;
            } else if (tag == 12) { if (--depth == fdepth) finger = -1; }
            else if (tag == 11 && page == 0x0D && u0 == 0x52) { h->mrid = rid; h->msz = size; }   /* the touchpad's input mode */
            else if (tag == 11 && page == 0x0D && u0 == 0x57) h->srid = rid;                  /* its surface and button switches */
            else if (tag == 8 && !(v & 1)) {                 /* an input, data (not padding) */
                for (int k = 0; k < count; k++) {
                    int u = k < nu ? usages[k] : nu ? usages[nu - 1] : umin + k, at = pos + k * size;
                    if (app == 0x0D0005) {                   /* a touchpad */
                        if (finger < 0) { if (page == 9 && u == 1) h->tbpos = at + 1; continue; }
                        if (page == 0x0D && u == 0x42) h->ftip[finger] = at;
                        if (page == 0x0D && u == 0x47) h->fok[finger] = at + 1;
                        if (page == 1 && u == 0x30) { h->fx[finger] = at; h->fsz = size; h->txmin = lmin; h->txmax = lmax; h->trid = rid; h->tw = phys_um(pmin, pmax, uexp, unit); }
                        if (page == 1 && u == 0x31) { h->fy[finger] = at; h->tymin = lmin; h->tymax = lmax; h->th = phys_um(pmin, pmax, uexp, unit); }
                    } else if (app == 0x010006) {            /* a keyboard */
                        if (page == 7 && (v & 2) && size == 1 && u == 0xE0) { h->kpos = at; h->krid = rid; }
                        if (page == 7 && !(v & 2) && size == 8 && !k && !h->kn) { h->kkeys = at; h->kn = count; }
                    } else if (app == 0x0C0001) {            /* consumer keys: an array of usages (volume, play...) */
                        if (page == 0x0C && !(v & 2) && size >= 8 && !k && !h->csz) { h->cpos = at; h->csz = size; h->crid = rid; }
                    } else if (app == 0x0D0004) {            /* a touch screen's first finger: a tablet (unless a mouse came first) */
                        if (!h->screen) { if (h->xsz || h->bn) continue; h->screen = h->absolute = 1; h->rid = rid; }
                        if (finger > 0 || rid != h->rid) continue;
                        if (page == 0x0D && u == 0x42 && !h->bn) { h->bpos = at; h->bn = 1; }
                        if (page == 1 && u == 0x30 && !h->xsz) { h->xpos = at; h->xsz = size; h->xmax = lmax; }
                        if (page == 1 && u == 0x31 && !h->ysz) { h->ypos = at; h->ysz = size; h->ymax = lmax; }
                    } else if (app >> 16 == 1 && !h->screen) {   /* a mouse, a tablet */
                        if (page == 9 && !h->bn) { h->bpos = at; h->bn = count; h->rid = rid; }
                        if (page == 1 && u == 0x30 && !h->xsz) { h->xpos = at; h->xsz = size; h->absolute = !(v & 4); h->xmax = lmax; h->rid = rid; }
                        if (page == 1 && u == 0x31 && !h->ysz) { h->ypos = at; h->ysz = size; h->ymax = lmax; }
                        if (page == 1 && u == 0x38 && !h->wsz) { h->wpos = at; h->wsz = size; }
                    }
                }
            }
            if (tag == 8) pos += size * count;
            nu = 0; umin = 0;
        }
    }
}

static void pointer(hidrep_t *h, const uint8_t *r, int len)
{
    con_inject_t v = { .kind = h->absolute ? INJ_ABS : INJ_REL };
    int b = h->bn ? bits(r, len, h->bpos, h->bn > 3 ? 3 : h->bn, 0) : 0;
    v.buttons = (b & 1 ? MB_LEFT : 0) | (b & 2 ? MB_RIGHT : 0) | (b & 4 ? MB_MIDDLE : 0);
    int32_t x = bits(r, len, h->xpos, h->xsz, !h->absolute), y = bits(r, len, h->ypos, h->ysz, !h->absolute);
    if (h->absolute) {
        v.x = (uint16_t)((int64_t)(x < 0 ? 0 : x) * 65535 / (h->xmax > 0 ? h->xmax : 32767));
        v.y = (uint16_t)((int64_t)(y < 0 ? 0 : y) * 65535 / (h->ymax > 0 ? h->ymax : 32767));
    } else { v.dx = (int16_t)x; v.dy = (int16_t)y; }
    if (h->wsz) v.wheel = (int8_t)bits(r, len, h->wpos, h->wsz, 1);
    hid_send(&v, 1);
}

/* The pad's ranges, and its size in µm: the descriptor's, else 10 cm wide, its units square. */
static void pad_size(const hidrep_t *h, int *rx, int *ry, int *w, int *hh)
{
    *rx = h->txmax - h->txmin > 0 ? h->txmax - h->txmin : 4000;
    *ry = h->tymax - h->tymin > 0 ? h->tymax - h->tymin : *rx * 2 / 3;
    *w = h->tw ? h->tw : 100000;
    *hh = h->th ? h->th : (int)((int64_t)*w * *ry / *rx);
}
int hid_pad_size(const hidrep_t *h, int *w, int *hh)
{
    int rx, ry;
    pad_size(h, &rx, &ry, w, hh);
    return h->tw && h->th;
}

/* A finger's move (pad units) -> mouse counts (a count is about a pixel at
 * the desktop's normal speed). Pointer acceleration as libinput's touchpad
 * profile, flat then linear: 8 counts per mm up to 25 mm/s (slow moves stay
 * precise), rising with the finger's speed to 32 at 175 mm/s and over. A
 * slow 5 cm stroke moves ~400 pixels, a quick one ~1600: a Surface's 2736
 * pixels are one quick stroke across its 10 cm pad. */
static void accel(hidrep_t *h, int dx, int dy, int64_t dt, int16_t *ox, int16_t *oy)
{
    int rx, ry, w, hh;
    pad_size(h, &rx, &ry, &w, &hh);
    int64_t mx = (int64_t)dx * w / rx, my = (int64_t)dy * hh / ry;          /* µm */
    int64_t ax = mx < 0 ? -mx : mx, ay = my < 0 ? -my : my, d = ax > ay ? ax + ay / 2 : ay + ax / 2;   /* (about the distance) */
    int64_t us = dt < 4000000 ? 4000 : dt > 50000000 ? 50000 : dt / 1000;   /* (a report interval, sensibly) */
    int64_t v = d * 1000 / us, g = v <= 25 ? 128 : 128 + 128 * (v - 25) / 50;   /* mm/s; the gain, 1/16 counts per mm */
    if (g > 512) g = 512;
    int64_t cx = mx * g / 1000 + h->rx, cy = my * g / 1000 + h->ry;
    h->rx = (int)(cx % 16); h->ry = (int)(cy % 16);
    *ox = (int16_t)(cx / 16); *oy = (int16_t)(cy / 16);
}

static void touchpad(hidrep_t *h, const uint8_t *r, int len)
{
    con_inject_t v[2] = { { .kind = INJ_REL }, { .kind = INJ_REL } };
    int n = 0, x = 0, y = 0, b = h->tbpos && bits(r, len, h->tbpos - 1, 1, 0);
    int64_t now = sys_clock();
    for (int i = 0; i < h->nf; i++)
        if (bits(r, len, h->ftip[i], 1, 0) && (!h->fok[i] || bits(r, len, h->fok[i] - 1, 1, 0)) && !n++)
            x = bits(r, len, h->fx[i], h->fsz, 0), y = bits(r, len, h->fy[i], h->fsz, 0);
    if (n && !h->n) { h->t0 = h->tlast = now; h->moved = h->maxn = h->click = 0; }   /* a touch begins */
    if (n > h->maxn) h->maxn = n;
    if (n && n == h->n) {                                    /* the same fingers moved */
        int dx = x - h->lx, dy = y - h->ly;
        h->moved += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (n == 1) accel(h, dx, dy, now - h->tlast, &v[0].dx, &v[0].dy);
        else for (h->sacc += dy; h->sacc / (h->txmax / 40 + 1); h->sacc -= (h->sacc > 0 ? 1 : -1) * (h->txmax / 40 + 1))
            v[0].wheel = (int8_t)(v[0].wheel + (h->sacc > 0 ? 1 : -1));     /* fingers up: the page goes down */
    } else h->rx = h->ry = h->sacc = 0;
    h->lx = x; h->ly = y; h->tlast = now;
    h->click |= b;
    v[0].buttons = b ? (h->maxn > 1 ? MB_RIGHT : MB_LEFT) : 0;
    int k = 1;
    if (!n && h->n && !h->click && now - h->t0 < 200000000 && h->moved < h->txmax / 40)   /* a tap: press and release */
        v[0].buttons = h->maxn > 1 ? MB_RIGHT : MB_LEFT, k = 2;
    h->n = n;
    if (k == 2 || v[0].dx || v[0].dy || v[0].wheel || v[0].buttons != h->btn) hid_send(v, k);
    h->btn = k == 2 ? 0 : v[0].buttons;
}

/* A consumer key pressed: volume, play/pause, next, previous, mute go to
 * the audio server (AUDIO_KEY, AUDIO_VOL), as a headset's buttons do. */
static void media(hidrep_t *h, const uint8_t *r, int len)
{
    static long audio;
    static int paused;
    int u = bits(r, len, h->cpos, h->csz > 16 ? 16 : h->csz, 0), k = 0;
    if (u == h->cprev) return;
    h->cprev = u;
    if (u == 0xE9) k = MK_VOLUP;
    else if (u == 0xEA) k = MK_VOLDOWN;
    else if (u == 0xCD) k = (paused = !paused) ? MK_PAUSE : MK_PLAY;
    else if (u == 0xB5) k = MK_NEXT;
    else if (u == 0xB6) k = MK_PREV;
    else if (u == 0xB7) k = MK_STOP;
    msg_t m = { .w = { AUDIO_KEY, (uint64_t)k } };
    if (u == 0xE2) {                                         /* mute: on or off */
        m.w[0] = AUDIO_VOL; m.w[1] = AUDIO_ASK;
        if (call_named(&audio, "audio", &m, 1) < 0 || (long)m.w[0] < 0) return;
        m = (msg_t){ .w = { AUDIO_VOL, (m.w[0] & 0xFF) | (~m.w[0] & AUDIO_MUTE) } };
    } else if (!k) return;
    call_named(&audio, "audio", &m, 1);
}

void hid_input(hidrep_t *h, const uint8_t *r, int len)
{
    if (h->trid && len > 0 && r[0] == h->trid) { touchpad(h, r + 1, len - 1); return; }
    if (h->csz && (h->crid ? len > 0 && r[0] == h->crid : !h->kn && !h->xsz)) { media(h, r + !!h->crid, len - !!h->crid); return; }
    if (h->kn && (!h->krid || (len > 0 && r[0] == h->krid))) {   /* a keyboard: into the boot layout */
        if (h->krid) r++, len--;
        uint8_t b[8] = { (uint8_t)bits(r, len, h->kpos, 8, 0) };
        for (int i = 0; i < h->kn && i < 6; i++) b[2 + i] = (uint8_t)bits(r, len, h->kkeys + 8 * i, 8, 0);
        hid_keys(h->kprev, b);
        return;
    }
    if (!h->xsz) return;
    if (h->rid) { if (len < 1 || r[0] != h->rid) return; r++; len--; }
    pointer(h, r, len);
}
