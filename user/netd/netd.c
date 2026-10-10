/*
 * netd - The network server: IPv4, ARP, ICMP (ping), UDP, TCP, and the
 * DHCP and DNS clients, serving the port "net" (mk/proto.h, docs/net.md).
 *
 *   programs ──NET_*──► netd ──NIC_SEND──► vnet ──► the network card
 *                        ▲                   │
 *                        └────NET_FRAMES─────┘ (frames received, in batches)
 *
 * One thread does everything, one message at a time, so no locks: a client
 * request, a batch of received frames, or a tick of the timer thread. Calls
 * that must wait (a connection, data, a DNS answer) keep the client's reply
 * token and answer it later. Frames to send collect in one buffer, sent to
 * the driver once per message handled.
 *
 * TCP keeps, per connection, a send buffer (data not yet acknowledged, for
 * retransmission) and a receive buffer; it measures the round trip (RFC
 * 6298) to set its retransmission timer, does slow start, congestion
 * avoidance and fast retransmit (RFC 5681), uses window scaling (RFC 7323)
 * so a 256 KiB receive window fits, keeps out-of-order segments, and ends
 * connections with FIN or RST like any other system.
 *
 * Not done (see docs/net.md): IPv6, fragments (DF is set, MSS keeps us
 * below the MTU), selective acknowledgements, keepalives, routing tables
 * beyond "local network or the gateway".
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

#define MS          1000000L                   /* ns */
#define SEC         1000000000L
#define MAXSOCK     64
#define SNDBUF      (64 << 10)
#define RCVBUF      (256 << 10)
#define RCV_WSCALE  3                          /* our window field counts 8-byte units */
#define MSS_OURS    1460
#define TIME_WAIT_NS (10 * SEC)                /* shorter than the classic 2 MSL: no reuse of
                                                  random ports within seconds anyway */
#define MAXOOO      32                         /* out-of-order segments kept per connection */

static int64_t now(void) { return sys_clock(); }

/* ---- Big-endian fields in packets. */
static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void p16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void p32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* The Internet checksum: one's complement sum of 16-bit words. */
static uint32_t csum_add(uint32_t s, const uint8_t *p, size_t n)
{
    for (; n > 1; p += 2, n -= 2) s += g16(p);
    if (n) s += (uint32_t)p[0] << 8;
    return s;
}
static uint16_t csum_fold(uint32_t s)
{
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

/* ---- The interface's state. */
static uint8_t mac[6];
static uint32_t my_ip, my_mask, my_gw, my_dns;
static int nic_up;
static long nic, port;
static int64_t rtc_base, rtc_ns;               /* the date, from the driver's clock chip read */
static net_info_t stats;

/* Frames to send: [16-bit length][frame]..., flushed once per message. */
static uint8_t txbuf[NET_MAX];
static size_t txlen;

static void tx_flush(void)
{
    if (!txlen) return;
    msg_t m = { .w = { NIC_SEND }, .sbuf = txbuf, .slen = txlen };
    call_named(&nic, "nic0", &m, 1);
    txlen = 0;
}

static void eth_send(const uint8_t dst[6], uint16_t type, const uint8_t *payload, size_t n)
{
    size_t fl = 14 + n < 60 ? 60 : 14 + n;       /* Ethernet frames are at least 60 bytes */
    if (txlen + 2 + fl > sizeof txbuf) tx_flush();
    uint8_t *f = txbuf + txlen + 2;
    memcpy(f, dst, 6); memcpy(f + 6, mac, 6); p16(f + 12, type);
    memcpy(f + 14, payload, n);
    memset(f + 14 + n, 0, fl - 14 - n);
    txbuf[txlen] = (uint8_t)fl; txbuf[txlen + 1] = (uint8_t)(fl >> 8);
    txlen += 2 + fl;
    stats.tx_packets++; stats.tx_bytes += fl;
}

/* ---- ARP: who has this IPv4 address? Answers are cached; IP packets for
 * an address not yet known wait (a few) until the answer comes. */
static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static struct { uint32_t ip; uint8_t mac[6]; int64_t expires; } arp[32];
static struct { uint32_t ip; uint16_t len; uint8_t *pkt; int64_t sent; int tries; } pend[16];

static void arp_send(int op, const uint8_t *tmac, uint32_t tip)
{
    uint8_t a[28];
    p16(a, 1); p16(a + 2, 0x0800); a[4] = 6; a[5] = 4; p16(a + 6, op);
    memcpy(a + 8, mac, 6); p32(a + 14, my_ip);
    memcpy(a + 18, op == 1 ? (const uint8_t *)"\0\0\0\0\0\0" : tmac, 6); p32(a + 24, tip);
    eth_send(op == 1 ? BCAST : tmac, 0x0806, a, 28);
}

static const uint8_t *arp_lookup(uint32_t ip)
{
    for (int i = 0; i < 32; i++) if (arp[i].ip == ip && arp[i].expires > now()) return arp[i].mac;
    return 0;
}

static void ip_out_frame(uint32_t hop, const uint8_t *pkt, size_t n);

static void arp_learn(uint32_t ip, const uint8_t *m)
{
    int slot = 0;
    for (int i = 0; i < 32; i++) {
        if (arp[i].ip == ip) { slot = i; break; }
        if (arp[i].expires < arp[slot].expires) slot = i;
    }
    arp[slot].ip = ip; memcpy(arp[slot].mac, m, 6); arp[slot].expires = now() + 300 * SEC;
    for (int i = 0; i < 16; i++)                 /* send what waited for this address */
        if (pend[i].pkt && pend[i].ip == ip) {
            ip_out_frame(ip, pend[i].pkt, pend[i].len);
            free(pend[i].pkt); pend[i].pkt = 0;
        }
}

static void ip_out_frame(uint32_t hop, const uint8_t *pkt, size_t n)
{
    const uint8_t *dm = hop == 0xffffffff ? BCAST : arp_lookup(hop);
    if (dm) { eth_send(dm, 0x0800, pkt, n); return; }
    for (int i = 0; i < 16; i++)
        if (!pend[i].pkt && (pend[i].pkt = malloc(n))) {
            memcpy(pend[i].pkt, pkt, n);
            pend[i].ip = hop; pend[i].len = (uint16_t)n; pend[i].sent = now(); pend[i].tries = 1;
            arp_send(1, 0, hop);
            return;
        }
    /* no room: dropped; TCP will retransmit */
}

/* Send an IPv4 packet: header + payload (the payload is copied). */
static uint16_t ip_id;
static void ip_send(uint32_t dst, int proto, const uint8_t *payload, size_t n)
{
    static uint8_t pkt[1600];
    if (n > 1480) return;
    uint8_t *h = pkt;
    h[0] = 0x45; h[1] = 0; p16(h + 2, 20 + n); p16(h + 4, ++ip_id); p16(h + 6, 0x4000);   /* DF */
    h[8] = 64; h[9] = (uint8_t)proto; p16(h + 10, 0);
    p32(h + 12, my_ip); p32(h + 16, dst);
    p16(h + 10, csum_fold(csum_add(0, h, 20)));
    memcpy(pkt + 20, payload, n);
    uint32_t hop = dst == 0xffffffff ? dst : ((dst & my_mask) == (my_ip & my_mask) ? dst : my_gw);
    if (!hop) return;
    ip_out_frame(hop, pkt, 20 + n);
}

/* TCP/UDP checksum over the "pseudo-header" and the segment. */
static uint16_t l4_csum(uint32_t src, uint32_t dst, int proto, const uint8_t *seg, size_t n)
{
    uint8_t ph[12];
    p32(ph, src); p32(ph + 4, dst); ph[8] = 0; ph[9] = (uint8_t)proto; p16(ph + 10, n);
    return csum_fold(csum_add(csum_add(0, ph, 12), seg, n));
}

static void udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const uint8_t *data, size_t n)
{
    static uint8_t seg[1500];
    if (n > 1472) return;
    p16(seg, sport); p16(seg + 2, dport); p16(seg + 4, 8 + n); p16(seg + 6, 0);
    memcpy(seg + 8, data, n);
    uint16_t c = l4_csum(my_ip, dst, 17, seg, 8 + n);
    p16(seg + 6, c ? c : 0xffff);
    ip_send(dst, 17, seg, 8 + n);
}

/* ---- Waiting clients: a reply token, what it waits for, until when. */
typedef struct { long token; int64_t deadline; size_t max; } wait_t;

static void answer(long token, long r, uint64_t w1, uint64_t w2, const void *data, size_t n)
{
    msg_t m = { .w = { (uint64_t)r, w1, w2 }, .sbuf = data, .slen = n };
    ipc_reply(token, &m);
}
static int64_t deadline_ms(uint64_t ms, int64_t dflt) { return ms ? now() + (int64_t)ms * MS : (dflt ? now() + dflt : 0); }

static uint8_t outbuf[NET_MAX];                /* data going back to a client (one reply at a time) */

/* ---- Byte rings: TCP's send and receive buffers. */
typedef struct { uint8_t *b; uint32_t size, head, len; } ring_t;
static void ring_put(ring_t *r, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) r->b[(r->head + r->len + i) % r->size] = p[i];
    r->len += n;
}
static void ring_get(const ring_t *r, uint32_t off, uint8_t *p, uint32_t n)   /* peek */
{
    uint32_t s = (r->head + off) % r->size, c = r->size - s < n ? r->size - s : n;
    memcpy(p, r->b + s, c);
    memcpy(p + c, r->b, n - c);
}
static void ring_drop(ring_t *r, uint32_t n) { r->head = (r->head + n) % r->size; r->len -= n; }

/* ---- Sockets. */
enum { T_CLOSED, T_LISTEN, T_SYN_SENT, T_SYN_RCVD, T_ESTAB, T_FIN_WAIT1, T_FIN_WAIT2,
       T_CLOSING, T_TIME_WAIT, T_CLOSE_WAIT, T_LAST_ACK, T_WAIT_NET };
typedef struct { uint32_t seq, len; uint8_t *data; } ooo_t;
typedef struct sock {
    long handle;
    int type, owner, uid, state, err;
    uint16_t lport, rport;
    uint32_t rip;
    /* TCP */
    uint32_t iss, snd_una, snd_nxt, snd_wnd, rcv_nxt, mss, cwnd, ssthresh, dupacks;
    int snd_wscale, closing, eof, fin_sent, retries, need_ack;
    int64_t rto, srtt, rttvar, rtx_at, timed_at, tw_at;   /* ns; 0 = not set */
    uint32_t timed_seq;
    ring_t snd, rcv;
    ooo_t ooo[MAXOOO];
    wait_t rd, wr, conn;                       /* waiting: data, room, the connection */
    struct sock *parent;                       /* SYN_RCVD/ESTAB child of a listener ... */
    struct sock *accq[16]; int naccq, backlog; /* ... the listener's established children */
    wait_t acc;
    /* UDP */
    struct dgram { struct dgram *next; uint32_t ip; uint16_t port, len; uint8_t data[]; } *dq, *dqt;
    int ndq;
} sock_t;
static sock_t *socks[MAXSOCK];
static long next_handle = 1;

static sock_t *sock_new(int type, int owner, int uid)
{
    for (int i = 0; i < MAXSOCK; i++)
        if (!socks[i]) {
            sock_t *s = malloc(sizeof *s);
            if (!s) return 0;
            memset(s, 0, sizeof *s);
            s->handle = next_handle++; s->type = type; s->owner = owner; s->uid = uid;
            socks[i] = s;
            stats.sockets++;
            return s;
        }
    return 0;
}

static void sock_free(sock_t *s)
{
    for (int i = 0; i < MAXSOCK; i++) if (socks[i] == s) socks[i] = 0;
    free(s->snd.b); free(s->rcv.b);
    for (int i = 0; i < MAXOOO; i++) free(s->ooo[i].data);
    while (s->dq) { struct dgram *d = s->dq; s->dq = d->next; free(d); }
    for (int i = 0; i < MAXSOCK; i++) if (socks[i] && socks[i]->parent == s) socks[i]->parent = 0;
    if (s->parent)
        for (int i = 0; i < s->parent->naccq; i++)
            if (s->parent->accq[i] == s) s->parent->accq[i] = s->parent->accq[--s->parent->naccq];
    free(s);
    stats.sockets--;
}

static sock_t *by_handle(long h, int pid)
{
    for (int i = 0; i < MAXSOCK; i++) if (socks[i] && socks[i]->handle == h && socks[i]->owner == pid) return socks[i];
    return 0;
}

static int port_used(int type, uint16_t p)
{
    for (int i = 0; i < MAXSOCK; i++) if (socks[i] && socks[i]->type == type && socks[i]->lport == p) return 1;
    return 0;
}

static uint16_t ephemeral(int type)
{
    uint16_t p;
    do { sys_random(&p, 2); p = 49152 + p % 16384; } while (port_used(type, p));
    return p;
}

/* Wake a waiter with a result. */
static void wake(wait_t *w, long r) { if (w->token) { answer(w->token, r, 0, 0, 0, 0); w->token = 0; } }

/* ---- TCP output. */
enum { FIN = 1, SYN = 2, RST = 4, PSH = 8, ACK = 16 };

static uint32_t rcv_window(sock_t *s)
{
    uint32_t free_ = s->rcv.b ? s->rcv.size - s->rcv.len : 0;
    return free_ >> RCV_WSCALE;
}

static void tcp_raw(uint32_t src_ip, uint32_t dst, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                    int flags, uint32_t win, const uint8_t *opt, size_t olen, const uint8_t *data, size_t n)
{
    static uint8_t seg[1600];
    size_t hl = 20 + olen;
    p16(seg, sport); p16(seg + 2, dport); p32(seg + 4, seq); p32(seg + 8, ack);
    seg[12] = (uint8_t)(hl / 4 << 4); seg[13] = (uint8_t)flags;
    p16(seg + 14, win > 65535 ? 65535 : win); p16(seg + 16, 0); p16(seg + 18, 0);
    memcpy(seg + 20, opt, olen);
    if (n) memcpy(seg + hl, data, n);
    p16(seg + 16, l4_csum(src_ip, dst, 6, seg, hl + n));
    ip_send(dst, 6, seg, hl + n);
}

static void tcp_seg(sock_t *s, uint32_t seq, int flags, const uint8_t *data, size_t n)
{
    uint8_t opt[12];
    size_t ol = 0;
    if (flags & SYN) {                           /* MSS, window scale */
        opt[0] = 2; opt[1] = 4; p16(opt + 2, MSS_OURS);
        opt[4] = 1; opt[5] = 3; opt[6] = 3; opt[7] = RCV_WSCALE;
        ol = 8;
    }
    uint32_t win = flags & SYN ? (rcv_window(s) << RCV_WSCALE > 65535 ? 65535 : rcv_window(s) << RCV_WSCALE) : rcv_window(s);
    tcp_raw(my_ip, s->rip, s->lport, s->rport, seq, s->rcv_nxt, flags | (flags & SYN && s->state == T_SYN_SENT ? 0 : ACK),
            win, opt, ol, data, n);
    s->need_ack = 0;
}

static void arm_rtx(sock_t *s) { s->rtx_at = now() + s->rto; }

/* Send what the windows allow: new data, then FIN once all data is out. */
static void tcp_output(sock_t *s)
{
    if (s->state != T_ESTAB && s->state != T_CLOSE_WAIT && s->state != T_FIN_WAIT1 &&
        s->state != T_CLOSING && s->state != T_LAST_ACK) return;
    uint32_t wnd = s->snd_wnd < s->cwnd ? s->snd_wnd : s->cwnd;
    static uint8_t buf[MSS_OURS];
    while (!s->fin_sent) {
        uint32_t sent = s->snd_nxt - s->snd_una;          /* in flight */
        uint32_t unsent = s->snd.len > sent ? s->snd.len - sent : 0;
        uint32_t room = wnd > sent ? wnd - sent : 0;
        uint32_t n = unsent < room ? unsent : room;
        if (n > s->mss) n = s->mss;
        if (!n) break;
        ring_get(&s->snd, sent, buf, n);
        tcp_seg(s, s->snd_nxt, PSH, buf, n);
        if (!s->timed_at) { s->timed_at = now(); s->timed_seq = s->snd_nxt + n; }
        s->snd_nxt += n;
        if (!s->rtx_at) arm_rtx(s);
    }
    uint32_t sent = s->snd_nxt - s->snd_una;
    if (s->closing && !s->fin_sent && sent == s->snd.len) {   /* all data out: FIN */
        tcp_seg(s, s->snd_nxt, FIN, 0, 0);
        s->snd_nxt++;
        s->fin_sent = 1;
        if (!s->rtx_at) arm_rtx(s);
    }
    if (s->need_ack) tcp_seg(s, s->snd_nxt, 0, 0, 0);
    if (!s->rtx_at && s->snd_wnd == 0 && s->snd.len > (s->snd_nxt - s->snd_una)) arm_rtx(s);   /* window probe */
}

static void tcp_drop(sock_t *s, int err)             /* the connection is gone */
{
    s->state = T_CLOSED;
    s->err = err;
    s->rtx_at = 0;
    wake(&s->conn, -err); wake(&s->rd, s->eof ? 0 : -err); wake(&s->wr, -err);
    if (!s->owner) sock_free(s);                 /* nobody holds it any more */
}

static void tcp_time_wait(sock_t *s)
{
    s->state = T_TIME_WAIT;
    s->tw_at = now() + TIME_WAIT_NS;
    s->rtx_at = 0;
}

/* ---- TCP input. */
static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

static void deliver_data(sock_t *s)                  /* wake a reader if there is data or EOF */
{
    if (!s->rd.token || (!s->rcv.len && !s->eof)) return;
    uint32_t n = s->rcv.len < s->rd.max ? s->rcv.len : (uint32_t)s->rd.max;
    ring_get(&s->rcv, 0, outbuf, n);
    uint32_t before = rcv_window(s);
    ring_drop(&s->rcv, n);
    answer(s->rd.token, n, 0, 0, outbuf, n);
    s->rd.token = 0;
    if (before < 2 * s->mss / (1 << RCV_WSCALE) + 1 && rcv_window(s) > before)   /* window opened: say so */
        tcp_seg(s, s->snd_nxt, 0, 0, 0);
}

static void ooo_store(sock_t *s, uint32_t seq, const uint8_t *d, uint32_t n)
{
    for (int i = 0; i < MAXOOO; i++) if (s->ooo[i].data && s->ooo[i].seq == seq) return;
    for (int i = 0; i < MAXOOO; i++)
        if (!s->ooo[i].data && (s->ooo[i].data = malloc(n))) {
            memcpy(s->ooo[i].data, d, n);
            s->ooo[i].seq = seq; s->ooo[i].len = n;
            return;
        }
}

static void accept_child(sock_t *l, sock_t *c)
{
    if (l->acc.token) {
        c->owner = l->owner;                     /* hand it over at once */
        answer(l->acc.token, c->handle, c->rip, c->rport, 0, 0);
        l->acc.token = 0;
        c->parent = 0;
    } else if (l->naccq < 16) l->accq[l->naccq++] = c;
}

static void parse_opts(sock_t *s, const uint8_t *o, size_t n)
{
    s->snd_wscale = 0;
    for (size_t i = 0; i < n;) {
        if (o[i] == 0) break;
        if (o[i] == 1) { i++; continue; }
        if (i + 1 >= n || o[i + 1] < 2 || i + o[i + 1] > n) break;
        if (o[i] == 2 && o[i + 1] == 4) { uint32_t m = g16(o + i + 2); if (m >= 536 && m < s->mss) s->mss = m; }
        if (o[i] == 3 && o[i + 1] == 3) s->snd_wscale = o[i + 2] > 14 ? 14 : o[i + 2];
        i += o[i + 1];
    }
}

static void tcp_new_conn(sock_t *s)                  /* buffers and starting values */
{
    s->snd.b = malloc(SNDBUF); s->snd.size = SNDBUF;
    s->rcv.b = malloc(RCVBUF); s->rcv.size = RCVBUF;
    sys_random(&s->iss, 4);
    s->snd_una = s->snd_nxt = s->iss;
    s->mss = 536;                                /* until the peer says more */
    s->rto = SEC; s->srtt = 0;
    s->cwnd = 10 * 1460; s->ssthresh = 1 << 30;
}

static void tcp_input(uint32_t src, const uint8_t *seg, size_t n)
{
    if (n < 20 || l4_csum(src, my_ip, 6, seg, n) != 0) return;   /* bad checksum */
    uint16_t sport = g16(seg), dport = g16(seg + 2);
    uint32_t seq = g32(seg + 4), ack = g32(seg + 8);
    size_t hl = (seg[12] >> 4) * 4;
    int fl = seg[13];
    uint32_t win = g16(seg + 14);
    if (hl < 20 || hl > n) return;
    const uint8_t *data = seg + hl;
    uint32_t dlen = (uint32_t)(n - hl);

    sock_t *s = 0, *l = 0;
    for (int i = 0; i < MAXSOCK; i++) {
        sock_t *t = socks[i];
        if (!t || t->type != NET_TCP || t->lport != dport) continue;
        if (t->state == T_LISTEN) l = t;
        else if (t->rip == src && t->rport == sport && t->state != T_CLOSED) { s = t; break; }
    }
    if (!s) {
        if (l && (fl & (SYN | ACK | RST)) == SYN) {         /* a new connection */
            int count = 0;
            for (int i = 0; i < MAXSOCK; i++) if (socks[i] && socks[i]->parent == l) count++;
            if (count >= l->backlog) return;
            sock_t *c = sock_new(NET_TCP, 0, l->uid);
            if (!c) return;
            tcp_new_conn(c);
            if (!c->snd.b || !c->rcv.b) { sock_free(c); return; }
            c->lport = dport; c->rport = sport; c->rip = src; c->parent = l;
            c->rcv_nxt = seq + 1;
            c->mss = 536;
            parse_opts(c, seg + 20, hl - 20);
            c->snd_wnd = win;                    /* not scaled in a SYN */
            c->state = T_SYN_RCVD;
            tcp_seg(c, c->snd_nxt, SYN, 0, 0);
            c->snd_nxt++;
            arm_rtx(c);
            return;
        }
        if (fl & RST) return;                    /* nobody here: reset */
        if (fl & ACK) tcp_raw(my_ip, src, dport, sport, ack, 0, RST, 0, 0, 0, 0, 0);
        else tcp_raw(my_ip, src, dport, sport, 0, seq + dlen + !!(fl & SYN) + !!(fl & FIN), RST | ACK, 0, 0, 0, 0, 0);
        return;
    }

    if (s->state == T_SYN_SENT) {
        if (fl & ACK && ack != s->iss + 1) { if (!(fl & RST)) tcp_raw(my_ip, src, dport, sport, ack, 0, RST, 0, 0, 0, 0, 0); return; }
        if (fl & RST) { if (fl & ACK) tcp_drop(s, ECONNREFUSED); return; }
        if (!(fl & SYN)) return;
        s->rcv_nxt = seq + 1;
        parse_opts(s, seg + 20, hl - 20);
        if (!(fl & ACK)) return;                 /* simultaneous open: not supported */
        s->snd_una = ack;
        s->snd_wnd = win << s->snd_wscale;
        s->state = T_ESTAB;
        s->rtx_at = 0; s->retries = 0;
        if (s->timed_at) { s->srtt = now() - s->timed_at; s->rttvar = s->srtt / 2; s->timed_at = 0; }
        s->rto = s->srtt + 4 * s->rttvar < 200 * MS ? 200 * MS : s->srtt + 4 * s->rttvar;
        tcp_seg(s, s->snd_nxt, 0, 0, 0);
        wake(&s->conn, 0);
        return;
    }

    /* Acceptable segment? (inside our receive window, or a bare ACK at its edge) */
    uint32_t rwin = s->rcv.b ? s->rcv.size - s->rcv.len : 0;
    int ok = dlen == 0 ? (seq_le(s->rcv_nxt, seq) && seq_le(seq, s->rcv_nxt + rwin)) || seq == s->rcv_nxt
                       : rwin && seq_lt(seq, s->rcv_nxt + rwin) && seq_lt(s->rcv_nxt, seq + dlen + 1);
    if (!ok && !(fl & RST)) { if (s->state != T_SYN_RCVD) tcp_seg(s, s->snd_nxt, 0, 0, 0); return; }
    if (fl & RST) {
        if (seq == s->rcv_nxt || (ok && dlen == 0)) tcp_drop(s, s->state == T_SYN_RCVD ? ECONNREFUSED : ECONNRESET);
        return;
    }
    if (fl & SYN) { tcp_raw(my_ip, src, dport, sport, s->snd_nxt, 0, RST, 0, 0, 0, 0, 0); tcp_drop(s, ECONNRESET); return; }
    if (!(fl & ACK)) return;

    /* ACK processing */
    if (s->state == T_SYN_RCVD) {
        if (seq_lt(ack, s->snd_una) || seq_lt(s->snd_nxt, ack)) return;
        s->state = T_ESTAB;
        s->snd_una = ack;
        s->snd_wnd = win << s->snd_wscale;
        s->rtx_at = 0;
        if (s->parent) accept_child(s->parent, s);
    }
    if (seq_lt(s->snd_una, ack) && seq_le(ack, s->snd_nxt)) {
        uint32_t acked = ack - s->snd_una, data_acked = acked;
        if (s->fin_sent && ack == s->snd_nxt) data_acked--;   /* our FIN counts one */
        if (data_acked > s->snd.len) data_acked = s->snd.len;
        ring_drop(&s->snd, data_acked);
        s->snd_una = ack;
        s->dupacks = 0;
        if (s->timed_at && seq_le(s->timed_seq, ack)) {      /* a round trip measured (RFC 6298) */
            int64_t r = now() - s->timed_at;
            if (!s->srtt) { s->srtt = r; s->rttvar = r / 2; }
            else {
                int64_t d = s->srtt > r ? s->srtt - r : r - s->srtt;
                s->rttvar = (3 * s->rttvar + d) / 4;
                s->srtt = (7 * s->srtt + r) / 8;
            }
            s->rto = s->srtt + 4 * s->rttvar;
            if (s->rto < 200 * MS) s->rto = 200 * MS;
            if (s->rto > 60 * SEC) s->rto = 60 * SEC;
            s->timed_at = 0;
        }
        if (s->cwnd < s->ssthresh) s->cwnd += acked < s->mss ? acked : s->mss;   /* slow start */
        else s->cwnd += s->mss * s->mss / s->cwnd + 1;                           /* avoidance */
        s->retries = 0;
        s->rtx_at = s->snd_una == s->snd_nxt ? 0 : now() + s->rto;
        if (s->wr.token && s->snd.len < s->snd.size) wake(&s->wr, 0);         /* room: send again */
        if (s->fin_sent && ack == s->snd_nxt) {                                /* our FIN acked */
            if (s->state == T_FIN_WAIT1) s->state = T_FIN_WAIT2;
            else if (s->state == T_CLOSING) tcp_time_wait(s);
            else if (s->state == T_LAST_ACK) { tcp_drop(s, 0); return; }
        }
    } else if (ack == s->snd_una && dlen == 0 && !(fl & FIN) && s->snd_nxt != s->snd_una &&
               (win << s->snd_wscale) == s->snd_wnd) {
        if (++s->dupacks == 3) {                 /* fast retransmit (RFC 5681) */
            uint32_t flight = s->snd_nxt - s->snd_una;
            s->ssthresh = flight / 2 > 2 * s->mss ? flight / 2 : 2 * s->mss;
            s->cwnd = s->ssthresh;
            uint32_t n = s->snd.len < s->mss ? s->snd.len : s->mss;
            static uint8_t b[MSS_OURS];
            if (n) { ring_get(&s->snd, 0, b, n); tcp_seg(s, s->snd_una, PSH, b, n); }
            s->timed_at = 0;
        }
    }
    s->snd_wnd = win << s->snd_wscale;

    /* Data */
    if (dlen && (s->state == T_ESTAB || s->state == T_FIN_WAIT1 || s->state == T_FIN_WAIT2)) {
        if (seq_lt(seq, s->rcv_nxt)) {           /* overlaps what we have: keep the new part */
            uint32_t skip = s->rcv_nxt - seq;
            if (skip >= dlen) { dlen = 0; } else { data += skip; dlen -= skip; seq = s->rcv_nxt; }
            fl &= dlen ? fl : ~FIN;
        }
        uint32_t room = s->rcv.size - s->rcv.len;
        if (dlen > room) { dlen = room; fl &= ~FIN; }
        if (dlen && seq == s->rcv_nxt) {
            ring_put(&s->rcv, data, dlen);
            s->rcv_nxt += dlen;
            for (int again = 1; again;) {        /* segments that now fit */
                again = 0;
                for (int i = 0; i < MAXOOO; i++) {
                    ooo_t *o = &s->ooo[i];
                    if (!o->data) continue;
                    if (seq_le(o->seq + o->len, s->rcv_nxt)) { free(o->data); o->data = 0; continue; }
                    if (seq_le(o->seq, s->rcv_nxt)) {
                        uint32_t sk = s->rcv_nxt - o->seq, m = o->len - sk;
                        if (m > s->rcv.size - s->rcv.len) m = s->rcv.size - s->rcv.len;
                        ring_put(&s->rcv, o->data + sk, m);
                        s->rcv_nxt += m;
                        free(o->data); o->data = 0;
                        again = 1;
                    }
                }
            }
            s->need_ack = 1;
        } else if (dlen) {                       /* a gap before it: keep it, ACK at once */
            ooo_store(s, seq, data, dlen);
            tcp_seg(s, s->snd_nxt, 0, 0, 0);
            fl &= ~FIN;
        }
    }
    if (fl & FIN && seq + dlen == s->rcv_nxt) {
        s->rcv_nxt++;
        s->eof = 1;
        s->need_ack = 1;
        if (s->state == T_ESTAB || s->state == T_SYN_RCVD) s->state = T_CLOSE_WAIT;
        else if (s->state == T_FIN_WAIT1) s->state = s->snd_una == s->snd_nxt ? T_TIME_WAIT : T_CLOSING;
        else if (s->state == T_FIN_WAIT2) tcp_time_wait(s);
        if (s->state == T_TIME_WAIT) tcp_time_wait(s);
    }
    deliver_data(s);
    tcp_output(s);
}

/* ---- UDP input, DHCP and DNS (both built in). */
static uint16_t dns_port;
static void dhcp_input(const uint8_t *p, size_t n);
static void dns_input(const uint8_t *p, size_t n);

static void udp_input(uint32_t src, const uint8_t *seg, size_t n)
{
    if (n < 8 || g16(seg + 4) > n || g16(seg + 4) < 8) return;
    n = g16(seg + 4);
    if (g16(seg + 6) && l4_csum(src, my_ip, 17, seg, n) != 0) return;   /* (0: no checksum) */
    uint16_t sport = g16(seg), dport = g16(seg + 2);
    if (dport == 68 && sport == 67) { dhcp_input(seg + 8, n - 8); return; }
    if (dport == dns_port && sport == 53) { dns_input(seg + 8, n - 8); return; }
    for (int i = 0; i < MAXSOCK; i++) {
        sock_t *s = socks[i];
        if (!s || s->type != NET_UDP || s->lport != dport) continue;
        if (s->rd.token) {                       /* someone waits: give it straight away */
            size_t m = n - 8 < s->rd.max ? n - 8 : s->rd.max;
            answer(s->rd.token, m, src, sport, seg + 8, m);
            s->rd.token = 0;
        } else if (s->ndq < 64) {
            struct dgram *d = malloc(sizeof *d + n - 8);
            if (!d) return;
            d->next = 0; d->ip = src; d->port = sport; d->len = (uint16_t)(n - 8);
            memcpy(d->data, seg + 8, n - 8);
            if (s->dqt) s->dqt->next = d; else s->dq = d;
            s->dqt = d; s->ndq++;
        }
        return;
    }
}

/* DHCP (RFC 2131): discover, offer, request, acknowledge. Renewed at half the lease. */
enum { D_INIT, D_SELECTING, D_REQUESTING, D_BOUND };
static int dhcp_state;
static uint32_t dhcp_xid, dhcp_server, dhcp_offer;
static int64_t dhcp_at, dhcp_renew;

static void dhcp_send(int type)
{
    uint8_t b[300];
    memset(b, 0, sizeof b);
    b[0] = 1; b[1] = 1; b[2] = 6;
    p32(b + 4, dhcp_xid);
    p16(b + 10, dhcp_state == D_BOUND ? 0 : 0x8000);   /* broadcast replies until configured */
    if (dhcp_state == D_BOUND) p32(b + 12, my_ip);
    memcpy(b + 28, mac, 6);
    p32(b + 236, 0x63825363);                    /* the "magic cookie" */
    uint8_t *o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)type;
    if (type == 3 && dhcp_state != D_BOUND) {
        *o++ = 50; *o++ = 4; p32(o, dhcp_offer); o += 4;
        *o++ = 54; *o++ = 4; p32(o, dhcp_server); o += 4;
    }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;   /* we want: mask, router, DNS */
    *o++ = 255;
    uint32_t saved = my_ip;
    if (dhcp_state != D_BOUND) my_ip = 0;
    udp_send(dhcp_state == D_BOUND ? dhcp_server : 0xffffffff, 68, 67, b, (size_t)(o - b));
    my_ip = saved;
    dhcp_at = now() + 2 * SEC;
}

static void wake_parked(void);

static void dhcp_input(const uint8_t *p, size_t n)
{
    if (n < 240 || p[0] != 2 || g32(p + 4) != dhcp_xid || g32(p + 236) != 0x63825363) return;
    int type = 0;
    uint32_t mask = 0, gw = 0, dns = 0, server = 0, lease = 3600;
    for (size_t i = 240; i + 1 < n && p[i] != 255;) {
        if (p[i] == 0) { i++; continue; }
        size_t l = p[i + 1];
        if (i + 2 + l > n) break;
        const uint8_t *v = p + i + 2;
        if (p[i] == 53 && l >= 1) type = v[0];
        if (p[i] == 1 && l >= 4) mask = g32(v);
        if (p[i] == 3 && l >= 4) gw = g32(v);
        if (p[i] == 6 && l >= 4) dns = g32(v);
        if (p[i] == 54 && l >= 4) server = g32(v);
        if (p[i] == 51 && l >= 4) lease = g32(v);
        i += 2 + l;
    }
    if (type == 2 && dhcp_state == D_SELECTING) {          /* an offer: ask for it */
        dhcp_offer = g32(p + 16);
        dhcp_server = server;
        dhcp_state = D_REQUESTING;
        dhcp_send(3);
    } else if (type == 5 && (dhcp_state == D_REQUESTING || dhcp_state == D_BOUND)) {
        int first = !my_ip;
        my_ip = g32(p + 16); my_mask = mask ? mask : 0xffffff00; my_gw = gw; my_dns = dns ? dns : gw;
        dhcp_state = D_BOUND;
        dhcp_at = 0;
        dhcp_renew = now() + (int64_t)(lease / 2) * SEC;
        if (first) {
            printf("netd: %u.%u.%u.%u/%u, gateway %u.%u.%u.%u, DNS %u.%u.%u.%u\n",
                   my_ip >> 24, my_ip >> 16 & 255, my_ip >> 8 & 255, my_ip & 255, 32 - __builtin_ctz(my_mask),
                   my_gw >> 24, my_gw >> 16 & 255, my_gw >> 8 & 255, my_gw & 255,
                   my_dns >> 24, my_dns >> 16 & 255, my_dns >> 8 & 255, my_dns & 255);
            wake_parked();
        }
    } else if (type == 6) {                                /* refused: start again */
        dhcp_state = D_SELECTING;
        sys_random(&dhcp_xid, 4);
        dhcp_send(1);
    }
}

/* DNS (RFC 1035): A records, a small cache, queries retried every second. */
#define NQ 16
static struct { char name[256]; uint16_t id; long tokens[8]; int ntok; int64_t deadline, at; int tries; } dq[NQ];
static struct { char name[256]; uint32_t ip; int64_t expires; } dcache[32];

static void dns_query(int i)
{
    uint8_t b[300];
    size_t n = 12;
    memset(b, 0, 12);
    p16(b, dq[i].id); p16(b + 2, 0x0100); p16(b + 4, 1);   /* recursion desired, 1 question */
    for (const char *s = dq[i].name; *s;) {
        const char *d = strchr(s, '.');
        size_t l = d ? (size_t)(d - s) : strlen(s);
        if (l == 0 || l > 63 || n + l + 6 > sizeof b) return;
        b[n++] = (uint8_t)l; memcpy(b + n, s, l); n += l;
        s += l + (d ? 1 : 0);
    }
    b[n++] = 0; p16(b + n, 1); p16(b + n + 2, 1); n += 4;
    udp_send(my_dns, dns_port, 53, b, n);
    dq[i].at = now() + SEC;
}

static size_t dns_skip(const uint8_t *p, size_t n, size_t i)   /* past a (compressed) name */
{
    while (i < n) {
        if (p[i] == 0) return i + 1;
        if ((p[i] & 0xc0) == 0xc0) return i + 2;
        i += 1 + p[i];
    }
    return n + 1;
}

static void dns_done(int i, long r, uint32_t ip)
{
    for (int t = 0; t < dq[i].ntok; t++) answer(dq[i].tokens[t], r, ip, 0, 0, 0);
    dq[i].ntok = 0;
    dq[i].name[0] = 0;
}

static void dns_input(const uint8_t *p, size_t n)
{
    if (n < 12) return;
    int i;
    for (i = 0; i < NQ; i++) if (dq[i].name[0] && dq[i].id == g16(p)) break;
    if (i == NQ) return;
    if ((p[3] & 15) == 3) { dns_done(i, -ENOENT, 0); return; }   /* no such name */
    size_t off = dns_skip(p, n, 12) + 4;
    int an = g16(p + 6);
    for (int k = 0; k < an && off + 10 <= n; k++) {
        off = dns_skip(p, n, off);
        if (off + 10 > n) break;
        int type = g16(p + off), rdl = g16(p + off + 8);
        uint32_t ttl = g32(p + off + 4);
        if (type == 1 && rdl == 4 && off + 14 <= n) {           /* an A record (CNAMEs are skipped) */
            uint32_t ip = g32(p + off + 10);
            int c = 0;
            for (int j = 0; j < 32; j++) if (dcache[j].expires < dcache[c].expires) c = j;
            strlcpy(dcache[c].name, dq[i].name, sizeof dcache[c].name);
            dcache[c].ip = ip;
            dcache[c].expires = now() + (int64_t)(ttl < 30 ? 30 : ttl > 3600 ? 3600 : ttl) * SEC;
            dns_done(i, 0, ip);
            return;
        }
        off += 10 + rdl;
    }
    dns_done(i, -ENOENT, 0);
}

static int parse_ip(const char *s, uint32_t *ip)    /* "a.b.c.d" */
{
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        long x = 0; int d = 0;
        while (*s >= '0' && *s <= '9') { x = x * 10 + *s++ - '0'; d++; }
        if (!d || x > 255 || (k < 3 && *s++ != '.')) return -1;
        v = v << 8 | (uint32_t)x;
    }
    if (*s) return -1;
    *ip = v;
    return 0;
}

static void resolve(long token, const char *name, uint64_t ms)
{
    uint32_t ip;
    if (!parse_ip(name, &ip)) { answer(token, 0, ip, 0, 0, 0); return; }
    if (!strcmp(name, "localhost")) { answer(token, 0, 0x7f000001, 0, 0, 0); return; }
    for (int j = 0; j < 32; j++)
        if (dcache[j].expires > now() && !strcmp(dcache[j].name, name)) { answer(token, 0, dcache[j].ip, 0, 0, 0); return; }
    int i, f = -1;
    for (i = 0; i < NQ; i++) {
        if (dq[i].name[0] && !strcmp(dq[i].name, name)) break;
        if (!dq[i].name[0] && f < 0) f = i;
    }
    if (i == NQ) {
        if (f < 0) { answer(token, -EBUSY, 0, 0, 0, 0); return; }
        i = f;
        strlcpy(dq[i].name, name, sizeof dq[i].name);
        sys_random(&dq[i].id, 2);
        dq[i].tries = 0; dq[i].ntok = 0;
        dq[i].deadline = deadline_ms(ms, 10 * SEC);
        if (my_ip && my_dns) { dns_query(i); dq[i].tries = 1; } else dq[i].at = now() + 100 * MS;
    }
    if (dq[i].ntok < 8) dq[i].tokens[dq[i].ntok++] = token;
    else answer(token, -EBUSY, 0, 0, 0, 0);
}

/* ---- ICMP: answer pings; send ours. */
static struct { long token; uint32_t ip; uint16_t seq; int64_t sent, deadline; } pings[8];
static uint16_t ping_seq;

static void icmp_input(uint32_t src, const uint8_t *p, size_t n)
{
    if (n < 8 || csum_fold(csum_add(0, p, n)) != 0) return;
    if (p[0] == 8) {                             /* echo request: answer it */
        static uint8_t r[1480];
        if (n > sizeof r) return;
        memcpy(r, p, n);
        r[0] = 0; p16(r + 2, 0);
        p16(r + 2, csum_fold(csum_add(0, r, n)));
        ip_send(src, 1, r, n);
    } else if (p[0] == 0 && g16(p + 4) == 0x5349) {   /* echo reply to one of ours ("SI") */
        for (int i = 0; i < 8; i++)
            if (pings[i].token && pings[i].ip == src && pings[i].seq == g16(p + 6)) {
                answer(pings[i].token, (now() - pings[i].sent) / 1000, 0, 0, 0, 0);
                pings[i].token = 0;
            }
    }
}

static void ping_send(int i)
{
    uint8_t p[40];
    memset(p, 0, sizeof p);
    p[0] = 8; p16(p + 4, 0x5349); p16(p + 6, pings[i].seq);
    memcpy(p + 8, "SIEOS ping", 10);
    p16(p + 2, csum_fold(csum_add(0, p, sizeof p)));
    pings[i].sent = now();
    ip_send(pings[i].ip, 1, p, sizeof p);
}

/* ---- Frames received. */
static void frame_input(const uint8_t *f, size_t n)
{
    stats.rx_packets++; stats.rx_bytes += n;
    if (n < 14) return;
    uint16_t type = g16(f + 12);
    const uint8_t *p = f + 14;
    n -= 14;
    if (type == 0x0806 && n >= 28 && g16(p) == 1 && g16(p + 2) == 0x0800) {
        uint32_t sip = g32(p + 14), tip = g32(p + 24);
        if (sip) arp_learn(sip, p + 8);
        if (g16(p + 6) == 1 && my_ip && tip == my_ip) arp_send(2, p + 8, sip);   /* asked for us */
        return;
    }
    if (type != 0x0800 || n < 20 || (p[0] >> 4) != 4) return;
    size_t hl = (p[0] & 15) * 4, tl = g16(p + 2);
    if (hl < 20 || tl < hl || tl > n || csum_fold(csum_add(0, p, hl)) != 0) return;
    if (g16(p + 6) & 0x3fff) return;             /* a fragment: not supported */
    uint32_t src = g32(p + 12), dst = g32(p + 16);
    int forus = dst == my_ip || dst == 0xffffffff || (my_mask && dst == (my_ip | ~my_mask)) || (!my_ip && p[9] == 17);
    if (!forus) return;
    const uint8_t *l4 = p + hl;
    size_t ln = tl - hl;
    switch (p[9]) {
    case 1:  icmp_input(src, l4, ln); break;
    case 6:  if (my_ip) tcp_input(src, l4, ln); break;
    case 17: udp_input(src, l4, ln); break;
    }
}

/* ---- Connections waiting for the network to be configured. */
static void tcp_connect_now(sock_t *s)
{
    s->state = T_SYN_SENT;
    s->timed_at = now();
    tcp_seg(s, s->snd_nxt, SYN, 0, 0);
    s->snd_nxt++;
    arm_rtx(s);
}

static void wake_parked(void)
{
    for (int i = 0; i < MAXSOCK; i++) if (socks[i] && socks[i]->state == T_WAIT_NET) tcp_connect_now(socks[i]);
    for (int i = 0; i < NQ; i++) if (dq[i].name[0] && !dq[i].tries) { dns_query(i); dq[i].tries = 1; }
    for (int i = 0; i < 8; i++) if (pings[i].token && !pings[i].sent) ping_send(i);
}

/* ---- Started on demand, netd has no address for its first moments (the
 * DHCP exchange). Connections, names and pings already wait for it; a
 * question about the configuration (NET_INFO) waits too, up to 3 s, so
 * the program that started the network sees it configured. */
static struct { long from; msg_t m; } held[8];
static int nheld;
static int64_t hold_until;

/* The cable (NET_LINK, from drivers that see it: the Raspberry Pis' cards;
 * virtio has no cable). Up: ask DHCP again, the network may be another one,
 * and hold questions about the configuration as at start. Down: nothing to
 * send meanwhile; the address stays until the next answer. */
static void link_change(int up)
{
    printf("netd: link %s\n", up ? "up" : "down");
    if (!up || !nic_up) return;
    dhcp_state = D_SELECTING;
    sys_random(&dhcp_xid, 4);
    hold_until = now() + 3 * SEC;
    dhcp_send(1);
}

/* ---- Timers: retransmissions, timeouts, DHCP, DNS, ARP. Returns the
 * next time something is due (0: nothing). */
static int64_t last_owner_check;

static int64_t timers(void)
{
    int64_t t = now(), next = 0;
#define DUE(x) do { int64_t _x = (x); if (_x && (!next || _x < next)) next = _x; } while (0)
    if (dhcp_at && t >= dhcp_at) dhcp_send(dhcp_state == D_REQUESTING ? 3 : 1);
    if (dhcp_state == D_BOUND && dhcp_renew && t >= dhcp_renew) { dhcp_renew = t + 60 * SEC; dhcp_send(3); }
    DUE(dhcp_at); DUE(dhcp_renew);
    if (nheld) DUE(hold_until);
    for (int i = 0; i < 16; i++)
        if (pend[i].pkt) {
            if (t - pend[i].sent > SEC) {
                if (++pend[i].tries > 3) { free(pend[i].pkt); pend[i].pkt = 0; continue; }
                pend[i].sent = t; arp_send(1, 0, pend[i].ip);
            }
            DUE(pend[i].sent + SEC);
        }
    for (int i = 0; i < NQ; i++)
        if (dq[i].name[0]) {
            if (t >= dq[i].deadline) { dns_done(i, -ETIMEDOUT, 0); continue; }
            if (t >= dq[i].at && my_ip && my_dns) { dns_query(i); dq[i].tries++; }
            DUE(dq[i].at); DUE(dq[i].deadline);
        }
    for (int i = 0; i < 8; i++)
        if (pings[i].token) {
            if (t >= pings[i].deadline) { answer(pings[i].token, -ETIMEDOUT, 0, 0, 0, 0); pings[i].token = 0; continue; }
            DUE(pings[i].deadline);
        }
    int owned = 0;
    for (int i = 0; i < MAXSOCK; i++) {
        sock_t *s = socks[i];
        if (!s) continue;
        owned |= s->owner != 0;
        if (s->rd.token && s->rd.deadline && t >= s->rd.deadline) { wake(&s->rd, -ETIMEDOUT); }
        if (s->acc.token && s->acc.deadline && t >= s->acc.deadline) { wake(&s->acc, -ETIMEDOUT); }
        if (s->conn.token && s->conn.deadline && t >= s->conn.deadline) {
            wake(&s->conn, -ETIMEDOUT);
            s->state = T_CLOSED;
            continue;
        }
        DUE(s->rd.token ? s->rd.deadline : 0); DUE(s->acc.token ? s->acc.deadline : 0);
        DUE(s->conn.token ? s->conn.deadline : 0);
        if (s->state == T_TIME_WAIT) {
            if (t >= s->tw_at) { s->state = T_CLOSED; if (!s->owner) { sock_free(s); continue; } }
            else DUE(s->tw_at);
        }
        if (s->rtx_at && t >= s->rtx_at) {       /* retransmission timeout */
            if (++s->retries > 12) { tcp_drop(s, ETIMEDOUT); continue; }
            s->rto = s->rto * 2 > 60 * SEC ? 60 * SEC : s->rto * 2;
            s->timed_at = 0;
            uint32_t flight = s->snd_nxt - s->snd_una;
            s->ssthresh = flight / 2 > 2 * s->mss ? flight / 2 : 2 * s->mss;
            s->cwnd = s->mss;
            if (s->state == T_SYN_SENT) tcp_seg(s, s->iss, SYN, 0, 0);
            else if (s->state == T_SYN_RCVD) tcp_seg(s, s->iss, SYN, 0, 0);
            else {                               /* go back to the first unacknowledged byte */
                int fin = s->fin_sent && s->snd.len == 0;
                s->snd_nxt = s->snd_una;
                if (s->fin_sent && !fin) s->fin_sent = 0;
                if (fin) { tcp_seg(s, s->snd_una, FIN, 0, 0); s->snd_nxt++; }
                else if (s->snd_wnd == 0 && s->snd.len) {   /* zero window: probe one byte */
                    uint8_t b; ring_get(&s->snd, 0, &b, 1);
                    tcp_seg(s, s->snd_una, 0, &b, 1);
                } else tcp_output(s);
            }
            s->rtx_at = t + s->rto;
        }
        DUE(s->rtx_at);
    }
    if (owned) {                                 /* every 2 s: are the owners still alive? */
        if (t - last_owner_check >= 2 * SEC) {
            last_owner_check = t;
            for (int i = 0; i < MAXSOCK; i++) {
                sock_t *s = socks[i];
                mk_ident_t id;
                if (!s || !s->owner || !sys_ident(s->owner, &id)) continue;
                s->owner = 0;                    /* gone: close it, abruptly */
                if (s->type == NET_TCP && s->state != T_CLOSED && s->state != T_LISTEN && s->state != T_TIME_WAIT &&
                    s->state != T_WAIT_NET && s->state != T_SYN_SENT)
                    tcp_raw(my_ip, s->rip, s->lport, s->rport, s->snd_nxt, s->rcv_nxt, RST | ACK, 0, 0, 0, 0, 0);
                if (s->type == NET_TCP && s->state == T_LISTEN)
                    for (int j = 0; j < MAXSOCK; j++) if (socks[j] && socks[j]->parent == s && !socks[j]->owner) sock_free(socks[j]);
                if (socks[i]) sock_free(s);
            }
        }
        DUE(last_owner_check + 2 * SEC);
    }
    return next;
}

/* ---- Client requests: the result to reply, unless `deferred` is set. */
static int deferred;                           /* the request answers (or keeps) the reply itself */

static long request(long from, msg_t *m, const uint8_t *data, size_t dlen)
{
    sock_t *s = 0;
    uint64_t op = m->w[0], timeout = m->w[3] >> 16;
    if (op != NET_SOCKET && op != NET_RESOLVE && op != NET_PING && op != NET_INFO && op != NET_TICK && op != NET_FRAMES) {
        s = by_handle((long)m->w[1], m->pid);
        if (!s) return -EBADF;
    }
    switch (op) {
    case NET_SOCKET: {
        if (m->w[1] != NET_TCP && m->w[1] != NET_UDP) return -EINVAL;
        sock_t *n = sock_new((int)m->w[1], m->pid, m->uid);
        return n ? n->handle : -EMFILE;
    }
    case NET_CONNECT: {
        if (s->type != NET_TCP || s->state != T_CLOSED || s->rcv.b) return -EINVAL;
        if (!nic_up) return -ENETUNREACH;
        s->rip = (uint32_t)m->w[2]; s->rport = (uint16_t)m->w[3];
        if (!s->rport || !s->rip) return -EINVAL;
        s->lport = ephemeral(NET_TCP);
        tcp_new_conn(s);
        if (!s->snd.b || !s->rcv.b) return -ENOMEM;
        s->conn = (wait_t){ from, deadline_ms(timeout, 30 * SEC), 0 };
        if (my_ip) tcp_connect_now(s); else s->state = T_WAIT_NET;
        { deferred = 1; return 0; }
    }
    case NET_LISTEN:
        if (s->type != NET_TCP || s->state != T_CLOSED || !m->w[2] || m->w[2] > 65535) return -EINVAL;
        if (m->w[2] < 1024 && m->uid != 0) return -EACCES;
        if (port_used(NET_TCP, (uint16_t)m->w[2])) return -EADDRINUSE;
        s->lport = (uint16_t)m->w[2];
        s->backlog = m->w[3] && m->w[3] < 16 ? (int)m->w[3] : 8;
        s->state = T_LISTEN;
        return 0;
    case NET_ACCEPT:
        if (s->state != T_LISTEN || s->acc.token) return -EINVAL;
        if (s->naccq) {
            sock_t *c = s->accq[0];
            memmove(s->accq, s->accq + 1, --s->naccq * sizeof s->accq[0]);
            c->owner = s->owner; c->parent = 0;
            answer(from, c->handle, c->rip, c->rport, 0, 0);
            { deferred = 1; return 0; }
        }
        s->acc = (wait_t){ from, timeout ? deadline_ms(timeout, 0) : 0, 0 };
        { deferred = 1; return 0; }
    case NET_SEND: {
        if (s->type != NET_TCP) return -EINVAL;
        if (s->state == T_SYN_SENT || s->state == T_WAIT_NET) return -ENOTCONN;
        if (s->closing || (s->state != T_ESTAB && s->state != T_CLOSE_WAIT)) return s->err ? -s->err : -EPIPE;
        uint32_t room = s->snd.size - s->snd.len, n = dlen < room ? (uint32_t)dlen : room;
        if (!n) { if (s->wr.token) return -EBUSY; s->wr = (wait_t){ from, 0, 0 }; { deferred = 1; return 0; } }   /* replies 0: try again */
        ring_put(&s->snd, data, n);
        tcp_output(s);
        return n;
    }
    case NET_RECV: {
        size_t max = m->w[2] < NET_MAX ? m->w[2] : NET_MAX;
        if (s->type == NET_UDP) goto recvfrom;
        if (s->rd.token) return -EBUSY;
        if (!s->rcv.b) return -ENOTCONN;
        if (!s->rcv.len && !s->eof && s->state == T_CLOSED) return s->err ? -s->err : 0;
        s->rd = (wait_t){ from, timeout ? deadline_ms(timeout, 0) : 0, max ? max : NET_MAX };
        deliver_data(s);
        { deferred = 1; return 0; }
    }
    case NET_BIND:
        if (s->type != NET_UDP || s->lport) return -EINVAL;
        if (m->w[2] && m->w[2] < 1024 && m->uid != 0) return -EACCES;
        if (m->w[2] && port_used(NET_UDP, (uint16_t)m->w[2])) return -EADDRINUSE;
        s->lport = m->w[2] ? (uint16_t)m->w[2] : ephemeral(NET_UDP);
        return 0;
    case NET_SENDTO:
        if (s->type != NET_UDP || dlen > 1472) return -EINVAL;
        if (!my_ip) return -ENETUNREACH;
        if (!s->lport) s->lport = ephemeral(NET_UDP);
        udp_send((uint32_t)m->w[2], s->lport, (uint16_t)m->w[3], data, dlen);
        return (long)dlen;
    case NET_RECVFROM:
    recvfrom: {
        if (s->type != NET_UDP || s->rd.token) return -EINVAL;
        size_t max = m->w[2] && m->w[2] < NET_MAX ? m->w[2] : NET_MAX;
        if (s->dq) {
            struct dgram *d = s->dq;
            s->dq = d->next; if (!s->dq) s->dqt = 0; s->ndq--;
            size_t c = d->len < max ? d->len : max;
            answer(from, c, d->ip, d->port, d->data, c);
            free(d);
            { deferred = 1; return 0; }
        }
        s->rd = (wait_t){ from, timeout ? deadline_ms(timeout, 0) : 0, max };
        { deferred = 1; return 0; }
    }
    case NET_CLOSE:
        wake(&s->rd, -EBADF); wake(&s->wr, -EBADF); wake(&s->acc, -EBADF); wake(&s->conn, -EBADF);
        s->owner = 0;
        if (s->type == NET_UDP || s->state == T_CLOSED || s->state == T_LISTEN || s->state == T_WAIT_NET ||
            s->state == T_SYN_SENT || s->state == T_TIME_WAIT) {
            if (s->state == T_LISTEN)
                for (int j = 0; j < MAXSOCK; j++) if (socks[j] && socks[j]->parent == s && !socks[j]->owner) {
                    tcp_raw(my_ip, socks[j]->rip, socks[j]->lport, socks[j]->rport, socks[j]->snd_nxt, socks[j]->rcv_nxt, RST | ACK, 0, 0, 0, 0, 0);
                    sock_free(socks[j]);
                }
            sock_free(s);
            return 0;
        }
        if (s->rcv.len) {                        /* unread data: abort, as other systems do */
            tcp_raw(my_ip, s->rip, s->lport, s->rport, s->snd_nxt, s->rcv_nxt, RST | ACK, 0, 0, 0, 0, 0);
            sock_free(s);
            return 0;
        }
        s->closing = 1;
        if (s->state == T_ESTAB || s->state == T_SYN_RCVD) s->state = T_FIN_WAIT1;
        else if (s->state == T_CLOSE_WAIT) s->state = T_LAST_ACK;
        tcp_output(s);
        return 0;
    case NET_RESOLVE: {
        char name[256];
        if (!dlen || dlen >= sizeof name) return -EINVAL;
        memcpy(name, data, dlen); name[dlen] = 0;
        if (name[dlen - 1] == 0) dlen--;
        if (!nic_up) return -ENETUNREACH;
        resolve(from, name, timeout);
        { deferred = 1; return 0; }
    }
    case NET_PING:
        for (int i = 0; i < 8; i++)
            if (!pings[i].token) {
                if (!nic_up) return -ENETUNREACH;
                pings[i] = (typeof(pings[0])){ from, (uint32_t)m->w[1], ++ping_seq, 0, deadline_ms(timeout, 5 * SEC) };
                if (my_ip) ping_send(i);
                { deferred = 1; return 0; }
            }
        return -EBUSY;
    case NET_INFO:
        stats.ip = my_ip; stats.mask = my_mask; stats.gw = my_gw; stats.dns = my_dns;
        memcpy(stats.mac, mac, 6); stats.mtu = 1500; stats.up = nic_up;
        stats.time = rtc_base ? rtc_base + (now() - rtc_ns) / SEC : 0;
        memcpy(outbuf, &stats, sizeof stats);
        answer(from, 0, 0, 0, outbuf, sizeof stats);
        { deferred = 1; return 0; }
    }
    return -ENOSYS;
}

/* ---- Started on demand: busy while a client has something open here (a
 * socket, a name being resolved, a ping); unused otherwise. The DHCP lease
 * is not kept: a new start asks for one again (a few ms on most networks). */
static int busy(void)
{
    for (int i = 0; i < MAXSOCK; i++) if (socks[i]) return 1;
    for (int i = 0; i < NQ; i++) if (dq[i].ntok) return 1;
    for (int i = 0; i < 8; i++) if (pings[i].token) return 1;
    return 0;
}

/* ---- The timer thread: asks how long to sleep, sleeps, asks again. */
static long tick_token;
static void ticker(void *arg)
{
    (void)arg;
    for (;;) {
        msg_t m = { .w = { NET_TICK } };
        ipc_call(port, &m);
        if ((long)m.w[0] > 0) sys_sleep(m.w[0]);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    port = port_create("net");
    sys_random(&dns_port, 2);
    dns_port = 32768 + dns_port % 16384;
    /* The driver: its address and the date. If there is no card, netd still
     * answers (ENETUNREACH), so programs never wait forever. */
    nic_info_t ni;
    msg_t q = { .w = { NIC_INFO }, .rbuf = &ni, .rlen = sizeof ni };
    long np = port_lookup("nic0");
    if (np > 0 && ipc_call(np, &q) >= 0 && (long)q.w[0] == 0 && q.rlen == sizeof ni) {
        nic = np;
        memcpy(mac, ni.mac, 6);
        rtc_base = ni.rtc; rtc_ns = ni.rtc_ns;
        nic_up = 1;
        sys_random(&dhcp_xid, 4);
        dhcp_state = D_SELECTING;
        hold_until = now() + 3 * SEC;
        dhcp_send(1);
        tx_flush();
    }
    thread_start(ticker, 0, 8192);
    static uint8_t in[NET_MAX];
    uint64_t asked = 0, seen = 0;                /* client requests; at init's last question */
    int self = 0;
    { mk_ident_t id; if (!sys_ident(0, &id)) self = id.pid; }
    for (;;) {
        msg_t m = { .rbuf = in, .rlen = NET_MAX };
        long from = ipc_recv(port, &m), r;
        if (from <= 0) continue;
        if (m.w[0] == NET_FRAMES) {
            if (m.uid != 0) r = -EPERM;
            else {
                for (size_t off = 0; off + 2 <= m.rlen;) {
                    size_t fl = in[off] | in[off + 1] << 8;
                    if (off + 2 + fl > m.rlen) break;
                    frame_input(in + off + 2, fl);
                    off += 2 + fl;
                }
                for (int i = 0; i < MAXSOCK; i++) if (socks[i] && socks[i]->need_ack) tcp_seg(socks[i], socks[i]->snd_nxt, 0, 0, 0);
                r = 0;
            }
            reply_val(from, r);
        } else if (m.w[0] == NET_LINK) {
            if (m.uid != 0) r = -EPERM;          /* only the (root) driver says so */
            else { link_change((int)m.w[1]); r = 0; }
            reply_val(from, r);
        } else if (m.w[0] == NET_TICK) {
            if (m.pid != self) reply_val(from, -EPERM);
            else tick_token = from;              /* answered below, when something is due */
        } else if (m.w[0] == SVC_MAYSTOP) {      /* init: unused for a while? then we end */
            r = maystop_answer(&m, asked, &seen, busy());
            reply_val(from, r);
            if (!r) return 0;
        } else if (m.w[0] == NET_INFO && nic_up && dhcp_state != D_BOUND && now() < hold_until && nheld < 8) {
            asked++;
            held[nheld].from = from;             /* answered once the address comes (below) */
            held[nheld++].m = m;
        } else {
            asked++;
            r = request(from, &m, in, m.rlen);
            if (!deferred) reply_val(from, r);
            deferred = 0;
        }
        if (nheld && (dhcp_state == D_BOUND || now() >= hold_until)) {
            for (int i = 0; i < nheld; i++) {
                r = request(held[i].from, &held[i].m, 0, 0);
                if (!deferred) reply_val(held[i].from, r);
                deferred = 0;
            }
            nheld = 0;
        }
        int64_t next = timers();
        if (tick_token && next) {
            int64_t d = next - now();
            msg_t t = { .w = { (uint64_t)(d > 0 ? d : 1) } };
            ipc_reply(tick_token, &t);
            tick_token = 0;
        }
        tx_flush();
    }
}
