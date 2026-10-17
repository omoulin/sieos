/*
 * unixd - Pipes and unix socket pairs for programs built on libposix
 * (docs/webkit-plan.md), port "unix", started on demand. What Linux's kernel
 * does for them, a server does here:
 *
 *   ends       each pipe or socket pair is two ends; a process holds an end
 *              (the creator, a process it was passed to, a child it was
 *              handed to); an end closes when nobody holds it any more
 *   channels   bytes (or messages, SOCK_SEQPACKET) flowing one way, from the
 *              end that writes to the end that reads: a pipe has one, a
 *              socket pair two. A reader waits while there is nothing and a
 *              writer remains; a writer while 256 KiB wait unread
 *   passing    a message may carry descriptors (SCM_RIGHTS): other ends, or
 *              shared memory objects (memfd). unixd holds them on the way and
 *              gives them to the process that reads the message
 *   stashes    what a program hands to one it starts (posix_spawn): ends,
 *              shared memory and a block of bytes (its arguments, environment
 *              and descriptor table), claimed by the child with a random cookie
 *   poll       UNIX_POLL tells what ends are ready for; if none is, unixd
 *              notes the caller (SYS_NOTE) when one becomes ready
 *
 * One thread, one message at a time; calls that must wait keep the caller's
 * token and are answered later (as netd does). A ticker thread asks every 2 s
 * to look for holders that ended without saying so (crashed, killed).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

#define LIMIT   (256 << 10)             /* unread bytes a channel takes before writers wait */
#define MAXDESC 8                       /* descriptors per message */
#define NWATCH  4
#define TICK_NS 2000000000UL

typedef struct rec {                    /* a message in a channel */
    struct rec *next;
    uint32_t len, off, ndesc;
    unix_desc_t desc[MAXDESC];
    uint8_t data[];
} rec_t;
typedef struct wait { long token; int pid; uint32_t max, flags; struct wait *next; uint8_t *msg; size_t mlen; } wait_t;
typedef struct end end_t;
typedef struct chan {
    rec_t *head, *tail;
    size_t bytes;
    int writers, readers;               /* ends still writing to it, reading from it */
    end_t *rend, *wend;                 /* the end that reads it, the one that writes it */
    wait_t *rq, *wq;                    /* readers waiting for data, writers for room */
} chan_t;
typedef struct { int pid, refs; } holder_t;
struct end {
    uint64_t id;
    int type, wr_shut, rd_shut, closed;
    chan_t *rx, *tx;
    holder_t *h; int nh;
    int inflight;                       /* in messages and stashes, on the way */
    int watch[NWATCH];
    end_t *next;
};
typedef struct stash {
    uint64_t cookie; int64_t since;
    uint32_t ndesc; unix_desc_t desc[64];
    size_t len; uint8_t *blob;
    struct stash *next;
} stash_t;

static end_t *ends;
static stash_t *stashes;
static uint64_t next_id = 1;
static long port;
static uint8_t in[UNIX_MAX + MAXDESC * sizeof(unix_desc_t) + 64], out[UNIX_MAX + MAXDESC * sizeof(unix_desc_t)];

static void *zalloc(size_t n) { void *p = malloc(n); if (p) memset(p, 0, n); return p; }
static end_t *end_of(uint64_t id) { for (end_t *e = ends; e; e = e->next) if (e->id == id) return e; return 0; }
static holder_t *holder(end_t *e, int pid) { for (int i = 0; i < e->nh; i++) if (e->h[i].pid == pid) return &e->h[i]; return 0; }
static int hold(end_t *e, int pid)
{
    holder_t *h = holder(e, pid);
    if (h) { h->refs++; return 0; }
    holder_t *n = realloc(e->h, (e->nh + 1) * sizeof *n);
    if (!n) return -ENOMEM;
    e->h = n;
    e->h[e->nh++] = (holder_t){ pid, 1 };
    return 0;
}

/* ---- What an end is ready for (NET_IN, NET_OUT, NET_ERR: the other side gone). */
static int readiness(end_t *e)
{
    int r = 0;
    if (e->rx) {
        if (e->rx->head || !e->rx->writers || e->rd_shut) r |= NET_IN;
        if (!e->rx->writers && !e->rx->head && e->type != UNIX_PIPE) r |= NET_ERR;
        if (!e->rx->writers && e->type == UNIX_PIPE) r |= NET_ERR;
    }
    if (e->tx) {
        if (!e->tx->readers) r |= NET_ERR | NET_OUT;
        else if (e->tx->bytes < LIMIT && !e->wr_shut) r |= NET_OUT;
    }
    return r;
}
static void note_end(end_t *e)
{
    if (!e) return;
    for (int i = 0; i < NWATCH; i++) if (e->watch[i]) { sys_note(e->watch[i]); e->watch[i] = 0; }
}

/* ---- Descriptors on the way: unixd holds them until someone takes them. */
static void desc_release(unix_desc_t *d);
static void end_close(end_t *e);
static void unref_inflight(end_t *e) { if (--e->inflight <= 0 && !e->nh) end_close(e); }
static void desc_release(unix_desc_t *d)
{
    if (d->kind == UD_END) { end_t *e = end_of(d->id); if (e) unref_inflight(e); }
    else if (d->kind == UD_SHM) sys_shm_close(d->id);
}
static long desc_take(unix_desc_t *d, int pid)          /* sender pid passes d: unixd holds it */
{
    if (d->kind == UD_END) {
        end_t *e = end_of(d->id);
        if (!e || !holder(e, pid)) return -EBADF;
        e->inflight++;
        d->extra = (uint32_t)e->type;
        return 0;
    }
    if (d->kind == UD_SHM) return sys_shm_hold(d->id, 0) ? -EBADF : 0;
    return 0;                                           /* (others are only described) */
}
static void desc_give(unix_desc_t *d, int pid)          /* ... and gives it to the reader pid */
{
    if (d->kind == UD_END) { end_t *e = end_of(d->id); if (e) { hold(e, pid); e->inflight--; } }
    else if (d->kind == UD_SHM) { sys_shm_hold(d->id, pid); sys_shm_close(d->id); }
}

/* ---- Channels */
static void chan_drop(chan_t *c)
{
    for (rec_t *r = c->head, *n; r; r = n) {
        n = r->next;
        for (uint32_t i = 0; i < r->ndesc; i++) desc_release(&r->desc[i]);
        free(r);
    }
    for (wait_t *w = c->rq, *n; w; w = n) { n = w->next; reply_val(w->token, 0); free(w); }
    for (wait_t *w = c->wq, *n; w; w = n) { n = w->next; reply_val(w->token, -EPIPE); free(w->msg); free(w); }
    free(c);
}
static long enqueue(chan_t *c, const uint8_t *msg, size_t mlen, uint32_t ndesc, int pid);
/* Answer whoever waits on c, as far as its state allows. */
static void serve(chan_t *c)
{
    while (c->rq && (c->head || !c->writers)) {
        wait_t *w = c->rq;
        c->rq = w->next;
        size_t n = 0, nd = 0;
        int peek = w->flags & UNIX_PEEK, seq = c->rend && c->rend->type == UNIX_SEQPACKET;
        rec_t *r = c->head;
        if (r && !peek && r->ndesc) {                   /* the first message's descriptors */
            nd = r->ndesc;
            for (uint32_t i = 0; i < r->ndesc; i++) desc_give(&r->desc[i], w->pid);
            memcpy(out, r->desc, nd * sizeof(unix_desc_t));
            r->ndesc = 0;
        }
        uint8_t *o = out + nd * sizeof(unix_desc_t);
        while (r && n < w->max) {                       /* stream: across messages (up to the next
                                                           with descriptors); seqpacket: one */
            if (n && r->ndesc) break;
            size_t k = r->len - r->off < w->max - n ? r->len - r->off : w->max - n;
            memcpy(o + n, r->data + r->off, k);
            n += k;
            rec_t *next = r->next;
            if (!peek) {
                size_t used = seq ? r->len - r->off : k;   /* (seqpacket: the rest is dropped) */
                r->off += used; c->bytes -= used;
                if (r->off == r->len) { c->head = next; if (!c->head) c->tail = 0; free(r); }
            }
            if (seq) break;
            r = next;
        }
        msg_t m = { .w = { n, nd }, .sbuf = out, .slen = nd * sizeof(unix_desc_t) + n };
        ipc_reply(w->token, &m);
        free(w);
    }
    while (c->wq && (c->bytes < LIMIT || !c->readers)) {
        wait_t *w = c->wq;
        c->wq = w->next;
        long r = c->readers ? enqueue(c, w->msg, w->mlen, w->flags, w->pid) : -EPIPE;
        reply_val(w->token, r);
        free(w->msg);
        free(w);
    }
    note_end(c->rend);
    note_end(c->wend);
}
/* Data (descriptors first) into c. -> data bytes taken. */
static long enqueue(chan_t *c, const uint8_t *msg, size_t mlen, uint32_t ndesc, int pid)
{
    (void)pid;
    size_t dl = ndesc * sizeof(unix_desc_t);
    rec_t *r = malloc(sizeof *r + mlen - dl);
    if (!r) return -ENOMEM;
    memset(r, 0, sizeof *r);
    r->len = (uint32_t)(mlen - dl);
    r->ndesc = ndesc;
    memcpy(r->desc, msg, dl);
    memcpy(r->data, msg + dl, r->len);
    if (c->tail) c->tail->next = r; else c->head = r;
    c->tail = r;
    c->bytes += r->len;
    return r->len;
}

/* ---- Ends */
static void end_close(end_t *e)
{
    if (e->closed) return;
    e->closed = 1;
    if (e->tx && !e->wr_shut) e->tx->writers--;
    if (e->rx) e->rx->readers--;
    chan_t *t = e->tx, *r = e->rx;
    if (t) { t->wend = 0; if (!t->readers && !t->writers) chan_drop(t); else serve(t); }
    if (r) { r->rend = 0; if (!r->readers && !r->writers) chan_drop(r); else serve(r); }
    note_end(e);
    for (end_t **q = &ends; *q; q = &(*q)->next) if (*q == e) { *q = e->next; break; }
    free(e->h);
    free(e);
}
static void drop_holder(end_t *e, int pid, int all)
{
    holder_t *h = holder(e, pid);
    if (!h) return;
    if (all || !--h->refs) *h = e->h[--e->nh];
    if (!e->nh && !e->inflight) end_close(e);
}
static end_t *end_new(int type, int pid)
{
    end_t *e = zalloc(sizeof *e);
    if (!e) return 0;
    e->id = next_id++;
    e->type = type;
    if (hold(e, pid)) { free(e); return 0; }
    e->next = ends;
    ends = e;
    return e;
}
static long pair(int type, int pid, uint64_t *a, uint64_t *b)
{
    if (type < UNIX_STREAM || type > UNIX_PIPE) return -EINVAL;
    end_t *x = end_new(type, pid), *y = end_new(type, pid);
    chan_t *c1 = zalloc(sizeof *c1), *c2 = type == UNIX_PIPE ? 0 : zalloc(sizeof *c2);
    if (!x || !y || !c1 || (type != UNIX_PIPE && !c2)) { if (x) end_close(x); if (y) end_close(y); free(c1); free(c2); return -ENOMEM; }
    c1->writers = c1->readers = 1;
    if (type == UNIX_PIPE) {                            /* x reads, y writes */
        x->rx = c1; y->tx = c1; c1->rend = x; c1->wend = y;
    } else {                                            /* x writes c1 to y, y writes c2 to x */
        c2->writers = c2->readers = 1;
        x->tx = c1; y->rx = c1; c1->wend = x; c1->rend = y;
        y->tx = c2; x->rx = c2; c2->wend = y; c2->rend = x;
    }
    *a = x->id; *b = y->id;
    return 0;
}

/* ---- Requests */
static int deferred;
static long request(long from, msg_t *m)
{
    int pid = m->pid;
    end_t *e = 0;
    if (m->w[0] == UNIX_SEND || m->w[0] == UNIX_RECV || m->w[0] == UNIX_CLOSE || m->w[0] == UNIX_SHUTDOWN) {
        e = end_of(m->w[1]);
        if (!e || !holder(e, pid)) return -EBADF;
    }
    switch (m->w[0]) {
    case UNIX_PAIR: {
        uint64_t a, b;
        long r = pair((int)m->w[1], pid, &a, &b);
        if (r) return r;
        msg_t rep = { .w = { 0, a, b } };
        ipc_reply(from, &rep);
        deferred = 1;
        return 0;
    }
    case UNIX_SEND: {
        uint32_t nd = (uint32_t)m->w[2];
        size_t dl = nd * sizeof(unix_desc_t);
        if (nd > MAXDESC || m->rlen < dl) return -EINVAL;
        if (!e->tx || e->wr_shut) return -EPIPE;
        chan_t *c = e->tx;
        if (!c->readers) return -EPIPE;
        if (e->type == UNIX_SEQPACKET && m->rlen - dl > UNIX_MAX) return -EMSGSIZE;
        unix_desc_t *d = (unix_desc_t *)in;
        for (uint32_t i = 0; i < nd; i++) {
            long r = desc_take(&d[i], pid);
            if (r) { while (i--) desc_release(&d[i]); return r; }
        }
        if (c->bytes >= LIMIT) {                       /* full: wait for room, or not */
            if (m->w[3] & UNIX_NONBLOCK) { for (uint32_t i = 0; i < nd; i++) desc_release(&d[i]); return -EAGAIN; }
            wait_t *w = zalloc(sizeof *w);
            uint8_t *copy = malloc(m->rlen);
            if (!w || !copy) { free(w); free(copy); return -ENOMEM; }
            memcpy(copy, in, m->rlen);
            *w = (wait_t){ from, pid, 0, nd, 0, copy, m->rlen };
            wait_t **q = &c->wq;
            while (*q) q = &(*q)->next;
            *q = w;
            deferred = 1;
            return 0;
        }
        long r = enqueue(c, in, m->rlen, nd, pid);
        serve(c);
        return r;
    }
    case UNIX_RECV: {
        if (!e->rx) return -EINVAL;
        chan_t *c = e->rx;
        if (!c->head && (!c->writers || e->rd_shut)) return 0;         /* the end: nothing more will come */
        if (!c->head && (m->w[3] & UNIX_NONBLOCK)) return -EAGAIN;
        wait_t *w = zalloc(sizeof *w);
        if (!w) return -ENOMEM;
        uint32_t max = m->w[2] < UNIX_MAX ? (uint32_t)m->w[2] : UNIX_MAX;
        *w = (wait_t){ from, pid, max, (uint32_t)m->w[3], 0, 0, 0 };
        wait_t **q = &c->rq;
        while (*q) q = &(*q)->next;
        *q = w;
        serve(c);
        deferred = 1;
        return 0;
    }
    case UNIX_CLOSE:
        drop_holder(e, pid, 0);
        return 0;
    case UNIX_SHUTDOWN:
        if ((m->w[2] == 1 || m->w[2] == 2) && e->tx && !e->wr_shut) { e->wr_shut = 1; e->tx->writers--; serve(e->tx); }
        if ((m->w[2] == 0 || m->w[2] == 2) && e->rx) { e->rd_shut = 1; serve(e->rx); }
        return 0;
    case UNIX_POLL: {
        size_t n = m->rlen / 8;
        uint64_t *ids = (uint64_t *)in;
        long ready = 0;
        if (n > UNIX_MAX / 8) return -EINVAL;
        for (size_t i = 0; i < n; i++) {         /* (the top byte: what is waited for) */
            end_t *x = end_of(ids[i] & ~(0xffULL << 56));
            int want = ids[i] >> 56 ? (int)(ids[i] >> 56) : NET_IN | NET_OUT;
            out[i] = x && holder(x, pid) ? (uint8_t)readiness(x) : NET_ERR;
            ready += (out[i] & (want | NET_ERR)) != 0;
        }
        for (size_t i = 0; !ready && i < n; i++) {     /* nothing ready: note the caller when something is */
            end_t *x = end_of(ids[i] & ~(0xffULL << 56));
            int k = 0;
            while (x && k < NWATCH && x->watch[k] && x->watch[k] != pid) k++;
            if (x && k < NWATCH) x->watch[k] = pid;
            else if (x) x->watch[0] = pid;
        }
        msg_t rep = { .w = { (uint64_t)ready }, .sbuf = out, .slen = n };
        ipc_reply(from, &rep);
        deferred = 1;
        return 0;
    }
    case UNIX_STASH: {
        uint32_t nd = (uint32_t)m->w[1];
        size_t dl = nd * sizeof(unix_desc_t);
        if (nd > 64 || m->rlen < dl) return -EINVAL;
        stash_t *s = zalloc(sizeof *s);
        if (!s || !(s->blob = malloc(m->rlen - dl + 1))) { free(s); return -ENOMEM; }
        memcpy(s->desc, in, dl);
        for (uint32_t i = 0; i < nd; i++) {
            long r = desc_take(&s->desc[i], pid);
            if (r) { while (i--) desc_release(&s->desc[i]); free(s->blob); free(s); return r; }
        }
        s->ndesc = nd;
        s->len = m->rlen - dl;
        memcpy(s->blob, in + dl, s->len);
        sys_random(&s->cookie, sizeof s->cookie);
        s->since = sys_clock();
        s->next = stashes;
        stashes = s;
        msg_t rep = { .w = { 0, s->cookie } };
        ipc_reply(from, &rep);
        deferred = 1;
        return 0;
    }
    case UNIX_CLAIM: case UNIX_UNSTASH: {
        stash_t *s = 0;
        for (stash_t **q = &stashes; *q; q = &(*q)->next)
            if ((*q)->cookie == m->w[1]) { s = *q; *q = s->next; break; }
        if (!s) return -ENOENT;
        size_t dl = s->ndesc * sizeof(unix_desc_t);
        if (m->w[0] == UNIX_CLAIM) {
            for (uint32_t i = 0; i < s->ndesc; i++) desc_give(&s->desc[i], pid);
            memcpy(out, s->desc, dl);
            memcpy(out + dl, s->blob, s->len < sizeof out - dl ? s->len : sizeof out - dl);
            msg_t rep = { .w = { s->len, s->ndesc }, .sbuf = out, .slen = dl + s->len };
            ipc_reply(from, &rep);
            deferred = 1;
        } else for (uint32_t i = 0; i < s->ndesc; i++) desc_release(&s->desc[i]);
        free(s->blob);
        free(s);
        return 0;
    }
    case UNIX_BYE:                                      /* the process ends: all its ends */
        for (end_t *x = ends, *n; x; x = n) { n = x->next; drop_holder(x, pid, 1); }
        return 0;
    }
    return -ENOSYS;
}

/* Every TICK_NS: holders that ended without a word; stashes never claimed. */
static void tick(void)
{
    for (end_t *x = ends, *n; x; x = n) {
        n = x->next;
        for (int i = x->nh - 1; i >= 0; i--)
            if (kill(x->h[i].pid, 0) == -ESRCH) { drop_holder(x, x->h[i].pid, 1); break; }   /* (x may be gone) */
    }
    int64_t now = sys_clock();
    for (stash_t **q = &stashes; *q; ) {
        stash_t *s = *q;
        if (now - s->since < 60000000000LL) { q = &s->next; continue; }
        *q = s->next;
        for (uint32_t i = 0; i < s->ndesc; i++) desc_release(&s->desc[i]);
        free(s->blob);
        free(s);
    }
}
static void ticker(void *a)
{
    (void)a;
    for (;;) { sys_sleep(TICK_NS); msg_t m = { .w = { UNIX_TICK } }; ipc_call(port, &m); }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    port = port_create("unix");
    if (port < 0) return 1;
    thread_start(ticker, 0, 8192);
    int self = 0;
    { mk_ident_t id; if (!sys_ident(0, &id)) self = id.pid; }
    uint64_t asked = 0, seen = 0;
    for (;;) {
        msg_t m = { .rbuf = in, .rlen = sizeof in };
        long from = ipc_recv(port, &m), r;
        if (from <= 0) continue;
        if (m.w[0] == UNIX_TICK) { r = m.pid == self ? (tick(), 0) : -EPERM; reply_val(from, r); continue; }
        if (m.w[0] == SVC_MAYSTOP) {
            r = maystop_answer(&m, asked, &seen, ends || stashes);
            reply_val(from, r);
            if (!r) return 0;
            continue;
        }
        asked++;
        r = request(from, &m);
        if (!deferred) reply_val(from, r);
        deferred = 0;
    }
}
