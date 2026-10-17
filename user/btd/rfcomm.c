/*
 * rfcomm.c - RFCOMM (TS 07.10 as Bluetooth uses it), L2CAP PSM 3: one
 * multiplexer, one data channel (DLC), for OBEX (obex.c).
 *
 *   frames     address (DLCI, C/R), control (SABM, UA, DM, DISC, UIH; P/F),
 *              length (1 or 2 bytes), [credits], data, FCS (CRC-8, reversed
 *              0x07, over address and control; and length but for UIH)
 *   open       initiator: SABM on DLCI 0, PN for the DLC (credit-based flow
 *              control, its frame size), SABM on it, MSC. Responder: the
 *              same answered; only our Object Push channel (OPP_CH) opens
 *   credits    each data frame costs one; we give the peer more (to 32)
 *              when it runs low, in our own data frames or an empty one
 *   control    PN, MSC, RPN, RLS, Test, FCon/FCoff answered; others NSC
 * The DLCI is the server channel times 2: the side that opened the
 * multiplexer always opens the DLC here.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "btd.h"

static struct {
    l2ch_t *h;
    int init, ch, dlci, open, n1, tx, rx;        /* init: we opened the multiplexer; tx, rx: credits each way */
    const uint8_t *q;                            /* data waiting for credits */
    int qn;
} rf;

static uint8_t fcs(const uint8_t *p, int n)
{
    uint8_t c = 0xFF;
    while (n--) { c ^= *p++; for (int b = 0; b < 8; b++) c = (uint8_t)(c & 1 ? c >> 1 ^ 0xE0 : c >> 1); }
    return (uint8_t)(0xFF - c);
}
/* A frame; credits >= 0: a UIH frame with P/F and that credit byte. */
static void frame(int dlci, int ctl, int cr, const uint8_t *d, int n, int credits)
{
    static uint8_t b[L2_MTU];
    int k = 3;
    b[0] = (uint8_t)(1 | cr << 1 | dlci << 2);
    b[1] = (uint8_t)(ctl | (credits >= 0) << 4);
    b[2] = (uint8_t)(n << 1 | (n < 128));
    if (n >= 128) b[k++] = (uint8_t)(n >> 7);
    if (credits >= 0) b[k++] = (uint8_t)credits;
    if (n) memcpy(b + k, d, n);
    b[k + n] = fcs(b, ctl == 0xEF ? 2 : 3);
    l2_send(rf.h, b, k + n + 1);
}
static void uih(const uint8_t *d, int n)         /* on the DLC, credits given when the peer runs low */
{
    int give = rf.rx < 16 ? 32 - rf.rx : -1;
    if (give > 0) rf.rx += give;
    frame(rf.dlci, 0xEF, rf.init, d, n, give);
}
static void mcc(int type, int c, const uint8_t *v, int n)   /* a multiplexer command (c 1) or answer, on DLCI 0 */
{
    uint8_t b[32] = { (uint8_t)(type << 2 | c << 1 | 1), (uint8_t)(n << 1 | 1) };
    if (n > 30) n = 30;
    memcpy(b + 2, v, n);
    frame(0, 0xEF, rf.init, b, n + 2, -1);
}
static int n1(void) { return (rf.h->rmtu < L2_MTU ? rf.h->rmtu : L2_MTU) - 6; }
static void pn(int c, int n)                     /* parameter negotiation: credit flow control, 7 credits for the peer */
{
    uint8_t v[8] = { (uint8_t)rf.dlci, (uint8_t)(c ? 0xF0 : 0xE0), 7, 0, (uint8_t)n, (uint8_t)(n >> 8), 0, 7 };
    rf.rx = 7;
    mcc(0x20, c, v, 8);
}

static void flush(void)
{
    while (rf.open && rf.qn && rf.tx > 0) {
        int k = rf.qn < rf.n1 ? rf.qn : rf.n1;
        uih(rf.q, k);
        rf.tx--; rf.q += k; rf.qn -= k;
    }
}
static void end(void)
{
    int was = rf.open;
    l2ch_t *h = rf.h;
    memset(&rf, 0, sizeof rf);
    if (h) l2_close(h);
    if (was) ob_close();
}
static void opened(void)
{
    uint8_t v[2] = { (uint8_t)(rf.dlci << 2 | 3), 0x8D };   /* MSC: ready (RTC, RTR, DV) */
    rf.open = 1;
    if (!rf.n1) rf.n1 = n1();
    mcc(0x38, 1, v, 2);
    bt_log("bt: RFCOMM: channel %d open, frames of %d bytes, %d credits\n", rf.dlci >> 1, rf.n1, rf.tx);
    ob_open(rf.h->c, rf.init);
}

static void mux(const uint8_t *d, int n)
{
    int t = d[0] >> 2, c = d[0] >> 1 & 1, l = d[1] >> 1;
    const uint8_t *v = d + 2;
    if (l + 2 > n) return;
    if (t == 0x20 && l >= 8) {                            /* PN */
        rf.tx = v[7] & 7;
        rf.n1 = g16(v + 4) < n1() ? g16(v + 4) : n1();
        if (c) { rf.dlci = v[0] & 0x3F; pn(0, rf.n1); }
        else if (rf.init) frame(rf.dlci, 0x3F, 1, 0, 0, -1);   /* agreed: SABM on the DLC */
    } else if (c && (t == 0x38 || t == 0x24 || t == 0x14 || t == 0x08 || t == 0x28 || t == 0x18)) mcc(t, 0, v, l);
    else if (c) mcc(0x04, 0, d, 1);                       /* not supported */
}

static void rf_recv(l2ch_t *h, const uint8_t *p, int n)
{
    if (h != rf.h || n < 4) return;
    int dlci = p[0] >> 2, ctl = p[1] & 0xEF, hl = p[2] & 1 ? 3 : 4, len = p[2] >> 1 | (hl == 4 ? p[3] << 7 : 0);
    if (ctl == 0xEF && p[1] & 0x10 && dlci) rf.tx += p[hl++];   /* credits */
    if (hl + len + 1 > n) return;
    switch (ctl) {
    case 0x2F:                                            /* SABM */
        if (dlci && (dlci != OPP_CH << 1 || rf.init)) { frame(dlci, 0x1F, !rf.init, 0, 0, -1); break; }   /* DM */
        frame(dlci, 0x73, !rf.init, 0, 0, -1);            /* UA */
        if (dlci) { rf.dlci = dlci; opened(); }
        break;
    case 0x63:                                            /* UA */
        if (!dlci && rf.init && !rf.dlci) { rf.dlci = rf.ch << 1; pn(1, n1()); }
        else if (dlci && dlci == rf.dlci && !rf.open) opened();
        break;
    case 0x43: frame(dlci, 0x73, !rf.init, 0, 0, -1); /* DISC: UA */ /* fall through */
    case 0x0F: if (!dlci || dlci == rf.dlci) end(); break;   /* DM */
    case 0xEF:
        if (!dlci && len >= 2) mux(p + hl, len);
        else if (dlci == rf.dlci && rf.open) {
            if (len) { rf.rx--; ob_recv(p + hl, len); }
            if (rf.open && rf.rx < 4 && !rf.qn) uih(0, 0);
            flush();
        }
        break;
    }
}
static void rf_open_cb(l2ch_t *h)
{
    if (h == rf.h) frame(0, 0x3F, 1, 0, 0, -1);           /* ours: SABM on DLCI 0 */
    else if (!rf.h) rf.h = h;                             /* the peer's: it will SABM */
    else l2_close(h);
}
static void rf_close_cb(l2ch_t *h) { if (h == rf.h) { rf.h = 0; end(); } }
static const l2ops_t ops = { rf_open_cb, rf_recv, rf_close_cb };

void rf_init(void) { l2_listen(3, &ops); }
int rf_open(conn_t *c, int ch)
{
    if (rf.h) return -1;
    rf.init = 1; rf.ch = ch;
    return (rf.h = l2_open(c, 3, &ops)) ? 0 : -1;
}
void rf_send(const uint8_t *p, int n) { rf.q = p; rf.qn = n; flush(); }
void rf_close(void)
{
    if (rf.open) frame(rf.dlci, 0x53, rf.init, 0, 0, -1);   /* DISC */
    end();
}
