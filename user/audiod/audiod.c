/*
 * audiod.c - The audio server, port "audio" (the protocol: mk/proto.h;
 * docs/audio.md): an Intel High Definition Audio controller (QEMU's
 * intel-hda and ich9-intel-hda, PC chipsets up to Raptor Lake) and its
 * codec. One output and one input stream, 48 kHz, 16-bit, stereo.
 *
 *   The controller: reset, then commands to the codec through the CORB (a
 *   ring we write) and answers through the RIRB (one it writes), polled.
 *   The codec: its widgets, walked from each speaker or headphone pin to a
 *   converter (DAC) and from a converter (ADC) to a microphone or line-in
 *   pin, through mixers and selectors; on the way every connection is
 *   chosen and every amplifier opened at 0 dB. The volume is the DACs'
 *   output amplifiers; a headphone plugged in silences the speaker.
 *
 *   Each stream: a 128 KiB ring in DMA memory, 8 periods of 16 KiB, an
 *   interrupt at the end of each. A write lands by IPC straight in the ring
 *   (the message's rbuf is the free room); a read is answered straight out
 *   of it. What the output stream has played is zeroed, so it plays silence
 *   when writes stop coming, and it stops a period after all is played (what
 *   the codec still holds); the input stream records while someone reads,
 *   and stops when nobody does.
 *
 *   Intel's chipsets (Linux, sound/pci/hda/hda_intel.c): traffic class 0
 *   (TCSEL); on PCHs, snooped DMA and no clock gating while the link resets
 *   (Skylake on). A controller whose PCI class says "audio device" (0401)
 *   instead of HDA (0403) has its DSP (SST/SOF) on: the HDA part still
 *   works, but microphones wired to the DSP do not.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "pci.h"

enum { GCAP = 0x00, GCTL = 0x08, STATESTS = 0x0E, INTCTL = 0x20, INTSTS = 0x24,
       CORBLBASE = 0x40, CORBUBASE = 0x44, CORBWP = 0x48, CORBRP = 0x4A, CORBCTL = 0x4C, CORBSIZE = 0x4E,
       RIRBLBASE = 0x50, RIRBUBASE = 0x54, RIRBWP = 0x58, RINTCNT = 0x5A, RIRBCTL = 0x5C, RIRBSTS = 0x5D,
       RIRBSIZE = 0x5E, SD0 = 0x80 };
enum { SD_CTL = 0, SD_STS = 3, SD_LPIB = 4, SD_CBL = 8, SD_LVI = 0xC, SD_FMT = 0x12, SD_BDPL = 0x18, SD_BDPU = 0x1C };
enum { W_DAC, W_ADC, W_MIX, W_SEL, W_PIN };      /* widget types */
#define RING (128 << 10)
#define PER  (16 << 10)
#define FMT  0x0011                              /* 48 kHz, 16-bit, 2 channels */

typedef struct {
    int sd;                                      /* its registers' offset */
    uint8_t *ring;
    uint64_t bdl;                                /* its buffer list's bus address */
    uint32_t hp, pos, n;                         /* the controller's place (whole periods); ours; bytes queued or recorded */
    uint32_t bit, tag, run, drain;               /* INTSTS bit, stream tag, running, playing a last silence */
    long tok;                                    /* a client waiting (0: none) ... */
    uint64_t want;                               /* ... for a read: how much it can take */
} stream_t;

static volatile uint8_t *regs;
static uint32_t *corb;
static volatile uint32_t *rirb;
static unsigned cwp, rrp;
static int cad, afg, irq, hp, spk, hpcap, vol = 80;
static uint8_t dac[2];
static stream_t out, in;

static uint8_t  r8(int r)              { return regs[r]; }
static void     w8(int r, uint8_t v)   { regs[r] = v; }
static void     w16(int r, uint16_t v) { *(volatile uint16_t *)(regs + r) = v; }
static uint32_t r32(int r)             { return mmio_r32(regs + r); }
static void     w32(int r, uint32_t v) { mmio_w32(regs + r, v); }
static int until(int r, uint8_t mask, uint8_t want)   /* 0 when the bits are so, -1 after 100 ms */
{
    for (long end = sys_clock() + 100000000; (r8(r) & mask) != want; ) if (sys_clock() > end) return -1;
    return 0;
}

/* A command to the codec -> its answer (0 if none came). v: 12-bit verb and
 * 8-bit payload, or 4-bit verb and 16-bit payload. */
static uint32_t verb(int nid, uint32_t v)
{
    cwp = (cwp + 1) & 255;
    corb[cwp] = (uint32_t)cad << 28 | (uint32_t)nid << 20 | v;
    dma_wmb();
    w16(CORBWP, cwp);
    for (long end = sys_clock() + 100000000; r8(RIRBWP) == rrp; ) if (sys_clock() > end) return 0;
    rrp = (rrp + 1) & 255;
    w8(RIRBSTS, 5);                              /* (QEMU waits for this before the next answer) */
    return rirb[2 * rrp];
}
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

static void set_vol(void)
{
    for (int i = 0; i < 2 && dac[i]; i++) {
        uint32_t c = ampcap(dac[i], 1), top = c & 0x7F ? c & 0x7F : c >> 8 & 0x7F;   /* 0 dB, else the most */
        int v = vol & 0xFF, mute = vol & AUDIO_MUTE || !v;
        verb(dac[i], 0x3B000 | (mute ? 0x80 : top * v / 100));
    }
}

/* The headphone jack: plugged in, the speaker is silenced. */
static void jack(void)
{
    if (!hp || !spk || !(hpcap & 4)) return;
    if (hpcap & 2) verb(hp, 0x70900);            /* (this pin senses only when asked) */
    verb(spk, 0x70700 | (verb(hp, 0xF0900) >> 31 ? 0 : 0x40));
}

/* The codec at address a: its audio function group, its pins and paths.
 * -> 0 if it has an output path (not an HDMI codec: those pins are digital). */
static void conv(int nid, uint32_t tag) { verb(nid, 0x20000 | FMT); verb(nid, 0x70600 | tag << 4); }
static int codec(int a)
{
    cad = a; afg = hp = spk = 0;
    uint32_t sub = par(0, 4), adc = 0, ipin = 0, id = par(0, 0);
    for (uint32_t f = sub >> 16 & 0xFF; f < (sub >> 16 & 0xFF) + (sub & 0xFF); f++)
        if ((par(f, 5) & 0xFF) == 1) afg = f;
    if (!afg) return -1;
    verb(afg, 0x70500);                          /* D0 */
    sub = par(afg, 4);
    for (uint32_t nid = sub >> 16 & 0xFF; nid < (sub >> 16 & 0xFF) + (sub & 0xFF); nid++) {
        uint32_t cap = par(nid, 9), type = cap >> 20 & 15;
        int dev;
        if (cap & 1 << 10) verb(nid, 0x70500);   /* widgets with their own power: on */
        if (cap & 1 << 9) continue;              /* digital */
        if (type == W_PIN && pincfg(nid, &dev) != 1 && dev <= 2 && (par(nid, 0xC) & 0x10)) {
            if (dev == 2) { if (!hp) hp = nid; } /* headphones */
            else if (!spk || dev == 1) spk = nid;   /* a speaker, else a line out */
        }
        if (type == W_ADC && !adc && (ipin = route(nid, W_PIN, 0))) {
            conv(adc = nid, in.tag);
            verb(ipin, 0x70720 | (par(ipin, 0xC) & 1 << 12 ? 4 : 0));   /* input; 80% bias (a microphone) */
        }
    }
    int *pin[2] = { &spk, &hp }, k = 0;
    dac[0] = dac[1] = 0;
    for (int i = 0; i < 2; i++) {
        int d = *pin[i] ? route(*pin[i], W_DAC, 0) : 0;
        if (!d) { *pin[i] = 0; continue; }
        uint32_t pc = par(*pin[i], 0xC);
        verb(*pin[i], 0x70740 | (i && pc & 8 ? 0x80 : 0));   /* output (and headphone drive) */
        if (pc & 1 << 16) verb(*pin[i], 0x70C02);   /* EAPD: its amplifier on */
        if (i) hpcap = pc;
        if (d != dac[0]) conv(dac[k++] = d, out.tag);
    }
    if (!k) return -1;
    printf("audiod: codec %d (%04x:%04x): speaker or line out pin %d, headphone pin %d, DAC %d %d; ADC %d <- pin %d\n",
           a, id >> 16, id & 0xFFFF, spk, hp, dac[0], dac[1], adc, ipin);
    set_vol();
    return 0;
}

static void stop(stream_t *s)
{
    w8(s->sd, 0);
    until(s->sd, 2, 0);
    s->run = s->drain = s->hp = s->pos = s->n = 0;
}
static void start(stream_t *s)
{
    int r = s->sd;
    w8(r, 1); until(r, 1, 1);                    /* reset */
    w8(r, 0); until(r, 1, 0);
    w8(r + 2, s->tag << 4);
    w32(r + SD_CBL, RING); w16(r + SD_LVI, RING / PER - 1); w16(r + SD_FMT, FMT);
    w32(r + SD_BDPL, (uint32_t)s->bdl); w32(r + SD_BDPU, (uint32_t)(s->bdl >> 32));
    w8(r, 6);                                    /* run, interrupt at each period's end */
    s->run = 1;
}
/* Whole periods the controller finished since last time. */
static uint32_t advance(stream_t *s)
{
    uint32_t at = r32(s->sd + SD_LPIB) % RING / PER * PER, d = (at + RING - s->hp) % RING;
    s->hp = at;
    return d;
}
static void reply_read(void)
{
    uint32_t n = in.n < RING - in.pos ? in.n : RING - in.pos;
    if (n > in.want) n = (uint32_t)in.want;
    msg_t m = { .w = { n }, .sbuf = in.ring + in.pos, .slen = n };
    ipc_reply(in.tok, &m);
    in.tok = 0; in.pos = (in.pos + n) % RING; in.n -= n;
}

static void interrupt(void)
{
    uint32_t s = r32(INTSTS);
    if (s & out.bit) {
        w8(out.sd + SD_STS, 0x1C);
        uint32_t from = out.hp, d = advance(&out), a = RING - from < d ? RING - from : d;
        memset(out.ring + from, 0, a);           /* played: silence from now on */
        memset(out.ring, 0, d - a);
        if (d < out.n) out.n -= d;
        else if (out.drain) stop(&out);
        else {                                   /* all played: one more period of silence (the codec's buffer) */
            out.drain = 1; out.n = PER; out.pos = (out.hp + PER) % RING;
        }
        if (out.tok) { reply_val(out.tok, 0); out.tok = 0; }   /* room: send again */
    }
    if (s & in.bit) {
        w8(in.sd + SD_STS, 0x1C);
        in.n += advance(&in);
        if (in.n > RING - 2 * PER) stop(&in);    /* nobody reads: stop recording */
        else if (in.tok && in.n) reply_read();
    }
    irq_ack(irq & ~IRQ_LEVEL);
}

static int setup(void)
{
    pci_t p;
    uint64_t len, pa;
    int found = !pci_scan(&p, 8, 0x040300 << 8, 0xFFFF0000, 0);
    for (int i = 0; !found && !pci_scan(&p, 8, 0x040100 << 8, 0xFFFF0000, i); i++)
        found = (p.id & 0xFFFF) == 0x8086;       /* Intel's, its DSP on (not AC'97: no memory BAR) */
    if (!found || !(pa = pci_bar(&p, 0, &len)) || !len || (long)(regs = map_phys(pa, 0x4000)) < 0) return -1;
    int intel = (p.id & 0xFFFF) == 0x8086, pch = intel && (p.id >> 16 < 0x2600 || p.id >> 16 >= 0x3B00);
    uint32_t cg = pci_cfg(&p, 0x48);             /* (pch: a PCH, not an ICH; 0x48: CGCTL) */
    if (intel) pci_cfgw(&p, 0x44, pci_cfg(&p, 0x44) & ~7u);       /* TCSEL: traffic class 0 */
    if (pch) {
        pci_cfgw(&p, 0x78, pci_cfg(&p, 0x78) & ~0x800u);          /* snooped DMA (DEVC) */
        pci_cfgw(&p, 0x48, cg & ~0x40u);         /* no clock gating while the link resets */
    }
    uint16_t gcap = (uint16_t)r32(GCAP);
    int iss = gcap >> 8 & 15, oss = gcap >> 12 & 15;
    printf("audiod: HDA %04x:%04x at %02x:%02x.%x, %s, HDA %d.%d, %d out and %d in streams, line %d\n",
           p.id & 0xFFFF, p.id >> 16, p.at >> 16, p.at >> 11 & 31, p.at >> 8 & 7,
           (pci_cfg(&p, 8) >> 16 & 0xFF) == 1 ? "DSP mode (audio device class)" : "HDA mode",
           r8(3), r8(2), oss, iss, p.irq & ~IRQ_LEVEL);
    if (!oss || !iss) return -1;
    w32(INTCTL, 0);
    for (int i = 0; i < iss + oss; i++) w8(SD0 + 0x20 * i, 0);   /* streams the firmware ran */
    w8(CORBCTL, 0); w8(RIRBCTL, 0);
    w32(GCTL, 0);                                /* reset the link and the codecs */
    if (until(GCTL, 1, 0)) return -1;
    sys_sleep(100000);
    w32(GCTL, 1);
    if (until(GCTL, 1, 1)) return -1;
    sys_sleep(1000000);                          /* the codecs say they are there (521 µs) */
    if (pch) pci_cfgw(&p, 0x48, cg);

    uint64_t cpa, rpa;
    uint8_t *page = dma_alloc(4096, &cpa), *rings = dma_alloc(2 * RING, &rpa);
    if ((long)page < 0 || (long)rings < 0) return -1;
    cpa += p.bus_off; rpa += p.bus_off;
    corb = (uint32_t *)page; rirb = (uint32_t *)(page + 1024);
    w32(CORBLBASE, (uint32_t)cpa); w32(CORBUBASE, (uint32_t)(cpa >> 32));
    w8(CORBSIZE, 2); w16(CORBWP, 0);             /* 256 entries */
    /* (the reset left both rings' pointers at 0) */
    w32(RIRBLBASE, (uint32_t)(cpa + 1024)); w32(RIRBUBASE, (uint32_t)((cpa + 1024) >> 32));
    w8(RIRBSIZE, 2); w16(RINTCNT, 1);
    w8(CORBCTL, 2); w8(RIRBCTL, 3);              /* both run ("interrupt" for QEMU's count; CIE stays off) */

    stream_t *s[2] = { &in, &out };
    for (int k = 0; k < 2; k++) {                /* in: the first input stream, out: the first output one */
        int i = k ? iss : 0;
        s[k]->sd = SD0 + 0x20 * i; s[k]->bit = 1u << i; s[k]->tag = k + 1;
        s[k]->ring = rings + k * RING;
        s[k]->bdl = cpa + 3072 + 128 * k;
        uint32_t *b = (uint32_t *)(page + 3072 + 128 * k);
        for (int j = 0; j < RING / PER; j++, b += 4) {
            uint64_t a = rpa + k * RING + j * PER;
            b[0] = (uint32_t)a; b[1] = (uint32_t)(a >> 32); b[2] = PER; b[3] = 1;   /* interrupt at its end */
        }
    }
    uint32_t codecs = r8(STATESTS) | r8(STATESTS + 1) << 8;
    int a = 0;
    while (a < 15 && (!(codecs >> a & 1) || codec(a))) a++;
    if (a == 15) { printf("audiod: no codec with a speaker, headphone or line out (codecs %x)\n", codecs); return -1; }
    irq = p.irq;
    w32(INTCTL, 1u << 31 | out.bit | in.bit);
    return irq < 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (setup()) { printf("audiod: no sound card\n"); return 2; }
    long port = port_create("audio"), from = 0, r = 0;
    if (irq_bind(irq, port)) return 2;
    for (int answer = 0;; ) {
        uint32_t room = RING - out.n < RING - out.pos ? RING - out.n : RING - out.pos;
        msg_t m = { .w = { r }, .rbuf = out.ring + out.pos, .rlen = room };
        from = answer ? ipc_reply_recv(from, port, &m) : ipc_recv(port, &m);
        answer = 0;
        if (from <= 0) { if (!from) interrupt(); continue; }
        uint32_t take = m.w[0] == AUDIO_WRITE ? m.rlen & ~3u : 0;   /* whole frames */
        memset(out.ring + out.pos + take, 0, m.rlen - take);   /* (the rest: the room stays silent) */
        switch (m.w[0]) {
        case AUDIO_WRITE:
            if (!room) {                         /* full: the answer waits for room */
                if (!out.tok) { out.tok = from; continue; }
                r = -EBUSY;
                break;
            }
            out.pos = (out.pos + take) % RING; out.n += take; out.drain = 0;
            if (!out.run && out.n) { jack(); start(&out); }
            r = take;
            break;
        case AUDIO_READ:
            if (in.tok) { r = -EBUSY; break; }
            if (!in.run) start(&in);
            in.tok = from; in.want = m.w[1];
            if (in.n) reply_read();
            continue;
        case AUDIO_VOL:
            if (!(m.w[1] & AUDIO_ASK)) {
                vol = m.w[1] & (AUDIO_MUTE | 0xFF);
                if ((vol & 0xFF) > 100) vol = (vol & AUDIO_MUTE) | 100;
                set_vol();
            }
            r = vol;
            break;
        default: r = -ENOSYS;
        }
        answer = 1;
    }
}
