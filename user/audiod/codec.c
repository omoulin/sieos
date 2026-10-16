/*
 * codec.c - audiod's codecs, through verbs only (included by audiod.c, which
 * gives verb(), h and cad; and by tests/hdmi, which answers the verbs from a
 * real codec's widget dump).
 *
 *   Analog (the first codec with a speaker, headphone or line out): the
 *   widgets walked from each output pin to a DAC and from an ADC to a
 *   microphone or line-in pin, through mixers and selectors; on the way every
 *   connection chosen and every amplifier opened at 0 dB. The volume is the
 *   DACs' output amplifiers; a headphone plugged in silences the speaker.
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
static int inpin(int nid)                        /* a microphone or line-in pin? */
{
    int dev, port = pincfg(nid, &dev);
    return wtype(nid) == W_PIN && port != 1 && (par(nid, 0xC) & 0x20) && (dev == 0xA || dev == 8);
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

/* The headphone jack: plugged in, the speaker is silenced. */
static void jack(void)
{
    if (sel || !hp || !spk || !(hpcap & 4)) return;
    at(ach, acad);
    if (hpcap & 2) verb(hp, 0x70900);            /* (this pin senses only when asked) */
    verb(spk, 0x70700 | (verb(hp, 0xF0900) >> 31 ? 0 : 0x40));
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
    uint32_t id = par(0, 0), sub = par(0, 4), adc = 0, ipin = 0, n0 = npin;
    int an = !ach;
    if (id >> 16 == 0x8086) {                    /* Intel display codec: all pins, converters, devices */
        int vn = (id & 0xFFFF) >= 0x2812 ? 2 : 8;   /* (its vendor widget: Tiger Lake on 2) */
        uint32_t v = verb(vn, 0xF8100);
        if ((v & 3) != 3) verb(vn, 0x78100 | (v & 0xFC) | 3);
    }
    for (uint32_t f = sub >> 16 & 0xFF; f < (sub >> 16 & 0xFF) + (sub & 0xFF); f++)
        if ((par(f, 5) & 0xFF) == 1) afg = f;
    if (!afg) return -1;
    verb(afg, 0x70500);                          /* D0 */
    if (an) hp = spk = 0;
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
        if (type == W_ADC && !adc && (ipin = route(nid, W_PIN, 0))) {
            conv(adc = nid, TAG_IN);
            verb(ipin, 0x70720 | (par(ipin, 0xC) & 1 << 12 ? 4 : 0));   /* input; 80% bias (a microphone) */
        }
    }
    if (npin > (int)n0)
        printf("audiod: codec %d (%04x:%04x): %d HDMI/DP pins, %d to %d, %d converters each\n",
               a, id >> 16, id & 0xFFFF, npin - n0, hpin[n0].nid, hpin[npin - 1].nid, par(hpin[n0].nid, 0xE) & 0x7F);
    if (!an) return -1;
    int *pin[2] = { &spk, &hp }, k = 0;
    dac[0] = dac[1] = 0;
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
    printf("audiod: codec %d (%04x:%04x): speaker or line out pin %d, headphone pin %d, DAC %d %d; ADC %d <- pin %d\n",
           a, id >> 16, id & 0xFFFF, spk, hp, dac[0], dac[1], adc, ipin);
    set_vol();
    return 0;
}
