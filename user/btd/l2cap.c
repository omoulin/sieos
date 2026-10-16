/*
 * l2cap.c - L2CAP (Core spec Vol 3 Part A), basic mode: channels over a
 * link, set up by signalling (CID 1 classic, 5 LE): connect (a PSM: what
 * service), configure each way (the MTU only; everything else accepted),
 * disconnect. Fixed channels: ATT (4, input.c), SMP (6, smp.c). LE's
 * connection parameter requests are granted (the controller is told).
 *
 * The channel API (btd.h) is what the services use: classic HID now
 * (input.c: PSMs 0x11 and 0x13), A2DP's AVDTP (0x19) and RFCOMM (3, for
 * OBEX) later, with no change here.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "btd.h"

#define MAXCH 16
enum { CH_FREE, CH_WAIT, CH_CONFIG, CH_OPEN };            /* state; done: 1 ours configured, 2 theirs */
static l2ch_t ch[MAXCH];
static struct { uint16_t psm; const l2ops_t *ops; } lis[6];
static uint8_t ident;

static void sig(conn_t *c, int code, int id, const uint8_t *d, int n)
{
    uint8_t b[4 + 16] = { (uint8_t)code, (uint8_t)id, (uint8_t)n };
    memcpy(b + 4, d, n);
    acl_send(c, c->le ? 5 : 1, b, 4 + n);
}

static l2ch_t *ch_new(conn_t *c, uint16_t psm, const l2ops_t *ops)
{
    for (int i = 0; i < MAXCH; i++)
        if (!ch[i].state) {
            ch[i] = (l2ch_t){ .c = c, .ops = ops, .psm = psm, .lcid = (uint16_t)(0x40 + i), .rmtu = 672, .state = CH_WAIT };
            return &ch[i];
        }
    return 0;
}
static l2ch_t *ch_of(conn_t *c, int lcid)
{
    int i = lcid - 0x40;
    return i >= 0 && i < MAXCH && ch[i].state && ch[i].c == c ? &ch[i] : 0;
}
static void ch_end(l2ch_t *h)
{
    if (!h->state) return;
    h->state = CH_FREE;
    if (h->ops->close) h->ops->close(h);
}
static void conf_req(l2ch_t *h)
{
    uint8_t d[8] = { 0, 0, 0, 0, 1, 2 };                  /* dcid, flags, the MTU option */
    p16(d, h->rcid); p16(d + 6, L2_MTU);
    h->state = CH_CONFIG;
    sig(h->c, 4, ++ident, d, 8);
}
static void configured(l2ch_t *h, int bit)
{
    h->done |= (uint8_t)bit;
    if (h->done == 3 && h->state == CH_CONFIG) { h->state = CH_OPEN; h->ops->open(h); }
}

void l2_listen(uint16_t psm, const l2ops_t *ops)
{
    for (int i = 0; i < 6; i++) if (!lis[i].psm || lis[i].psm == psm) { lis[i].psm = psm; lis[i].ops = ops; return; }
}

l2ch_t *l2_open(conn_t *c, uint16_t psm, const l2ops_t *ops)
{
    l2ch_t *h = ch_new(c, psm, ops);
    if (!h) return 0;
    uint8_t d[4];
    p16(d, psm); p16(d + 2, h->lcid);
    sig(c, 2, h->ident = ++ident, d, 4);
    return h;
}

int l2_send(l2ch_t *h, const void *p, int n)
{
    if (h->state != CH_OPEN || n > h->rmtu) return -1;
    acl_send(h->c, h->rcid, p, n);
    return 0;
}

void l2_close(l2ch_t *h)
{
    if (!h->state) return;
    uint8_t d[4];
    p16(d, h->rcid); p16(d + 2, h->lcid);
    sig(h->c, 6, ++ident, d, 4);
    h->state = CH_FREE;                                   /* (the answer finds no channel: fine) */
}

void l2_gone(conn_t *c)
{
    for (int i = 0; i < MAXCH; i++) if (ch[i].state && ch[i].c == c) ch_end(&ch[i]);
}

static void sig_rx(conn_t *c, int code, int id, const uint8_t *d, int n)
{
    uint8_t r[12] = { 0 };
    l2ch_t *h;
    switch (code) {
    case 0x02: {                                          /* Connection Request */
        int psm = g16(d), result = 2, i;                  /* 2: no such service */
        for (i = 0; i < 6 && lis[i].psm != psm; i++) ;
        h = 0;
        if (i < 6) result = !(c->dev->d.flags & BTF_BONDED) ? 3 : !(h = ch_new(c, (uint16_t)psm, lis[i].ops)) ? 4 : 0;
        p16(r, h ? h->lcid : 0); p16(r + 2, g16(d + 2)); p16(r + 4, result);
        sig(c, 3, id, r, 8);
        if (h) { h->rcid = g16(d + 2); conf_req(h); }
        break;
    }
    case 0x03:                                            /* Connection Response */
        if (!(h = ch_of(c, g16(d + 2))) || h->state != CH_WAIT || g16(d + 4) == 1) break;   /* (1: pending) */
        if (g16(d + 4)) { bt_log("bt: channel for PSM %04x refused (%d)\n", h->psm, g16(d + 4)); ch_end(h); break; }
        h->rcid = g16(d);
        conf_req(h);
        break;
    case 0x04:                                            /* Configure Request: their MTU; the rest accepted */
        if (!(h = ch_of(c, g16(d)))) { p16(r, 2); sig(c, 1, id, r, 2); break; }
        for (int i = 4; i + 2 <= n && i + 2 + d[i + 1] <= n; i += 2 + d[i + 1])
            if ((d[i] & 0x7F) == 1 && d[i + 1] == 2) h->rmtu = g16(d + i + 2);
        p16(r, h->rcid);
        sig(c, 5, id, r, 6);
        configured(h, 2);
        break;
    case 0x05:                                            /* Configure Response */
        if ((h = ch_of(c, g16(d)))) { if (g16(d + 4)) ch_end(h); else configured(h, 1); }
        break;
    case 0x06:                                            /* Disconnection Request */
        memcpy(r, d, 4);
        sig(c, 7, id, r, 4);
        if ((h = ch_of(c, g16(d)))) ch_end(h);
        break;
    case 0x08: sig(c, 9, id, 0, 0); break;                /* Echo */
    case 0x0A:                                            /* Information: no extended features */
        p16(r, g16(d)); p16(r + 2, g16(d) == 2 ? 0 : 1);
        sig(c, 0x0B, id, r, g16(d) == 2 ? 8 : 4);
        break;
    case 0x12:                                            /* LE: Connection Parameter Update */
        sig(c, 0x13, id, r, 2);
        if (n >= 8) { uint8_t q[14] = { 0 }; p16(q, c->handle); memcpy(q + 2, d, 8); cmd(0x2013, q, 14); }
        break;
    case 0x01:                                            /* Command Reject: a request of ours */
        for (int i = 0; i < MAXCH; i++) if (ch[i].state == CH_WAIT && ch[i].c == c && ch[i].ident == id) ch_end(&ch[i]);
        break;
    case 0x07: case 0x09: case 0x0B: case 0x13: break;
    default: sig(c, 1, id, r, 2);                         /* not understood */
    }
}

void l2_rx(conn_t *c, const uint8_t *p, int n)
{
    int len = g16(p), cid = g16(p + 2);
    p += 4;
    if (len > n - 4) return;
    if (cid == 1 || cid == 5)                             /* signalling: several commands may share a frame */
        for (int i = 0; i + 4 <= len && i + 4 + g16(p + i + 2) <= len; i += 4 + g16(p + i + 2))
            sig_rx(c, p[i], p[i + 1], p + i + 4, g16(p + i + 2));
    else if (cid == 4) att_rx(c, p, len);
    else if (cid == 6) smp_rx(c, p, len);
    else { l2ch_t *h = ch_of(c, cid); if (h && h->state == CH_OPEN) h->ops->recv(h, p, len); }
}
