/*
 * tests/ssam/test.c - ssam-test: the Surface Serial Hub protocol
 * (user/acpid/ssam.c) on the host, against a pretend Surface Aggregator
 * Module that answers as a Surface Pro 7's does (firmware version, the
 * battery's _STA/_BIX/_BST, the charger's _PSR, enabling events) and can
 * misbehave: an ACK lost, a NAK, a bad CRC, bytes of noise, a frame sent
 * twice, an event before the answer, silence. Time is the test's own (1 ms
 * a wait), so the 1 s and 3 s timeouts take no time.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include "ssam.h"

static int fails, checks;
static void check(const char *what, int ok) { checks++; if (!ok) fails++; printf("%s %s\n", ok ? "  ok" : "  FAILED", what); }

/* ---- The wire: bytes to the host (a queue), the clock. */
static uint8_t q[8192];
static int qh, qt;
static int64_t clk;
static int h_getc(ssam_t *s) { (void)s; return qh < qt ? q[qh++] : -1; }
static int64_t h_now(void) { return clk; }
static void to_host(const uint8_t *p, int n) { if (qt + n <= (int)sizeof q) memcpy(q + qt, p, n), qt += n; }

/* ---- The pretend SAM. */
static struct {
    uint8_t rb[512]; int rn;
    uint8_t seq;
    int silent, lose_ack, nak_first, bad_crc, noise, twice, event_first;   /* misbehaviours, each for the next request */
    int frames, acks_in, naks_in, data_in, cmds;  /* what the host sent */
    uint8_t lastcmd[80]; int lastn;
    uint8_t pending[256]; int pn;                  /* our last data frame, until the host ACKs it (sent again on its NAK) */
} sam;
static void sam_frame(int type, int seq, const uint8_t *p, int n, int bad)
{
    uint8_t m[300] = { 0xAA, 0x55, (uint8_t)type, (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)seq };
    uint16_t c = ssam_crc(m + 2, 4);
    m[6] = (uint8_t)c; m[7] = (uint8_t)(c >> 8);
    memcpy(m + 8, p, n);
    c = ssam_crc(m + 8, n);
    m[8 + n] = (uint8_t)c; m[9 + n] = (uint8_t)(c >> 8);
    if (type == 0x80) memcpy(sam.pending, m, 10 + n), sam.pn = 10 + n;   /* (kept good; the one sent may be bad) */
    if (bad) m[8 + n] ^= 0x01;
    to_host(m, 10 + n);
}
/* A command from SAM: the answer to rqid, or an event (rqid 1-38). */
static void sam_cmd(int tc, int iid, int rqid, int cid, const uint8_t *d, int n, int bad)
{
    uint8_t p[200] = { 0x80, (uint8_t)tc, 0x00, 0x01, (uint8_t)iid, (uint8_t)rqid, (uint8_t)(rqid >> 8), (uint8_t)cid };
    memcpy(p + 8, d, n);
    sam_frame(0x80, sam.seq++, p, 8 + n, bad);
}
static void le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void sam_answer(const uint8_t *c, int n)
{
    int tc = c[1], iid = c[4], rqid = c[5] | c[6] << 8, cid = c[7];
    uint8_t r[128] = { 0 };
    int k = 0;
    sam.cmds++;
    memcpy(sam.lastcmd, c, n < 80 ? n : 80); sam.lastn = n;
    if (sam.event_first) { sam.event_first = 0; sam_cmd(2, 1, 2, 0x16, r, 0, 0); }   /* (the battery's state changed) */
    if (tc == 1 && cid == 0x13) le32(r, 0x04 << 24 | 0x01AD << 8 | 0x02), k = 4;
    else if (tc == 1) r[0] = 0, k = 1;                                   /* D0 entry, display on, events on: 0, fine */
    else if (tc == 2 && cid == 1) le32(r, 0x1F), k = 4;
    else if (tc == 2 && cid == 2) {                                       /* _BIX: revision 0, mWh, design 46500, last full 44000, ... */
        r[0] = 0; le32(r + 1, 0); le32(r + 5, 46500); le32(r + 9, 44000); le32(r + 13, 1); le32(r + 17, 7580);
        memcpy(r + 61, "M1087371", 8); k = 119;
    } else if (tc == 2 && cid == 3) { le32(r, 2); le32(r + 4, 15000); le32(r + 8, 22000); le32(r + 12, 8200); k = 16; }   /* charging */
    else if (tc == 2 && cid == 0x0d) le32(r, 1), k = 4;
    sam_cmd(tc, iid, rqid, cid, r, k, sam.bad_crc ? sam.bad_crc-- : 0);
    if (sam.twice) { sam.twice = 0; to_host(sam.pending, sam.pn); }      /* (as if our ACK had not come) */
}
static void h_put(ssam_t *s, const uint8_t *p, int n)
{
    (void)s;
    if (sam.silent) return;
    memcpy(sam.rb + sam.rn, p, n); sam.rn += n;
    while (sam.rn >= 10) {
        const uint8_t *f = sam.rb + 2;
        int len = f[1] | f[2] << 8;
        if (sam.rn < 10 + len) return;
        int ok = sam.rb[0] == 0xAA && sam.rb[1] == 0x55 && ssam_crc(f, 4) == (f[4] | f[5] << 8) && ssam_crc(sam.rb + 8, len) == (sam.rb[8 + len] | sam.rb[9 + len] << 8);
        uint8_t type = f[0], seq = f[3], body[300];
        memcpy(body, sam.rb + 8, len);
        memmove(sam.rb, sam.rb + 10 + len, sam.rn - 10 - len); sam.rn -= 10 + len;
        sam.frames++;
        if (!ok) { printf("  (SAM: a bad frame from the host)\n"); fails++; continue; }
        if (type == 0x40) { sam.acks_in++; sam.pn = 0; continue; }
        if (type == 0x04) { sam.naks_in++; if (sam.pn) to_host(sam.pending, sam.pn); continue; }
        sam.data_in++;
        if (sam.lose_ack) { sam.lose_ack = 0; continue; }                 /* lost on the way: no ACK, no answer */
        if (sam.nak_first) { sam.nak_first = 0; sam_frame(0x04, 0, 0, 0, 0); continue; }
        if (sam.noise) { sam.noise = 0; to_host((const uint8_t[]){ 0x00, 0x13, 0xAA, 0x37 }, 4); }
        sam_frame(0x40, seq, 0, 0, 0);
        sam_answer(body, len);
    }
}
static void h_wait(ssam_t *s) { (void)s; clk += 1000000; }

static int events, ev_cid;
static void h_event(ssam_t *s, int tc, int cid, int iid, const uint8_t *p, int n) { (void)s; (void)iid; (void)p; (void)n; if (tc == 2) events++, ev_cid = cid; }

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int main(void)
{
    ssam_t s = { .getc = h_getc, .put = h_put, .wait = h_wait, .now = h_now, .event = h_event, .want = -1 };
    uint8_t r[128];
    printf("ssam-test: the Surface Serial Hub protocol against a pretend SAM\n");
    check("CRC-16-CCITT (0xFFFF first) of \"123456789\" is 29B1", ssam_crc((const uint8_t *)"123456789", 9) == 0x29B1);

    int n = ssam_request(&s, SSAM_TC_SAM, SSAM_TID_SAM, 0, 0x13, 0, 0, r, 4);
    check("firmware version: 4 bytes, 4.429.2", n == 4 && u32(r) == (0x04u << 24 | 0x01AD << 8 | 0x02));
    check("  as a command: 80, category 1, target 1, source 0, instance 0, request id 39, command 13", sam.lastn == 8 &&
          !memcmp(sam.lastcmd, (const uint8_t[]){ 0x80, 1, 1, 0, 0, 39, 0, 0x13 }, 8));
    check("  one frame sent, its answer ACKed, nothing waits", sam.data_in == 1 && sam.acks_in == 1 && !sam.pn && clk == 0);

    const uint8_t ev[5] = { 2, 1, 2, 0, 0 };
    n = ssam_request(&s, SSAM_TC_SAM, SSAM_TID_SAM, 0, 0x0b, ev, 5, r, 1);
    check("battery events on (request 0b, 5 bytes: category 2, sequenced, request id 2): 0", n == 1 && !r[0] && sam.lastn == 13 && !memcmp(sam.lastcmd + 8, ev, 5));

    n = ssam_request(&s, SSAM_TC_BAT, SSAM_TID_SAM, 1, 2, 0, 0, r, sizeof r);
    check("_BIX: 119 bytes: mWh, design 46500, last full 44000, model M1087371", n == 119 && u32(r + 1) == 0 && u32(r + 5) == 46500 && u32(r + 9) == 44000 && !memcmp(r + 61, "M1087371", 8));
    n = ssam_request(&s, SSAM_TC_BAT, SSAM_TID_SAM, 1, 3, 0, 0, r, sizeof r);
    check("_BST: charging, 15000 mW, 22000 mWh, 8200 mV", n == 16 && u32(r) == 2 && u32(r + 4) == 15000 && u32(r + 8) == 22000 && u32(r + 12) == 8200);

    int f0 = sam.data_in;
    sam.lose_ack = 1; clk = 0;
    n = ssam_request(&s, SSAM_TC_BAT, SSAM_TID_SAM, 1, 1, 0, 0, r, 4);
    check("a frame lost: sent again after 1 s, the same number, answered (_STA 1F)", n == 4 && u32(r) == 0x1F && s.resent == 1 && sam.data_in == f0 + 2 && clk >= 1000000000 && clk < 1100000000);

    sam.nak_first = 1; clk = 0;
    n = ssam_request(&s, SSAM_TC_BAT, SSAM_TID_SAM, 1, 0x0d, 0, 0, r, 4);
    check("NAKed: sent again at once; _PSR 1 (on the charger)", n == 4 && u32(r) == 1 && s.resent == 2 && clk < 10000000);

    int naks = sam.naks_in, bad = s.bad;
    sam.bad_crc = 1; sam.noise = 1;
    n = ssam_request(&s, SSAM_TC_BAT, SSAM_TID_SAM, 1, 1, 0, 0, r, 4);
    check("noise, then an answer with a bad CRC: NAKed (noise, frame), SAM sends it again: answered", n == 4 && u32(r) == 0x1F && s.bad == bad + 1 && sam.naks_in >= naks + 1);

    ssam_poll(&s);                                                       /* (the bad answer, sent again on each NAK: ACKed) */
    int a = sam.acks_in, c = sam.cmds;
    sam.twice = 1;
    n = ssam_request(&s, SSAM_TC_BAT, SSAM_TID_SAM, 1, 3, 0, 0, r, sizeof r);
    ssam_poll(&s);
    check("an answer sent twice: both ACKed, used once", n == 16 && sam.acks_in == a + 2 && sam.cmds == c + 1);

    sam.event_first = 1; events = 0;
    n = ssam_request(&s, SSAM_TC_BAT, SSAM_TID_SAM, 1, 1, 0, 0, r, 4);
    check("an event (battery state, 16) before the answer: both taken, the event once", n == 4 && events == 1 && ev_cid == 0x16);
    uint8_t none[1];
    sam_cmd(2, 1, 2, 0x17, none, 0, 0);                                  /* the charger, unasked */
    to_host(sam.pending, sam.pn);                                         /* (and again: our ACK "lost") */
    ssam_poll(&s);
    check("an event while idle (charger, 17), sent twice: ACKed twice, taken once", events == 2 && ev_cid == 0x17 && !sam.pn);

    sam.silent = 1; clk = 0;
    n = ssam_request(&s, SSAM_TC_BAT, SSAM_TID_SAM, 1, 1, 0, 0, r, 4);
    check("silence: no ACK (-1) after three tries, 3 s", n == -1 && clk >= 3000000000 && clk < 3100000000);
    sam.silent = 0;
    n = ssam_request(&s, SSAM_TC_SAM, SSAM_TID_SAM, 0, 0x13, 0, 0, r, 4);
    check("then it answers again", n == 4);

    printf(fails ? "ssam-test: %d of %d checks FAILED\n" : "ssam-test: all %d checks passed\n", fails ? fails : checks, checks);
    return fails != 0;
}
