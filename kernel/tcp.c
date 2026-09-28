/*
 * tcp.c - Transmission Control Protocol (RFC 793, simplified).
 *
 * Supported: active and passive open with MSS option, in-order data
 * transfer with flow control (receiver window), cumulative ACKs,
 * go-back-N retransmission with exponential backoff, zero-window probes,
 * orderly release (FIN) in both directions, TIME_WAIT, and RST handling.
 * Not implemented: out-of-order reassembly, congestion control, SACK,
 * window scaling, urgent data, keepalives.
 */
#include "net.h"
#include "proc.h"
#include "mm.h"
#include "poll.h"

#define NTCB     64
#define TCP_BUF  32768
#define TCP_MSS  1460
#define RTO_MIN  300           /* ms */
#define RTO_MAX  8000
#define MAX_RETRIES 8
#define SYN_RETRIES 5
#define TIME_WAIT_MS 2000
#define FIN_WAIT2_ORPHAN_MS 60000
#define BACKLOG  16

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

struct tcb {
    bool used;
    int state;
    uint32_t lip, rip;
    uint16_t lport, rport;
    uint32_t iss, snd_una, snd_nxt, snd_wnd;
    uint16_t rmss;
    uint32_t irs, rcv_nxt;
    uint8_t *sbuf;
    uint32_t slen;              /* bytes in sbuf; sbuf[0] has sequence snd_una */
    uint8_t *rbuf;
    uint32_t rhead, rlen;
    uint32_t last_wnd;
    bool fin_queued, fin_sent, fin_acked, fin_rcvd;
    uint64_t rto_at;            /* tick of the retransmission deadline, 0 = idle */
    uint32_t rto;
    int retries;
    uint64_t deadline;          /* TIME_WAIT / orphan FIN_WAIT_2 expiry */
    int err;
    struct socket *owner;
    struct tcb *listener;       /* parent while not yet accepted */
    bool accept_ready;
};

static struct tcb tcbs[NTCB];
static uint32_t isn_counter;

static inline bool seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline bool seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

static uint64_t ms_to_ticks(uint32_t ms)
{
    uint64_t t = (uint64_t)ms * TIMER_HZ / 1000;
    return t ? t : 1;
}

static uint32_t new_isn(void)
{
    isn_counter += 64000;
    return (uint32_t)(ticks * 250000) + isn_counter + (uint32_t)(uintptr_t)&isn_counter;
}

static void wake(struct tcb *t)
{
    if (t->owner)
        socket_wake(t->owner);
    if (t->listener && t->listener->owner)
        socket_wake(t->listener->owner);
    poll_wakeup();
}

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

struct tcb *tcp_alloc(void)
{
    for (int i = 0; i < NTCB; i++) {
        struct tcb *t = &tcbs[i];
        if (t->used)
            continue;
        memset(t, 0, sizeof(*t));
        t->sbuf = kmalloc(TCP_BUF);
        t->rbuf = kmalloc(TCP_BUF);
        if (!t->sbuf || !t->rbuf) {
            kfree(t->sbuf);
            kfree(t->rbuf);
            return NULL;
        }
        t->used = true;
        t->state = TCP_CLOSED;
        t->rto = 1000;
        t->rmss = 536;
        return t;
    }
    return NULL;
}

static void tcb_free(struct tcb *t)
{
    kfree(t->sbuf);
    kfree(t->rbuf);
    t->sbuf = t->rbuf = NULL;
    t->used = false;
}

/* Free a closed connection nobody refers to any more. */
static void maybe_free(struct tcb *t)
{
    if (t->used && !t->owner && (t->state == TCP_CLOSED) && !t->accept_ready)
        tcb_free(t);
}

void tcp_set_owner(struct tcb *t, struct socket *s)
{
    t->owner = s;
    if (s) {
        t->listener = NULL;
        t->accept_ready = false;
    }
}

int tcp_state(struct tcb *t) { return t->state; }
int tcp_error(struct tcb *t) { return t->err; }

void tcp_endpoints(struct tcb *t, uint32_t *lip, uint16_t *lport, uint32_t *rip, uint16_t *rport)
{
    *lip = t->lip;
    *lport = t->lport;
    *rip = t->rip;
    *rport = t->rport;
}

void tcp_queues(struct tcb *t, uint32_t *rx, uint32_t *tx)
{
    *rx = t->rlen;
    *tx = t->slen;
}

bool tcp_port_in_use(uint16_t port)
{
    for (int i = 0; i < NTCB; i++)
        if (tcbs[i].used && tcbs[i].lport == port && tcbs[i].state != TCP_CLOSED)
            return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* Segment output                                                      */
/* ------------------------------------------------------------------ */

static uint16_t rcv_window(struct tcb *t)
{
    uint32_t w = TCP_BUF - t->rlen;
    return w > 65535 ? 65535 : w;
}

static void send_raw(uint32_t lip, uint32_t rip, uint16_t lport, uint16_t rport, uint32_t seq,
                     uint32_t ack, uint8_t flags, uint16_t wnd, const void *data, size_t len, bool mss_opt)
{
    uint8_t seg[ETH_MTU];
    size_t hlen = mss_opt ? 24 : 20;
    if (hlen + len > ETH_MTU - IP_HLEN)
        return;
    seg[0] = lport >> 8;
    seg[1] = lport;
    seg[2] = rport >> 8;
    seg[3] = rport;
    uint32_t nseq = htonl(seq), nack = htonl(ack);
    memcpy(seg + 4, &nseq, 4);
    memcpy(seg + 8, &nack, 4);
    seg[12] = (hlen / 4) << 4;
    seg[13] = flags;
    seg[14] = wnd >> 8;
    seg[15] = wnd;
    seg[16] = seg[17] = 0;                /* checksum */
    seg[18] = seg[19] = 0;                /* urgent pointer */
    if (mss_opt) {
        seg[20] = 2;
        seg[21] = 4;
        seg[22] = TCP_MSS >> 8;
        seg[23] = TCP_MSS & 0xFF;
    }
    memcpy(seg + hlen, data, len);
    size_t total = hlen + len;
    uint16_t c = csum_fold(csum_add(pseudo_sum(lip, rip, IPPROTO_TCP, total), seg, total));
    memcpy(seg + 16, &c, 2);
    ip_send(lip, rip, IPPROTO_TCP, seg, total);
}

static void send_seg(struct tcb *t, uint8_t flags, uint32_t seq, const void *data, size_t len)
{
    uint16_t wnd = rcv_window(t);
    t->last_wnd = wnd;
    send_raw(t->lip, t->rip, t->lport, t->rport, seq, t->rcv_nxt, flags | (t->state == TCP_SYN_SENT ? 0 : F_ACK),
             wnd, data, len, flags & F_SYN);
}

static void send_ack(struct tcb *t)
{
    send_seg(t, 0, t->snd_nxt, NULL, 0);
}

static void arm_rto(struct tcb *t)
{
    if (!t->rto_at)
        t->rto_at = ticks + ms_to_ticks(t->rto);
}

static bool can_send_data(int st)
{
    return st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT || st == TCP_FIN_WAIT_1 ||
           st == TCP_CLOSING || st == TCP_LAST_ACK;
}

static void output(struct tcb *t)
{
    if (!can_send_data(t->state))
        return;
    for (;;) {
        uint32_t off = t->snd_nxt - t->snd_una;
        if (off < t->slen) {
            uint32_t wnd_left = t->snd_wnd > off ? t->snd_wnd - off : 0;
            uint32_t n = MIN(MIN(t->slen - off, (uint32_t)t->rmss), wnd_left);
            if (n == 0) {
                if (t->snd_wnd == 0)
                    arm_rto(t);                  /* persist timer: probe later */
                break;
            }
            send_seg(t, F_PSH, t->snd_nxt, t->sbuf + off, n);
            t->snd_nxt += n;
            arm_rto(t);
            continue;
        }
        if (t->fin_queued && off == t->slen) {  /* all data out: send FIN */
            send_seg(t, F_FIN, t->snd_nxt, NULL, 0);
            t->snd_nxt++;
            t->fin_sent = true;
            if (t->state == TCP_ESTABLISHED)
                t->state = TCP_FIN_WAIT_1;
            else if (t->state == TCP_CLOSE_WAIT)
                t->state = TCP_LAST_ACK;
            arm_rto(t);
        }
        break;
    }
}

static void enter_time_wait(struct tcb *t)
{
    t->state = TCP_TIME_WAIT;
    t->rto_at = 0;
    t->deadline = ticks + ms_to_ticks(TIME_WAIT_MS);
}

static void set_closed(struct tcb *t, int err)
{
    if (err)
        t->err = err;
    t->state = TCP_CLOSED;
    t->rto_at = 0;
    wake(t);
    if (!t->owner && !t->accept_ready) {
        if (t->listener)
            t->listener = NULL;
        tcb_free(t);
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void send_reset(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, uint32_t seq,
                       uint32_t ack, uint8_t flags, size_t len)
{
    if (flags & F_RST)
        return;
    if (flags & F_ACK)
        send_raw(dst, src, dport, sport, ack, 0, F_RST, 0, NULL, 0, false);
    else
        send_raw(dst, src, dport, sport, 0, seq + len + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0),
                 F_RST | F_ACK, 0, NULL, 0, false);
}

static struct tcb *lookup(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport)
{
    struct tcb *listen = NULL;
    for (int i = 0; i < NTCB; i++) {
        struct tcb *t = &tcbs[i];
        if (!t->used || t->lport != dport)
            continue;
        if (t->state == TCP_LISTEN) {
            if (!t->lip || t->lip == dst)
                listen = t;
            continue;
        }
        if (t->state != TCP_CLOSED && t->rport == sport && t->rip == src && (t->lip == dst || !t->lip))
            return t;
    }
    return listen;
}

static int pending_children(struct tcb *l)
{
    int n = 0;
    for (int i = 0; i < NTCB; i++)
        if (tcbs[i].used && tcbs[i].listener == l)
            n++;
    return n;
}

static void parse_mss(struct tcb *t, const uint8_t *opt, size_t len)
{
    for (size_t i = 0; i < len;) {
        if (opt[i] == 0)
            break;
        if (opt[i] == 1) {
            i++;
            continue;
        }
        if (i + 1 >= len || opt[i + 1] < 2)
            break;
        if (opt[i] == 2 && opt[i + 1] == 4 && i + 3 < len) {
            uint16_t mss = (opt[i + 2] << 8) | opt[i + 3];
            t->rmss = MIN(mss, TCP_MSS);
        }
        i += opt[i + 1];
    }
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len)
{
    if (len < 20)
        return;
    if (csum_fold(csum_add(pseudo_sum(src, dst, IPPROTO_TCP, len), seg, len)) != 0)
        return;
    uint16_t sport = (seg[0] << 8) | seg[1], dport = (seg[2] << 8) | seg[3];
    uint32_t seq, ack;
    memcpy(&seq, seg + 4, 4);
    memcpy(&ack, seg + 8, 4);
    seq = ntohl(seq);
    ack = ntohl(ack);
    size_t hlen = (seg[12] >> 4) * 4;
    uint8_t flags = seg[13];
    uint16_t wnd = (seg[14] << 8) | seg[15];
    if (hlen < 20 || hlen > len)
        return;
    const uint8_t *data = seg + hlen;
    size_t dlen = len - hlen;

    struct tcb *t = lookup(src, dst, sport, dport);
    if (!t) {
        send_reset(src, dst, sport, dport, seq, ack, flags, dlen);
        return;
    }

    /* ---- LISTEN: a new connection request ---- */
    if (t->state == TCP_LISTEN) {
        if (flags & F_RST)
            return;
        if (flags & F_ACK) {
            send_reset(src, dst, sport, dport, seq, ack, flags, dlen);
            return;
        }
        if (!(flags & F_SYN) || pending_children(t) >= BACKLOG)
            return;
        struct tcb *c = tcp_alloc();
        if (!c)
            return;
        c->lip = dst;
        c->rip = src;
        c->lport = dport;
        c->rport = sport;
        c->irs = seq;
        c->rcv_nxt = seq + 1;
        c->iss = new_isn();
        c->snd_una = c->iss;
        c->snd_nxt = c->iss + 1;
        c->snd_wnd = wnd;
        c->listener = t;
        c->state = TCP_SYN_RCVD;
        parse_mss(c, seg + 20, hlen - 20);
        send_seg(c, F_SYN, c->iss, NULL, 0);
        arm_rto(c);
        return;
    }

    /* ---- SYN_SENT: waiting for SYN+ACK ---- */
    if (t->state == TCP_SYN_SENT) {
        bool ack_ok = (flags & F_ACK) && ack == t->iss + 1;
        if (flags & F_RST) {
            if (ack_ok)
                set_closed(t, ECONNREFUSED);
            return;
        }
        if ((flags & F_ACK) && !ack_ok) {
            send_reset(src, dst, sport, dport, seq, ack, flags, dlen);
            return;
        }
        if ((flags & F_SYN) && ack_ok) {
            t->irs = seq;
            t->rcv_nxt = seq + 1;
            t->snd_una = ack;
            t->snd_wnd = wnd;
            parse_mss(t, seg + 20, hlen - 20);
            t->state = TCP_ESTABLISHED;
            t->rto_at = 0;
            t->retries = 0;
            t->rto = 1000;
            send_ack(t);
            wake(t);
        }
        return;
    }

    /* ---- synchronised states ---- */
    if (flags & F_RST) {
        if (seq == t->rcv_nxt || (seq_le(t->rcv_nxt, seq) && seq_lt(seq, t->rcv_nxt + rcv_window(t) + 1)))
            set_closed(t, t->state == TCP_SYN_RCVD ? ECONNREFUSED : ECONNRESET);
        return;
    }
    if (flags & F_SYN) {                         /* duplicate SYN: re-acknowledge */
        if (t->state == TCP_SYN_RCVD)
            send_seg(t, F_SYN, t->iss, NULL, 0);
        else
            send_ack(t);
        return;
    }
    if (!(flags & F_ACK))
        return;

    if (t->state == TCP_SYN_RCVD) {
        if (ack != t->iss + 1) {
            send_reset(src, dst, sport, dport, seq, ack, flags, dlen);
            return;
        }
        t->snd_una = ack;
        t->state = TCP_ESTABLISHED;
        t->rto_at = 0;
        t->retries = 0;
        t->rto = 1000;
        if (t->listener)
            t->accept_ready = true;
        wake(t);
    }

    /* ACK processing */
    if (seq_lt(t->snd_una, ack) && seq_le(ack, t->snd_nxt)) {
        uint32_t acked = ack - t->snd_una;
        uint32_t data_acked = MIN(acked, t->slen);
        if (data_acked) {
            memmove(t->sbuf, t->sbuf + data_acked, t->slen - data_acked);
            t->slen -= data_acked;
        }
        t->snd_una += data_acked;
        if (acked > data_acked && t->fin_sent) {
            t->snd_una++;
            t->fin_acked = true;
        }
        t->retries = 0;
        t->rto = MAX(RTO_MIN, t->rto / 2);
        t->rto_at = t->snd_una != t->snd_nxt ? ticks + ms_to_ticks(t->rto) : 0;
        wake(t);
        if (t->fin_acked) {
            if (t->state == TCP_FIN_WAIT_1) {
                t->state = TCP_FIN_WAIT_2;
                if (!t->owner)
                    t->deadline = ticks + ms_to_ticks(FIN_WAIT2_ORPHAN_MS);
            } else if (t->state == TCP_CLOSING) {
                enter_time_wait(t);
            } else if (t->state == TCP_LAST_ACK) {
                set_closed(t, 0);
                return;
            }
        }
    }
    t->snd_wnd = wnd;

    /* Data */
    bool need_ack = false;
    bool accepting = t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 || t->state == TCP_FIN_WAIT_2;
    size_t taken = 0;
    if (dlen) {
        need_ack = true;
        if (accepting && seq == t->rcv_nxt) {
            size_t space = TCP_BUF - t->rlen;
            taken = MIN(dlen, space);
            for (size_t i = 0; i < taken; i++)
                t->rbuf[(t->rhead + t->rlen + i) % TCP_BUF] = data[i];
            t->rlen += taken;
            t->rcv_nxt += taken;
            if (taken)
                wake(t);
        }
    }
    /* FIN (only once all preceding data has been accepted) */
    if ((flags & F_FIN) && seq + dlen == t->rcv_nxt && (taken == dlen)) {
        need_ack = true;
        if (!t->fin_rcvd) {
            t->fin_rcvd = true;
            t->rcv_nxt++;
            wake(t);
        }
        switch (t->state) {
        case TCP_ESTABLISHED: t->state = TCP_CLOSE_WAIT; break;
        case TCP_FIN_WAIT_1:
            if (t->fin_acked)
                enter_time_wait(t);
            else
                t->state = TCP_CLOSING;
            break;
        case TCP_FIN_WAIT_2: enter_time_wait(t); break;
        case TCP_TIME_WAIT: enter_time_wait(t); break;
        }
    } else if ((flags & F_FIN) && t->fin_rcvd) {
        need_ack = true;                         /* retransmitted FIN */
    }
    if (need_ack)
        send_ack(t);
    output(t);
}

/* ------------------------------------------------------------------ */
/* Timers                                                              */
/* ------------------------------------------------------------------ */

void tcp_tick(void)
{
    for (int i = 0; i < NTCB; i++) {
        struct tcb *t = &tcbs[i];
        if (!t->used)
            continue;
        if ((t->state == TCP_TIME_WAIT || (t->state == TCP_FIN_WAIT_2 && !t->owner && t->deadline)) &&
            ticks >= t->deadline) {
            set_closed(t, 0);
            continue;
        }
        if (!t->rto_at || ticks < t->rto_at)
            continue;
        t->rto_at = 0;
        int limit = (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RCVD) ? SYN_RETRIES : MAX_RETRIES;
        if (++t->retries > limit) {
            if (t->state != TCP_SYN_SENT)
                send_seg(t, F_RST, t->snd_nxt, NULL, 0);
            set_closed(t, ETIMEDOUT);
            continue;
        }
        t->rto = MIN(RTO_MAX, t->rto * 2);
        if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RCVD) {
            send_seg(t, F_SYN, t->iss, NULL, 0);
            arm_rto(t);
            continue;
        }
        if (t->snd_wnd == 0 && t->slen > 0 && t->snd_nxt == t->snd_una) {
            send_seg(t, F_PSH, t->snd_una, t->sbuf, 1);   /* zero-window probe */
            arm_rto(t);
            continue;
        }
        t->snd_nxt = t->snd_una;                  /* go back N */
        if (t->fin_sent && !t->fin_acked)
            t->fin_sent = false;
        output(t);
    }
}

/* ------------------------------------------------------------------ */
/* User interface                                                      */
/* ------------------------------------------------------------------ */

int tcp_listen(struct tcb *t, uint32_t lip, uint16_t lport)
{
    t->lip = lip;
    t->lport = lport;
    t->state = TCP_LISTEN;
    return 0;
}

struct tcb *tcp_accept_ready(struct tcb *l)
{
    for (int i = 0; i < NTCB; i++) {
        struct tcb *c = &tcbs[i];
        if (c->used && c->listener == l && c->accept_ready) {
            c->listener = NULL;
            c->accept_ready = false;
            return c;
        }
    }
    return NULL;
}

int tcp_connect(struct tcb *t, uint32_t lip, uint16_t lport, uint32_t rip, uint16_t rport)
{
    t->lip = lip;
    t->lport = lport;
    t->rip = rip;
    t->rport = rport;
    t->iss = new_isn();
    t->snd_una = t->iss;
    t->snd_nxt = t->iss + 1;
    t->state = TCP_SYN_SENT;
    send_seg(t, F_SYN, t->iss, NULL, 0);
    arm_rto(t);
    while (t->state == TCP_SYN_SENT) {
        if (signal_pending(current))
            return -EINTR;
        sleep_on(t->owner);
    }
    if (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT)
        return 0;
    return -(t->err ? t->err : ECONNREFUSED);
}

long tcp_send(struct tcb *t, const void *buf, size_t n, bool nonblock)
{
    size_t done = 0;
    while (done < n) {
        if (t->err)
            return done ? (long)done : -t->err;
        if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT) {
            if (!done)
                signal_send(current, SIGPIPE);
            return done ? (long)done : -EPIPE;
        }
        if (t->fin_queued)
            return done ? (long)done : -EPIPE;
        uint32_t space = TCP_BUF - t->slen;
        if (space == 0) {
            if (nonblock)
                return done ? (long)done : -EAGAIN;
            if (signal_pending(current))
                return done ? (long)done : -ERESTART;
            sleep_on(t->owner);
            continue;
        }
        uint32_t c = MIN(space, (uint32_t)(n - done));
        memcpy(t->sbuf + t->slen, (const uint8_t *)buf + done, c);
        t->slen += c;
        done += c;
        output(t);
    }
    return done;
}

long tcp_recv(struct tcb *t, void *buf, size_t n, bool nonblock, int timeout_ms)
{
    uint64_t deadline = timeout_ms > 0 ? ticks + ms_to_ticks(timeout_ms) + 1 : 0;
    while (t->rlen == 0) {
        if (t->fin_rcvd || t->state == TCP_CLOSED || t->state == TCP_TIME_WAIT)
            return t->err && !t->fin_rcvd ? -t->err : 0;
        if (t->state == TCP_LISTEN)
            return -ENOTCONN;
        if (nonblock || (deadline && ticks >= deadline))
            return -EAGAIN;
        if (signal_pending(current))
            return -ERESTART;
        curlwp->wake_tick = deadline;
        sleep_on(t->owner);
        curlwp->wake_tick = 0;
    }
    size_t c = MIN(n, (size_t)t->rlen);
    for (size_t i = 0; i < c; i++)
        ((uint8_t *)buf)[i] = t->rbuf[(t->rhead + i) % TCP_BUF];
    t->rhead = (t->rhead + c) % TCP_BUF;
    t->rlen -= c;
    /* Window update if we had been advertising a small window. */
    if (t->last_wnd < TCP_BUF / 4 && rcv_window(t) >= TCP_BUF / 2 && can_send_data(t->state))
        send_ack(t);
    else if (t->last_wnd < TCP_BUF / 4 && rcv_window(t) >= TCP_BUF / 2 &&
             (t->state == TCP_FIN_WAIT_1 || t->state == TCP_FIN_WAIT_2))
        send_ack(t);
    return c;
}

void tcp_shutdown_write(struct tcb *t)
{
    if (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) {
        t->fin_queued = true;
        output(t);
    }
}

void tcp_abort(struct tcb *t)
{
    if (t->state != TCP_CLOSED && t->state != TCP_LISTEN && t->state != TCP_SYN_SENT && t->state != TCP_TIME_WAIT)
        send_seg(t, F_RST, t->snd_nxt, NULL, 0);
    t->owner = NULL;
    t->listener = NULL;
    t->accept_ready = false;
    t->state = TCP_CLOSED;
    tcb_free(t);
}

void tcp_close(struct tcb *t)
{
    switch (t->state) {
    case TCP_LISTEN:
        for (int i = 0; i < NTCB; i++)
            if (tcbs[i].used && tcbs[i].listener == t)
                tcp_abort(&tcbs[i]);
        tcb_free(t);
        return;
    case TCP_CLOSED:
    case TCP_SYN_SENT:
        tcb_free(t);
        return;
    case TCP_SYN_RCVD:
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        t->fin_queued = true;
        output(t);
        if (t->state == TCP_FIN_WAIT_2 && !t->owner)
            t->deadline = ticks + ms_to_ticks(FIN_WAIT2_ORPHAN_MS);
        break;
    case TCP_FIN_WAIT_2:
        t->deadline = ticks + ms_to_ticks(FIN_WAIT2_ORPHAN_MS);
        break;
    default:
        break;                                   /* already closing */
    }
    maybe_free(t);
}

bool tcp_readable(struct tcb *t)
{
    if (t->state == TCP_LISTEN) {
        for (int i = 0; i < NTCB; i++)
            if (tcbs[i].used && tcbs[i].listener == t && tcbs[i].accept_ready)
                return true;
        return false;
    }
    return t->rlen > 0 || t->fin_rcvd || t->state == TCP_CLOSED || t->err;
}

bool tcp_writable(struct tcb *t)
{
    return (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) && t->slen < TCP_BUF;
}

int tcp_info(struct sockinfo *out, int max, int start)
{
    (void)start;
    int n = 0;
    for (int i = 0; i < NTCB && n < max; i++) {
        struct tcb *t = &tcbs[i];
        if (!t->used)
            continue;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].proto = IPPROTO_TCP;
        out[n].state = t->state;
        out[n].lip = t->lip;
        out[n].rip = t->rip;
        out[n].lport = t->lport;
        out[n].rport = t->rport;
        out[n].rxq = t->rlen;
        out[n].txq = t->slen;
        out[n].uid = -1;
        n++;
    }
    return n;
}
