/*
 * sdp.c - SDP (Core spec Vol 3 Part B), L2CAP PSM 1: what we offer, and
 * what a device offers.
 *
 *   server   four records: A2DP source (AVDTP on PSM 0x19), Object Push
 *            (RFCOMM channel OPP_CH, OBEX), AVRCP 1.4 target (category 1:
 *            the headset's buttons) and controller (category 2: its
 *            volume), both AVCTP on PSM 0x17. Service Search, Service
 *            Attribute and Service Search Attribute requests; the answer
 *            cut to the asker's size with a 2-byte continuation (an offset)
 *   client   one query at a time: Service Search Attribute for one UUID,
 *            every attribute (continuations followed), then a walk over the
 *            data elements: the L2CAP PSM, AVDTP's version, the RFCOMM
 *            channel, a HID report map (the string after 0x22)
 * Data elements are big-endian: a type and size byte, then the data.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "btd.h"

static int be(const uint8_t *p, int n) { int v = 0; while (n--) v = v << 8 | *p++; return v; }

/* The element at p (n bytes there) -> its header's length (0: malformed); its type and data length. */
static int de(const uint8_t *p, int n, int *t, int *len)
{
    if (n < 1) return 0;
    int sz = p[0] & 7, h = sz < 5 ? 1 : 1 + (1 << (sz - 5));
    *t = p[0] >> 3;
    *len = !*t ? 0 : sz < 5 ? 1 << sz : n < h ? 0 : be(p + 1, h - 1);
    return h + *len <= n && h <= n ? h : 0;
}
static int uuid16(const uint8_t *p, int n)       /* a 16-bit UUID (also from 32 or 128 bits on the base UUID) */
{
    static const uint8_t base[12] = { 0, 0, 0x10, 0, 0x80, 0, 0, 0x80, 0x5F, 0x9B, 0x34, 0xFB };
    return n == 2 ? be(p, 2) : n == 4 ? be(p, 4) : n == 16 && !be(p, 2) && !memcmp(p + 4, base, 12) ? be(p + 2, 2) : -1;
}

/* A walk over every element, nested ones too: a uint right after a UUID is that UUID's value. */
typedef struct { int want, found, last, prev; sdp_res_t r; } walk_t;
static void walk(const uint8_t *p, int n, walk_t *w)
{
    for (int h, t, l; (h = de(p, n, &t, &l)); p += h + l, n -= h + l) {
        if (t == 6 || t == 7) { walk(p + h, l, w); continue; }
        int v = t == 3 ? uuid16(p + h, l) : t == 1 && l <= 4 ? be(p + h, l) : -1;
        if (t == 3 && v == w->want) w->found = 1;
        if (t == 1 && w->last == 0x0100) w->r.psm = v;
        if (t == 1 && w->last == 0x0019) w->r.avdtp = v;
        if (t == 1 && w->last == 0x0003) w->r.ch = v;
        if (t == 4 && w->prev == 0x22 && !w->r.map) { w->r.map = p + h; w->r.mapn = l; }
        w->last = t == 3 ? v : 0;
        w->prev = t == 1 ? v : -1;
    }
}

/* ---- The server: each record its attributes, (id, value) pairs. */
static const uint8_t rec_a2dp[] = {
    0x09, 0, 0, 0x0A, 0, 1, 0, 1,                                 /* handle 0x10001 */
    0x09, 0, 1, 0x35, 3, 0x19, 0x11, 0x0A,                        /* classes: audio source */
    0x09, 0, 4, 0x35, 16, 0x35, 6, 0x19, 1, 0, 0x09, 0, 0x19, 0x35, 6, 0x19, 0, 0x19, 0x09, 1, 3,   /* L2CAP 0x19, AVDTP 1.3 */
    0x09, 0, 5, 0x35, 3, 0x19, 0x10, 0x02,                        /* browse group: public */
    0x09, 0, 9, 0x35, 8, 0x35, 6, 0x19, 0x11, 0x0D, 0x09, 1, 3,   /* profile: A2DP 1.3 */
    0x09, 3, 0x11, 0x09, 0, 1 };                                  /* features: player */
static const uint8_t rec_opp[] = {
    0x09, 0, 0, 0x0A, 0, 1, 0, 2,                                 /* handle 0x10002 */
    0x09, 0, 1, 0x35, 3, 0x19, 0x11, 0x05,                        /* classes: Object Push */
    0x09, 0, 4, 0x35, 17, 0x35, 3, 0x19, 1, 0, 0x35, 5, 0x19, 0, 3, 0x08, OPP_CH, 0x35, 3, 0x19, 0, 8,   /* L2CAP, RFCOMM, OBEX */
    0x09, 0, 5, 0x35, 3, 0x19, 0x10, 0x02,
    0x09, 0, 9, 0x35, 8, 0x35, 6, 0x19, 0x11, 0x05, 0x09, 1, 0,   /* profile: OPP 1.0 */
    0x09, 3, 3, 0x35, 2, 0x08, 0xFF };                            /* formats: any */
#define AVCTP 0x09, 0, 4, 0x35, 16, 0x35, 6, 0x19, 1, 0, 0x09, 0, 0x17, 0x35, 6, 0x19, 0, 0x17, 0x09, 1, 3,   /* L2CAP 0x17, AVCTP 1.3 */ \
              0x09, 0, 5, 0x35, 3, 0x19, 0x10, 0x02, 0x09, 0, 9, 0x35, 8, 0x35, 6, 0x19, 0x11, 0x0E, 0x09, 1, 4   /* AVRCP 1.4 */
static const uint8_t rec_tg[] = {
    0x09, 0, 0, 0x0A, 0, 1, 0, 3, 0x09, 0, 1, 0x35, 3, 0x19, 0x11, 0x0C, AVCTP,   /* handle 0x10003: remote control target */
    0x09, 3, 0x11, 0x09, 0, 1 };                                  /* features: category 1 (player) */
static const uint8_t rec_ct[] = {
    0x09, 0, 0, 0x0A, 0, 1, 0, 4, 0x09, 0, 1, 0x35, 6, 0x19, 0x11, 0x0E, 0x19, 0x11, 0x0F, AVCTP,   /* 0x10004: remote control (controller) */
    0x09, 3, 0x11, 0x09, 0, 2 };                                  /* category 2 (amplifier): the volume */
static const struct { const uint8_t *p; int n; } rec[] = { { rec_a2dp, sizeof rec_a2dp }, { rec_opp, sizeof rec_opp }, { rec_tg, sizeof rec_tg }, { rec_ct, sizeof rec_ct } };
#define NREC (int)(sizeof rec / sizeof rec[0])

static int matches(int r, const uint8_t *pat, int n)   /* has record r every UUID of the pattern? */
{
    int h, t, l, ok = 0;
    for (; (h = de(pat, n, &t, &l)); pat += h + l, n -= h + l, ok = 1) {
        walk_t w = { .want = t == 3 ? uuid16(pat + h, l) : -2 };
        walk(rec[r].p, rec[r].n, &w);
        if (!w.found) return 0;
    }
    return ok;
}
static int wanted(const uint8_t *ids, int n, int id)   /* is id in the attribute list (ids, ranges)? */
{
    for (int h, t, l; (h = de(ids, n, &t, &l)); ids += h + l, n -= h + l)
        if (l == 2 ? be(ids + h, 2) == id : l == 4 && id >= be(ids + h, 2) && id <= be(ids + h + 2, 2)) return 1;
    return 0;
}
static int seq(uint8_t *b, int n)                /* a sequence's header for n bytes -> its length */
{
    if (n < 256) { b[0] = 0x35; b[1] = (uint8_t)n; return 2; }
    b[0] = 0x36; b[1] = (uint8_t)(n >> 8); b[2] = (uint8_t)n; return 3;
}
static int attrs(uint8_t *b, int r, const uint8_t *ids, int n)   /* record r's wanted attributes, as a sequence */
{
    uint8_t t[128];
    int k = 0;
    for (int i = 0, h, ty, l; i < rec[r].n; i += 3 + h + l) {
        h = de(rec[r].p + i + 3, rec[r].n - i - 3, &ty, &l);
        if (wanted(ids, n, be(rec[r].p + i + 1, 2))) { memcpy(t + k, rec[r].p + i, 3 + h + l); k += 3 + h + l; }
    }
    int s = seq(b, k);
    memcpy(b + s, t, k);
    return s + k;
}

static void srv_recv(l2ch_t *h, const uint8_t *p, int n)
{
    static uint8_t all[512], r[L2_MTU];
    int pdu = p[0], t, l, k = 0, an = 0, hl, patn = 4;
    uint8_t *o = r + 5;
    const uint8_t *q = p + 9, *e = p + n, *pat = p + 5;  /* the pattern (or a handle) */
    if (n < 9 || 5 + be(p + 3, 2) > n) goto bad;
    if (pdu != 0x04) {
        if (!(hl = de(p + 5, n - 5, &t, &l)) || t != 6) goto bad;
        pat = p + 5 + hl; patn = l; q = pat + l;
    }
    if (pdu == 0x02) {                                    /* Service Search: the handles */
        if (q + 3 > e) goto bad;
        for (int i = 0; i < NREC; i++) if (matches(i, pat, patn) && k < be(q, 2)) { o[4 + 4 * k] = 0; o[5 + 4 * k] = 1; o[6 + 4 * k] = 0; o[7 + 4 * k] = (uint8_t)(i + 1); k++; }
        o[0] = 0; o[1] = (uint8_t)k; o[2] = 0; o[3] = (uint8_t)k; o[4 + 4 * k] = 0;
        k = 5 + 4 * k;
    } else if (pdu == 0x04 || pdu == 0x06) {              /* Service (Search) Attribute: the attributes */
        int max = q + 2 <= e ? be(q, 2) : 0, ih = de(q + 2, (int)(e - q - 2), &t, &l);
        if (max < 7 || !ih) goto bad;
        const uint8_t *ids = q + 2 + ih, *cont = ids + l;
        if (pdu == 0x04) { int i = be(pat, 4) - 0x10001; if (i < 0 || i >= NREC) goto bad; an = attrs(all, i, ids, l); }
        else {
            uint8_t b[400];
            for (int i = 0; i < NREC; i++) if (matches(i, pat, patn)) k += attrs(b + k, i, ids, l);
            an = seq(all, k); memcpy(all + an, b, k); an += k;
        }
        int off = cont < e && *cont == 2 && cont + 3 <= e ? be(cont + 1, 2) : 0;
        if (max > h->rmtu - 10) max = h->rmtu - 10;
        if (max > (int)sizeof r - 16) max = (int)sizeof r - 16;
        k = an - off < max ? an - off : max;
        if (k < 0) goto bad;
        o[0] = (uint8_t)(k >> 8); o[1] = (uint8_t)k;
        memcpy(o + 2, all + off, k);
        if (off + k < an) { o[2 + k] = 2; o[3 + k] = (uint8_t)((off + k) >> 8); o[4 + k] = (uint8_t)(off + k); k += 5; }
        else { o[2 + k] = 0; k += 3; }
    } else goto bad;
    r[0] = (uint8_t)(pdu + 1); r[1] = p[1]; r[2] = p[2]; r[3] = (uint8_t)(k >> 8); r[4] = (uint8_t)k;
    l2_send(h, r, 5 + k);
    return;
bad:
    { uint8_t x[7] = { 0x01, p[1], p[2], 0, 2, 0, 3 }; l2_send(h, x, 7); }   /* invalid request syntax */
}
static void srv_open(l2ch_t *h) { (void)h; }
static const l2ops_t srv = { srv_open, srv_recv, 0 };

/* ---- The client. */
static struct {
    l2ch_t *h;
    sdp_done_t done;
    int uuid, n, tid;
    uint8_t buf[2048];
} q;
static void ask(const uint8_t *cont, int cn)
{
    uint8_t b[32] = { 0x06, 0, (uint8_t)++q.tid, 0, (uint8_t)(15 + cn), 0x35, 3, 0x19, (uint8_t)(q.uuid >> 8), (uint8_t)q.uuid,
                      (uint8_t)((L2_MTU - 16) >> 8), (uint8_t)(L2_MTU - 16), 0x35, 5, 0x0A, 0, 0, 0xFF, 0xFF };
    b[19] = (uint8_t)cn;
    memcpy(b + 20, cont, cn);
    l2_send(q.h, b, 20 + cn);
}
static void end(conn_t *c, const sdp_res_t *r)
{
    sdp_done_t f = q.done;
    if (q.h) l2_close(q.h);
    q.h = 0;
    f(c, r);
}
static void cli_open(l2ch_t *h) { (void)h; ask(0, 0); }
static void cli_recv(l2ch_t *h, const uint8_t *p, int n)
{
    int k = n >= 7 ? be(p + 5, 2) : 0;
    if (n < 8 || p[0] != 0x07 || 7 + k + 1 + p[7 + k] > n) {
        bt_log("bt: SDP: the device answers %02x\n", n ? p[0] : 0);
        end(h->c, 0);
        return;
    }
    if (q.n + k > (int)sizeof q.buf) k = (int)sizeof q.buf - q.n;
    memcpy(q.buf + q.n, p + 7, k); q.n += k;
    int cn = p[7 + be(p + 5, 2)];
    if (cn && cn <= 16 && q.n < (int)sizeof q.buf) { ask(p + 8 + be(p + 5, 2), cn); return; }
    walk_t w = { .want = q.uuid };
    walk(q.buf, q.n, &w);
    bt_log("bt: SDP %04x on %s: %s, PSM %x, AVDTP %x, RFCOMM channel %d, %d bytes\n", q.uuid, h->c->dev->d.name,
           w.found ? "found" : "none", w.r.psm, w.r.avdtp, w.r.ch, q.n);
    end(h->c, w.found ? &w.r : 0);
}
static void cli_close(l2ch_t *h) { if (q.h == h) { q.h = 0; q.done(h->c, 0); } }
static const l2ops_t cli = { cli_open, cli_recv, cli_close };

int sdp_query(conn_t *c, int uuid, sdp_done_t done)
{
    if (q.h) return -1;
    q.done = done; q.uuid = uuid; q.n = 0;
    q.h = l2_open(c, 1, &cli);
    return q.h ? 0 : -1;
}

void sdp_init(void) { l2_listen(1, &srv); }
