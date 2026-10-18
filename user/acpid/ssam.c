/*
 * ssam.c - The Surface Aggregator Module (ssam.h): the Surface Serial Hub
 * protocol, then (not on the host) its UART and what acpid asks of it.
 *
 *  - The UART (ACPI MSHW0084's _CRS: UartSerialBus, its speed set by the
 *    device's _INI from a firmware variable, RTS/CTS; on a Surface Pro 7
 *    \_SB.PCI0.UA00, PCI 00:1e.0) is Intel's LPSS "Serial IO" UART, a
 *    DesignWare 16550 with 32-bit registers. Its clock is LPSS's 120 MHz
 *    (Linux: intel-lpss-pci, spt_uart_info) through a fractional divider
 *    (private register 0x200: M bits 1-15, N 16-30, update bit 31): set to
 *    16 x the speed, the 16550's divisor 1, as Linux's 8250_dw does. No
 *    interrupt: with RTS/CTS, SAM waits while our FIFO is half full, so
 *    reading every 50 ms loses nothing (1 ms while a request waits).
 *  - At start, as Linux: the firmware's version (SAM 0x13), "D0 entry"
 *    (0x34), "display on" (0x16), then the battery's events (enable, 0x0b:
 *    category 2, sequenced, request id 2).
 *  - The battery (target category 2, SAM, instance 1): _STA (0x01), _BIX
 *    (0x02), _BST (0x03), the charger's _PSR (0x0d): the same values as
 *    ACPI's methods (Linux: surface_battery, surface_charger), so acpid's
 *    battery model takes them unchanged. Its events: 0x15 the capacities
 *    changed, 0x16 the state, 0x17 the charger.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "ssam.h"

uint16_t ssam_crc(const uint8_t *p, int n)         /* CRC-16-CCITT (Linux: crc_itu_t), from 0xFFFF */
{
    uint16_t c = 0xFFFF;
    while (n-- > 0) {
        c ^= (uint16_t)(*p++ << 8);
        for (int b = 0; b < 8; b++) c = (uint16_t)(c & 0x8000 ? c << 1 ^ 0x1021 : c << 1);
    }
    return c;
}

static void frame(ssam_t *s, int type, int seq, const uint8_t *p, int n)
{
    uint8_t m[10 + 80] = { 0xAA, 0x55, (uint8_t)type, (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)seq };
    uint16_t c = ssam_crc(m + 2, 4);
    m[6] = (uint8_t)c; m[7] = (uint8_t)(c >> 8);
    memcpy(m + 8, p, n);
    c = ssam_crc(m + 8, n);
    m[8 + n] = (uint8_t)c; m[9 + n] = (uint8_t)(c >> 8);
    s->put(s, m, 10 + n);
}

/* A command came: an answer we wait for, or an event. */
static void command(ssam_t *s, const uint8_t *p, int n)
{
    if (n < 8 || p[0] != 0x80) return;
    int rqid = p[5] | p[6] << 8;
    if (rqid == s->want && !s->got) {
        s->rlen = n - 8 < s->cap ? n - 8 : s->cap;
        memcpy(s->resp, p + 8, s->rlen);
        s->got = 1;
    } else if (rqid >= 1 && rqid <= 38 && s->event) s->event(s, p[1], p[7], p[4], p + 8, n - 8);
}

static void drop(ssam_t *s, int k) { memmove(s->rb, s->rb + k, s->rn - k); s->rn -= k; }

/* One message out of what came -> 1, 0: not a whole one yet. Bytes before
 * a SYN (after a bad CRC, or lost) are skipped with a NAK: SAM sends again
 * what we did not ACK, as Linux does. */
static int message(ssam_t *s)
{
    int i = 0;
    while (i + 1 < s->rn && !(s->rb[i] == 0xAA && s->rb[i + 1] == 0x55)) i++;
    if (i) { drop(s, i); frame(s, 0x04, 0, 0, 0); }
    if (s->rn < 10) return 0;
    const uint8_t *f = s->rb + 2, *p = s->rb + 8;
    int len = f[1] | f[2] << 8;
    if (ssam_crc(f, 4) != (f[4] | f[5] << 8) || len > (int)sizeof s->rb - 10) { s->bad++; drop(s, 2); return 1; }
    if (s->rn < 10 + len) return 0;
    if (ssam_crc(p, len) != (p[len] | p[len + 1] << 8)) { s->bad++; drop(s, 2); return 1; }
    int type = f[0], seq = f[3], dup = 0;
    if (type == 0x40 && seq == s->ackseq) s->acked = 1;
    else if (type == 0x04) s->nak = 1;
    else if (type == 0x80) {                        /* sequenced: ACKed, used once */
        uint8_t ack = (uint8_t)seq;
        frame(s, 0x40, ack, 0, 0);
        for (int k = 0; k < 8 && k < s->nseen; k++) dup |= s->seen[k] == seq;
        if (!dup) s->seen[s->nseen++ % 8] = (uint8_t)seq;
    }
    if ((type == 0x80 || type == 0x00) && !dup) command(s, p, len);
    drop(s, 10 + len);
    return 1;
}

static int rx(ssam_t *s)                            /* -> bytes that came */
{
    int c, got = 0;
    while (s->rn < (int)sizeof s->rb && (c = s->getc(s)) >= 0) s->rb[s->rn++] = (uint8_t)c, got++;
    while (message(s)) ;
    if (s->rn == (int)sizeof s->rb) s->rn = 0;      /* (full of nothing whole) */
    return got;
}
void ssam_poll(ssam_t *s) { while (rx(s)) ; }

int ssam_request(ssam_t *s, int tc, int tid, int iid, int cid, const void *data, int n, void *resp, int cap)
{
    uint8_t c[8 + 64];
    if (n > 64) return -2;
    for (int tries = 0; tries < 3; tries++) {
        s->rqid = (uint16_t)(s->rqid < 39 ? 39 : s->rqid + 1);
        c[0] = 0x80; c[1] = (uint8_t)tc; c[2] = (uint8_t)tid; c[3] = 0; c[4] = (uint8_t)iid;
        c[5] = (uint8_t)s->rqid; c[6] = (uint8_t)(s->rqid >> 8); c[7] = (uint8_t)cid;
        memcpy(c + 8, data, n);
        s->want = s->rqid; s->got = 0; s->resp = resp; s->cap = cap;
        s->ackseq = s->seq++;
        s->acked = 0;
        for (int t = 0; t < 3 && !s->acked; t++) {   /* the frame: ACKed within 1 s, else (or NAKed) sent again */
            s->nak = 0;
            if (t) s->resent++;
            frame(s, 0x80, s->ackseq, c, 8 + n);
            for (int64_t end = s->now() + 1000000000; !s->acked && !s->nak && s->now() < end; ) if (!rx(s)) s->wait(s);
        }
        if (!s->acked) { s->want = -1; return -1; }
        for (int64_t end = s->now() + 3000000000LL; !s->got && s->now() < end; ) if (!rx(s)) s->wait(s);
        if (s->got) { s->want = -1; return s->rlen; }
    }
    s->want = -1;
    return -2;
}

#ifndef SSAM_HOST
/* ---- On the Surface: its UART, and acpid's questions. */
#include "aml.h"
#include "pci.h"

static ssam_t sam = { .want = -1 };
static volatile uint32_t *ua;                       /* the UART's registers (32 bits each) */
static volatile int busy;
volatile int sam_news;                              /* events: 1 the battery or charger changed, 2 its capacities too */
int sam_on;
enum { RBR = 0, DLL = 0, IER = 1, DLH = 1, FCR = 2, LCR = 3, MCR = 4, LSR = 5 };

static int u_getc(ssam_t *s) { (void)s; return ua[LSR] & 1 ? (int)(ua[RBR] & 0xFF) : -1; }
static void u_put(ssam_t *s, const uint8_t *p, int n)
{
    (void)s;
    for (int i = 0; i < n; i++) {
        for (long t = sys_clock() + 20000000; !(ua[LSR] & 0x20); ) if (sys_clock() > t) return;   /* (CTS held off 20 ms: given up) */
        ua[RBR] = p[i];
    }
}
static void u_wait(ssam_t *s) { (void)s; sys_sleep(1000000); }
static int64_t u_now(void) { return sys_clock(); }
static void u_event(ssam_t *s, int tc, int cid, int iid, const uint8_t *p, int n)
{
    (void)s; (void)iid; (void)p; (void)n;
    if (tc != SSAM_TC_BAT) return;
    if (cid == 0x15) sam_news |= 3;
    else if (cid == 0x16 || cid == 0x17) sam_news |= 1;
}
static void lock(void) { while (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) sys_yield(); }
static void unlock(void) { __atomic_store_n(&busy, 0, __ATOMIC_RELEASE); }

static int req(int tc, int iid, int cid, const void *d, int n, void *r, int cap)
{
    lock();
    int k = ssam_request(&sam, tc, SSAM_TID_SAM, iid, cid, d, n, r, cap);
    unlock();
    return k;
}
void sam_poll(void) { if (!sam_on) return; lock(); ssam_poll(&sam); unlock(); }

static uint32_t gcd(uint32_t a, uint32_t b) { while (b) { uint32_t t = a % b; a = b; b = t; } return a; }

int sam_start(int dev)
{
    aml_hid_t r;
    int64_t x, adr;
    char p[64];
    aml_eval(dev, "_INI", 0, 0, &x, 1);            /* (its speed, from the firmware's variable, into its _CRS) */
    if (aml_res(dev, "_CRS", &r) || !r.baud || !r.bus[0]) { log_line("SAM: no UART in its _CRS\n"); return -1; }
    uint32_t baud = r.baud > 1 ? r.baud : 3000000;
    strlcpy(p + strlcpy(p, r.bus, 56), "._ADR", 8);
    if (aml_int(p, 0, 0, &adr)) { log_line("SAM: %s: no address\n", r.bus); return -1; }
    pci_t c = { .at = (uint32_t)((adr >> 16 & 31) << 11 | (adr & 7) << 8) };
    uint32_t id = pci_cfg(&c, 0);
    if (id == 0xFFFFFFFF) { log_line("SAM: %s: no PCI device at 00:%02x.%x\n", r.bus, (int)(adr >> 16 & 31), (int)(adr & 7)); return -1; }
    uint32_t bar = pci_cfg(&c, 0x10), barh = pci_cfg(&c, 0x14);
    int was = pci_d0(&c);                          /* power management: to D0, its BAR kept (the firmware leaves it in D3) */
    uint64_t len, pa = pci_bar(&c, 0, &len);
    volatile uint8_t *b = pa ? map_phys(pa, 4096) : 0;
    if (!pa || (long)b < 0) {
        log_line("SAM: UART 00:%02x.%x: no registers (BAR0 %08x%08x, now %08x%08x; was in D%d)\n", (int)(adr >> 16 & 31), (int)(adr & 7),
                 barh, bar, pci_cfg(&c, 0x14), pci_cfg(&c, 0x10), was);
        return -1;
    }
    volatile uint32_t *priv = (volatile uint32_t *)(b + 0x200);
    priv[1] = 0; sys_sleep(1000000); priv[1] = 7;   /* LPSS: reset, then out of it (function, DMA; Linux's intel-lpss) */
    sys_sleep(1000000);
    /* the clock: 120 MHz x M / N = 16 x the speed */
    uint32_t want = 16 * baud, g = gcd(want, 120000000), m = want / g, nn = 120000000 / g;
    while (m > 0x7FFF || nn > 0x7FFF) m >>= 1, nn >>= 1;
    priv[0] = m << 1 | nn << 16 | 1;
    priv[0] = m << 1 | nn << 16 | 1 | 1u << 31;
    ua = (volatile uint32_t *)b;
    ua[IER] = 0;
    ua[FCR] = 0x87;                                 /* FIFOs on and emptied; RTS dropped at half full */
    for (int t = 0; t < 10; t++) {                  /* (DesignWare: LCR ignored while busy receiving: Linux's dw8250_check_lcr) */
        ua[LCR] = 0x80; ua[DLL] = 1; ua[DLH] = 0;   /* divisor 1 */
        ua[LCR] = 3;                                /* 8 bits, no parity, 1 stop bit */
        if ((ua[LCR] & 0xFF) == 3) break;
        ua[FCR] = 0x87; while (ua[LSR] & 1) (void)ua[RBR];
    }
    ua[MCR] = r.flow ? 0x23 : 0x03;                 /* RTS, DTR; automatic RTS/CTS */
    while (ua[LSR] & 1) (void)ua[RBR];
    log_line("SAM: UART %s, PCI 00:%02x.%x (%04x:%04x) at %lx%s, %u bit/s%s (clock M/N %u/%u)%s\n", r.bus, (int)(adr >> 16 & 31), (int)(adr & 7),
             id & 0xFFFF, id >> 16, (long)pa, was == 3 ? " (was in D3)" : was ? " (was in D1/D2)" : "", baud, r.flow ? ", RTS/CTS" : "", m, nn, r.baud > 1 ? "" : "; speed not in its _CRS: assumed");
    sam.getc = u_getc; sam.put = u_put; sam.wait = u_wait; sam.now = u_now; sam.event = u_event;
    uint8_t v[4], k;
    int e = req(SSAM_TC_SAM, 0, 0x13, 0, 0, v, 4);
    if (e < 4) { log_line("SAM: %s (%d messages refused)\n", e == -1 ? "no ACK" : "no answer", sam.bad); return -1; }
    uint32_t ver = v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24;
    log_line("SAM: firmware %u.%u.%u\n", ver >> 24, ver >> 8 & 0xFFFF, ver & 0xFF);
    req(SSAM_TC_SAM, 0, 0x34, 0, 0, &k, 1);        /* D0 entry */
    req(SSAM_TC_SAM, 0, 0x16, 0, 0, &k, 1);        /* display on */
    const uint8_t ev[5] = { SSAM_TC_BAT, 1, SSAM_TC_BAT, 0, 0 };   /* category, sequenced, request id 2, instance 0 */
    if (req(SSAM_TC_SAM, 0, 0x0b, ev, 5, &k, 1) != 1 || k) log_line("SAM: battery events not on: polled\n");
    sam_on = 1;
    return 0;
}

/* The battery and charger as ACPI's methods give them: "_STA", "_BIX"
 * (the first 4 numbers: revision, unit, design, last full), "_BST",
 * "_PSR" -> how many numbers; -1: no answer. */
int sam_battery(const char *what, int64_t *v, int max)
{
    uint8_t b[120];
    int cid = what[1] == 'S' && what[2] == 'T' ? 1 : what[2] == 'I' ? 2 : what[2] == 'S' ? 3 : 0x0d;
    int n = req(SSAM_TC_BAT, 1, cid, 0, 0, b, sizeof b), k = 0;
    if (cid == 2) { if (n < 13) return -1; v[k++] = b[0]; for (int i = 1; i + 4 <= n && k < max; i += 4) v[k++] = b[i] | b[i + 1] << 8 | b[i + 2] << 16 | (uint32_t)b[i + 3] << 24; }
    else for (int i = 0; i + 4 <= n && k < max; i += 4) v[k++] = b[i] | b[i + 1] << 8 | b[i + 2] << 16 | (uint32_t)b[i + 3] << 24;
    return n < 4 ? -1 : k;
}
#endif
