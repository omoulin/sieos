/*
 * obex.c - File transfer: OBEX Object Push over RFCOMM, both ways, one
 * transfer at a time.
 *
 *   sending   bt send ADDR FILE (BT_SEND, then BT_DATA pieces): the link
 *             (paged if need be), SDP for the device's Object Push channel,
 *             RFCOMM, CONNECT, PUTs (name and length first, a body each, as
 *             big as the device takes; the last one final), each answered
 *             "continue" before the next; done on its "success"
 *   receiving a bonded, encrypted device opens our channel (OPP_CH, in our
 *             SDP record): CONNECT, PUTs into a file (file_open: the desktop
 *             user's ~/Downloads, btd.c), DISCONNECT. Nothing is asked.
 * Packets: an opcode or answer code, a 16-bit length, headers (Name in
 * UTF-16, Length, Body, End of Body). Ours are at most OBMAX bytes.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "btd.h"

#define OBMAX 8192
enum { O_IDLE, O_LINK, O_SDP, O_RFC, O_CONNECT, O_READY, O_PUT, O_LAST, O_DONE, O_FAIL, O_RX };
static struct {
    int st, left, n, peer, hdr, file;            /* left: seconds before giving up; n: bytes of a packet so far */
    btdev_t *dev;                                /* peer: the largest packet it takes; hdr: name and length sent */
    conn_t *c;
    uint32_t size, done;
    char name[64];
    uint8_t buf[OBMAX];                          /* the packet being put together, or sent */
} ob;

static void fail(const char *why)
{
    int st = ob.st;
    bt_log("bt: OBEX: %s\n", why);
    strlcpy(bt.why, why, sizeof bt.why);
    ob.st = O_FAIL;
    if (st >= O_RFC) rf_close();
}

/* ---- Sending. */
static void found(conn_t *c, const sdp_res_t *r)
{
    if (ob.st != O_SDP) return;
    if (!r || !r->ch) { fail("the device takes no files (no Object Push)"); return; }
    ob.st = O_RFC;
    if (rf_open(c, r->ch)) fail("RFCOMM is busy");
}
int opp_link(conn_t *c)
{
    if (ob.st != O_LINK || c->dev != ob.dev) return 0;
    ob.st = O_SDP;
    if (sdp_query(c, 0x1105, found)) fail("SDP is busy");
    return 1;
}
int opp_send(const uint8_t *addr, const char *name, uint32_t size)
{
    btdev_t *d = dev_find(addr);
    if (!d || !(d->d.flags & BTF_BONDED) || d->d.type != BTT_CLASSIC) return -ENOENT;
    if (ob.st != O_IDLE && ob.st != O_DONE && ob.st != O_FAIL) return -EBUSY;
    ob.st = O_LINK; ob.dev = d; ob.size = size; ob.done = 0; ob.hdr = 0; ob.left = 30; bt.why[0] = 0;
    strlcpy(ob.name, name, sizeof ob.name);
    bt_log("bt: OBEX: sending %s (%u bytes) to %s\n", ob.name, (unsigned)size, d->d.name);
    conn_t *c = bt_link(d);
    if (c) opp_link(c);
    return 0;
}
/* The file's next n bytes (last: they end it) -> bytes taken, -EAGAIN (not
 * ready: again later), -EIO; with n = 0 and last: -EAGAIN until delivered, then 0. */
int opp_data(const uint8_t *p, int n, int last)
{
    if (ob.st == O_DONE || ob.st == O_FAIL) { int r = ob.st == O_DONE ? 0 : -EIO; if (!n) ob.st = O_IDLE; return r; }
    if (ob.st != O_READY) return ob.st == O_IDLE || ob.st == O_RX ? -EINVAL : -EAGAIN;
    uint8_t *b = ob.buf;
    int k = 3, cap = (ob.peer < OBMAX ? ob.peer : OBMAX) - 3;
    if (!ob.hdr) {                                        /* Name (UTF-16, from UTF-8), Length */
        int at = k;
        b[k] = 0x01; k += 3;
        for (const uint8_t *s = (const uint8_t *)ob.name; *s; k += 2) {
            int u = *s++;
            if (u >= 0xC0) { int x = u >= 0xE0 ? 2 : 1; u &= x == 2 ? 15 : 31; while (x-- && (*s & 0xC0) == 0x80) u = u << 6 | (*s++ & 63); }
            pb16(b + k, u);
        }
        pb16(b + k, 0); k += 2;
        pb16(b + at + 1, k - at);
        b[k] = 0xC3; pb16(b + k + 1, (int)(ob.size >> 16)); pb16(b + k + 3, (int)ob.size); k += 5;
        ob.hdr = 1;
    }
    int m = n < cap - k ? n : cap - k, fin = last && m == n;
    b[k] = fin ? 0x49 : 0x48; pb16(b + k + 1, 3 + m);
    memcpy(b + k + 3, p, m);
    k += 3 + m;
    b[0] = fin ? 0x82 : 0x02; pb16(b + 1, k);
    ob.done += (uint32_t)m; ob.st = fin ? O_LAST : O_PUT; ob.left = 30;
    rf_send(b, k);
    return m || !fin ? m : -EAGAIN;
}
static void answer(int len)                          /* the device's answer to our packet */
{
    int code = ob.buf[0];
    if (ob.st == O_CONNECT && code == 0xA0 && len >= 7) {
        ob.peer = ob.buf[5] << 8 | ob.buf[6];
        if (ob.peer < 255) ob.peer = 255;
        bt_log("bt: OBEX: connected, packets up to %d bytes\n", ob.peer);
        ob.st = O_READY;
    } else if (ob.st == O_PUT && code == 0x90) ob.st = O_READY;
    else if (ob.st == O_LAST && code == 0xA0) {
        static const uint8_t bye[3] = { 0x81, 0, 3 };
        bt_log("bt: OBEX: %s sent to %s (%u bytes)\n", ob.name, ob.dev->d.name, (unsigned)ob.done);
        ob.st = O_DONE;
        rf_send(bye, 3);
        rf_close();
    } else fail(code == 0xC3 || code == 0xC1 || code == 0xCD ? "the device refused the file" : "the transfer failed");
}

/* ---- Receiving. */
static void reply(int code)
{
    static const uint8_t con[7] = { 0xA0, 0, 7, 0x10, 0, OBMAX >> 8, OBMAX & 255 };
    if (code == 0x80) { rf_send(con, 7); return; }       /* (to CONNECT) */
    ob.buf[0] = (uint8_t)code; ob.buf[1] = 0; ob.buf[2] = 3;
    rf_send(ob.buf, 3);
}
static void request(int len)
{
    int op = ob.buf[0], bad = 0;
    if (op == 0x80) { reply(0x80); return; }
    if (op == 0x81) { reply(0xA0); return; }             /* DISCONNECT */
    if (op == 0xFF) { if (ob.file) { file_close(0); ob.file = 0; } reply(0xA0); return; }   /* ABORT */
    if ((op & 0x7F) != 0x02) { reply(0xD1); return; }    /* not implemented */
    for (int i = 3, hl; i < len && !bad; i += hl) {       /* the headers */
        int hi = ob.buf[i], two = hi >> 6 < 2;            /* two: a 16-bit length follows */
        hl = two ? (i + 3 <= len ? ob.buf[i + 1] << 8 | ob.buf[i + 2] : 0) : hi >> 6 == 2 ? 2 : 5;
        if (hl < 2 + two || i + hl > len) { bad = 1; break; }
        const uint8_t *v = ob.buf + i + 1 + 2 * two;
        if (hi == 0x01 && !ob.file) {                     /* Name: UTF-16 to UTF-8, no '/', no leading '.' */
            int o = 0;
            for (int j = 0; j + 1 < hl - 3 && o < 60; j += 2) {
                int u = v[j] << 8 | v[j + 1];
                if (!u) break;
                if (u < 32 || u == '/' || u == '\\' || (!o && u == '.')) u = '_';
                if (u < 0x80) ob.name[o++] = (char)u;
                else if (u < 0x800) { ob.name[o++] = (char)(0xC0 | u >> 6); ob.name[o++] = (char)(0x80 | (u & 63)); }
                else { ob.name[o++] = (char)(0xE0 | u >> 12); ob.name[o++] = (char)(0x80 | (u >> 6 & 63)); ob.name[o++] = (char)(0x80 | (u & 63)); }
            }
            ob.name[o] = 0; ob.done = 0;
        }
        if (hi == 0xC3 && !ob.file) ob.size = (uint32_t)v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3];
        if (hi == 0x48 || hi == 0x49) {                   /* Body, End of Body */
            if (!ob.file && (bad = !ob.c->enc || !ob.name[0] || file_open(ob.c->dev, ob.name, ob.size) ? 2 : 0)) break;
            ob.file = 1;
            if (file_write(v, hl - 3) != hl - 3) bad = 3;
            ob.done += (uint32_t)(hl - 3);
        }
    }
    if (op == 0x82 && !bad && !ob.file)                   /* (an empty file) */
        bad = !ob.c->enc || !ob.name[0] || file_open(ob.c->dev, ob.name, ob.size) ? 2 : (ob.file = 1, 0);
    if (bad) {
        bt_log("bt: OBEX: %s from %s refused (%s)\n", ob.name, ob.c->dev->d.name, bad == 2 ? "not taken" : bad == 3 ? "cannot write it" : "bad packet");
        if (ob.file) file_close(0);
        ob.file = 0; ob.name[0] = 0;
        reply(bad == 2 ? 0xC3 : bad == 3 ? 0xD0 : 0xC0);
        return;
    }
    if (op == 0x82) {                                     /* final */
        file_close(1);
        bt_log("bt: OBEX: %s received from %s (%u bytes)\n", ob.name, ob.c->dev->d.name, (unsigned)ob.done);
        ob.file = 0; ob.name[0] = 0;
    }
    reply(op == 0x82 ? 0xA0 : 0x90);
}

/* ---- From RFCOMM. */
void ob_open(conn_t *c, int out)
{
    ob.c = c; ob.n = 0;
    if (out && ob.st == O_RFC) {
        static const uint8_t con[7] = { 0x80, 0, 7, 0x10, 0, OBMAX >> 8, OBMAX & 255 };   /* CONNECT: version 1.0 */
        ob.st = O_CONNECT;
        rf_send(con, 7);
    } else if (!out && (ob.st == O_IDLE || ob.st == O_DONE || ob.st == O_FAIL)) {
        ob.st = O_RX; ob.file = 0; ob.name[0] = 0;
        bt_log("bt: OBEX: %s connects to send a file\n", c->dev->d.name);
    } else rf_close();
}
void ob_recv(const uint8_t *p, int n)
{
    if (ob.n + n > OBMAX) { ob.n = 0; if (ob.st == O_RX) reply(0xCD); else fail("a packet too big"); return; }
    memcpy(ob.buf + ob.n, p, n);
    ob.n += n;
    int len = ob.n >= 3 ? ob.buf[1] << 8 | ob.buf[2] : 0;
    if (len < 3 || ob.n < len) return;
    ob.n = 0;
    if (ob.st == O_RX) request(len); else answer(len);
}
void ob_close(void)
{
    if (ob.st == O_RX) {
        if (ob.file) { file_close(0); bt_log("bt: OBEX: %s from %s broke off\n", ob.name, ob.c->dev->d.name); }
        ob.file = 0; ob.st = O_IDLE;
    } else if (ob.st >= O_RFC && ob.st < O_DONE) { ob.st = O_FAIL; strlcpy(bt.why, "the link was lost", sizeof bt.why); bt_log("bt: OBEX: the link was lost\n"); }
}
void opp_tick(void)
{
    if (ob.st >= O_LINK && ob.st < O_DONE && !--ob.left) fail("the device does not answer");
}
