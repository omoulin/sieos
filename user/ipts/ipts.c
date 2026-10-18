/*
 * ipts - A Surface's touch screen (Intel Precise Touch & Stylus), x86-64:
 * the smallest path, one finger as an absolute pointer.
 *
 * The touch controller is not a HID device the host sees: Intel's ME runs
 * it, and talks to us through its own MEI ("Management Engine Interface")
 * function, the "iTouch" one (Ice Lake: PCI 00:16.4, 8086:34e4; Kaby
 * Lake: 8086:9d3e). Linux: drivers/misc/mei (the MEI) and linux-surface's
 * drivers/hid/ipts (the IPTS client).
 *
 *  1. MEI: two rings of 32-bit slots, ours (H_CB_WW, H_CSR) and the ME's
 *     (ME_CB_RW, ME_CSR_HA), read and write pointers in the CSRs. A reset
 *     handshake (H_RST, ME_RDY, H_RDY), then messages: a header (ME client
 *     address, host client address, length, "complete"), the bytes. Polled,
 *     no interrupt.
 *  2. HBM, the bus's own messages (addresses 0/0): version (2.0), the ME's
 *     clients (a bitmap), each one's properties (a UUID), connect to IPTS's
 *     (3e8d0870-271a-4208-8eb5-9acb9402ae04). Flow control: one message
 *     each way per credit (we give one before each answer we want; the ME
 *     gives one before each command it takes). A "fixed address" client
 *     takes no connect (the ME refuses one: status 5, "not allowed") and no
 *     credits; its messages carry host address 0 (Linux's mei_cl_connect).
 *     A connect refused otherwise: a disconnect (a connection the firmware
 *     left?), then once more.
 *  3. IPTS: GET_DEVICE_INFO (the sensor: vendor, product, buffer sizes,
 *     "EDS" interface version), SET_MODE, SET_MEM_WINDOW (16 data buffers,
 *     16 feedback buffers, a doorbell, a work queue, in DMA memory), then
 *     READY_FOR_DATA: its answer means a buffer was filled (the doorbell
 *     says which); we read it, give it back (FEEDBACK), ask again.
 *  4. On the Surface Pro 4 to 7 (EDS 1), "event" mode is single-touch: the
 *     ME itself turns the sensor's frames into a HID report (id 0x40: tip,
 *     X, Y of 32767; linux-surface's ipts_singletouch_descriptor), which
 *     hidrep.c reads as a touch screen: the console gets INJ_ABS, the left
 *     button while touching. Several fingers and the pen need the raw
 *     ("poll") mode: heat maps and the pen's own reports, which Linux hands
 *     to iptsd in user space (not done: docs/surface-pro-7.md).
 *
 * Every step is logged (the serial console, the log viewer). With no
 * iTouch MEI, or one that will not start (3 tries), it ends (0) after one
 * line saying why, and init does not start it again.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef IPTS_HOST
#include "hidrep.h"
#include "pci.h"
#endif

/* ---- The MEI registers. */
enum { H_CB_WW = 0, H_CSR = 4, ME_CB_RW = 8, ME_CSR = 0xC, H_D0I3C = 0x800 };
enum { IE = 1, IS = 2, IG = 4, RDY = 8, RST = 0x10 };
#ifndef IPTS_HOST
static volatile uint8_t *mm;
static uint32_t rd(int r) { return *(volatile uint32_t *)(mm + r); }
static void wr(int r, uint32_t v) { *(volatile uint32_t *)(mm + r) = v; }

static struct { char b[199]; size_t n; } line;   /* (what SYS_DEBUG takes; one thread) */
static void line_put(void *c, char ch) { (void)c; if (line.n < sizeof line.b) line.b[line.n++] = ch; }
static void log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    memcpy(line.b, "ipts: ", 6); line.n = 6;
    vformat(line_put, 0, fmt, ap);
    sys_debug(line.b, line.n);
    va_end(ap);
}
#endif

static void hcsr(uint32_t set) { wr(H_CSR, (rd(H_CSR) & ~(uint32_t)IS) | set); }   /* (IS left alone: writing 1 clears it) */
static int depth;                                  /* our ring's slots */

/* The handshake: reset, the ME ready, out of reset, us ready. -> 0 */
static int mei_reset(void)
{
    uint32_t h = rd(H_CSR);
    if (h & RST) wr(H_CSR, h & ~(uint32_t)(RST | IS));
    wr(H_CSR, ((rd(H_CSR) | RST | IG | IS) & ~(uint32_t)IE));
    for (int64_t t = sys_clock() + 200000000; rd(ME_CSR) & RDY && sys_clock() < t; ) sys_sleep(1000000);   /* (the ME takes the reset: not ready...) */
    for (int64_t t = sys_clock() + 2000000000; !(rd(ME_CSR) & RDY); sys_sleep(1000000))                  /* (... then ready again) */
        if (sys_clock() > t) { log_line("MEI: the ME is not ready (ME_CSR %08x)\n", rd(ME_CSR)); return -1; }
    h = rd(H_CSR);
    wr(H_CSR, (h & ~(uint32_t)(RST | IS)) | IG);   /* out of reset */
    hcsr(IG | RDY);
    depth = rd(H_CSR) >> 24;
    return 0;
}

/* A message to the ME (pieces of what our ring holds): wait for room. -> 0 */
static int mei_send(int me, int host, const void *p, int n)
{
    const uint8_t *b = p;
    int max = (depth - 1) * 4;
    if (max <= 0) return -1;
    do {
        int k = n > max ? max : n, slots = 1 + (k + 3) / 4;
        for (int64_t t = sys_clock() + 1000000000; depth - (int)(((rd(H_CSR) >> 16 & 0xFF) - (rd(H_CSR) >> 8 & 0xFF)) & 0xFF) < slots; sys_sleep(100000))
            if (sys_clock() > t || !(rd(ME_CSR) & RDY)) { log_line("MEI: cannot send (H_CSR %08x, ME_CSR %08x)\n", rd(H_CSR), rd(ME_CSR)); return -1; }
        wr(H_CB_WW, (uint32_t)(me | host << 8 | k << 16 | (k == n ? 1u << 31 : 0)));
        for (int i = 0; i < k; i += 4) { uint32_t w = 0; memcpy(&w, b + i, k - i < 4 ? k - i : 4); wr(H_CB_WW, w); }
        hcsr(IG);
        b += k; n -= k;
    } while (n > 0);
    return 0;
}

/* A message from the ME, if one came: -> its length (into buf; *me, *host:
 * its addresses), 0: none, -1: the ME reset itself. Pieces are put
 * together. */
static int mei_recv(uint8_t *buf, int cap, int *me, int *host)
{
    static int have;
    for (;;) {
        uint32_t m = rd(ME_CSR);
        if (!(m & RDY)) return -1;
        if (!(((m >> 16 & 0xFF) - (m >> 8 & 0xFF)) & 0xFF)) return 0;
        uint32_t hd = rd(ME_CB_RW);
        int len = hd >> 16 & 0x1FF, slots = (len + 3) / 4;
        for (int64_t t = sys_clock() + 10000000; (int)(((rd(ME_CSR) >> 16 & 0xFF) - (rd(ME_CSR) >> 8 & 0xFF)) & 0xFF) < slots; )
            if (sys_clock() > t) { log_line("MEI: a message cut short\n"); return -1; }
        for (int i = 0; i < slots; i++) {
            uint32_t w = rd(ME_CB_RW);
            for (int j = 0; j < 4 && 4 * i + j < len; j++) if (have + 4 * i + j < cap) buf[have + 4 * i + j] = (uint8_t)(w >> 8 * j);
        }
        hcsr(IG);
        have += len;
        if (!(hd >> 31)) continue;                 /* (more pieces) */
        *me = hd & 0xFF; *host = hd >> 8 & 0xFF;
        int n = have < cap ? have : cap;
        have = 0;
        return n;
    }
}

/* ---- HBM and the IPTS client. */
static const uint8_t ipts_uuid[16] = { 0x70, 0x08, 0x8d, 0x3e, 0x1a, 0x27, 0x08, 0x42, 0x8e, 0xb5, 0x9a, 0xcb, 0x94, 0x02, 0xae, 0x04 };
enum { HOST = 1 };                                 /* our client's address (0 for a fixed-address one) */
static int me_addr = -1, host = HOST, credits, maxmsg;
static const char *why;                            /* the last failure, for the final line */
static int refused;
static uint8_t rx[512];

/* An HBM answer (command | 0x80), waited for (up to 3 s); the ME's flow
 * control credits taken on the way. -> its length, -1 */
static int hbm_wait(int cmd)
{
    int me, ho, n;
    for (int64_t t = sys_clock() + 3000000000; sys_clock() < t; ) {
        if ((n = mei_recv(rx, sizeof rx, &me, &ho)) < 0) return -1;
        if (!n) { sys_sleep(1000000); continue; }
        if (me || ho) continue;                    /* (a client's message now: dropped) */
        if (rx[0] == 0x08 && rx[1] == me_addr) credits++;
        else if (rx[0] == cmd) return n;
        else log_line("HBM: %02x while waiting for %02x\n", rx[0], cmd);
    }
    log_line("HBM: no answer %02x\n", cmd);
    why = "the ME does not answer";
    return -1;
}
static int hbm(const uint8_t *m, int n, int answer) { return mei_send(0, 0, m, n) ? -1 : answer ? hbm_wait(answer) : 0; }

static int connect(void)
{
    uint8_t v[4] = { 0x01, 0, 0, 2 };              /* host version 2.0 */
    if (hbm(v, 4, 0x81) < 4) return -1;
    if (!rx[1]) {                                  /* (not supported: the ME's own, if older) */
        log_line("HBM: 2.0 refused, the ME has %d.%d\n", rx[3], rx[2]);
        v[2] = rx[2]; v[3] = rx[3];
        if (hbm(v, 4, 0x81) < 4 || !rx[1]) return -1;
    }
    log_line("HBM version %d.%d (the ME's up to %d.%d)\n", v[3], v[2], rx[3], rx[2]);
    uint8_t e[4] = { 0x04 }, valid[32];
    if (hbm(e, 4, 0x84) < 36) return -1;
    memcpy(valid, rx + 4, 32);
    int n = 0, ver = 0, conns = 0, fixed = 0, single = 0;
    for (int a = 1; a < 256; a++) {
        if (!(valid[a / 8] >> a % 8 & 1)) continue;
        n++;
        uint8_t q[4] = { 0x05, (uint8_t)a };
        if (hbm(q, 4, 0x85) < 28 || rx[2]) continue;
        if (!memcmp(rx + 4, ipts_uuid, 16))        /* its properties: protocol version, connections, fixed address, single buffer, size */
            me_addr = a, ver = rx[20], conns = rx[21], fixed = rx[22], single = rx[23] & 1, maxmsg = rx[24] | rx[25] << 8 | rx[26] << 16 | rx[27] << 24;
    }
    log_line("ME clients: %d; IPTS's: %s %d (protocol %d, %d connection%s%s%s, messages up to %d bytes)\n", n, me_addr < 0 ? "none" : "address", me_addr,
             ver, conns, conns == 1 ? "" : "s", fixed ? ", fixed address" : "", single ? ", one buffer" : "", maxmsg);
    if (me_addr < 0) { why = "the ME has no IPTS client"; return -1; }
    if (fixed) { host = 0; credits = 1 << 30; return 0; }   /* (no connect, no flow control) */
    host = HOST;
    for (int k = 0; k < 2; k++) {
        uint8_t c[4] = { 0x06, (uint8_t)me_addr, HOST };
        if (hbm(c, 4, 0x86) < 4) return -1;
        if (!rx[3]) return 0;
        log_line("IPTS: connect refused (%d: %s)%s\n", rx[3], rx[3] == 1 ? "no such client" : rx[3] == 2 ? "already connected" : rx[3] == 3 ? "no resources" :
                 rx[3] == 5 ? "not allowed" : "?", k ? "" : ": disconnecting it, then again");
        uint8_t d[4] = { 0x07, (uint8_t)me_addr, HOST };   /* (a connection left by the firmware) */
        if (!k && hbm(d, 4, 0x87) < 4) break;
    }
    why = "the ME refuses the connection to its IPTS client"; refused = 1;
    return -1;
}

/* An IPTS command (code, payload), and its answer (code | 1 << 31): status,
 * then up to 80 bytes. -> the status, -1 */
static uint8_t ans[88];
static int command(uint32_t code, const void *p, int n)
{
    uint8_t c[4 + 320];
    memcpy(c, &code, 4);
    if (n) memcpy(c + 4, p, n);
    for (int64_t t = sys_clock() + 1000000000; !credits; ) {   /* the ME's credit to send */
        int me, ho, k = mei_recv(rx, sizeof rx, &me, &ho);
        if (k < 0 || sys_clock() > t) { log_line("IPTS: no credit to send %x\n", code); why = "no flow control credit"; return -1; }
        if (!k) sys_sleep(1000000);
        else if (!me && !ho && rx[0] == 0x08 && rx[1] == me_addr) credits++;
    }
    credits--;
    uint8_t fc[8] = { 0x08, (uint8_t)me_addr, HOST };   /* our credit: its answer (a fixed-address client takes none) */
    return (host && mei_send(0, 0, fc, 8)) || mei_send(me_addr, host, c, 4 + n) ? -1 : 0;
}
/* Its answer: -> 1 (in ans), 0: not yet, -1: gone. Credits, other messages on the way. */
static int answer(uint32_t code)
{
    int me, ho, k;
    while ((k = mei_recv(rx, sizeof rx, &me, &ho)) > 0) {
        if (!me && !ho) { if (rx[0] == 0x08 && rx[1] == me_addr) credits++; else log_line("HBM: %02x\n", rx[0]); continue; }
        uint32_t c = rx[0] | rx[1] << 8 | rx[2] << 16 | (uint32_t)rx[3] << 24;
        if (c == (code | 1u << 31)) { memcpy(ans, rx, k < 88 ? k : 88); return 1; }
        log_line("IPTS: answer %x while waiting for %x\n", c, code);
    }
    return k < 0 ? -1 : 0;
}
static int call(uint32_t code, const void *p, int n)    /* a command and its answer (1 s): -> its status, -1 */
{
    if (command(code, p, n)) return -1;
    for (int64_t t = sys_clock() + 1000000000; sys_clock() < t; sys_sleep(1000000)) {
        int r = answer(code);
        if (r < 0) break;
        if (r) { int st = ans[4]; return st == 0x0A ? 0 : st; }   /* (COMPAT_CHECK_FAIL: some always say it; Linux goes on) */
    }
    log_line("IPTS: no answer to %x\n", code);
    why = "IPTS does not answer";
    return -1;
}

/* ---- The buffers, the sensor. */
enum { NBUF = 16, GET_DEVICE_INFO = 1, SET_MODE = 2, SET_MEM_WINDOW = 3, READY_FOR_DATA = 5, FEEDBACK = 6 };
static uint8_t *data[NBUF];
static volatile uint32_t *doorbell;
static hidrep_t rep;
static int touches;
static const uint8_t single_touch[] = {          /* report 0x40: tip, 7 bits, X, Y (0-32767) (linux-surface's) */
    0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01, 0x85, 0x40, 0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
    0x95, 0x07, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x75, 0x10, 0x95, 0x01, 0x26, 0xFF, 0x7F, 0x81, 0x02, 0x09, 0x31, 0x26,
    0xFF, 0x7F, 0x81, 0x02, 0xC0 };

static int ipts_start(void)
{
    if (call(GET_DEVICE_INFO, 0, 0)) return -1;
    const uint8_t *i = ans + 8;
    uint32_t dsize = i[12] | i[13] << 8 | i[14] << 16 | (uint32_t)i[15] << 24, fsize = i[16] | i[17] << 8 | i[18] << 16 | (uint32_t)i[19] << 24;
    log_line("sensor %04x:%04x, hardware %x, firmware %x, buffers %u and %u bytes, mode %d, %d contacts, EDS %d.%d, interface %d\n",
             i[0] | i[1] << 8, i[2] | i[3] << 8, i[4] | i[5] << 8 | i[6] << 16 | (uint32_t)i[7] << 24, i[8] | i[9] << 8 | i[10] << 16 | (uint32_t)i[11] << 24,
             dsize, fsize, i[20], i[24], i[29], i[28], i[32]);
    if (i[32] > 1) { log_line("interface %d: the HID mode of newer Surfaces: not done\n", i[32]); return -1; }
    if (!dsize || dsize > (1 << 20) || fsize > (1 << 20)) return -1;
    uint8_t mode[16] = { 0 };                       /* event mode: single touch, as a HID report */
    if (call(SET_MODE, mode, 16)) { log_line("SET_MODE failed (%d)\n", ans[4]); return -1; }
    static uint32_t w[80];                          /* SET_MEM_WINDOW: data, upper halves, work queue, doorbell, feedback, ... */
    static uint32_t had;                            /* (the buffers' size: kept when the ME is started again) */
    uint64_t pa;
    for (int k = 0; k < NBUF && had != dsize; k++) {
        if ((long)(data[k] = dma_alloc((dsize + 4095) & ~4095u, &pa)) < 0) return -1;
        w[k] = (uint32_t)pa; w[16 + k] = (uint32_t)(pa >> 32);
        if ((long)dma_alloc((fsize + 4095) & ~4095u, &pa) < 0) return -1;
        w[36 + k] = (uint32_t)pa; w[52 + k] = (uint32_t)(pa >> 32);
    }
    if (had != dsize) {
        if ((long)dma_alloc(4096, &pa) < 0) return -1;   /* the work queue */
        w[32] = (uint32_t)pa; w[33] = (uint32_t)(pa >> 32);
        if ((long)(doorbell = dma_alloc(4096, &pa)) < 0) return -1;
        w[34] = (uint32_t)pa; w[35] = (uint32_t)(pa >> 32);
        if ((long)dma_alloc((fsize + 4095) & ~4095u, &pa) < 0) return -1;   /* hid2me */
        w[68] = (uint32_t)pa; w[69] = (uint32_t)(pa >> 32); w[70] = fsize;
        w[71] = 16 << 8 | 8192u << 16;              /* (reserved, item size 16, work queue 8192) */
        had = dsize;
    }
    *doorbell = 0;
    if (call(SET_MEM_WINDOW, w, 320)) { log_line("SET_MEM_WINDOW failed (%d)\n", ans[4]); return -1; }
    rep = (hidrep_t){ 0 };
    hid_parse(&rep, single_touch, sizeof single_touch);
    log_line("event mode (one finger), %d buffers of %u bytes\n", NBUF, dsize);
    return command(READY_FOR_DATA, 0, 0);
}

/* The ME filled a buffer (READY_FOR_DATA's answer): its report to the
 * console, the buffer given back, the next one asked for. -> 0 */
static int filled(void)
{
    int st = ans[4];
    if (st && st != 0x0E) { log_line("READY_FOR_DATA: status %d\n", st); if (st != 0x09) return -1; }
    if (!st) {
        uint32_t b = *doorbell % NBUF;
        *doorbell += 1;
        const uint8_t *d = data[b];
        uint32_t type = d[0] | d[1] << 8, size = d[4] | d[5] << 8 | d[6] << 16;
        if (type == 3 && size >= 6 && size < 64) {  /* a HID report */
            if (touches < 3 && d[64] == 0x40 && d[65] & 1)
                log_line("touch at %d, %d (of 32767)\n", d[66] | d[67] << 8, d[68] | d[69] << 8), touches++;
            hid_input(&rep, d + 64, (int)size);
        }
        uint32_t fb[4] = { b };
        int r = call(FEEDBACK, fb, 16);             /* (an empty feedback buffer: INVALID_PARAMS, refilled anyway, as Linux finds) */
        if (r < 0) return -1;
    }
    return command(READY_FOR_DATA, 0, 0);
}

#ifndef IPTS_HOST
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    static const uint32_t ids[] = { 0x34E48086, 0x9D3E8086 };   /* Ice Lake's iTouch MEI, Kaby Lake's */
    pci_t p;
    int k = 0;
    while (k < 2 && pci_scan(&p, 0, ids[k], ~0u, 0)) k++;
    if (k == 2) { log_line("no iTouch MEI: no IPTS touch screen\n"); return 0; }
    why = "the ME does not start";
    uint64_t len, pa = pci_bar(&p, 0, &len);
    if (!pa || (long)(mm = map_phys(pa, 4096)) < 0) { log_line("MEI: no registers\n"); return 0; }
    uint32_t hfs = pci_cfg(&p, 0x40);
    log_line("MEI %02x:%02x.%x (%04x:%04x): registers %lx, firmware status %08x, H_CSR %08x, ME_CSR %08x\n", p.at >> 16, p.at >> 11 & 31, p.at >> 8 & 7,
             p.id & 0xFFFF, p.id >> 16, (long)pa, hfs, rd(H_CSR), rd(ME_CSR));
    if (hfs >> 31 && rd(H_D0I3C) & 4) {           /* in D0i3 (the firmware status says it can be): out of it */
        wr(H_D0I3C, (rd(H_D0I3C) & ~4u) | 2);
        for (int i = 0; i < 2000 && rd(H_D0I3C) & 1; i++) sys_sleep(1000000);
    }
    for (int tries = 0; tries < 3; tries++, sys_sleep(1000000000)) {
        me_addr = -1; credits = 0;
        if (mei_reset() || connect() || ipts_start()) { if (refused) break; continue; }   /* (refused twice: trying again changes nothing) */
        hid_send(0, 0);                             /* con: a pointer is here */
        int64_t last = sys_clock();
        for (;;) {                                  /* every 4 ms while touched lately, else every 50 ms */
            int r = answer(READY_FOR_DATA);
            if (r < 0 || (r && filled())) break;
            if (r) { last = sys_clock(); continue; }
            sys_sleep(sys_clock() - last < 5000000000L ? 4000000 : 50000000);
        }
        log_line("the ME reset or stopped: starting again\n");
        tries = 0;
    }
    log_line("no touch screen: %s (given up; 'svc restart ipts' tries again)\n", why ? why : "?");
    return 0;
}
#endif
