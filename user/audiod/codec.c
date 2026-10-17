/*
 * codec.c - audiod's codecs, through verbs only (included by audiod.c, which
 * gives verb(), h and cad; and by tests/hdmi, which answers the verbs from a
 * real codec's widget dump).
 *
 *   Analog (the first codec with a speaker, headphone or line out): the
 *   widgets walked from each output pin to a DAC and from an ADC to a
 *   microphone or line-in pin, through mixers and selectors; on the way every
 *   connection chosen and every amplifier opened at 0 dB. The volume is the
 *   DACs' output amplifiers; a headphone plugged in silences the speaker, and
 *   recording then prefers the jack's microphone (a headset's).
 *
 *   Realtek's ALC225 family (ALC215/225/285/289/295/299; Dell names them
 *   ALC3253/3254/3271...; Linux: patch_realtek.c): a combo jack whose mode
 *   is set through the vendor widget's coefficients: unplugged, or a
 *   headset whose microphone is wired CTIA (iPhone) or OMTP (older Nokia),
 *   told apart by measuring (alc_headset_mode_*, alc_determine_headset_type).
 *   Dell's firmware leaves the headset microphone's pin (25) "not
 *   connected": it is made one (Linux's DELL4_MIC_NO_PRESENCE), on Dells
 *   only (their subsystem id).
 *
 *   Realtek's ALC294 family (ALC234/274/294: a Surface Pro 7's ALC274, its
 *   subsystem Realtek's own): Linux's alc294_init, nothing more (no
 *   fixup for the Surface there): the EAPD coefficients, the headphone
 *   amplifier's depop, then the generic walk (speakers, headphones, the
 *   built-in microphones on the codec's digital-microphone pin: the
 *   firmware's NHLT lists none for the DSP).
 *
 *   HDMI / DisplayPort (any codec, any controller; Linux: patch_hdmi.c): the
 *   digital pins, each with its devices (DP multi-stream: one per display
 *   pipe). A device whose pin sense says "ELD valid" has a monitor with sound,
 *   its name in the ELD (eld.h): a sink. Playing to it: the pin's first
 *   converter, 2 channels, the stream's tag and format, the pin's output on,
 *   and an audio infoframe (2 channels, front left and right). Intel
 *   display codecs hide their second pins and devices until the vendor verb
 *   0x781 sets "all pins and converters" and "DP 1.2" (Haswell on).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "eld.h"

enum { W_DAC, W_ADC, W_MIX, W_SEL, W_PIN };      /* widget types */
enum { TAG_IN = 1, TAG_OUT = 2 };                /* the streams' tags */
#define FMT 0x0011                               /* 48 kHz, 16-bit, 2 channels */

typedef struct { hda_t *c; uint8_t cad, nid, dev, cvt, dp; char name[17]; } sink_t;
static hda_t *h, *ach;                           /* the controller spoken to; the analog codec's */
static int cad, acad, afg, hp, spk, hpcap, vol = 80, npin, nsink, sel;   /* sel: 0 analog, else sink sel (sink[sel - 1]) */
static int adc, ipin, jk, alc, alc294, hs = -1, nadc;   /* the ADC recording, its pin; jk: a jack's microphone only; an ALC225-family codec, its jack's mode; an ALC294-family codec */
static uint8_t adcs[4];
static uint8_t dac[2];
typedef struct { hda_t *c; uint8_t cad, nid; } hpin_t;
static hpin_t hpin[16];                          /* HDMI / DP pins */
static sink_t sink[4], cur;                      /* the sinks; the one playing (sel > 0) */
static uint32_t verb(int nid, uint32_t v);

static void at(hda_t *c, int a) { h = c; cad = a; }
static uint32_t par(int nid, int p) { return verb(nid, 0xF0000 | p); }
static int wtype(int nid) { return par(nid, 9) >> 20 & 15; }
static uint32_t ampcap(int nid, int o) { uint32_t c = par(nid, o ? 0x12 : 0x0D); return c ? c : par(afg, o ? 0x12 : 0x0D); }
static void amp(int nid, int o, int idx, int mute)   /* open at 0 dB, or muted */
{
    verb(nid, 0x30000 | (o ? 0xB000 : 0x7000) | idx << 8 | (mute ? 0x80 : (ampcap(nid, o) & 0x7F)));
}
/* A pin's default configuration (port: 1 = nothing attached; device). */
static int pincfg(int nid, int *dev) { uint32_t c = verb(nid, 0xF1C00); *dev = c >> 20 & 15; return c >> 30; }
static int inpin(int nid)                        /* a microphone or line-in pin? (jk: a jack's, something in it) */
{
    int dev, port = pincfg(nid, &dev);
    uint32_t pc = par(nid, 0xC);
    if (wtype(nid) != W_PIN || port == 1 || !(pc & 0x20) || (dev != 0xA && dev != 8)) return 0;
    return !jk || (!port && (!(pc & 4) || verb(nid, 0xF1C00) & 0x100 || verb(nid, 0xF0900) >> 31));   /* (its own sense, if any) */
}

/* Follow nid's connections down to a DAC (from an output pin) or an input
 * pin (from an ADC), through mixers and selectors, choosing the connection
 * and opening the amplifiers on the way (a mixer's other inputs muted).
 * -> the widget found at the end, or 0. */
static int route(int nid, int want, int depth)
{
    uint8_t c[16];
    uint32_t len = par(nid, 0x0E), n = len & 0x7F;
    if (len & 0x80 || n > 16) return 0;          /* (long lists: not on these codecs) */
    for (uint32_t i = 0; i < n; i += 4) { uint32_t e = verb(nid, 0xF0200 | i); memcpy(c + i, &e, 4); }
    int type = wtype(nid);
    for (uint32_t i = 0; i < n; i++) {
        int t = wtype(c[i]), end = 0;
        if (t == want && (want == W_DAC || inpin(c[i]))) end = c[i];
        else if ((t == W_MIX || t == W_SEL) && depth < 4) end = route(c[i], want, depth + 1);
        if (!end) continue;
        if (type != W_MIX && n > 1) verb(nid, 0x70100 | i);
        for (uint32_t j = 0; j < n; j++) if (j == i || type == W_MIX) amp(nid, 0, j, j != i);
        amp(nid, 1, 0, 0);
        if (end == c[i]) amp(end, want == W_DAC, 0, 0);
        return end;
    }
    return 0;
}

static void conv(int nid, uint32_t tag) { verb(nid, 0x20000 | FMT); verb(nid, 0x70600 | tag << 4); }
static void devsel(const sink_t *s) { at(s->c, s->cad); verb(s->nid, 0x73500 | s->dev); }

/* The volume: the DACs' amplifiers (an HDMI sink: the monitor's own; muted:
 * its converter stopped). */
static void set_vol(void)
{
    int v = vol & 0xFF, mute = vol & AUDIO_MUTE || !v;
    if (sel) { devsel(&cur); conv(cur.cvt, mute ? 0 : TAG_OUT); return; }
    at(ach, acad);
    for (int i = 0; i < 2 && dac[i]; i++) {
        uint32_t c = ampcap(dac[i], 1), top = c & 0x7F ? c & 0x7F : c >> 8 & 0x7F;   /* 0 dB, else the most */
        verb(dac[i], 0x3B000 | (mute ? 0x80 : top * v / 100));
    }
}

static int plugged(void)                         /* headphones in the jack? (the analog codec's) */
{
    if (!hp || !(hpcap & 4)) return 0;
    if (hpcap & 2) verb(hp, 0x70900);            /* (this pin senses only when asked) */
    return verb(hp, 0xF0900) >> 31;
}

/* An ALC225-family codec's coefficient (of widget nid) changed -> its old value. */
__attribute__((noinline)) static uint32_t coef(int nid, int i, uint32_t mask, uint32_t set)
{
    verb(nid, 0x50000 | i);
    uint32_t v = verb(nid, 0xC0000) & 0xFFFF;
    if (mask) { verb(nid, 0x50000 | i); verb(nid, 0x40000 | (v & ~mask) | set); }
    return v;
}
#define C(i, mask, set) coef(0x20, i, mask, set)
static void headset(int in)                      /* the combo jack's mode, when it changed */
{
    static const uint16_t pre[7][4] = { { 0x20, 0x4A, 0x100, 0 }, { 0x57, 5, 0x4000, 0 }, { 0x20, 0x63, 0xC000, 0xC000 },
        { 0x20, 0x4A, 0x30, 0x20 }, { 0x20, 0x4A, 0xC00, 0xC00 }, { 0x20, 0x45, 0xFC00, 0xD000 }, { 0x20, 0x4A, 0xC00, 0 } };
    if (!alc || in == hs) return;
    for (int i = 0; i < 7; i++) coef(pre[i][0], pre[i][1], pre[i][2], pre[i][3]);   /* (alc225_pre_hsmode) */
    if ((hs = in)) {
        C(0x67, 0xF000, 0x1000);                 /* measure: the ring as CTIA's ground; else as OMTP's */
        C(0x45, 0xFC00, 0xD000); C(0x49, 0x300, C(0x45, 0, 0) & 1 << 9 ? 0x200 : 0x100);
        sys_sleep(800000000);
        int ctia = (C(0x46, 0, 0) & 0xF0) == 0xF0;
        if (!ctia) { C(0x45, 0xFC00, 0xE000); C(0x49, 0x300, 0x100); sys_sleep(100000000); ctia = (C(0x46, 0, 0) & 0xF0) != 0xF0; }
        C(0x4A, 0x1C0, 0x1C0); C(0x4A, 0x30, 0x30); C(0x67, 0xF000, 0x3000);
        int b9 = C(0x45, 0, 0) >> 9 & 1;
        C(0x45, 0xFC00, ctia ? 0xD400 : 0xE400); C(0x63, 0xC000, ctia && b9 ? 0x4000 : 0x8000);
        printf("audiod: headset jack: %s wiring, its microphone on\n", ctia ? "CTIA (iPhone)" : "OMTP");
    } else C(0x63, 0xC000, 0);                   /* unplugged */
    if (hp) { verb(hp, 0x70740 | (hpcap & 8 ? 0x80 : 0)); amp(hp, 1, 0, 0); }   /* (the headphones' output again) */
}

/* The headphone jack: plugged in, the speaker is silenced. */
static void jack(void)
{
    if (sel || !ach) return;
    at(ach, acad);
    int in = plugged();
    if (spk) verb(spk, 0x70700 | (in ? 0 : 0x40));
    headset(in);
}

/* The microphone, as recording starts: an ADC's path to a jack's (a
 * headset's) when headphones are in, else to the first one (built in). */
static void mic(void)
{
    int a = 0, in;
    if (!nadc) return;
    at(ach, acad);
    headset(in = plugged());
    for (ipin = 0, jk = in; jk >= 0 && !ipin; jk--)
        for (int i = 0; i < nadc && !ipin; i++) if ((ipin = route(adcs[i], W_PIN, 0))) a = adcs[i];
    jk = 0;
    if (a != adc) { if (adc) conv(adc, 0); if ((adc = a)) conv(adc, TAG_IN); }
    if (ipin) verb(ipin, 0x70720 | (par(ipin, 0xC) & 1 << 12 ? 4 : 0));   /* input; 80% bias (a microphone) */
}

/* Output k (0: analog, else sink k - 1) gets the output stream's tag, the
 * other one none. */
static void route_out(int k)
{
    if (ach) { at(ach, acad); for (int i = 0; i < 2 && dac[i]; i++) conv(dac[i], k ? 0 : TAG_OUT); }
    if (sel) { devsel(&cur); conv(cur.cvt, 0); }
    if ((sel = k)) {
        const sink_t *s = &cur;
        cur = sink[k - 1];
        /* to the pin (the device's), or (1 << 31) its converter: its first converter; digital on, 2
         * channels; output on, unmuted; converter channels 0, 1 to slots 0, 1; the infoframe stopped,
         * rewritten from its start (HDMI's, DP's), sent at every frame */
        static const uint32_t seq[9] = { 0x70100, 1u << 31 | 0x70D01, 1u << 31 | 0x72D01, 0x70740, 0x3B000, 0x73400, 0x73411, 0x73200, 0x73000 };
        static const uint8_t ai[2][9] = { { 0x84, 1, 10, 0x70, 1 }, { 0x84, 0x1B, 0x11 << 2, 1 } };
        devsel(s);
        for (int i = 0; i < 9; i++) verb(seq[i] >> 31 ? s->cvt : s->nid, seq[i] & 0xFFFFF);
        for (int i = 0; i < 9; i++) verb(s->nid, 0x73100 | ai[s->dp][i]);
        verb(s->nid, 0x73000);
        verb(s->nid, 0x732C0);
    }
    set_vol();
}

/* The sinks: HDMI / DP pin devices with a valid ELD (asked again each time:
 * the display writes it when it lights a monitor). -> 1 if the one playing
 * is gone. */
static int hdmi_scan(int log)
{
    int k = nsink = 0;
    for (int i = 0; i < npin; i++) {
        int nid = hpin[i].nid;
        at(hpin[i].c, hpin[i].cad);
        for (int d = 0; d < 4 && nsink < 4; d++) {   /* its devices (DP multi-stream: those it selects) */
            sink_t *s = &sink[nsink];
            uint8_t e[36];
            char name[17];
            int n = 0;
            verb(nid, 0x73500 | d);
            if (d && (verb(nid, 0xF3500) & 0x3F) != (uint32_t)d) break;
            if (!(verb(nid, 0xF0900) & 1u << 30) || !(par(nid, 0x0E) & 0x7F)) continue;   /* no ELD; no converter */
            for (uint32_t r; n < 36 && (r = verb(nid, 0xF2F00 | n)) >> 31; n++) e[n] = (uint8_t)r;
            if (eld_name(e, n, name)) continue;
            *s = (sink_t){ hpin[i].c, (uint8_t)cad, (uint8_t)nid, (uint8_t)d, (uint8_t)verb(nid, 0xF0200), e[5] >> 2 & 1, "Monitor" };
            if (name[0]) memcpy(s->name, name, 17);
            if (log) printf("audiod: HDMI/DP sink %d: codec %d pin %d device %d converter %d: %s (%s)\n",
                            nsink + 1, cad, nid, d, s->cvt, s->name, s->dp ? "DP" : "HDMI");
            if (sel && s->c == cur.c && s->cad == cur.cad && s->nid == nid && s->dev == d) k = nsink + 1;
            nsink++;
        }
    }
    return sel && !(sel = k);
}

/* The codec at address a: its audio function group, its widgets; the
 * analog paths (the first such codec only), its HDMI / DP pins.
 * -> 0 if it has an analog output. */
static int codec(int a)
{
    at(h, a);
    afg = 0;
    uint32_t id = par(0, 0), sub = par(0, 4), n0 = npin, lo = id & 0xFFFF;
    int an = !ach;
    if (id >> 16 == 0x8086) {                    /* Intel display codec: all pins, converters, devices */
        int vn = lo >= 0x280F ? 2 : lo == 0x2800 || lo == 0x280C || lo == 0x280D ? 11 : 8;   /* (its vendor widget: Ice Lake on 2) */
        uint32_t v = verb(vn, 0xF8100);
        if ((v & 3) != 3) verb(vn, 0x78100 | (v & 0xFC) | 3);
    }
    for (uint32_t f = sub >> 16 & 0xFF; f < (sub >> 16 & 0xFF) + (sub & 0xFF); f++)
        if ((par(f, 5) & 0xFF) == 1) afg = f;
    if (!afg) return -1;
    verb(afg, 0x70500);                          /* D0 */
    if (an) {
        int d;
        hp = spk = adc = nadc = 0;
        alc = id >> 16 == 0x10EC && (lo == 0x215 || lo == 0x225 || lo == 0x285 || lo == 0x289 || lo == 0x295 || lo == 0x299);
        if (alc && verb(afg, 0xF2000) >> 16 == 0x1028 && pincfg(0x19, &d) == 1) {   /* Dell: a headset microphone, no jack sense of its own */
            for (int i = 0; i < 4; i++) verb(0x19, (0x71Cu + i) << 8 | (0x01A1913C >> 8 * i & 0xFF));
            printf("audiod: Realtek ALC%x on a Dell: pin 25 is the headset's microphone\n", lo);
        }
        if (alc) { C(0x36, 1 << 13, 0); C(0x4A, 0xC00, 0); coef(0x57, 4, 7, 4); }   /* (alc_fill_eapd_coef; alc225_init: headphone amplifier at high power) */
        alc294 = id >> 16 == 0x10EC && (lo == 0x234 || lo == 0x274 || lo == 0x294);
        if (alc294) {                            /* the ALC294 family (a Surface Pro 7's ALC274): alc_fill_eapd_coef, the jack microphone's bias by verb */
            if (lo != 0x294) C(0x6E, 0xFFFF, 0x0C25);
            C(0x10, 1 << 15, 0); C(0x6B, 0x18, 0x18);
            printf("audiod: Realtek ALC%x (subsystem %08x)\n", lo, verb(afg, 0xF2000));
        }
    }
    sub = par(afg, 4);
    for (uint32_t nid = sub >> 16 & 0xFF; nid < (sub >> 16 & 0xFF) + (sub & 0xFF); nid++) {
        uint32_t cap = par(nid, 9), type = cap >> 20 & 15;
        int dev = 0, port = type == W_PIN ? pincfg(nid, &dev) : 1;
        if (cap & 1 << 10) verb(nid, 0x70500);   /* widgets with their own power: on */
        if (cap & 1 << 9) {                      /* digital: HDMI or DP pins */
            if (port != 1 && par(nid, 0xC) & (1 << 7 | 1 << 24) && npin < 16) hpin[npin++] = (hpin_t){ h, (uint8_t)a, (uint8_t)nid };
            continue;
        }
        if (!an) continue;
        if (port != 1 && dev <= 2 && (par(nid, 0xC) & 0x10)) {
            if (dev == 2) { if (!hp) hp = nid; } /* headphones */
            else if (!spk || dev == 1) spk = nid;   /* a speaker, else a line out */
        }
        if (type == W_ADC && nadc < 4) adcs[nadc++] = (uint8_t)nid;
    }
    if (npin > (int)n0)
        printf("audiod: codec %d (%04x:%04x): %d HDMI/DP pins, %d to %d, %d converters each\n",
               a, id >> 16, id & 0xFFFF, npin - n0, hpin[n0].nid, hpin[npin - 1].nid, par(hpin[n0].nid, 0xE) & 0x7F);
    if (!an) return -1;
    int *pin[2] = { &spk, &hp }, k = 0;
    dac[0] = dac[1] = 0;
    if (alc294 && hp) {                          /* the headphone amplifier's depop, once (alc294_hp_init): pin muted and off, */
        amp(hp, 1, 0, 1); verb(hp, 0x70700);     /* depop by hand, started, waited for (up to 1 s), back to automatic */
        sys_sleep(100000000);
        C(0x6F, 0xF, 0); coef(0x58, 0, 0x8000, 0x8000);
        for (int i = 0; i < 20 && coef(0x58, 1, 0, 0) & 0x80; i++) sys_sleep(50000000);
        C(0x6F, 0xF, 0xB);
        sys_sleep(50000000);
    }
    for (int i = 0; i < 2; i++) {
        int d = *pin[i] ? route(*pin[i], W_DAC, 0) : 0;
        if (!d) { *pin[i] = 0; continue; }
        uint32_t pc = par(*pin[i], 0xC);
        verb(*pin[i], 0x70740 | (i && pc & 8 ? 0x80 : 0));   /* output (and headphone drive) */
        if (pc & 1 << 16) verb(*pin[i], 0x70C02);   /* EAPD: its amplifier on */
        if (i) hpcap = pc;
        if (d != dac[0]) conv(dac[k++] = d, TAG_OUT);
    }
    if (!k) return -1;
    ach = h; acad = a;
    mic();
    printf("audiod: codec %d (%04x:%04x): speaker or line out pin %d, headphone pin %d, DAC %d %d; ADC %d <- pin %d\n",
           a, id >> 16, id & 0xFFFF, spk, hp, dac[0], dac[1], adc, ipin);
    set_vol();
    return 0;
}
