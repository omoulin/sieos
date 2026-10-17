/*
 * igfx.c - The Intel display engine of the laptop's processor (Raptor Lake-S
 * / Alder Lake-S "UHD Graphics", PCI 8086:a788 and kin, display version 12),
 * set up by us rather than kept as the firmware left it. x86 only.
 *
 * What it drives: the built-in eDP panel on DDI A, and up to two DP or HDMI
 * monitors on the ports TC1-TC4 (on ADL-S these are combo PHYs B-E, not
 * Type-C PHYs). Each shows the same PW x PH picture, fitted to its own mode
 * (aspect kept) by its pipe's scaler. Pipe = transcoder = DPLL: A for the
 * panel, B and C for monitors.
 *
 * The picture's memory is the firmware's frame buffer: stolen memory the
 * firmware already mapped through the GGTT and scanned out (its PLANE_SURF),
 * which Atlas writes through the aperture (BAR2), write-combining. RAM from
 * dma_alloc would be cached, and the display engine does not snoop the
 * CPU's caches; so the GGTT stays as it is, and the picture is at most the
 * firmware's size.
 *
 * Sequences: Intel's PRM for Tiger Lake / Alder Lake-S, vol. 12 "Display
 * Engine" ("Sequences for DisplayPort", "for HDMI", "Panel power", "CDCLK");
 * register offsets and bits checked against Linux's i915 (display/intel_ddi.c,
 * intel_dpll_mgr.c, intel_combo_phy.c, intel_dp_link_training.c, intel_pps.c,
 * intel_backlight.c, intel_cdclk.c, skl_universal_plane.c, skl_scaler.c,
 * skl_watermark.c, intel_bios.c, intel_opregion.c, intel_gmbus.c).
 *   1. probe, touching nothing: the VBT from the ACPI OpRegion (ports, AUX
 *      channels, DDC pins, the panel's power sequence and backlight), DPCD and
 *      EDID over DP AUX (eDP, DP) or GMBUS (HDMI), for each output the best
 *      mode its link and CDCLK can carry (no DSC: a lower refresh instead);
 *   2. everything the firmware enabled off (backlight, pipes, DDIs, panel
 *      power, DPLLs); CDCLK raised through pcode if a mode needs it; power
 *      wells, DBUF;
 *   3. per output: DPLL, its clock to the DDI, transcoder clock, DDI function,
 *      voltage swing, DP link training (or the TMDS buffer for HDMI), timings,
 *      M/N, pipe, scaler, DBUF share and watermarks, plane 1; the panel's power
 *      and backlight (the PCH's PWM; screen_backlight() for Settings).
 * Everything is logged (the console, and /etc/igfx.log: registers, modes,
 * link training). A failure before step 2 leaves the firmware's screen as it
 * was; after it, the panel is set up again once with its slowest mode. A
 * modeset that never ended (the log's last line is its marker) makes the next
 * start keep the firmware's screen: remove /etc/igfx.log to try again.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#if defined(__x86_64__)
#include <stdarg.h>
#include "mk.h"
#include "draw.h"
#include "eld.h"

/* ---- Registers (BAR0), time, the log. */
static volatile uint32_t *mm;
static uint32_t rd(uint32_t r) { return mm[r >> 2]; }
static void wr(uint32_t r, uint32_t v) { mm[r >> 2] = v; }
static __attribute__((noinline)) void rmw(uint32_t r, uint32_t clr, uint32_t set) { wr(r, (rd(r) & ~clr) | set); }
static void us(long n) { if (n >= 1000) sys_sleep(n * 1000); else for (long t = sys_clock() + n * 1000; sys_clock() < t; ) ; }
#define ANY 0xFFFFFFFFu                                  /* poll(): any bit of the mask set */
static __attribute__((noinline)) int poll(uint32_t r, uint32_t mask, uint32_t val, int ms)   /* -> 0, -1: timed out */
{
    for (long end = sys_clock() + ms * 1000000L;;) {
        int late = sys_clock() > end;
        uint32_t v = rd(r) & mask;
        if (val == ANY ? v != 0 : v == val) return 0;
        if (late) return -1;
        if (ms > 5) sys_sleep(200000);
    }
}

static char lb[16384];
static unsigned ln;
static void lput(void *c, char ch) { (void)c; if (ln < sizeof lb) lb[ln++] = ch; }
static int lg(const char *f, ...)                        /* -> -1, for "return lg(...)" on errors */
{
    unsigned s = ln;
    va_list ap;
    va_start(ap, f);
    for (const char *p = "igfx: "; *p; p++) lput(0, *p);
    vformat(lput, 0, f, ap);
    va_end(ap);
    sys_debug(lb + s, ln - s);
    return -1;
}
static void save(void)
{
    long f = fs_open("/etc/igfx.log", FS_WRONLY | FS_CREAT | FS_TRUNC, 0644);
    if (f >= 0) { fs_write(f, 0, lb, ln); fs_close(f); }
}
#define MARK "modeset started; if this is the last line, it never ended: the next start keeps the firmware's screen\n"

/* ---- The outputs. */
typedef struct { int clock, hd, hss, hse, ht, vd, vss, vse, vt, flags; } mode_t;   /* kHz; flags: 1 +hsync, 2 +vsync */
typedef struct {
    int port, kind, aux, ddc, rev;   /* DDI (0 = A, 3-6 = TC1-TC4); 'e' eDP, 'd' DP, 'h' HDMI; AUX channel, GMBUS pin, lane reversal */
    int pipe, on;
    uint8_t dpcd[16];
    int rates[8], nrates, edp14;     /* DP: link rates (port clock, kHz); eDP 1.4 rate table (LINK_RATE_SET) */
    int clk, lanes, rsel;            /* the link: port clock (DP: link rate / 10; HDMI: TMDS clock), lanes, rate index */
    mode_t m;
    int eldn;                        /* a monitor with sound: its ELD (eld.h) for the HDMI codec */
    uint8_t eld[ELD_MAX];
} out_t;
static out_t out[3];
static int nout, ref, cdmax, fbw, fbh, stride, a4, active, failed;
static uint32_t surf;
static int t_up, t_down, t_cycle, t8, t9;               /* the panel's delays: 100 us units (PP registers), ms */
static int blc, blmax, blmin, blhz, blfw;               /* backlight PWM controller, period, lowest duty, VBT Hz, firmware's on */
static const uint32_t phys[] = { 0x162000, 0x6C000, 0x160000, 0x161000, 0x16B000 };   /* combo PHYs A-E */
static uint32_t phy(int port) { return phys[port ? port - 2 : 0]; }
static const uint8_t clkoff[5] = { 10, 11, 24, 4, 5 };   /* DPCLKA_CFGCR: each PHY's DDI clock gate bit */

/* ---- DP AUX (intel_dp_aux.c): a request of up to 20 bytes, a reply of up to 20. */
static int aux_xfer(int ch, const uint8_t *tx, int tn, uint8_t *rx)   /* -> reply bytes, -1 */
{
    uint32_t c = 0x64010 + ch * 0x100;                   /* DP_AUX_CH_CTL; DP_AUX_CH_DATA1-5 follow */
    for (int t = 0; t < 5; t++) {                        /* (at least 3 tries: DP spec) */
        for (int i = 0; i < tn; i += 4) {
            uint32_t v = 0;
            for (int k = 0; k < 4; k++) v = v << 8 | (i + k < tn ? tx[i + k] : 0);
            wr(c + 4 + i, v);
        }
        /* send; clear done and errors; 4 ms timeout; sync pulses: 32, fast wake 18 */
        wr(c, 1u << 31 | 1u << 30 | 1u << 28 | 3u << 26 | 1u << 25 | tn << 20 | 17 << 5 | 31);
        if (poll(c, 1u << 31, 0, 5)) return -1;
        uint32_t s = rd(c);
        wr(c, s | 1u << 30 | 1u << 28 | 1u << 25);
        if (s & 1u << 28) continue;                      /* timed out (no sink, or not ready) */
        if (s & 1u << 25) { us(400); continue; }         /* receive error */
        int n = s >> 20 & 31;
        for (int i = 0; i < n; i++) rx[i] = rd(c + 4 + (i & ~3)) >> (24 - 8 * (i & 3));
        return n;
    }
    return -1;
}
/* A request (native or I2C-over-AUX) until it is neither deferred nor short: 0 */
static int aux(out_t *o, int cmd, unsigned a, uint8_t *b, int n)
{
    uint8_t tx[20] = { cmd << 4 | (a >> 16 & 15), a >> 8, a, n - 1 }, rx[20];
    int w = !(cmd & 1), tn = n ? 4 + (w ? n : 0) : 3;    /* (n 0: address only, I2C start/stop) */
    if (w && n) memcpy(tx + 4, b, n);
    for (int d = 0; d < 16; d++) {
        int r = aux_xfer(o->aux, tx, tn, rx);
        if (r < 1) return -1;
        if (rx[0] & 0x30 || rx[0] & 0xC0) {              /* native / I2C reply: 1 NACK, 2 DEFER */
            if ((rx[0] & 0x30) == 0x10 || (rx[0] & 0xC0) == 0x40) return -1;
            us(500);
            continue;
        }
        if (w || !n) return 0;
        if (r - 1 >= n) { memcpy(b, rx + 1, n); return 0; }
    }
    return -1;
}
static int dpcd_rd(out_t *o, unsigned a, void *b, int n) { return aux(o, 9, a, b, n); }
static int dpcd_wr(out_t *o, unsigned a, int n, ...)     /* n bytes given as arguments */
{
    uint8_t b[8];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) b[i] = va_arg(ap, int);
    va_end(ap);
    return aux(o, 8, a, b, n);
}

/* ---- EDID (256 bytes: the base block and one extension) over I2C-over-AUX
 * or GMBUS (the PCH's I2C engine on the port's DDC pins). */
static int edid_aux(out_t *o, uint8_t *e)
{
    uint8_t z = 0;
    int r = aux(o, 4, 0x50, &z, 1);                      /* I2C write, middle of transaction: offset 0 */
    for (int i = 0; !r && i < 256; i += 16) r = aux(o, 5, 0x50, e + i, 16);   /* I2C reads, 16 bytes */
    aux(o, 1, 0x50, 0, 0);                               /* stop */
    return r;
}
static int edid_gmbus(int pin, uint8_t *e)
{
    int r = 0;
    wr(0xC5100, pin);                                    /* GMBUS0: the pin pair, 100 kHz */
    wr(0xC5104, 1u << 30 | 3u << 25 | 256 << 16 | 0x50 << 1 | 1);   /* GMBUS1: index 0, then read 256 bytes from 0x50 */
    for (int i = 0; !r && i < 256; i += 4) {
        r = poll(0xC5108, 1u << 11 | 1u << 10, ANY, 50) || rd(0xC5108) & 1u << 10 ? -1 : 0;   /* GMBUS2: ready / NAK */
        uint32_t v = rd(0xC510C);
        for (int k = 0; k < 4; k++) e[i + k] = v >> 8 * k;
    }
    if (!r) { poll(0xC5108, 1u << 14, 1u << 14, 10); wr(0xC5104, 1u << 30 | 4u << 25); }   /* wait phase, stop */
    poll(0xC5108, 1u << 9, 0, 10);
    if (r) { wr(0xC5104, 1u << 31); wr(0xC5104, 0); }    /* NAK: reset the controller */
    wr(0xC5100, 0);
    return r;
}
/* The detailed timings of the base block and of a CEA extension -> count */
static int dtds(const uint8_t *e, mode_t *m, int max)
{
    int n = 0;
    for (int b = 0; b < 2 && (!b || (e[126] && e[128] == 2 && e[130] >= 4)); b++)
        for (int i = b ? 128 + e[130] : 54; i + 18 <= (b ? 255 : 126) && n < max; i += 18) {
            const uint8_t *d = e + i;
            mode_t x = { .clock = (d[0] | d[1] << 8) * 10 };
            if (!x.clock || d[17] & 0x80) continue;      /* a display descriptor; interlaced */
            x.hd = d[2] | (d[4] & 0xF0) << 4; x.vd = d[5] | (d[7] & 0xF0) << 4;
            x.ht = x.hd + (d[3] | (d[4] & 15) << 8); x.vt = x.vd + (d[6] | (d[7] & 15) << 8);
            x.hss = x.hd + (d[8] | (d[11] >> 6) << 8); x.hse = x.hss + (d[9] | (d[11] >> 4 & 3) << 8);
            x.vss = x.vd + (d[10] >> 4 | (d[11] >> 2 & 3) << 4); x.vse = x.vss + ((d[10] & 15) | (d[11] & 3) << 4);
            x.flags = (d[17] & 0x18) == 0x18 ? d[17] >> 1 & 3 : 0;
            if (x.hd && x.vd && x.ht > x.hse && x.vt > x.vse) m[n++] = x;
        }
    return n;
}
static void mlog(const char *what, const mode_t *m)
{
    int r = (int)((long)m->clock * 100000 / ((long)m->ht * m->vt));
    lg("  %s %dx%d %d.%02d Hz, %d kHz, h %d %d %d, v %d %d %d%s%s\n", what, m->hd, m->vd, r / 100, r % 100, m->clock,
       m->hss, m->hse, m->ht, m->vss, m->vse, m->vt, m->flags & 1 ? " +h" : "", m->flags & 2 ? " +v" : "");
}

/* ---- A combo PHY DPLL for a port clock (icl_calc_wrpll; DP: the 8.1 GHz
 * DCO of i915's DP tables when it divides) -> CFGCR0/1, 0 */
static int pll(int clock, int dp, uint32_t *c0, uint32_t *c1)
{
    static const uint8_t div[] = { 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 24, 28, 30, 32, 36, 40, 42, 44, 48, 50, 52, 54,
        56, 60, 64, 66, 68, 70, 72, 76, 78, 80, 84, 88, 90, 92, 96, 98, 100, 102, 3, 5, 7, 9, 15, 21 };
    long best = 0, bc = 1L << 40;
    int bd = 0, p, q = 1, k = 1;
    for (unsigned i = 0; i < sizeof div; i++) {
        long dco = clock * 5L * div[i], cen = dco > 8999000 ? dco - 8999000 : 8999000 - dco;   /* DCO 7998-10000 MHz, nearest its middle */
        if (dco < 7998000 || dco > 10000000) continue;
        if (dp && dco == 8100000) cen = -1;
        if (cen < bc) { bc = cen; bd = div[i]; best = dco; }
    }
    if (!bd) return -1;
    if (bd & 1) { p = bd <= 7 ? bd : bd / 3; k = bd <= 7 ? 1 : 3; }   /* (icl_wrpll_get_multipliers) */
    else if (bd == 2) p = 2;
    else { k = 2; p = bd % 4 == 0 ? 2 : bd % 6 == 0 ? 3 : bd % 5 == 0 ? 5 : 7; q = bd / (p * 2); }
    long d = (best << 15) / (ref == 38400 ? 19200 : ref);   /* (38.4 MHz: the DPLL halves it) */
    uint32_t frac = d & 0x7FFF;
    if (ref == 38400) frac = (frac + 1) / 2;             /* Display WA #22010492432: half the fraction */
    *c0 = frac << 10 | (uint32_t)(d >> 15);
    *c1 = (q > 1 ? q << 10 | 1 << 9 : 0) | (k == 3 ? 4 : k) << 6 | (p == 2 ? 1 : p == 3 ? 2 : p == 5 ? 4 : 8) << 2;
    return 0;
}

/* ---- The CDCLK m needs: 2 pixels a cycle, more when the scaler shrinks the picture. */
static int need(const mode_t *m)
{
    long r = m->clock;
    if (fbw > m->hd) r = r * fbw / m->hd;
    if (fbh > m->vd) r = r * fbh / m->vd;
    return (int)((r + 1) / 2);
}
/* ---- Can o show m (link, CDCLK, PLL)? Sets o's link. */
static int fits(out_t *o, const mode_t *m)
{
    uint32_t a, b;
    if (m->hd > 5120 || m->vd > 4096 || m->hd < 640 || m->vd < 480 || need(m) > cdmax) return 0;
    if (o->kind == 'h') { o->clk = m->clock; o->lanes = 4; return m->clock <= 340000 && !pll(m->clock, 0, &a, &b); }
    for (int r = 0; r < o->nrates; r++)                  /* the slowest link that carries it: 24 bits a pixel, 8b/10b */
        for (int l = 1; l <= (o->dpcd[2] & 31) && l <= 4; l *= 2)
            if (m->clock * 3L <= (long)o->rates[r] * l && !pll(o->rates[r], 1, &a, &b)) {
                o->clk = o->rates[r]; o->lanes = l; o->rsel = r;
                return 1;
            }
    return 0;
}
static int pick(out_t *o, const mode_t *c, int n, int slow)   /* the biggest that fits; its fastest (or slowest) -> 0 */
{
    int best = -1;
    for (int i = 0; i < n; i++) {
        long a = (long)c[i].hd * c[i].vd, ba = best < 0 ? 0 : (long)c[best].hd * c[best].vd;
        if (fits(o, &c[i]) && (a > ba || (a == ba && (slow ? c[i].clock < c[best].clock : c[i].clock > c[best].clock)))) best = i;
    }
    if (best < 0) return lg("port %d: no mode fits\n", o->port);
    o->m = c[best];
    fits(o, &o->m);
    mlog("chosen:", &o->m);
    if (o->kind != 'h') lg("  link: %d lanes at %d.%02d Gbit/s\n", o->lanes, o->clk / 100000, o->clk / 1000 % 100);
    return 0;
}

/* ---- Probe an output: DPCD, EDID, modes -> 0 if something is there */
static mode_t modes[3][12];
static int nmodes[3];
static int probe(out_t *o, int i)
{
    uint8_t e[256];
    o->rsel = -1;
    if (o->kind != 'h') {
        rmw(0x45444, 0, 2u << 2 * o->aux);               /* PWR_WELL_CTL_AUX2: this AUX channel's power */
        poll(0x45444, 1u << 2 * o->aux, 1u << 2 * o->aux, 2);
        if (dpcd_rd(o, 0, o->dpcd, 16)) {
            if (o->kind == 'e' || !o->ddc) return o->kind == 'e' ? lg("eDP: no answer on AUX %d\n", o->aux) : -1;
            o->kind = 'h';                               /* (a DP++ port with an HDMI adaptor) */
        }
    }
    if (o->kind != 'h') {
        if (o->dpcd[14] & 0x80) dpcd_rd(o, 0x2200, o->dpcd, 16);   /* DP 1.3+: the extended capabilities */
        lg("port %d (%s): DPCD %02x %02x %02x %02x ... max %d lanes, rate code %02x\n", o->port, o->kind == 'e' ? "eDP" : "DP",
           o->dpcd[0], o->dpcd[1], o->dpcd[2], o->dpcd[3], o->dpcd[2] & 31, o->dpcd[1]);
        uint8_t t[16], rev = 0;
        if (o->kind == 'e' && !dpcd_rd(o, 0x700, &rev, 1) && rev >= 3 && !dpcd_rd(o, 0x10, t, 16) && (t[0] | t[1]))
            for (int k = 0; k < 8 && (t[2 * k] | t[2 * k + 1]); k++) o->rates[o->nrates++] = (t[2 * k] | t[2 * k + 1] << 8) * 20;
        o->edp14 = o->nrates > 0;
        static const int std[] = { 162000, 270000, 540000, 810000 }, code[] = { 6, 10, 20, 30 };
        for (int k = 0; !o->edp14 && k < 4; k++) if (code[k] <= o->dpcd[1]) o->rates[o->nrates++] = std[k];
        for (int k = 0; k < o->nrates; k++) lg("  rate %d kHz%s\n", o->rates[k], o->edp14 ? " (eDP table)" : "");
        if (edid_aux(o, e)) return lg("port %d: no EDID over AUX\n", o->port);
    } else if (edid_gmbus(o->ddc, e)) return -1;
    else lg("port %d (HDMI): EDID on GMBUS pin %d\n", o->port, o->ddc);
    if (e[0] || e[1] != 0xFF) return lg("port %d: bad EDID\n", o->port);
    lg("  EDID %02x%02x product %02x%02x, %d extension(s)\n", e[8], e[9], e[11], e[10], e[126]);
    nmodes[i] = dtds(e, modes[i], 12);
    if (o->kind != 'e' && (o->eldn = eld_build(e, o->kind == 'd', o->eld)))
        lg("  sound: ELD %d bytes, %d audio descriptors, speakers %02x\n", o->eldn, o->eld[5] >> 4, o->eld[7]);
    for (int k = 0; k < nmodes[i]; k++) mlog("mode", &modes[i][k]);
    return 0;
}

/* ---- The VBT (in the ACPI OpRegion, found through PCI config 0xFC, "ASLS"). */
static unsigned u16(const uint8_t *p) { return p[0] | p[1] << 8; }
static const uint8_t *block(const uint8_t *bdb, int id, unsigned *size)   /* (intel_bios.c find_raw_section) */
{
    for (unsigned i = u16(bdb + 18), total = u16(bdb + 20); i + 3 < total; ) {
        unsigned b = bdb[i], n = b == 53 && bdb[i + 3] >= 3 ? u16(bdb + i + 4) | u16(bdb + i + 6) << 16 : u16(bdb + i + 1);
        i += 3;
        if (i + n > total) return 0;
        if ((int)b == id) { *size = n; return bdb + i; }
        i += n;
    }
    return 0;
}
static void vbt(void)
{
    const uint8_t *op = map_phys(pci_read(2 << 11 | 0xFC), 8192), *v, *b;
    unsigned n, pt = 0;
    if ((long)op <= 0 || memcmp(op, "IntelGraphicsMem", 16)) { lg("no ACPI OpRegion: defaults\n"); return; }
    uint64_t rvda = *(const uint64_t *)(op + 0x3BA);     /* ASLE mailbox: RVDA, RVDS (a VBT bigger than 6 KiB) */
    uint32_t rvds = *(const uint32_t *)(op + 0x3C2);
    v = op + 0x400;                                      /* mailbox 4 */
    if (op[0x17] >= 2 && rvda && rvds)                   /* OpRegion 2.1+: relative to the OpRegion */
        v = map_phys(op[0x17] > 2 || op[0x16] >= 1 ? pci_read(2 << 11 | 0xFC) + rvda : rvda, rvds);
    if ((long)v <= 0 || memcmp(v, "$VBT", 4) || memcmp(v + *(const uint32_t *)(v + 28), "BIOS_DATA_BLOCK", 15)) { lg("no VBT: defaults\n"); return; }
    const uint8_t *bdb = v + *(const uint32_t *)(v + 28);
    int ver = u16(bdb + 16);
    if ((b = block(bdb, 40, &n)) && b[0] < 16) pt = b[0];   /* LFP options: the panel type */
    lg("VBT version %d, panel type %u\n", ver, pt);
    if ((b = block(bdb, 27, &n)) && n >= 160) {          /* eDP: power sequence (100 us units) */
        b += pt * 10;
        if ((int)u16(b) > t_up) t_up = u16(b);
        if ((int)u16(b + 2) / 10 > t8) t8 = u16(b + 2) / 10;
        if ((int)u16(b + 4) / 10 > t9) t9 = u16(b + 4) / 10;
        if ((int)u16(b + 6) > t_down) t_down = u16(b + 6);
        if ((int)u16(b + 8) > t_cycle) t_cycle = u16(b + 8);
    }
    if ((b = block(bdb, 43, &n)) && b[0] >= 6) {         /* LFP backlight: frequency, minimum, controller */
        const uint8_t *d = b + 1 + pt * b[0];
        blhz = u16(d + 1);
        blmin = d[3];
        if (ver >= 191 && !blfw && n > 1 + 16u * b[0] + 16 + pt) blc = b[1 + 16 * b[0] + 16 + pt] >> 4 & 1;
    }
    if (!(b = block(bdb, 2, &n)) || n < 5 || b[4] < 27) return;   /* general definitions: the child devices */
    static const int8_t port_of[13] = { 0, 3, 4, 5, -1, -1, -1, 3, 4, 5, 0, 6, 6 };   /* VBT dvo_port -> DDI (ADL-S) */
    for (const uint8_t *c = b + 5; c + b[4] <= b + n; c += b[4]) {
        unsigned type = u16(c + 2), p = c[16] < 13 ? (unsigned)port_of[c[16]] : -1u;
        int dp = type & 1 << 2 || c[24] & 2, hdmi = c[24] & 1 || (type & 1 << 4 && !(type & 1 << 11));
        if (!type || p > 6 || (!dp && !hdmi)) continue;
        int auxch = c[25] == 0x40 ? 0 : c[25] == 0x50 ? 6 : c[25] >> 4 ? (c[25] >> 4) + 2 : (int)p;   /* (adls_aux_ch_map) */
        int ddc = c[19] == 1 ? 1 : c[19] >= 2 && c[19] <= 5 ? c[19] + 7 : 0;                        /* (adls_ddc_pin_map) */
        lg("VBT child: type %04x port %u aux %02x ddc %u%s%s%s\n", type, p, c[25], c[19], dp ? " DP" : "", hdmi ? " HDMI" : "", c[23] & 2 ? " reversed" : "");
        out_t *o = type & 1 << 12 && dp ? &out[0] : nout < 3 ? &out[nout++] : 0;   /* internal: the panel */
        if (o) *o = (out_t){ .port = p, .kind = o == out ? 'e' : dp ? 'd' : 'h', .aux = auxch, .ddc = hdmi ? ddc : 0, .rev = c[23] >> 1 & 1 };
    }
}

/* ---- Voltage swing (icl_combo_phy_set_signal_levels, icl_ddi_combo_vswing_program):
 * entries {swing select, N scalar, cursor coeff, post cursor 1} from i915's
 * ADL-S tables (intel_ddi_buf_trans.c), indexed by (vswing, pre-emphasis). */
static const uint8_t dp_hbr[10][4] = {                   /* tgl_combo_phy_trans_dp_hbr (RBR, HBR) */
    { 0xA, 0x32, 0x3F, 0x00 }, { 0xA, 0x4F, 0x37, 0x08 }, { 0xC, 0x71, 0x2F, 0x10 }, { 0x6, 0x7D, 0x2B, 0x14 },
    { 0xA, 0x4C, 0x3F, 0x00 }, { 0xC, 0x73, 0x34, 0x0B }, { 0x6, 0x7F, 0x2F, 0x10 }, { 0xC, 0x6C, 0x3C, 0x03 },
    { 0x6, 0x7F, 0x35, 0x0A }, { 0x6, 0x7F, 0x3F, 0x00 } };
static const uint8_t dp_hbr2[10][4] = {                  /* adls_combo_phy_trans_dp_hbr2_hbr3 (and eDP HBR3) */
    { 0xA, 0x35, 0x3F, 0x00 }, { 0xA, 0x4F, 0x37, 0x08 }, { 0xC, 0x63, 0x31, 0x0E }, { 0x6, 0x7F, 0x2C, 0x13 },
    { 0xA, 0x47, 0x3F, 0x00 }, { 0xC, 0x63, 0x37, 0x08 }, { 0x6, 0x73, 0x32, 0x0D }, { 0xC, 0x58, 0x3F, 0x00 },
    { 0x6, 0x7F, 0x35, 0x0A }, { 0x6, 0x7F, 0x3F, 0x00 } };
static const uint8_t hdmi_lvl[4] = { 0x6, 0x7F, 0x35, 0x0A };   /* icl_combo_phy_trans_hdmi, its default entry */
static void levels(out_t *o, int vs, int pe)
{
    uint32_t b = phy(o->port), v;
    const uint8_t *e = o->kind == 'h' ? hdmi_lvl : (o->clk > 270000 ? dp_hbr2 : dp_hbr)[(vs == 0 ? 0 : vs == 1 ? 4 : vs == 2 ? 7 : 9) + pe];
    v = rd(b + 0x804);                                   /* 1. PCS_DW1: common keeper for DP, not HDMI */
    wr(b + 0x604, o->kind == 'h' ? v & ~(1u << 26) : v | 1u << 26);
    rmw(b + 0x14, 0, 3);                                 /* 3. CL_DW5: SUS clock config */
    if (o->kind == 'e') rmw(b + 0x28, 3 << 2, 0);        /* CL_DW10: no eDP 4K2K override (not "HOBL") */
    wr(b + 0x694, rd(b + 0x894) & ~(1u << 31));          /* 4. TX_DW5: training off, to change the swing */
    v = rd(b + 0x894) & ~(7u << 18 | 7u << 3 | 1u << 25 | 1u << 26 | 3u << 29);
    wr(b + 0x694, v | 2u << 18 | 6u << 3 | 1u << 29);    /* 5. scaling mode 2, rterm 6, tap 3 off */
    for (int l = 0; l < 4; l++) {
        uint32_t L = b + 0x880 + l * 0x100;              /* TX_DWn of lane l */
        int load = o->clk <= 600000 && (o->lanes == 4 ? l >= 1 : l == 1 || l == 2);   /* 2. loadgen select */
        rmw(L + 8, 1u << 15 | 7u << 11 | 0xFF, (e[0] >> 3) << 15 | (e[0] & 7) << 11 | 0x98);   /* DW2: swing, rcomp scalar */
        rmw(L + 16, 1u << 31 | 0x3FFFF, (uint32_t)load << 31 | e[3] << 12 | e[2]);              /* DW4: post cursors, coeff */
        rmw(L + 28, 0x7Fu << 24, (uint32_t)e[1] << 24);                                          /* DW7: N scalar */
    }
    wr(b + 0x694, rd(b + 0x894) | 1u << 31);             /* 6. training on: the values take effect */
    static const uint8_t down[5] = { 0, 0xE, 0xC, 0, 0 }, rdown[5] = { 0, 0x7, 0x3, 0, 0 };
    rmw(b + 0x28, 0xF0, (o->rev ? rdown : down)[o->lanes] << 4);   /* 7.f CL_DW10: power up the used lanes */
}

/* ---- DP link training (DP 1.4 3.5.1.2-3; intel_dp_link_training.c) -> 0 */
static int train(out_t *o)
{
    uint32_t T = o->pipe * 0x1000, ef = o->dpcd[2] & 0x80;
    uint8_t s[6], set[4];
    int vs = 0, pe = 0, eq = 0, pat = 1, cr_us = o->dpcd[0] >= 0x14 || !(o->dpcd[14] & 0x7F) ? 100 : (o->dpcd[14] & 0x7F) * 4000;
    int eq_us = o->dpcd[14] & 0x7F ? (o->dpcd[14] & 0x7F) * 4000 : 400;
    for (int t = 0; t < 3 && dpcd_wr(o, 0x600, 1, 1); t++) us(1000);   /* sink to D0 */
    if (o->edp14) dpcd_wr(o, 0x101, 1, o->lanes | ef), dpcd_wr(o, 0x115, 1, o->rsel);
    else dpcd_wr(o, 0x100, 2, o->clk / 27000, o->lanes | ef);   /* LINK_BW_SET: 0.27 Gbit/s units */
    dpcd_wr(o, 0x107, 2, 0, 1);                          /* no downspread; 8b/10b */
    levels(o, 0, 0);                                     /* 7.e, 7.f */
    wr(T + 0x60540, 1u << 31 | ef << 11);               /* 7.c DP_TP_CTL: on, SST, pattern 1, enhanced framing */
    wr(0x64000 + o->port * 0x100, 1u << 31 | o->rev << 16 | (o->port ? 0 : a4) | (o->lanes - 1) << 1);   /* DDI_BUF_CTL */
    if (poll(0x64000 + o->port * 0x100, 1u << 7, 0, 10)) return lg("DDI %d buffer stays idle\n", o->port);
    for (int i = 0, same = 0; ; i++) {
        static const uint32_t tp[5] = { 0, 0, 1, 4, 5 };   /* DP_TP_CTL patterns 1-4 */
        rmw(T + 0x60540, 7 << 8, tp[pat] << 8);
        levels(o, vs, pe);
        for (int l = 0; l < 4; l++) set[l] = vs | pe << 3 | (vs == 3) << 2 | (pe == 3 - vs) << 5;
        uint8_t b[5] = { pat == 4 ? 7 : pat | 0x20, set[0], set[1], set[2], set[3] };   /* (TPS1-3 unscrambled) */
        if (aux(o, 8, 0x102, b, 1 + o->lanes)) return lg("AUX write failed in training\n");
        us(eq ? eq_us : cr_us);
        if (dpcd_rd(o, 0x202, s, 6)) return lg("AUX read failed in training\n");
        int cr = 1, ok = s[2] & 1, nvs = 0, npe = 0;     /* lanes: 1 CR done, 2 EQ done, 4 symbol locked; interlane aligned */
        for (int l = 0; l < o->lanes; l++) {
            int st = s[l / 2] >> 4 * (l & 1), adj = s[4 + l / 2] >> 4 * (l & 1);
            cr &= st & 1; ok &= (st & 7) == 7;
            if ((adj & 3) > nvs) nvs = adj & 3;
            if ((adj >> 2 & 3) > npe) npe = adj >> 2 & 3;
        }
        if (npe > 3 - nvs) npe = 3 - nvs;
        lg("  %s %d: status %02x %02x %02x, swing %d pre-emphasis %d, asked %d %d\n", eq ? "EQ" : "CR", i, s[0], s[1], s[2], vs, pe, nvs, npe);
        if (!eq && cr) { eq = 1; i = -1; pat = o->dpcd[3] & 0x80 ? 4 : o->dpcd[2] & 0x40 ? 3 : 2; }
        else if (eq && ok) break;
        else if (eq && (!cr || i == 5)) return lg("channel equalization failed\n");
        else if (!eq && (i == 10 || vs == 3 || (nvs == vs && npe == pe && ++same == 5))) return lg("clock recovery failed\n");
        if (nvs != vs || npe != pe) same = 0;
        vs = nvs; pe = npe;
    }
    dpcd_wr(o, 0x102, 1, 0);                             /* training off */
    rmw(T + 0x60540, 7 << 8, 2 << 8);                    /* 7.j idle patterns, then 7.k normal */
    if (poll(T + 0x60544, 1u << 25, 1u << 25, 2)) lg("no idle patterns sent\n");
    rmw(T + 0x60540, 7 << 8, 3 << 8);
    lg("port %d: link trained\n", o->port);
    return 0;
}

/* ---- The panel's power (intel_pps.c, PCH PPS 0xC7200: status, control, on, off) and backlight. */
static int panel(int on)
{
    if (!on) { rmw(0xC7204, 15, 0); return poll(0xC7200, 0xB0000000, 0, 5000); }   /* power, reset, backlight, VDD off */
    if (poll(0xC7200, 0xB800000F, 0, 5000)) return lg("panel: power cycle delay never ended\n");   /* (T12) */
    rmw(0xC7204, 0, 1);
    return poll(0xC7200, 0xB000000F, 0x80000008, 5000) ? lg("panel: no power (PP_STATUS %08x)\n", rd(0xC7200)) : 0;
}
static int bright = 100;
#define BLC(r) (0xC8250 + blc * 0x100 + (r))             /* BXT_BLC_PWM_CTL, +4 FREQ, +8 DUTY */
static void backlight(int on)
{
    if (!on) { rmw(BLC(0), 1u << 31, 0); rmw(0xC7204, 4, 0); us(t9 * 1000L); return; }
    us(t8 * 1000L);                                      /* T8 */
    rmw(0xC7204, 0, 4);                                  /* PP_CONTROL: backlight enable */
    uint32_t c = rd(BLC(0)) & 1u << 29;                  /* (polarity, as set up) */
    wr(BLC(0), c);
    wr(BLC(4), blmax);
    wr(BLC(8), (uint32_t)((long)blmax * bright / 100));
    wr(BLC(0), c | 1u << 31);
}
int screen_backlight(int pct)
{
    if (!active || !out[0].on || !blmax) return -1;
    if (pct >= 0) {
        int lo = blmin * 100 / 255;
        bright = pct < lo ? lo : pct > 100 ? 100 : pct;
        wr(BLC(8), (uint32_t)((long)blmax * bright / 100));
    }
    return bright;
}
void screen_blank(int off) { if (active && out[0].on && blmax) backlight(!off); }   /* (the lid) */
int screen_wake(void);                                   /* (below: after modeset) */
static void native(int *w, int *h) { *w = out[0].m.hd < fbw ? out[0].m.hd : fbw; *h = out[0].m.vd < fbh ? out[0].m.vd : fbh; }
int screen_native(int *w, int *h) { if (active && out[0].on) native(w, h); return active && out[0].on; }

/* ---- Turning things off. */
static void pipe_off(int t)
{
    uint32_t T = t * 0x1000, f = rd(T + 0x60400);
    int port = (f >> 27 & 15) - 1;
    for (int k = 0; k < 3; k++) { wr(T + 0x70180 + k * 0x100, 0); wr(T + 0x7019C + k * 0x100, rd(T + 0x7019C + k * 0x100)); }   /* planes */
    wr(T + 0x70080, 0); wr(T + 0x70084, rd(T + 0x70084));   /* cursor */
    if (rd(T + 0x70008) & 1u << 31) { rmw(T + 0x70008, 1u << 31, 0); poll(T + 0x70008, 1u << 30, 0, 100); }   /* TRANSCONF */
    for (int k = 0; k < 2; k++) { uint32_t S = 0x68180 + t * 0x800 + k * 0x100; wr(S, 0); wr(S - 0x10, 0); wr(S - 0xC, 0); }   /* scalers */
    rmw(0x650C0, 5u << 4 * t, 0);                        /* its audio, its ELD not valid (AUD_PIN_ELD_CP_VLD) */
    rmw(T + 0x60400, 1u << 31, 0);                       /* the DDI function */
    if (port >= 0) {
        rmw(0x64000 + port * 0x100, 1u << 31, 0);        /* DDI buffer, DP transport, wait idle */
        rmw(T + 0x60540, 1u << 31, 0);
        poll(0x64000 + port * 0x100, 1u << 7, 1u << 7, 10);
        int ph = port ? port - 2 : 0;                    /* the DDI's clock gated */
        rmw(ph < 3 ? 0x164280 : 0x1642BC, 0, 1u << clkoff[ph]);
    }
    wr(T + 0x60400, 0);
    wr(0x46140 + 4 * t, 0);                              /* TRANS_CLK_SEL */
}
static uint32_t pll_en(int id) { return id < 3 ? 0x46010 + 4 * id : 0x46030; }
static void pll_off(int id)
{
    uint32_t e = pll_en(id);
    if (rd(e) & 1u << 31) { rmw(e, 1u << 31, 0); poll(e, 1u << 30, 0, 2); }
    rmw(e, 1u << 27, 0); poll(e, 1u << 26, 0, 2);
}

/* ---- CDCLK (bxt_set_cdclk for display 11-13): pcode, its PLL, the divider. */
static int pcode(uint32_t cmd, uint32_t val)             /* -> the reply, -1 */
{
    if (rd(0x138124) & 1u << 31) return -1;              /* GEN6_PCODE_MAILBOX busy */
    wr(0x138128, val); wr(0x13812C, 0); wr(0x138124, 1u << 31 | cmd);
    return poll(0x138124, 1u << 31, 0, 10) || rd(0x138124) & 0xFF ? -1 : (int)rd(0x138128);
}
static int cdclk_now(void)
{
    static const uint8_t d[4] = { 2, 3, 4, 8 };          /* CD2X divider: vco / 2 / {1, 1.5, 2, 4} */
    uint32_t p = rd(0x46070);                            /* BXT_DE_PLL_ENABLE: on, locked, ratio */
    return (p >> 30) == 3 ? (int)((p & 0xFF) * ref / d[rd(0x46000) >> 22 & 3]) : ref / 2;
}
static int cdclk_set(int cd)
{
    int r = -1;
    for (int t = 0; t < 20 && !(r >= 0 && r & 1); t++) if (!((r = pcode(7, 3)) >= 0 && r & 1)) us(1000);   /* prepare -> ready */
    if (!(r >= 0 && r & 1)) return lg("pcode refused the CDCLK change\n");
    rmw(0x46070, 1u << 31, 0);
    if (poll(0x46070, 1u << 30, 0, 2)) return lg("CDCLK PLL stays locked\n");
    wr(0x46070, cd * 2 / ref);
    wr(0x46070, cd * 2 / ref | 1u << 31);
    if (poll(0x46070, 1u << 30, 1u << 30, 2)) return lg("CDCLK PLL does not lock\n");
    wr(0x46000, 7u << 19 | (cd - 750) / 500);           /* CDCLK_CTL: divider 1, no pipe sync, frequency */
    if (pcode(7, 3) < 0) lg("pcode refused the voltage level\n");   /* level 3: 652.8 MHz */
    lg("CDCLK now %d kHz\n", cdclk_now());
    return 0;
}

/* ---- The picture on o's pipe: PW x PH, fitted in the mode by scaler 1; plane 1. */
static uint16_t phase(int src, int dst)                  /* (skl_scaler_calc_phase, no chroma) */
{
    int p = -0x8000 + (int)(((long)src << 16) / dst) / 2;
    return p < 0 ? (uint16_t)((p + 0x10000) >> 2 & 0xFFFE) : (uint16_t)((p >> 2 & 0xFFFE) | 1);
}
static void source(out_t *o)
{
    uint32_t T = o->pipe * 0x1000, S = 0x68180 + o->pipe * 0x800;
    int w = o->m.hd, h = o->m.vd;
    if ((long)PW * h > (long)PH * w) h = (int)((long)PH * w / PW); else w = (int)((long)PW * h / PH);
    wr(T + 0x6001C, (PW - 1) << 16 | (PH - 1));          /* PIPESRC */
    if (PW == o->m.hd && PH == o->m.vd) { wr(S, 0); wr(S - 0x10, 0); wr(S - 0xC, 0); return; }
    wr(S, 1u << 31);                                     /* PS_CTRL: on, the pipe's, normal, medium filter */
    wr(S + 8, phase(PH, h)); wr(S + 0x14, phase(PW, w)); /* vertical, horizontal phase */
    wr(S - 0x10, (o->m.hd - w) / 2 << 16 | (o->m.vd - h) / 2);   /* window position, size (arms it) */
    wr(S - 0xC, w << 16 | h);
}
static void plane(out_t *o)
{
    uint32_t T = o->pipe * 0x1000;
    wr(T + 0x70188, stride / 64);                        /* PLANE_STRIDE: 64-byte units (linear) */
    wr(T + 0x7018C, 0);
    wr(T + 0x70190, (PH - 1) << 16 | (PW - 1));          /* PLANE_SIZE */
    wr(T + 0x701A4, 0);
    wr(T + 0x701CC, 1u << 13);                           /* PLANE_COLOR_CTL: plane gamma off */
    wr(T + 0x70180, 1u << 31 | 4u << 24);                /* PLANE_CTL: on, XRGB 8888 (0x00RRGGBB), linear */
    wr(T + 0x7019C, surf);                               /* PLANE_SURF: arms the plane */
}

/* ---- Sound to a monitor (intel_audio.c hsw_audio_codec_enable): the
 * transcoder's audio on, its N/CTS (DP: Maud/Naud) left to the hardware,
 * from the pixel clock's class; the ELD into the HDMI codec (which tells the
 * HDA side: "ELD valid" on the pin device of this pipe). HDMI: in HDMI mode
 * (not DVI) with an AVI infoframe (RGB, full range, no VIC). */
static void audio(out_t *o)
{
    static const int hz[14] = { 25175, 25200, 27000, 27027, 54000, 54054, 74176, 74250, 148352, 148500, 296703, 297000, 593407, 594000 };
    uint32_t t = o->pipe, A = 0x65000 + t * 0x100, w;
    int k = 1;                                           /* (25.2 MHz if none: bspec's default) */
    for (int i = 0; i < 14; i++) if (o->m.clock - hz[i] < 10 && hz[i] - o->m.clock < 10) k = i;
    rmw(0x650C0, 1u << 4 * t, 4u << 4 * t);              /* AUD_PIN_ELD_CP_VLD: audio out on, ELD not valid */
    rmw(A, 0x3FFFFFF0, o->kind == 'h' ? (uint32_t)k << 16 : 1u << 29);   /* AUD_CFG: pixel clock (HDMI) or DP; N by the hardware */
    rmw(A + 0xB4, 0x1F << 5, 0);                         /* AUD_DIP_ELD_CTRL: from the ELD's start */
    for (int i = 0; i < ELD_MAX; i += 4) { memcpy(&w, o->eld + i, 4); wr(A + 0x50, w); }   /* AUD_EDID_DATA */
    rmw(0x650C0, 0, 1u << 4 * t);                        /* ELD valid */
    lg("pipe %c: sound on (AUD_CFG %08x, ELD_CP_VLD %08x)\n", 'A' + t, rd(A), rd(0x650C0));
}
static void avi(uint32_t T)                              /* (hsw_write_infoframe) */
{
    rmw(T + 0x60200, 1u << 12, 0);                       /* VIDEO_DIP_CTL: AVI off while written */
    for (int i = 0; i < 8; i++) wr(T + 0x60220 + 4 * i, i ? i == 1 ? 0x00081057 : 0 : 0x000D0282);   /* type 82 v2, 13 bytes; sum; A0, R = 8 */
    rmw(T + 0x60200, 0, 1u << 12);
}

/* ---- One output on (the PRM's "Enable sequence" for DP / HDMI). */
static int enable(out_t *o, int mask)
{
    int p = o->pipe, dp = o->kind != 'h', ph = o->port ? o->port - 2 : 0;
    uint32_t T = p * 0x1000, B = 0x64000 + o->port * 0x100, e = pll_en(p), c0, c1, cf = p < 2 ? 0x164284 + 8 * p : p == 2 ? 0x164294 : 0x1642C0;
    const mode_t *m = &o->m;
    if (pll(o->clk, dp, &c0, &c1)) return lg("no DPLL setting for %d kHz\n", o->clk);
    rmw(0x45454, 0, 2u << 2 * o->port);                  /* 1. PWR_WELL_CTL_DDI2: the DDI's IO power */
    if (poll(0x45454, 1u << 2 * o->port, 1u << 2 * o->port, 2)) return lg("DDI %d IO power well\n", o->port);
    if (o->kind == 'e' && panel(1)) return -1;           /* 2. panel power */
    rmw(e, 0, 1u << 27);                                 /* 4. DPLL: power, dividers, on, lock */
    if (poll(e, 1u << 26, 1u << 26, 2)) return lg("DPLL %d power\n", p);
    wr(cf, c0); wr(cf + 4, c1); rd(cf + 4);
    rmw(e, 0, 1u << 31);
    if (poll(e, 1u << 30, 1u << 30, 5)) return lg("DPLL %d does not lock (CFGCR %08x %08x)\n", p, c0, c1);
    lg("DPLL %d: %d kHz, CFGCR0 %08x CFGCR1 %08x\n", p, o->clk, c0, c1);
    uint32_t k = ph < 3 ? 0x164280 : 0x1642BC;           /* DPCLKA_CFGCR: this DPLL to the DDI, then ungated (two writes) */
    rmw(k, 3u << ph % 3 * 2, (uint32_t)p << ph % 3 * 2);
    rmw(k, 1u << clkoff[ph], 0);
    wr(0x46140 + 4 * p, (o->port + 1) << 28);            /* 7.a TRANS_CLK_SEL: the port's clock */
    uint32_t f = (o->port + 1) << 27 | (m->flags & 2) << 16 | (m->flags & 1) << 16 | (dp ? 2u << 24 | (o->lanes - 1) << 1 : o->eldn ? 0 : 1u << 24);
    wr(T + 0x60400, f);                                  /* 7.b TRANS_DDI_FUNC_CTL: port, DP SST / HDMI (sound) or DVI, 8 bpc, lanes */
    if (dp && train(o)) return -1;                       /* 7.c-7.k */
    if (dp) {                                            /* M/N: 24-bit pixels over the link; TU 64 */
        uint64_t N = 0x800000, M = (uint64_t)m->clock * 3 * N / ((uint64_t)o->clk * o->lanes);
        wr(T + 0x60030, 63u << 25 | (uint32_t)M); wr(T + 0x60034, N);
        wr(T + 0x60040, (uint32_t)((uint64_t)m->clock * 0x80000 / o->clk)); wr(T + 0x60044, 0x80000);
    }
    wr(T + 0x60000, (m->ht - 1) << 16 | (m->hd - 1));    /* transcoder timings */
    wr(T + 0x60004, (m->ht - 1) << 16 | (m->hd - 1));
    wr(T + 0x60008, (m->hse - 1) << 16 | (m->hss - 1));
    wr(T + 0x6000C, (m->vt - 1) << 16 | (m->vd - 1));
    wr(T + 0x60010, (m->vt - 1) << 16 | (m->vd - 1));
    wr(T + 0x60014, (m->vse - 1) << 16 | (m->vss - 1));
    wr(T + 0x60028, 0); wr(T + 0x6002C, 0);              /* no vsync shift, no pixel multiplier */
    wr(T + 0x70030, 1u << 8);                            /* PIPE_MISC: 8 bpc, pixel rounding truncate */
    rmw(T + 0x70038, 0, 1u << 15 | 1u << 7);             /* PIPE_CHICKEN: frame buffer pixels through unchanged */
    wr(T + 0x70008, 0);                                  /* TRANSCONF: progressive (not on yet) */
    source(o);
    wr(0x45270 + 4 * p, (uint32_t)(((long)m->ht * 8000 + m->clock - 1) / m->clock) & 0x1FF);   /* WM_LINETIME: 1/8 us */
    wr(T + 0x7003C, 16u << 20 | 1u << 17 | 1u << 16 | 2u << 14 | 12u << 8 | 2);   /* PIPE_MBUS_DBOX_CTL (tgl credits) */
    /* DBUF share (tgl_allowed_dbufs: S1 = blocks 0-1023, S2 = 1024-2047), watermarks:
     * level 0 (and SAGV) as large as the share, so the plane always fetches early */
    int n = (mask & 1) + (mask >> 1 & 1) + (mask >> 2 & 1), s0 = 0, s1 = 2048;
    if (n > 1) { if (p == 2 || (p == 0 && mask == 3)) s0 = 1024; else { s1 = 1024; if (n == 3) { if (p) s0 = 512; else s1 = 512; } } }
    wr(T + 0x7027C, (uint32_t)(s1 - 1) << 16 | s0);      /* PLANE_BUF_CFG */
    uint32_t wm = 1u << 31 | 1u << 30 | (s1 - s0 - 1);   /* enabled, lines ignored, blocks */
    wr(T + 0x70240, wm); wr(T + 0x70258, wm);
    for (int l = 1; l < 8; l++) wr(T + 0x70240 + 4 * l, 0);
    wr(T + 0x70268, 0); wr(T + 0x7025C, 0); wr(T + 0x7017C, 0);
    if (!dp && o->eldn) avi(T);
    wr(T + 0x60404, 0); wr(T + 0x60400, f | 1u << 31);  /* the DDI function on, then the transcoder */
    rmw(T + 0x70008, 0, 1u << 31);
    if (poll(T + 0x70008, 1u << 30, 1u << 30, 100)) return lg("pipe %c does not start\n", 'A' + p);
    if (!dp) {                                           /* HDMI: swing, lanes, then the TMDS buffer */
        levels(o, 0, 0);
        wr(B, 1u << 31 | o->rev << 16 | (o->port ? 0 : a4));
        if (poll(B, 1u << 7, 0, 10)) return lg("DDI %d buffer stays idle\n", o->port);
    }
    plane(o);
    if (o->kind == 'e') backlight(1);
    rmw(0x44404 + 0x10 * p, 1u << 31, 0); wr(0x44408 + 0x10 * p, 1u << 31);   /* FIFO underruns: unmasked, cleared */
    us(50000);
    uint8_t s[3] = { 0 };
    if (dp) dpcd_rd(o, 0x202, s, 3);
    lg("pipe %c on: port %d, %s, link status %02x %02x %02x\n", 'A' + p, o->port,
       rd(0x44408 + 0x10 * p) & 1u << 31 ? "FIFO UNDERRUN" : "no underrun", s[0], s[1], s[2]);
    rmw(0x44404 + 0x10 * p, 0, 1u << 31);
    if (o->eldn) audio(o);
    o->on = 1;
    return 0;
}

/* ---- The registers, for the log. */
static void dump(const char *when)
{
    static const uint32_t r[] = { 0x45404, 0x45444, 0x45454, 0x42000, 0x45504, 0x44FE8, 0x44300, 0x46000, 0x46070, 0x51004,
        0x46010, 0x46014, 0x46018, 0x46030, 0x164280, 0x1642BC, 0x164284, 0x164288, 0x16428C, 0x164290, 0x164294, 0x164298,
        0x64000, 0x64300, 0x64400, 0x64500, 0x64600, 0xC7200, 0xC7204, 0xC7208, 0xC720C, 0xC8250, 0xC8254, 0xC8258, 0xC8350, 0xC8354,
        0x162100, 0x6C100, 0x160100, 0x161100, 0x16B100, 0x162028, 0x6C028, 0x160028, 0x161028, 0x16B028, 0xC8358, 0x7003C };
    static const uint32_t t[] = { 0x60400, 0x70008, 0x60000, 0x60008, 0x6000C, 0x60014, 0x6001C, 0x60030, 0x60034, 0x60040, 0x60044,
        0x60540, 0x70030, 0x70180, 0x70188, 0x70190, 0x7019C, 0x7027C, 0x70240, 0x46140 };
    lg("registers %s:\n", when);
    for (unsigned i = 0; i < sizeof r / 4; i += 4)
        lg("  %06x %08x  %06x %08x  %06x %08x  %06x %08x\n", r[i], rd(r[i]), r[i + 1], rd(r[i + 1]), r[i + 2], rd(r[i + 2]), r[i + 3], rd(r[i + 3]));
    for (int p = 0; p < 4; p++) {
        if (!(rd(p * 0x1000 + 0x60400) | rd(p * 0x1000 + 0x70008))) continue;
        uint32_t v[20];
        for (int i = 0; i < 20; i++) v[i] = rd(t[i] == 0x46140 ? t[i] + 4 * p : t[i] + p * 0x1000);
        for (int i = 0; i < 20; i += 10)
            lg("  %c%d: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n", 'A' + p, i / 10, v[i], v[i + 1], v[i + 2], v[i + 3],
               v[i + 4], v[i + 5], v[i + 6], v[i + 7], v[i + 8], v[i + 9]);
    }
}

/* ---- Everything on: the firmware's set-up off, then ours -> 0 */
static int modeset(void)
{
    int mask = 0;
    for (int i = 0; i < nout; i++) if (out[i].m.clock) mask |= 1 << out[i].pipe;
    if (rd(0xC8250) & 1u << 31 || rd(0xC8350) & 1u << 31) backlight(0);
    for (int t = 0; t < 4; t++) pipe_off(t);
    if (rd(0xC7204) & 1 && panel(0)) lg("panel: power stays on\n");
    for (int id = 0; id < 4; id++) pll_off(id);
    for (int i = 1; i <= 3; i++) {                       /* PW2-PW4: request, state, fuses distributed (PG2-PG4) */
        rmw(0x45404, 0, 2u << 2 * i);
        if (poll(0x45404, 1u << 2 * i, 1u << 2 * i, 2) || poll(0x42000, 1u << (26 - i), 1u << (26 - i), 2)) return lg("power well %d\n", i + 1);
    }
    wr(0x45504, 0);                                      /* DC_STATE_EN: no DC5/DC6 while we drive it */
    rmw(0x44FE8, 0, 1u << 31); rmw(0x44300, 0, 1u << 31);   /* DBUF slices S1, S2 */
    if (poll(0x44FE8, 1u << 30, 1u << 30, 2) || poll(0x44300, 1u << 30, 1u << 30, 2)) return lg("DBUF power\n");
    int snd = 0;
    for (int i = 1; i < nout; i++) snd |= out[i].m.clock && out[i].eldn;
    if (snd) {                                           /* display audio (i915_audio_component_get_power, codec_wake_override) */
        wr(0x65900, 0x8010);                             /* AUD_FREQ_CNTRL: 8T, 96 MHz BCLK (AUD_FREQ_GEN12) */
        rmw(0x48414, 0, 1u << 31);                       /* AUD_PIN_BUF_CTL: on */
        rmw(0x65F10, 1u << 15, 0); us(1000);             /* AUD_CHICKENBIT: the codec's wake signal, left on, */
        rmw(0x65F10, 0, 1u << 15); us(1000);             /* so the HDA link finds the HDMI codec after its resets */
        lg("display audio on: AUD_FREQ_CNTRL %08x, AUD_PIN_BUF_CTL %08x, AUD_CHICKENBIT %08x\n", rd(0x65900), rd(0x48414), rd(0x65F10));
    }
    int nd = 0, cd = cdclk_now(), hbr3 = 0;
    for (int i = 0; i < nout; i++) {
        if (!out[i].m.clock) continue;
        if (need(&out[i].m) > nd) nd = need(&out[i].m);
        hbr3 |= out[i].clk > 594000 && out[i].kind != 'h';   /* (a port clock above 594 MHz needs voltage level 2) */
    }
    lg("CDCLK %d kHz, needs %d kHz%s\n", cd, nd, hbr3 ? " (and level 2 for the link)" : "");
    if ((nd > cd || (hbr3 && cd <= 326400)) && cdclk_set(cdmax)) return -1;
    if (enable(&out[0], mask)) return -1;
    for (int i = 1; i < nout; i++)
        if (out[i].m.clock && enable(&out[i], mask)) { lg("port %d left off\n", out[i].port); pipe_off(out[i].pipe); pll_off(out[i].pipe); }
    return 0;
}

/* Awake after a suspend: the display lost everything; set up again as at
 * the start (the state found then kept) -> 0; -1: not ours. */
int screen_wake(void)
{
    if (!active) return -1;
    lg("awake: modeset again\n");
    int r = modeset();
    dump(r ? "after waking: FAILED" : "after waking");
    save();
    return r;
}

int igfx_init(const mk_screen_t *g)                      /* -> 0: ours; -1: not here (or failed); 1: ours, PW x PH refused */
{
    if (active) {
        if (!PW) { PW = fbw; PH = fbh; }
        if (PW > fbw || PH > fbh) return 1;
        for (int i = 0; i < nout; i++) if (out[i].on) { source(&out[i]); plane(&out[i]); }
        return 0;
    }
    uint32_t id = pci_read(2 << 11);                     /* bus 0, device 2: the integrated graphics */
    if (failed || (id & 0xFFFF) != 0x8086 || ((id >> 16 & 0xFFF0) != 0xA780 && (id >> 16 & 0xFFE0) != 0x4680)) return -1;
    failed = 1;
    size_t ol = 0;
    char *old = file_get("/etc/igfx.log", &ol);
    int hung = old && ol >= sizeof MARK - 1 && !memcmp(old + ol - (sizeof MARK - 1), MARK, sizeof MARK - 1);
    free(old);
    lg("Intel display %04x:%04x, firmware screen %ux%u (pitch %u) at %lx\n", id & 0xFFFF, id >> 16, g->width, g->height, g->pitch, (unsigned long)g->pa);
    if (hung) { lg("the last modeset never ended: the firmware's screen this time (remove /etc/igfx.log to try again)\n"); return -1; }
    uint64_t bar0 = (pci_read(2 << 11 | 0x10) & ~0xFUL) | (uint64_t)pci_read(2 << 11 | 0x14) << 32;
    uint64_t bar2 = (pci_read(2 << 11 | 0x18) & ~0xFUL) | (uint64_t)pci_read(2 << 11 | 0x1C) << 32;
    mm = map_phys(bar0, 4 << 20);                        /* the registers (the GGTT is at +8 MiB: unused) */
    if ((long)mm <= 0) return lg("BAR0 %lx: no mapping\n", (unsigned long)bar0), save(), -1;
    dump("as the firmware left them");
    static const int refs[4] = { 24000, 19200, 38400, 24000 };
    ref = refs[rd(0x51004) >> 29 & 3];                   /* SKL_DSSM: the reference clock */
    cdmax = ref == 24000 ? 648000 : 652800;
    a4 = rd(0x64000) & 1u << 4;                          /* (DDI A's 4 lanes, as the firmware set it) */
    for (int t = 0; t < 4 && !surf; t++)                 /* the firmware's picture: the panel's plane 1 */
        if ((rd(t * 0x1000 + 0x60400) >> 27 & 15) == 1 && rd(t * 0x1000 + 0x70180) & 1u << 31) {
            surf = rd(t * 0x1000 + 0x7019C) & ~0xFFFu;
            stride = (rd(t * 0x1000 + 0x70188) & 0x7FF) * 64;
        }
    fbw = g->width; fbh = g->height;
    lg("firmware plane: GGTT %x, stride %d; aperture %lx\n", surf, stride, (unsigned long)bar2);
    if (!stride || bar2 + surf != g->pa || stride != (int)g->pitch * 4) return lg("the firmware's screen is not its plane: not touched\n"), save(), -1;
    /* the panel's power sequence and backlight: as the firmware set them, or the VBT's if longer */
    uint32_t on = rd(0xC7208), off = rd(0xC720C), pc = rd(0xC7204) >> 4 & 31;
    t_up = on >> 16 & 0x1FFF; t8 = (on & 0x1FFF) / 10; t_down = off >> 16 & 0x1FFF; t9 = (off & 0x1FFF) / 10; t_cycle = pc ? (pc - 1) * 1000 : 0;
    blfw = rd(0xC8250) >> 31 | (rd(0xC8350) >> 31) << 1;   /* (the controller the firmware lit) */
    blc = blfw == 2;
    out[0] = (out_t){ .kind = 'e' };
    nout = 1;
    vbt();
    if (!(blmax = rd(BLC(4))) && blhz) blmax = (rd(0xC2014) & 1u << 8 ? 24000000 : 19200000) / blhz;   /* (raw clock: SFUSE_STRAP) */
    if (blfw && blmax) bright = (int)((long)rd(BLC(8)) * 100 / blmax);
    size_t cl = 0;
    char *conf = file_get("/etc/display.conf", &cl);     /* the brightness chosen in Settings */
    for (char *c = conf; c && *c; c = strchr(c, '\n') ? strchr(c, '\n') + 1 : c + strlen(c))
        if (!memcmp(c, "brightness=", 11)) for (c += 11, bright = 0; *c >= '0' && *c <= '9'; c++) bright = bright * 10 + *c - '0';
    free(conf);
    if (bright < 1 || bright > 100) bright = 100;
    if (!t_up) t_up = 2100;
    if (!t_down) t_down = 5000;
    if (!t_cycle) t_cycle = 5100;
    if (!t8) t8 = 50;
    if (!t9) t9 = 50;
    lg("panel delays (100 us): up %d, down %d, cycle %d; backlight on %d ms, off %d ms; PWM %d on controller %d, min %d/255\n",
       t_up, t_down, t_cycle, t8, t9, blmax, blc, blmin);
    lg("reference %d kHz, CDCLK %d kHz (max %d)\n", ref, cdclk_now(), cdmax);
    for (int p = 0; p < 5; p++) if (!(rd(phys[p] + 0x100) & 1u << 31)) lg("combo PHY %c not initialized by the firmware\n", 'A' + p);
    if (probe(&out[0], 0) || pick(&out[0], modes[0], nmodes[0], 0)) return lg("the panel cannot be set up: firmware screen kept\n"), save(), -1;
    if (!PW) native(&PW, &PH);
    if (PW > fbw || PH > fbh) { PW = fbw; PH = fbh; }
    int k = 1;
    for (int i = 1; i < nout; i++) {                     /* monitors: those that answer, two at most */
        out[i].pipe = k;
        if (k < 3 && rd(phy(out[i].port) + 0x100) >> 31 && !probe(&out[i], i) && !pick(&out[i], modes[i], nmodes[i], 0)) k++;
        else out[i].m.clock = 0;
    }
    wr(0xC7208, (uint32_t)t_up << 16 | 1);               /* PP_ON/OFF_DELAYS (backlight delays: ours, by hand) */
    wr(0xC720C, (uint32_t)t_down << 16 | 1);
    rmw(0xC7204, 31 << 4, (uint32_t)((t_cycle + 999) / 1000 + 1) << 4);
    lg("PW %d x PH %d\n", PW, PH);
    unsigned keep = ln;
    lg(MARK);
    save();
    ln = keep;                                           /* (the marker stays only in the file) */
    int r = modeset();
    if (r) {
        lg("modeset failed: again with the panel's slowest mode, no monitors\n");
        dump("after the failure");
        nout = 1;
        r = pick(&out[0], modes[0], nmodes[0], 1) || modeset();
    }
    dump("now");
    active = !r;
    lg(r ? "FAILED: the screen may stay dark; the next start keeps the firmware's screen\n" MARK : "done\n");
    save();
    return r ? -1 : 0;
}
#endif
