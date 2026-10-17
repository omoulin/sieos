/*
 * audiod.c - The audio server, port "audio" (the protocol: mk/proto.h;
 * docs/audio.md): Intel High Definition Audio controllers (QEMU's
 * intel-hda and ich9-intel-hda, PC chipsets up to Raptor Lake; an NVIDIA
 * GPU's HDMI one) and their codecs (codec.c). One output and one input
 * stream, 48 kHz, 16-bit, stereo; the output to the speakers (analog), to
 * an HDMI / DisplayPort monitor with sound (AUDIO_OUT), on its controller,
 * or to Bluetooth headphones: then each write goes on to btd (its port
 * "btaudio", straight from where it landed here) with the volume.
 *
 *   A controller: reset, then commands to a codec through the CORB (a ring
 *   we write) and answers through the RIRB (one it writes), polled.
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
#define RING (128 << 10)
#define PER  (16 << 10)

typedef struct {                                 /* a controller */
    volatile uint8_t *regs;
    uint32_t *corb;
    volatile uint32_t *rirb;
    unsigned cwp, rrp;
    int irq, iss;                                /* its line; input streams (the first output one's index) */
    uint64_t bdl;                                /* its buffer lists' bus address (input, then output) */
} hda_t;
#include "codec.c"

typedef struct {
    hda_t *c;                                    /* its controller */
    int sd;                                      /* its registers' offset */
    uint8_t *ring;
    uint32_t hp, pos, n;                         /* the controller's place (whole periods); ours; bytes queued or recorded */
    uint32_t bit, tag, run, drain;               /* INTSTS bit, stream tag, running, playing a last silence */
    long tok;                                    /* a client waiting (0: none) ... */
    uint64_t want;                               /* ... for a read: how much it can take */
} stream_t;

static hda_t hc[2];
static int nh, bt;                               /* bt: playing to Bluetooth headphones */
static long btp;
static stream_t out, in;

static long bt_call(msg_t *m)                    /* btd's port "btaudio" -> w[0] or -error */
{
    long e = btp > 0 || (btp = port_find("btaudio")) > 0 ? ipc_call(btp, m) : -ENOENT;
    if (e < 0) btp = 0;
    return e < 0 ? e : (long)m->w[0];
}

static uint8_t  r8(int r)              { return h->regs[r]; }
static void     w8(int r, uint8_t v)   { h->regs[r] = v; }
static void     w16(int r, uint16_t v) { *(volatile uint16_t *)(h->regs + r) = v; }
static uint32_t r32(int r)             { return mmio_r32(h->regs + r); }
static void     w32(int r, uint32_t v) { mmio_w32(h->regs + r, v); }
static int until(int r, uint8_t mask, uint8_t want)   /* 0 when the bits are so, -1 after 100 ms */
{
    for (long end = sys_clock() + 100000000; (r8(r) & mask) != want; ) if (sys_clock() > end) return -1;
    return 0;
}

/* A command to codec cad of controller h -> its answer (0 if none came).
 * v: 12-bit verb and 8-bit payload, or 4-bit verb and 16-bit payload. */
static uint32_t verb(int nid, uint32_t v)
{
    h->cwp = (h->cwp + 1) & 255;
    h->corb[h->cwp] = (uint32_t)cad << 28 | (uint32_t)nid << 20 | v;
    dma_wmb();
    w16(CORBWP, h->cwp);
    for (long end = sys_clock() + 100000000; r8(RIRBWP) == h->rrp; ) if (sys_clock() > end) return 0;
    h->rrp = (h->rrp + 1) & 255;
    w8(RIRBSTS, 5);                              /* (QEMU waits for this before the next answer) */
    return h->rirb[2 * h->rrp];
}

static void stop(stream_t *s)
{
    h = s->c;
    w8(s->sd, 0);
    until(s->sd, 2, 0);
    s->run = s->drain = s->hp = s->pos = s->n = 0;
}
static void start(stream_t *s)
{
    int r = s->sd;
    h = s->c;
    w8(r, 1); until(r, 1, 1);                    /* reset */
    w8(r, 0); until(r, 1, 0);
    w8(r + 2, s->tag << 4);
    w32(r + SD_CBL, RING); w16(r + SD_LVI, RING / PER - 1); w16(r + SD_FMT, FMT);
    uint64_t b = h->bdl + (s == &out ? 128 : 0);
    w32(r + SD_BDPL, (uint32_t)b); w32(r + SD_BDPU, (uint32_t)(b >> 32));
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

/* Output k (0: analog, else sink k, nsink + 1: Bluetooth): the output stream moves to its controller. */
static void output(int k)
{
    if ((!k && !ach) || k > nsink + 1) return;
    if (out.run) stop(&out);
    if (out.tok) { reply_val(out.tok, 0); out.tok = 0; }   /* (a writer waiting: send again) */
    if ((bt = k > nsink)) return;
    memset(out.ring, 0, RING);                   /* (what was queued: dropped) */
    route_out(k);
    out.c = k ? cur.c : ach;
    out.sd = SD0 + 0x20 * out.c->iss; out.bit = 1u << out.c->iss;
}

static void interrupt(void)
{
    for (h = hc; h < hc + nh; h++) {
        uint32_t s = r32(INTSTS);
        if (out.c == h && s & out.bit) {
            w8(out.sd + SD_STS, 0x1C);
            uint32_t from = out.hp, d = advance(&out), a = RING - from < d ? RING - from : d;
            memset(out.ring + from, 0, a);       /* played: silence from now on */
            memset(out.ring, 0, d - a);
            if (d < out.n) out.n -= d;
            else if (out.drain) stop(&out);
            else {                               /* all played: one more period of silence (the codec's buffer) */
                out.drain = 1; out.n = PER; out.pos = (out.hp + PER) % RING;
            }
            if (out.tok) { reply_val(out.tok, 0); out.tok = 0; }   /* room: send again */
        }
        if (in.c == h && s & in.bit) {
            w8(in.sd + SD_STS, 0x1C);
            in.n += advance(&in);
            if (in.n > RING - 2 * PER) stop(&in);   /* nobody reads: stop recording */
            else if (in.tok && in.n) reply_read();
        }
        irq_ack(h->irq & ~IRQ_LEVEL);
    }
}

#if defined(__x86_64__)
/* An NVIDIA GPU's HDA function (its HDMI / DP sound), enabled if the
 * firmware hid it: bit 25 of its function 0's register 0x488, in the
 * extended configuration space (Intel's PCIEXBAR; Linux: quirk_nvidia_hda). */
static void nvidia(void)
{
    pci_t g;
    uint64_t x = (pci_read(0) & 0xFFFF) == 0x8086 ? pci_read(0x60) | (uint64_t)pci_read(0x64) << 32 : 0;
    for (int i = 0; x & 1 && !pci_scan(&g, 0, 0x10DE, 0xFFFF, i); i++) {
        volatile uint32_t *r = pci_cfg(&g, 8) >> 24 == 3 && !(g.at & 0x700) ? map_phys((x & 0x7FFC000000) + ((uint64_t)g.at << 4), 4096) : 0;
        if ((long)r <= 0 || r[0x488 / 4] & 1u << 25) continue;
        r[0x488 / 4] |= 1u << 25;
        printf("audiod: NVIDIA GPU at %02x:%02x.0: its HDA function enabled\n", g.at >> 16, g.at >> 11 & 31);
    }
}
#endif

/* A controller: reset, CORB and RIRB, buffer lists, its codecs -> 0 */
static int controller(pci_t *p, uint64_t rpa)
{
    uint64_t len, pa, cpa;
    h = &hc[nh];
    if (p->irq < 0 || !(pa = pci_bar(p, 0, &len)) || !len || (long)(h->regs = map_phys(pa, 0x4000)) < 0) return -1;
    int intel = (p->id & 0xFFFF) == 0x8086, pch = intel && (p->id >> 16 < 0x2600 || p->id >> 16 >= 0x3B00);
    uint32_t cg = pci_cfg(p, 0x48);              /* (pch: a PCH, not an ICH; 0x48: CGCTL) */
    if (intel) pci_cfgw(p, 0x44, pci_cfg(p, 0x44) & ~7u);       /* TCSEL: traffic class 0 */
    if (pch) {
        pci_cfgw(p, 0x78, pci_cfg(p, 0x78) & ~0x800u);          /* snooped DMA (DEVC) */
        pci_cfgw(p, 0x48, cg & ~0x40u);          /* no clock gating while the link resets */
    }
    if ((p->id & 0xFFFF) == 0x10DE) pci_cfgw(p, 0x4C, pci_cfg(p, 0x4C) | 0x0F0101);   /* NVIDIA: snooped DMA */
    uint16_t gcap = (uint16_t)r32(GCAP);
    int iss = gcap >> 8 & 15, oss = gcap >> 12 & 15;
    printf("audiod: HDA %04x:%04x at %02x:%02x.%x, %s, HDA %d.%d, %d out and %d in streams, line %d\n",
           p->id & 0xFFFF, p->id >> 16, p->at >> 16, p->at >> 11 & 31, p->at >> 8 & 7,
           (pci_cfg(p, 8) >> 16 & 0xFF) == 1 ? "DSP mode (audio device class)" : "HDA mode",
           r8(3), r8(2), oss, iss, p->irq & ~IRQ_LEVEL);
    if (!oss) return -1;
    w32(INTCTL, 0);
    for (int i = 0; i < iss + oss; i++) w8(SD0 + 0x20 * i, 0);   /* streams the firmware ran */
    w8(CORBCTL, 0); w8(RIRBCTL, 0);
    w32(GCTL, 0);                                /* reset the link and the codecs */
    if (until(GCTL, 1, 0)) return -1;
    sys_sleep(100000);
    w32(GCTL, 1);
    if (until(GCTL, 1, 1)) return -1;
    sys_sleep(1000000);                          /* the codecs say they are there (521 µs) */
    if (pch) pci_cfgw(p, 0x48, cg);

    uint8_t *page = dma_alloc(4096, &cpa);
    if ((long)page < 0) return -1;
    cpa += p->bus_off; rpa += p->bus_off;
    h->corb = (uint32_t *)page; h->rirb = (uint32_t *)(page + 1024);
    w32(CORBLBASE, (uint32_t)cpa); w32(CORBUBASE, (uint32_t)(cpa >> 32));
    w8(CORBSIZE, 2); w16(CORBWP, 0);             /* 256 entries */
    /* (the reset left both rings' pointers at 0) */
    w32(RIRBLBASE, (uint32_t)(cpa + 1024)); w32(RIRBUBASE, (uint32_t)((cpa + 1024) >> 32));
    w8(RIRBSIZE, 2); w16(RINTCNT, 1);
    w8(CORBCTL, 2); w8(RIRBCTL, 3);              /* both run ("interrupt" for QEMU's count; CIE stays off) */
    h->bdl = cpa + 3072;
    uint32_t *b = (uint32_t *)(page + 3072);     /* the input ring's list, then the output one's */
    for (int j = 0; j < 2 * RING / PER; j++, b += 4) {
        uint64_t a = rpa + (uint64_t)j * PER;
        b[0] = (uint32_t)a; b[1] = (uint32_t)(a >> 32); b[2] = PER; b[3] = 1;   /* interrupt at its end */
    }
    h->irq = p->irq; h->iss = iss;
    nh++;
    uint32_t codecs = r8(STATESTS) | r8(STATESTS + 1) << 8;
    for (int a = 0; a < 15; a++) if (codecs >> a & 1) codec(a);
    w32(INTCTL, 1u << 31 | 1u << iss | (iss ? 1 : 0));
    return 0;
}

static int setup(void)
{
    pci_t p;
    uint64_t rpa;
    uint8_t *rings = dma_alloc(2 * RING, &rpa);
    if ((long)rings < 0) return -1;
#if defined(__x86_64__)
    nvidia();
#endif
    for (int i = 0; nh < 2 && !pci_scan(&p, 8, 0x040300 << 8, 0xFFFF0000, i); i++) controller(&p, rpa);   /* HDA */
    for (int i = 0; nh < 2 && !pci_scan(&p, 8, 0x040100 << 8, 0xFFFF0000, i); i++)
        if ((p.id & 0xFFFF) == 0x8086) controller(&p, rpa);   /* Intel's, its DSP on (not AC'97: no memory BAR) */
    hdmi_scan(1);
    if (!ach && !nsink) printf("audiod: no codec with a speaker, headphone, line out or monitor%s\n", port_find("bt") > 0 ? ": Bluetooth only" : "");
    if (!ach && !nsink && port_find("bt") <= 0) return -1;
    in.ring = rings; in.tag = TAG_IN; in.c = ach ? ach : hc; in.sd = SD0; in.bit = 1;
    out.ring = rings + RING; out.tag = TAG_OUT;
    msg_t q = { .w = { AUDIO_OUT } };
    output(bt_call(&q) >= 0 ? nsink + 1 : !ach);  /* headphones, else analog, else the first monitor */
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (setup()) { printf("audiod: no sound card\n"); return 2; }
    long port = port_create("audio"), from = 0, r = 0;
    for (int i = 0; i < nh; i++) if (irq_bind(hc[i].irq, port)) return 2;
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
            if (bt) {                                /* on to btd, from where it landed */
                msg_t f = { .w = { AUDIO_WRITE, (uint64_t)vol }, .sbuf = out.ring + out.pos, .slen = take };
                if ((r = bt_call(&f)) < 0) { output(!ach); r = 0; }   /* gone: back to the speakers (sent again) */
                break;
            }
            if (!out.c) { r = -ENOENT; break; }
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
            if (in.tok || !ach) { r = in.tok ? -EBUSY : -ENOENT; break; }
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
        case AUDIO_OUT: {                        /* (scanned again: a monitor or headphones gone send the sound back to analog) */
            char names[9 + 4 * 17 + 33] = "Speakers\n", *e = names + 9;
            int k = m.w[1] & AUDIO_ASK ? -1 : (int)m.w[1];
            if (hdmi_scan(0) && !bt) output(!ach);
            for (int i = 0; i < nsink; i++) { size_t l = strlen(sink[i].name); memcpy(e, sink[i].name, l); e += l; *e++ = '\n'; }
            msg_t q = { .w = { AUDIO_OUT }, .rbuf = e, .rlen = 32 };
            int hb = bt_call(&q) >= 0;               /* Bluetooth headphones, last */
            if (hb) { e += q.rlen; *e++ = '\n'; }
            if (bt && !hb) output(!ach);
            if (k == AUDIO_BT) k = hb ? nsink + 1 : 99;
            if (k > nsink + hb || (!k && !ach)) { r = -EINVAL; break; }
            if (k >= 0 && k != (bt ? nsink + 1 : sel)) output(k);
            msg_t a = { .w = { (uint64_t)(bt ? nsink + 1 : sel), (uint64_t)(nsink + 1 + hb) }, .sbuf = names, .slen = (uint64_t)(e - names) };
            ipc_reply(from, &a);
            continue;
        }
        default: r = -ENOSYS;
        }
        answer = 1;
    }
}
