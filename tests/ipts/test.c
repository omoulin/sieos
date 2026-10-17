/*
 * tests/ipts/test.c - ipts-test: the IPTS touch screen driver (user/ipts)
 * on the host, against a pretend iTouch MEI: its registers and rings (a
 * small ring, 64 slots, so the 324-byte SET_MEM_WINDOW goes in two
 * pieces), the ME's HBM (version, clients, properties, connect, flow
 * control) and IPTS client as a Surface Pro 7's answers (EDS 1; FEEDBACK
 * answered "invalid parameters", as there), then single-touch reports
 * written into the data buffers: the console must get an absolute pointer.
 * Checked too: no message sent to the ME without its credit, none from it
 * without ours.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include "hidrep.h"

static int fails, checks;
static void check(const char *what, int ok) { checks++; if (!ok) fails++; printf("%s %s\n", ok ? "  ok" : "  FAILED", what); }

/* ---- What ipts.c calls. */
static int64_t clk;
long sys_clock(void) { return clk; }
long sys_sleep(uint64_t ns) { clk += (int64_t)ns; return 0; }
long sys_wake(int tid) { (void)tid; return 0; }
long sys_yield(void) { return 0; }
long thread_start(void (*fn)(void *), void *arg, size_t stack) { (void)fn; (void)arg; (void)stack; return 99; }
static con_inject_t inj[64];
static int ninj;
long call_named(long *port, const char *name, msg_t *m, int repeat)
{
    (void)port; (void)repeat;
    if (!strcmp(name, "console")) for (size_t i = 0; i < m->slen / sizeof(con_inject_t) && ninj < 64; i++) inj[ninj++] = ((const con_inject_t *)m->sbuf)[i];
    return 0;
}
static void log_line(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("ipts: "); vprintf(fmt, ap); va_end(ap); }
static void *dma_alloc(size_t n, uint64_t *pa) { void *p = calloc(1, n); *pa = (uint64_t)(uintptr_t)p; return p; }

/* ---- The pretend MEI and ME. */
enum { DEPTH = 64 };
static struct {
    uint32_t hcsr, mecsr, hring[256], mering[256];
    uint8_t hwp, hrp, mwp, mrp;
    int ready, me_credit, host_credits, violations, pieces, ready_asked, mode;
    uint8_t msg[1024]; int msgn;
    uint8_t *data[16]; volatile uint32_t *doorbell;
    uint32_t pending[64][100]; int npend, plen[64], pme[64];   /* client answers waiting for the host's credit */
} me;
static void me_put(int meaddr, int host, const void *p, int n)        /* a message to the host (whole) */
{
    uint32_t w[128] = { 0 };
    memcpy(w, p, n);
    me.mering[me.mwp++] = (uint32_t)(meaddr | host << 8 | n << 16 | 1u << 31);
    for (int i = 0; i < (n + 3) / 4; i++) me.mering[me.mwp++] = w[i];
}
static void me_client(const void *p, int n)                         /* to the host's client: needs its credit */
{
    if (me.host_credits) { me.host_credits--; me_put(7, 1, p, n); return; }
    memcpy(me.pending[me.npend], p, n); me.plen[me.npend++] = n;
}
static void me_credit_to_host(void) { uint8_t f[8] = { 0x08, 7, 1 }; me.me_credit = 1; me_put(0, 0, f, 8); }
static const uint8_t uuid[16] = { 0x70, 0x08, 0x8d, 0x3e, 0x1a, 0x27, 0x08, 0x42, 0x8e, 0xb5, 0x9a, 0xcb, 0x94, 0x02, 0xae, 0x04 };
static void me_ipts(const uint8_t *c, int n)
{
    uint32_t code; memcpy(&code, c, 4);
    uint32_t r[22] = { code | 1u << 31, 0 };
    uint8_t *pl = (uint8_t *)(r + 2);
    if (code == 1) {                                                   /* GET_DEVICE_INFO: 045e:099f, 16 KiB buffers, EDS 1 */
        pl[0] = 0x5e; pl[1] = 0x04; pl[2] = 0x9f; pl[3] = 0x09; pl[4] = 0x10; pl[8] = 0x2c;
        pl[13] = 0x40; pl[17] = 0x40; pl[24] = 10; pl[28] = 0; pl[29] = 1; pl[32] = 1;
    } else if (code == 2) me.mode = c[4] + 1;
    else if (code == 3) {                                              /* SET_MEM_WINDOW: where the buffers are */
        const uint32_t *w = (const uint32_t *)(c + 4);
        if (n != 324) r[1] = 3;
        for (int k = 0; k < 16; k++) me.data[k] = (uint8_t *)(uintptr_t)((uint64_t)w[16 + k] << 32 | w[k]);
        me.doorbell = (volatile uint32_t *)(uintptr_t)((uint64_t)w[35] << 32 | w[34]);
        if (w[71] != (16 << 8 | 8192u << 16)) r[1] = 1;
    } else if (code == 5) { me.ready_asked++; return; }                /* READY_FOR_DATA: answered when touched */
    else if (code == 6) r[1] = 1;                                      /* FEEDBACK (empty): INVALID_PARAMS */
    me_client(r, 88);
}
static void me_take(void)                                              /* the host rang (IG): its ring read */
{
    while (me.hrp != me.hwp) {
        uint32_t h = me.hring[me.hrp++ % 256];
        int n = h >> 16 & 0x1FF, a = h & 0xFF, host = h >> 8 & 0xFF;
        for (int i = 0; i < (n + 3) / 4; i++) { uint32_t w = me.hring[me.hrp++ % 256]; memcpy(me.msg + me.msgn + 4 * i, &w, 4); }
        me.msgn += n; me.pieces++;
        if (!(h >> 31)) continue;
        const uint8_t *m = me.msg;
        int len = me.msgn;
        me.msgn = 0;
        if (!a && !host) {                                             /* HBM */
            uint8_t r[40] = { (uint8_t)(m[0] | 0x80) };
            if (m[0] == 0x01) { r[1] = m[3] == 2; r[2] = 2; r[3] = 2; me_put(0, 0, r, 4); }
            else if (m[0] == 0x04) { r[4] = 1 << 2; r[4 + 7 / 8] |= 1 << 7; me_put(0, 0, r, 36); }
            else if (m[0] == 0x05) { r[1] = m[1]; memcpy(r + 4, m[1] == 7 ? uuid : (const uint8_t *)"another client..", 16); r[25] = 0x10; me_put(0, 0, r, 28); }
            else if (m[0] == 0x06) { r[1] = m[1]; r[2] = m[2]; r[3] = m[1] == 7 && m[2] == 1 ? 0 : 1; me_put(0, 0, r, 4); if (!r[3]) me_credit_to_host(); }
            else if (m[0] == 0x08 && m[1] == 7) {
                me.host_credits++;
                if (me.npend) { me.host_credits--; me_put(7, 1, me.pending[0], me.plen[0]); memmove(me.pending, me.pending[1], --me.npend * sizeof me.pending[0]); memmove(me.plen, me.plen + 1, me.npend * sizeof(int)); }
            }
            continue;
        }
        if (a != 7 || host != 1 || !me.me_credit) { me.violations++; continue; }
        me.me_credit = 0;
        me_ipts(m, len);
        me_credit_to_host();                                           /* ready for the next */
    }
}
static uint32_t rd(int r)
{
    if (r == 4) return (me.hcsr & 0x1F) | (uint32_t)me.hrp << 8 | (uint32_t)me.hwp << 16 | (uint32_t)DEPTH << 24;
    if (r == 8) return me.mering[me.mrp++ % 256];
    if (r == 0xC) return (me.ready ? 8u : 0) | (uint32_t)me.mrp << 8 | (uint32_t)me.mwp << 16 | (uint32_t)DEPTH << 24;
    return 0;
}
static void wr(int r, uint32_t v)
{
    if (r == 0) { if ((uint8_t)(me.hwp - me.hrp) >= DEPTH) me.violations++; me.hring[me.hwp++ % 256] = v; return; }
    if (r != 4) return;
    if (v & 0x10) { me.ready = 1; me.hrp = me.hwp = me.mrp = me.mwp = 0; }   /* reset: the ME ready again */
    me.hcsr = v & 0x1D;
    if (v & 4 && !(v & 0x10)) me_take();
}
/* A touch: the ME writes the single-touch report into the next buffer and answers READY_FOR_DATA. */
static void touch(int tip, int x, int y)
{
    if (!me.ready_asked) { printf("  (not asked for data)\n"); fails++; return; }
    me.ready_asked--;
    uint8_t *d = me.data[*me.doorbell % 16];
    memset(d, 0, 128);
    d[0] = 3; d[4] = 6;
    d[64] = 0x40; d[65] = (uint8_t)tip; d[66] = (uint8_t)x; d[67] = (uint8_t)(x >> 8); d[68] = (uint8_t)y; d[69] = (uint8_t)(y >> 8);
    uint32_t r[22] = { 5 | 1u << 31, 0 };
    me_client(r, 88);
}

#define IPTS_HOST
#include "ipts.c"

static void serve(void)                                                /* as main's loop: answers taken until there are none */
{
    for (int i = 0; i < 20; i++) { int r = answer(READY_FOR_DATA); if (r < 0) { fails++; return; } if (!r) return; if (filled()) { fails++; return; } }
}

int main(void)
{
    printf("ipts-test: the IPTS driver against a pretend iTouch MEI\n");
    check("MEI reset: ready, our ring 64 slots", !mei_reset() && depth == DEPTH && me.ready && (me.hcsr & 8));
    check("HBM 2.0, two ME clients, IPTS's at address 7, connected", !connect() && me_addr == 7 && maxmsg == 0x1000);
    check("IPTS started: info, event mode, memory window (in two pieces), data asked for", !ipts_start() && me.mode == 1 && me.data[15] && me.doorbell && me.ready_asked == 1);
    check("  SET_MEM_WINDOW sent in pieces (324 bytes, 252 a piece)", me.pieces > 8);
    ninj = 0;
    touch(1, 16384, 8192); serve();
    touch(1, 16400, 8200); serve();
    touch(0, 16400, 8200); serve();
    check("a touch, a move, lifted: three absolute pointer events, the left button while touching", ninj == 3 && inj[0].kind == INJ_ABS &&
          inj[0].buttons == MB_LEFT && inj[0].x == 16384 * 65535 / 32767 && inj[0].y == 8192 * 65535 / 32767 && inj[1].buttons == MB_LEFT && !inj[2].buttons);
    check("  each buffer given back (FEEDBACK), data asked for again; the doorbell at 3", me.ready_asked == 1 && *me.doorbell == 3);
    for (int i = 0; i < 20; i++) { touch(1, 100 * i, 50 * i); serve(); }
    check("20 more: through all 16 buffers and round again", ninj == 23 && *me.doorbell == 23 && inj[22].x == 1900 * 65535 / 32767);
    check("no message without a credit, our ring never overfilled", !me.violations && !me.npend);
    printf(fails ? "ipts-test: %d of %d checks FAILED\n" : "ipts-test: all %d checks passed\n", fails ? fails : checks, checks);
    return fails != 0;
}
