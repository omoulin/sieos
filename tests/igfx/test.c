/*
 * tests/igfx/test.c - the Intel display's monitor hot-plug (user/atlas/igfx.c:
 * screen_plug) on the host, against a fake display engine: registers that
 * answer as the hardware's do (power wells, DPLLs, transcoders, DDI buffers,
 * pcode), the PCH's hot-plug pins (SDEISR), DP AUX and GMBUS to fake
 * monitors (two real EDIDs: tests/hdmi/edid.h). The panel runs on pipe A as
 * Atlas's start left it. Checked: a DP monitor plugged in, an HDMI one on a
 * DP++ port that does not answer at first (a bounce), two monitors, one
 * unplugged, the other port given a DP monitor again (DP first), no third
 * pipe, everything unplugged; each time the pipes, DPLLs, sound (ELD valid)
 * and DBUF shares (tgl_allowed_dbufs); never two running pipes on the same
 * DBUF blocks, never a write that changes the panel's pipe, port or clocks.
 * Then the same on an Ice Lake (display 11): the panel on the eDP
 * transcoder, a DP monitor in DP-alt mode on a Type-C port (MG PLL, the FIA's
 * lanes and PHY ownership), an HDMI one on DDI B (DPLL 1), two Thunderbolt
 * monitors sharing the TBT PLL, a DP-alt port with two lanes; ICL's DBUF
 * shares (icl_allowed_dbufs).
 *
 * Usage: test [-v] (-v: igfx's log too).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "../hdmi/edid.h"

static uint32_t R[1 << 20];                      /* the registers (BAR0's first 4 MiB) */
static uint32_t rd(uint32_t r);
static void wr(uint32_t r, uint32_t v);
int PW = 1920, PH = 1080, SW, SH;
#include "../../user/atlas/igfx.c"

static int fails, watch, verbose, gpin, gidx;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static struct { const uint8_t *edid; int dp, mute, off; } sink[7];   /* per port (ADL-S 3-6: TC1-TC4; ICL 1: B, 2-5: TC1-4); mute: does not answer yet */

/* ---- What igfx.c takes from SIEOS. */
static long now;
long sys_clock(void) { return now += 1000; }
long sys_sleep(uint64_t ns) { now += ns; return 0; }
long sys_debug(const char *s, size_t n) { if (verbose) fwrite(s, 1, n, stdout); return 0; }
void *map_phys(uint64_t pa, size_t size) { (void)pa; (void)size; return 0; }
uint32_t pci_read(uint32_t a) { (void)a; return 0; }
long fs_open(const char *path, int flags, int mode) { (void)path; (void)flags; (void)mode; return -1; }
long fs_write(long h, uint64_t off, const void *buf, size_t n) { (void)h; (void)off; (void)buf; return n; }
long fs_close(long h) { (void)h; return 0; }
char *file_get(const char *path, size_t *len) { (void)path; (void)len; return 0; }
void vformat(void (*put)(void *ctx, char c), void *ctx, const char *fmt, va_list ap)
{
    char b[512];
    vsnprintf(b, sizeof b, fmt, ap);
    for (char *p = b; *p; p++) put(ctx, *p);
}

/* ---- The fake display engine. */
static int runs_(int p) { return R[(icl && !p ? 0x7F008 : p * 0x1000 + 0x70008) >> 2] >> 31; }
static uint32_t cfg(int p) { return R[(p * 0x1000 + 0x7027C) >> 2]; }
static void overlap(void)                        /* two running pipes on the same DBUF blocks? */
{
    for (int p = 0; p < 3; p++)
        for (int q = p + 1; q < 3; q++)
            if (runs_(p) && runs_(q) && (cfg(p) & 0x7FF) <= (cfg(q) >> 16 & 0x7FF) && (cfg(q) & 0x7FF) <= (cfg(p) >> 16 & 0x7FF)) {
                printf("FAIL: pipes %c and %c share DBUF blocks (%08x, %08x)\n", 'A' + p, 'A' + q, cfg(p), cfg(q));
                fails++;
            }
}
static int panel_reg(uint32_t r)                 /* the panel's pipe, port, clocks (its DBUF share and watermarks may change) */
{
    if (r == 0x7027C || (r >= 0x70240 && r <= 0x7025C) || r == 0x70268 || r == 0x7017C) return 0;
    return (r >= 0x60000 && r < 0x61000) || (r >= 0x70000 && r < 0x71000) || (r >= 0x68000 && r < 0x68400) || r == 0x64000 ||
           r == 0x46010 || r == 0x164284 || r == 0x164288 || r == 0x46140 || r == 0x45270 || (r >= 0xC7200 && r < 0xC7210) ||
           (r >= 0xC8250 && r < 0xC8360) || r == 0x46000 || r == 0x46070 || (r >= 0x162000 && r < 0x163000) ||
           (r >= 0x6F000 && r < 0x70000) || r == 0x7F008 || r == 0x164000 || r == 0x164004 || r == 0x64040;   /* (ICL's eDP transcoder, DPLL 0) */
}
static uint8_t dpcd(unsigned a)
{
    static const uint8_t caps[16] = { 0x12, 0x14, 0x84, 0x01 }, st[6] = { 0x77, 0x77, 0x81 };   /* DP 1.2, HBR2, 4 lanes; trained at once */
    return a < 16 ? caps[a] : a >= 0x202 && a < 0x208 ? st[a - 0x202] : 0;
}
static void faux(uint32_t c, uint32_t v)         /* a DP AUX request to the sink of channel = port */
{
    int ch = (c - 0x64010) >> 8, tn = v >> 20 & 31, n = 0;
    uint8_t tx[20], rx[20];
    for (int i = 0; i < tn; i++) tx[i] = R[(c + 4 + (i & ~3)) >> 2] >> (24 - 8 * (i & 3));
    v &= ~(1u << 31 | 1u << 30 | 1u << 28 | 1u << 25 | 31u << 20);   /* (busy; done, timeout, error: written 1 to clear) */
    if (ch > 6 || !sink[ch].edid || !sink[ch].dp || sink[ch].mute) { R[c >> 2] = v | 1u << 30 | 1u << 28; return; }
    int cmd = tx[0] >> 4, len = tn > 3 ? tx[3] + 1 : 0;
    unsigned a = (tx[0] & 15u) << 16 | tx[1] << 8 | tx[2];
    rx[n++] = 0;                                 /* ACK */
    if (cmd == 9) for (int k = 0; k < len; k++) rx[n++] = dpcd(a + k);
    else if (cmd == 5 || cmd == 1) for (int k = 0; k < len; k++) rx[n++] = sink[ch].edid[sink[ch].off++ & 255];
    else if ((cmd == 4 || cmd == 0) && len) sink[ch].off = tx[4];
    for (int i = 0; i < n; i += 4) {
        uint32_t w = 0;
        for (int k = 0; k < 4; k++) w = w << 8 | (i + k < n ? rx[i + k] : 0);
        R[(c + 4 + i) >> 2] = w;
    }
    R[c >> 2] = v | 1u << 30 | (uint32_t)n << 20;
}
static int gsink(void) { int p = !icl ? gpin - 6 : gpin == 2 ? 1 : gpin - 7; return p >= 1 && p <= 6 && sink[p].edid && !sink[p].dp && !sink[p].mute ? p : 0; }   /* (GMBUS pin 9 = TC1; ICL 2 = B) */
static uint32_t rd(uint32_t r)
{
    if (r == 0xC5108) return gsink() ? 1u << 11 | 1u << 14 : 1u << 10;   /* GMBUS2: ready, wait phase / NAK */
    if (r == 0xC510C) { uint32_t w = 0; for (int k = 0; gsink() && k < 4; k++) w |= (uint32_t)sink[gsink()].edid[gidx++ & 255] << 8 * k; return w; }
    if ((r & 0xFFFF0FFF) == 0x60544 || (r & 0xFFFFF0FF) == 0x64044) return 1u << 25;   /* DP_TP_STATUS: idle patterns sent */
    return R[r >> 2];
}
static void wr(uint32_t r, uint32_t v)
{
    if (watch && panel_reg(r) && R[r >> 2] != v) { printf("FAIL: the panel's register %06x written: %08x -> %08x\n", r, R[r >> 2], v); fails++; }
    R[r >> 2] = v;
    if ((r >= 0x46010 && r <= 0x4603C && !(r & 3)) || r == 0x46070) R[r >> 2] = (v & ~0x44000000u) | (v >> 1 & 0x44000000u);   /* PLLs: on, locked */
    else if (r == 0x45404 || r == 0x45444 || r == 0x45454) R[r >> 2] = (v & 0xAAAAAAAAu) | (v >> 1 & 0x55555555u);   /* power wells */
    else if (((r & 0xFFFF0FFF) == 0x70008 && r < 0x74000) || r == 0x7F008) { R[r >> 2] = (v & ~(1u << 30)) | (v >> 1 & 1u << 30); overlap(); }   /* TRANSCONF */
    else if ((r & 0xFFFF0FFF) == 0x7027C) overlap();
    else if (r >= 0x64000 && r <= 0x64600 && !(r & 0xFF)) R[r >> 2] = (v & ~0x80u) | (v >> 31 ? 0 : 0x80);   /* DDI_BUF_CTL: idle */
    else if (r >= 0x64010 && r < 0x64A00 && (r & 0xFF) == 0x10 && v >> 31) faux(r, v);
    else if (r == 0x138124) { R[r >> 2] = 0; R[0x138128 >> 2] = 1; }   /* pcode: done, "ready" */
    else if (r == 0xC5100) gpin = v & 31;
    else if (r >= 0x44408 && r < 0x44440) R[r >> 2] = 0;   /* (FIFO underruns: none, written 1 to clear) */
    else if (r == 0xC5104 && v >> 30 & 1) gidx = 0;
}

/* ---- Monitors in and out. */
static void plug(int port, const uint8_t *edid, int dp, int mute)
{
    sink[port].edid = edid; sink[port].dp = dp; sink[port].mute = mute;
    R[0xC4000 >> 2] |= 1u << (21 + port);
}
static void unplug(int port) { sink[port].edid = 0; R[0xC4000 >> 2] &= ~(1u << (21 + port)); }
static void plug_tc(int port, const uint8_t *edid, int mode, int lanes)   /* ICL Type-C: 5 DP-alt, 6 TBT (FIA DFLEXDPSP) */
{
    sink[port].edid = edid; sink[port].dp = 1; sink[port].mute = 0;
    R[0x1638A0 >> 2] |= (uint32_t)(1 << mode | lanes) << 8 * (port - 2);
}
static void unplug_tc(int port) { sink[port].edid = 0; R[0x1638A0 >> 2] &= ~(0xFFu << 8 * (port - 2)); }
static int lit(int i, int pipe)                  /* out[i] on pipe: running, its PLL locked, the port its transcoder's */
{
    uint32_t f = R[(pipe * 0x1000 + 0x60400) >> 2];
    return out[i].on && out[i].pipe == pipe && runs_(pipe) && R[pll_of(&out[i]) >> 2] >> 30 & 1 &&
           (icl ? f >> 28 & 7 : (f >> 27 & 15) - 1) == (uint32_t)out[i].port;
}
static int dark_(int pipe, uint32_t e) { return !runs_(pipe) && !(R[e >> 2] >> 31) && !(R[0x650C0 >> 2] >> 4 * pipe & 5) && !R[(pipe * 0x1000 + 0x60400) >> 2]; }
#define dark(pipe) dark_(pipe, pll_en(pipe))
static int sound(int pipe) { return (R[0x650C0 >> 2] >> 4 * pipe & 5) == 5; }   /* audio out on, ELD valid */
#define SHARE(p, s0, s1) (cfg(p) == ((uint32_t)(s1) << 16 | (s0)))

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    /* As igfx_init leaves it: the panel on pipe A (DDI A, DPLL 0) with all the DBUF; CDCLK 307.2 MHz. */
    R[0x70008 >> 2] = 0xC0000000; R[0x7027C >> 2] = 0x07FF0000; R[0x60400 >> 2] = 1u << 31 | 1u << 27 | 2u << 24;
    R[0x7019C >> 2] = surf = 0x100000; R[0x70180 >> 2] = 1u << 31 | 4u << 24; R[0x46010 >> 2] = 0xCC000000;
    R[0x64000 >> 2] = 1u << 31; R[0x46070 >> 2] = 3u << 30 | 16; R[0x45404 >> 2] = 0xFF;
    for (int p = 1; p < 5; p++) R[(phys[p] + 0x100) >> 2] = 1u << 31;   /* combo PHYs B-E initialized */
    ref = 38400; cdmax = 652800; fbw = 1920; fbh = 1080; stride = 7680; active = 1;
    out[0] = (out_t){ .kind = 'e', .vk = 'e', .on = 1, .m = { 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 3 } };
    for (int i = 1; i < 4; i++) out[i] = (out_t){ .port = i + 2, .kind = 'd', .vk = 'd', .aux = i + 2, .ddc = i + 8 };   /* TC1-TC3: DP++ */
    nout = 4;
    watch = 1;
    CHECK(screen_hpd() == 0);
    screen_plug();                               /* nothing plugged: nothing done */
    CHECK(!out[1].on && !out[2].on && SHARE(0, 0, 2047));

    /* 1. The Dell (DP, 4K, speakers) on TC1: pipe B, the panel moved to S2. */
    plug(3, dell, 1, 0);
    CHECK(screen_hpd() == 1 << 3);
    screen_plug();
    CHECK(lit(1, 1) && out[1].kind == 'd' && out[1].m.hd == 3840 && out[1].lanes == 4 && out[1].clk == 540000);
    CHECK(sound(1) && SHARE(0, 1024, 2047) && SHARE(1, 0, 1023));

    /* 2. The Samsung (HDMI, speakers) on TC2, a DP++ port: its pin up before
     * it answers (a bounce): nothing; then it answers: pipe C, HDMI mode. */
    plug(4, samsung, 0, 1);
    screen_plug();
    CHECK(!out[2].on && dark(2) && lit(1, 1) && SHARE(0, 1024, 2047) && SHARE(1, 0, 1023));
    sink[4].mute = 0;
    screen_plug();
    CHECK(lit(2, 2) && out[2].kind == 'h' && out[2].m.hd == 1920 && !(R[0x62400 >> 2] >> 24 & 7) && sound(2));
    CHECK(SHARE(0, 0, 511) && SHARE(1, 512, 1023) && SHARE(2, 1024, 2047) && lit(1, 1));

    /* 3. A third monitor (TC3): no pipe left. */
    plug(5, dell, 1, 0);
    screen_plug();
    CHECK(!out[3].on && strstr(lb, "no free pipe"));
    unplug(5);

    /* 4. The Dell unplugged: pipe B, DPLL 1, its sound and port off; A and C: S1 and S2. */
    unplug(3);
    screen_plug();
    CHECK(!out[1].on && dark(1) && !(R[0x45454 >> 2] & 2u << 6) && lit(2, 2) && sound(2));
    CHECK(SHARE(0, 0, 1023) && SHARE(2, 1024, 2047));

    /* 5. The Samsung unplugged, the Dell on its port: DP again (not HDMI), pipe B. */
    unplug(4);
    screen_plug();
    CHECK(dark(2) && SHARE(0, 0, 2047));
    plug(4, dell, 1, 0);
    screen_plug();
    CHECK(lit(2, 1) && out[2].kind == 'd' && sound(1) && SHARE(0, 1024, 2047) && SHARE(1, 0, 1023));

    /* 6. The Samsung on TC1 (pipe C), then both out at once. */
    plug(3, samsung, 0, 0);
    screen_plug();
    CHECK(lit(1, 2) && out[1].kind == 'h' && lit(2, 1) && SHARE(0, 0, 511) && SHARE(1, 512, 1023) && SHARE(2, 1024, 2047));
    unplug(3); unplug(4);
    screen_plug();
    CHECK(dark(1) && dark(2) && !out[1].on && !out[2].on && SHARE(0, 0, 2047));
    CHECK(runs_(0) && R[0x60400 >> 2] >> 31 && R[0x7019C >> 2] == surf);   /* the panel: still on */


    /* ---- Ice Lake. The panel on the eDP transcoder (pipe A, DDI A, DPLL 0) with DBUF slice 1; CDCLK 652.8 MHz.
     * Monitors: HDMI on DDI B (GMBUS pin 2), TC1-TC3 (ports 2-4, AUX C-E, DP). */
    watch = 0;
    memset(R, 0, sizeof R); memset(sink, 0, sizeof sink); memset(out, 0, sizeof out);
    icl = 1; ln = boot = 0;
    R[0x7F008 >> 2] = 0xC0000000; R[0x6F400 >> 2] = 1u << 31 | 2u << 24; R[0x7027C >> 2] = 0x03FF0000;
    R[0x7019C >> 2] = surf; R[0x70180 >> 2] = 1u << 31 | 4u << 24; R[0x46010 >> 2] = 0xCC000000;
    R[0x64000 >> 2] = 1u << 31; R[0x46070 >> 2] = 3u << 30 | 34; R[0x45404 >> 2] = 0xFF;
    R[(phys[1] + 0x100) >> 2] = 1u << 31;                /* combo PHY B initialized */
    R[0x163890 >> 2] = 15; R[0x163880 >> 2] = 0x433;     /* FIA: the Type-C PHYs ready; pin assignments C, C, D */
    out[0] = (out_t){ .kind = 'e', .vk = 'e', .on = 1, .m = { 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 3 } };
    out[1] = (out_t){ .port = 1, .kind = 'h', .vk = 'h', .aux = 1, .ddc = 2 };
    for (int i = 2; i < 5; i++) out[i] = (out_t){ .port = i, .kind = 'd', .vk = 'd', .aux = i, .ddc = i + 7 };
    nout = 5;
    watch = 1;
    CHECK(screen_hpd() == 0);

    /* 7. The Dell in DP-alt mode on TC1 (4 lanes): pipe B, MG PLL 1, the PHY the display's. */
    plug_tc(2, dell, 5, 15);
    CHECK(screen_hpd() == 1 << 2);
    screen_plug();
    CHECK(lit(2, 1) && out[2].tcm == 'a' && out[2].lanes == 4 && out[2].clk == 540000 && pll_of(&out[2]) == 0x46030);
    CHECK(R[0x46108 >> 2] == 8u << 28 && R[0x163894 >> 2] == 1 && (R[0x1638C0 >> 2] & 15) == 15 && (R[0x1683A0 >> 2] & 0xC0) == 0x80);
    CHECK(R[0x46144 >> 2] == 3u << 29 && R[0x64240 >> 2] >> 31 && R[0x64200 >> 2] >> 31 && !(R[0x164280 >> 2] & 1u << 12) && sound(1));
    CHECK(SHARE(0, 0, 1023) && SHARE(1, 1024, 2047));

    /* 8. The Samsung (HDMI) on DDI B: pipe C, DPLL 1; A and B share slice 1. */
    sink[1].edid = samsung; R[0xC4000 >> 2] |= 1u << 17;
    CHECK(screen_hpd() == (1 << 1 | 1 << 2));
    screen_plug();
    CHECK(lit(1, 2) && pll_of(&out[1]) == 0x46014 && (R[0x164280 >> 2] >> 2 & 3) == 1 && !(R[0x164280 >> 2] & 1u << 11) && sound(2));
    CHECK(R[0x164080 >> 2] && (R[0x164084 >> 2] & 3) == 3 && SHARE(0, 0, 511) && SHARE(1, 512, 1023) && SHARE(2, 1024, 2047));

    /* 9. TC1 unplugged: pipe B, MG PLL 1 off, the PHY given back. */
    unplug_tc(2);
    screen_plug();
    CHECK(!out[2].on && dark_(1, 0x46030) && !R[0x163894 >> 2] && !R[0x46108 >> 2] && R[0x164280 >> 2] & 1u << 12 && lit(1, 2));
    CHECK(SHARE(0, 0, 1023) && SHARE(2, 1024, 2047));

    /* 10. DDI B unplugged; two Thunderbolt monitors on TC2 and TC3: one TBT PLL for both, off with the last. */
    sink[1].edid = 0; R[0xC4000 >> 2] &= ~(1u << 17);
    screen_plug();
    CHECK(dark_(2, 0x46014) && SHARE(0, 0, 1023));
    plug_tc(3, dell, 6, 0);
    screen_plug();
    CHECK(lit(3, 1) && out[3].tcm == 't' && R[0x46020 >> 2] >> 30 & 1 && R[0x4610C >> 2] == 0xEu << 28 && R[0x64310 >> 2] & 1u << 11);
    CHECK(R[0x45444 >> 2] & 2u << 18 && !(R[0x45454 >> 2] & 2u << 6) && !R[0x163894 >> 2]);
    plug_tc(4, samsung, 6, 0);                           /* (no DP: TBT carries DP only) */
    sink[4].edid = dell;
    screen_plug();
    CHECK(lit(4, 2) && lit(3, 1) && R[0x46110 >> 2] == 0xEu << 28);
    unplug_tc(3);
    screen_plug();
    CHECK(!out[3].on && !runs_(1) && R[0x46020 >> 2] >> 30 & 1 && lit(4, 2));
    unplug_tc(4);
    screen_plug();
    CHECK(!out[4].on && !(R[0x46020 >> 2] >> 31) && SHARE(0, 0, 1023));

    /* 11. A DP-alt port with two DP lanes (pin assignment D): the Dell at a mode two lanes carry. */
    plug_tc(4, dell, 5, 3);
    screen_plug();
    CHECK(lit(4, 1) && out[4].lanes == 2 && out[4].maxl == 2 && out[4].m.clock < 300000 && (R[0x1638C0 >> 2] >> 8 & 15) == 3);
    CHECK((R[0x16A3A0 >> 2] & 0xC0) == 0x80 && (R[0x16A7A0 >> 2] & 0xC0) == 0x80 && R[0x46038 >> 2] >> 30 & 1);
    CHECK(runs_(0) && R[0x6F400 >> 2] >> 31 && R[0x7019C >> 2] == surf);   /* the panel: still on */

    printf(fails ? "igfx-test: %d FAILED\n" : "igfx-test: all passed\n", fails);
    return fails != 0;
}
