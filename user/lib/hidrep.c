/*
 * hidrep.c - HID input reports, for USB and I2C mice, tablets and
 * touchpads: the report descriptor says where things are, each report
 * becomes CON_INJECT events (moves and buttons) for the console.
 *
 * A precision touchpad ("PTP", digitizer page, Windows' touchpads) reports
 * its fingers, in absolute positions, once it is told to (input mode 3;
 * until then it acts as a mouse). Here: one finger moves the pointer
 * (relative, like a mouse), two fingers scroll, a short touch is a click
 * (two fingers: the right button), pressing the pad is a click (with two
 * fingers down: the right button). Contacts the pad thinks are a palm
 * (confidence 0) are left out.
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

/* Where the buttons, X, Y, wheel and fingers are. */
void hid_parse(hidrep_t *h, const uint8_t *p, int n)
{
    int page = 0, size = 0, count = 0, rid = 0, pos = 0, umin = 0, nu = 0, usages[16], depth = 0, app = 0, finger = -1, fdepth = 0;
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
        } else if (type == 0) {                              /* main */
            int u0 = nu ? usages[0] : umin;
            if (tag == 10) {                                 /* a collection: the application's, a finger's */
                if (!depth) app = page << 16 | u0;
                if (page == 0x0D && u0 == 0x22 && h->nf < HID_FINGERS) finger = h->nf++, fdepth = depth;
                depth++;
            } else if (tag == 12) { if (--depth == fdepth) finger = -1; }
            else if (tag == 11 && page == 0x0D && u0 == 0x52) { h->mrid = rid; h->msz = size; }   /* the touchpad's input mode */
            else if (tag == 8 && !(v & 1)) {                 /* an input, data (not padding) */
                for (int k = 0; k < count; k++) {
                    int u = k < nu ? usages[k] : nu ? usages[nu - 1] : umin + k, at = pos + k * size;
                    if (app == 0x0D0005) {                   /* a touchpad */
                        if (finger < 0) { if (page == 9 && u == 1) h->tbpos = at + 1; continue; }
                        if (page == 0x0D && u == 0x42) h->ftip[finger] = at;
                        if (page == 0x0D && u == 0x47) h->fok[finger] = at + 1;
                        if (page == 1 && u == 0x30) { h->fx[finger] = at; h->fsz = size; h->txmax = lmax; h->trid = rid; }
                        if (page == 1 && u == 0x31) h->fy[finger] = at;
                    } else if (app >> 16 == 1) {             /* a mouse, a tablet */
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

/* Touchpad units -> mouse counts (x16): the pad's width is about 2500 counts. */
static int scale(hidrep_t *h, int d, int *rem)
{
    int v = d * 2500 * 16 / (h->txmax > 0 ? h->txmax : 4000) + *rem;
    *rem = v % 16;
    return v / 16;
}

static void touchpad(hidrep_t *h, const uint8_t *r, int len)
{
    con_inject_t v[2] = { { .kind = INJ_REL }, { .kind = INJ_REL } };
    int n = 0, x = 0, y = 0, b = h->tbpos && bits(r, len, h->tbpos - 1, 1, 0);
    int64_t now = sys_clock();
    for (int i = 0; i < h->nf; i++)
        if (bits(r, len, h->ftip[i], 1, 0) && (!h->fok[i] || bits(r, len, h->fok[i] - 1, 1, 0)) && !n++)
            x = bits(r, len, h->fx[i], h->fsz, 0), y = bits(r, len, h->fy[i], h->fsz, 0);
    if (n && !h->n) { h->t0 = now; h->moved = h->maxn = h->click = 0; }      /* a touch begins */
    if (n > h->maxn) h->maxn = n;
    if (n && n == h->n) {                                    /* the same fingers moved */
        int dx = x - h->lx, dy = y - h->ly;
        h->moved += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (n == 1) { v[0].dx = (int16_t)scale(h, dx, &h->rx); v[0].dy = (int16_t)scale(h, dy, &h->ry); }
        else for (h->sacc += dy; h->sacc / (h->txmax / 40 + 1); h->sacc -= (h->sacc > 0 ? 1 : -1) * (h->txmax / 40 + 1))
            v[0].wheel = (int8_t)(v[0].wheel + (h->sacc > 0 ? 1 : -1));     /* fingers up: the page goes down */
    } else h->rx = h->ry = h->sacc = 0;
    h->lx = x; h->ly = y;
    h->click |= b;
    v[0].buttons = b ? (h->maxn > 1 ? MB_RIGHT : MB_LEFT) : 0;
    int k = 1;
    if (!n && h->n && !h->click && now - h->t0 < 200000000 && h->moved < h->txmax / 40)   /* a tap: press and release */
        v[0].buttons = h->maxn > 1 ? MB_RIGHT : MB_LEFT, k = 2;
    h->n = n;
    if (k == 2 || v[0].dx || v[0].dy || v[0].wheel || v[0].buttons != h->btn) hid_send(v, k);
    h->btn = k == 2 ? 0 : v[0].buttons;
}

void hid_input(hidrep_t *h, const uint8_t *r, int len)
{
    if (h->trid && len > 0 && r[0] == h->trid) { touchpad(h, r + 1, len - 1); return; }
    if (!h->xsz) return;
    if (h->rid) { if (len < 1 || r[0] != h->rid) return; r++; len--; }
    pointer(h, r, len);
}
