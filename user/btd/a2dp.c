/*
 * a2dp.c - Headphones and speakers: A2DP source (AVDTP 1.3, SBC), one
 * sink at a time.
 *
 *   signalling   L2CAP PSM 0x19. As initiator (we connected: bt connect,
 *                pairing; or 3 s after a bonded sink came back and did not
 *                start itself): SDP (an audio sink? AVDTP's version), then
 *                Discover, Get Capabilities of each sink endpoint until one
 *                has SBC, Set Configuration, Open, the media channel. As
 *                acceptor, the sink does that to our one endpoint (SEID 1,
 *                audio source). Start when sound comes, Suspend after 5 s of
 *                none. Our SBC: 48 kHz, joint stereo, 16 blocks, 8 subbands,
 *                loudness, bitpool 53 or the sink's most
 *   media        a second channel on PSM 0x19: RTP (version 2, type 96,
 *                timestamp in samples), one byte of SBC header (frames in
 *                the packet), as many frames as the sink's MTU takes
 *   sound        audiod's writes land in pcm_ring (btd.c, port "btaudio");
 *                a2dp_pump sends each packet when the media clock says,
 *                from the time streaming began (or caught up after a gap)
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "btd.h"

enum { A_IDLE, A_SDP, A_SIG, A_CONF, A_OPEN, A_STREAM };   /* A_SIG: configuring; A_CONF: configured; A_OPEN: media up */
static struct {
    conn_t *c, *later;                           /* later: a sink that came back by itself */
    l2ch_t *sig, *media;
    int st, init, wait, idle, label, want;       /* init: we are the initiator; want: our command waiting for its answer */
    int nsep, isep, min;
    uint8_t seid, sep[8];                        /* the sink's endpoint; the candidates */
    uint16_t seq;
    uint32_t ts;
    int64_t next;                                /* when the next packet is due (ns) */
    sbc_t sbc;
} av;
uint8_t pcm_ring[PCM_RING];
uint32_t pcm_w, pcm_r;
volatile int a2dp_new;

static void sig(int type, int id, const uint8_t *d, int n, int label)
{
    uint8_t b[16] = { (uint8_t)(label << 4 | type), (uint8_t)id };
    memcpy(b + 2, d, n);
    l2_send(av.sig, b, n + 2);
}
static void cmd1(int id, const uint8_t *d, int n) { av.want = id; av.label = (av.label + 1) & 15; sig(0, id, d, n, av.label); }
static void cmd_seid(int id, int seid) { uint8_t s = (uint8_t)(seid << 2); cmd1(id, &s, 1); }

static void reset(const char *why)
{
    if (!av.c) return;
    bt_log("bt: %s: A2DP ends (%s)\n", av.c->dev->d.name, why);
    av.c->dev->d.flags &= ~BTF_AUDIO;
    if (av.media) l2_close(av.media);
    if (av.sig) l2_close(av.sig);
    memset(&av, 0, sizeof av);
    pcm_r = pcm_w;
}

/* Media transport and SBC: what we take, or (conf) what is configured. */
static int caps(uint8_t *b, int conf)
{
    static const uint8_t ours[10] = { 1, 0, 7, 6, 0, 0, 0x11, 0x15, 2, 53 };   /* 48 kHz, joint; 16 blocks, 8 subbands, loudness */
    memcpy(b, ours, 10);
    if (conf) { b[8] = (uint8_t)av.min; b[9] = av.sbc.bitpool; }
    return 10;
}
/* The SBC codec in a capability list, if it takes our configuration -> its 4 bytes. */
static const uint8_t *sbc_of(const uint8_t *p, int n)
{
    for (int i = 0; i + 2 <= n && i + 2 + p[i + 1] <= n; i += 2 + p[i + 1])
        if (p[i] == 7 && p[i + 1] >= 6 && !(p[i + 2] >> 4) && !p[i + 3]) {
            const uint8_t *s = p + i + 4;
            return (s[0] & 0x11) == 0x11 && (s[1] & 0x15) == 0x15 && s[2] <= s[3] ? s : 0;
        }
    return 0;
}
static void configure(const uint8_t *s)
{
    int bp = s[3] < 53 ? s[3] : 53;
    av.min = s[2]; av.sbc.bitpool = (uint8_t)(bp < s[2] ? s[2] : bp); av.sbc.freq = 3; av.st = A_CONF;
    bt_log("bt: %s: A2DP: SBC 48 kHz joint stereo, 16 blocks, 8 subbands, loudness; bitpool %d (it takes %d-%d), %d bytes a frame\n",
           av.c->dev->d.name, av.sbc.bitpool, s[2], s[3], sbc_len(av.sbc.bitpool));
}
static void next_sep(void)
{
    if (av.isep < av.nsep) cmd_seid(2, av.sep[av.isep++]);
    else reset("no sink endpoint takes our SBC");
}

static void sig_recv(l2ch_t *h, const uint8_t *p, int n)
{
    if (h != av.sig || n < 2) return;
    int label = p[0] >> 4, type = p[0] & 3, id = p[1] & 0x3F, k = 0;
    uint8_t r[12];
    const uint8_t *s;
    if (!type) {                                          /* a command from the sink */
        switch (id) {
        case 0x01: r[0] = (uint8_t)(1 << 2 | (av.st >= A_CONF) << 1); r[1] = 0; k = 2; break;   /* Discover: SEID 1, audio source */
        case 0x02: case 0x0C: k = caps(r, 0); break;      /* Get (All) Capabilities */
        case 0x04: k = caps(r, 1); break;                 /* Get Configuration */
        case 0x03:                                        /* Set Configuration */
            if (av.st >= A_CONF || n < 4 || !(s = sbc_of(p + 4, n - 4))) { r[0] = 7; r[1] = 0x29; sig(3, id, r, 2, label); return; }
            av.seid = p[3] >> 2;
            configure(s);
            break;
        case 0x07: if (av.media) { av.st = A_STREAM; av.next = 0; bt_log("bt: A2DP: streaming (the sink started)\n"); } break;
        case 0x09: if (av.st == A_STREAM) av.st = A_OPEN; break;
        case 0x06: case 0x08: case 0x0A: break;           /* Open (the media channel comes), Close, Abort */
        default: { uint8_t g[2] = { (uint8_t)(label << 4 | 1), (uint8_t)id }; l2_send(h, g, 2); return; }   /* general reject */
        }
        sig(2, id, r, k, label);
        return;
    }
    if (id != av.want || label != av.label) return;       /* an answer to ours */
    av.want = 0;
    if (type != 2) { bt_log("bt: A2DP: the sink refuses command %d (%02x)\n", id, n > 2 ? p[2] : 0); reset("refused"); return; }
    switch (id) {
    case 0x01:                                            /* Discover: its free audio sinks */
        for (int i = 2; i + 1 < n && av.nsep < 8; i += 2)
            if (!(p[i] & 2) && !(p[i + 1] >> 4) && p[i + 1] & 8) av.sep[av.nsep++] = p[i] >> 2;
        bt_log("bt: A2DP: %d free sink endpoint%s\n", av.nsep, av.nsep == 1 ? "" : "s");
        next_sep();
        break;
    case 0x02:                                            /* Get Capabilities */
        if (!(s = sbc_of(p + 2, n - 2))) { next_sep(); break; }
        av.seid = av.sep[av.isep - 1];
        configure(s);
        r[0] = (uint8_t)(av.seid << 2); r[1] = 1 << 2;
        k = caps(r + 2, 1);
        cmd1(0x03, r, 2 + k);
        break;
    case 0x03: cmd_seid(0x06, av.seid); break;            /* configured: Open */
    case 0x06: if (!(av.media = l2_open(av.c, 0x19, h->ops))) reset("no media channel"); break;
    case 0x07: av.st = A_STREAM; av.next = 0; bt_log("bt: A2DP: streaming\n"); break;
    case 0x09: av.st = A_OPEN; bt_log("bt: A2DP: suspended\n"); break;
    }
}

static void ch_open(l2ch_t *h)
{
    if (h == av.sig) { if (av.init) cmd1(1, 0, 0); return; }   /* ours: Discover */
    if (!av.c || (av.c == h->c && !av.sig)) {             /* the sink's signalling channel */
        av.c = h->c; av.sig = h; av.init = 0; av.st = A_SIG; av.wait = 0;
        return;
    }
    if (h != av.media) {
        if (av.c != h->c || av.media || av.st != A_CONF) { l2_close(h); return; }   /* (busy) */
        av.media = h;
    }
    av.st = A_OPEN;                                       /* the media channel: ready */
    av.c->dev->d.flags |= BTF_AUDIO;
    a2dp_new = 1;
    int fl = sbc_len(av.sbc.bitpool), k = ((h->rmtu < L2_MTU ? h->rmtu : L2_MTU) - 13) / fl;
    bt_log("bt: %s: A2DP ready: %d SBC frames a packet (MTU %d), %d kbit/s\n", av.c->dev->d.name, k > 15 ? 15 : k, h->rmtu, fl * 8 * 375 / 1000);
}
static void ch_close(l2ch_t *h) { if (h == av.sig || h == av.media) reset("channel closed"); }
static const l2ops_t ops = { ch_open, sig_recv, ch_close };

static void found(conn_t *c, const sdp_res_t *r)
{
    if (av.c != c || av.st != A_SDP) return;
    if (!r) { reset("no audio sink service"); return; }
    av.st = A_SIG;
    if (!(av.sig = l2_open(c, r->psm ? (uint16_t)r->psm : 0x19, &ops))) reset("no channel");
}
static void start(conn_t *c)
{
    av.c = c; av.st = A_SDP; av.init = 1;
    bt_log("bt: %s: A2DP: looking for its audio sink\n", c->dev->d.name);
    if (sdp_query(c, 0x110B, found)) reset("SDP busy");
}

void a2dp_init(void) { l2_listen(0x19, &ops); }
void a2dp_link(conn_t *c)
{
    if (av.c) return;
    if (c->out) start(c);
    else { av.later = c; av.wait = 3; }
}
void a2dp_tick(void)
{
    if (av.wait && !--av.wait && !av.c && av.later->dev && av.later->enc) start(av.later);
    if (av.st < A_OPEN || ++av.idle < 3) return;
    if (av.st == A_OPEN) pcm_r = pcm_w;                   /* not started in 3 s: what waits is dropped */
    else if (av.idle >= 5 && !av.want) cmd_seid(0x09, av.seid);   /* no sound for 5 s: Suspend */
}
const char *a2dp_sink(void) { return av.st >= A_OPEN ? av.c->dev->d.name : 0; }

int a2dp_put(uint32_t at, int n, int vol)
{
    if (av.st < A_OPEN) return -ENOENT;
    if (at != pcm_w) return 0;                            /* (the ring was emptied meanwhile) */
    pcm_w += (uint32_t)n; av.idle = 0;
    av.sbc.gain = vol & AUDIO_MUTE ? 0 : (vol & 0xFF) * (vol & 0xFF) * 65536 / 10000;
    if (av.st == A_OPEN && !av.want) cmd_seid(0x07, av.seid);   /* Start */
    return n;
}

int64_t a2dp_pump(int64_t now)
{
    if (av.st != A_STREAM) return 0;
    int fl = sbc_len(av.sbc.bitpool), k = ((av.media->rmtu < L2_MTU ? av.media->rmtu : L2_MTU) - 13) / fl;
    if (k > 15) k = 15;
    for (static uint8_t b[L2_MTU]; pcm_w - pcm_r >= (uint32_t)k * 512; ) {
        if (av.next > now) return av.next - now;
        if (now - av.next > 100000000) av.next = now;     /* (a gap: from now on) */
        b[0] = 0x80; b[1] = 96; pb16(b + 2, av.seq); pb16(b + 4, (int)(av.ts >> 16)); pb16(b + 6, (int)av.ts);
        pb16(b + 8, 0); pb16(b + 10, 1); b[12] = (uint8_t)k;
        int n = 13;
        for (int i = 0; i < k; i++, pcm_r += 512) n += sbc_frame(&av.sbc, (const int16_t *)pcm_ring, pcm_r / 2, PCM_RING / 2 - 1, b + n);
        if (bt.acl_free > 0) l2_send(av.media, b, n);     /* (the radio is behind: this one is dropped) */
        av.seq++; av.ts += (uint32_t)k * 128; av.next += (int64_t)k * 128 * 1000000000 / 48000;
    }
    return 0;
}
