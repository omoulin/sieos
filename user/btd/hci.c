/*
 * hci.c - HCI, the host's side of the controller (Core spec Vol 4 Part E):
 * commands (as many in flight as the controller says it takes), events,
 * ACL data (fragmented to its buffers, sent as they free up: "Number Of
 * Completed Packets"); the devices a scan finds; connections; classic
 * pairing, which the controller does itself (Secure Simple Pairing: the
 * host answers its questions: IO capabilities, the 6-digit code, keys).
 *
 * Pairing, as the user sees it (BT_PAIR, then BT_STATUS):
 *   classic  Create Connection, Authentication Requested; the controller
 *            asks for a link key (none: pairing), our IO capability
 *            (display + yes/no), then the code to compare (or to type on
 *            a keyboard), and gives the new link key: kept. Encryption on,
 *            then the HID channels (input.c).
 *   LE       LE Create Connection; SMP (smp.c); encryption; HID over GATT.
 * Bonded devices come back by themselves: a classic one pages us (page
 * scan stays on), LE ones are connected as soon as they advertise (the
 * controller's accept list, one pending LE Create Connection for all).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "btd.h"

struct bt bt;

static const char *hex6(const uint8_t *a)        /* an address, as people write it */
{
    static char s[2][18];
    static int k;
    char *o = s[k ^= 1];
    for (int i = 0; i < 6; i++) {
        o[3 * i] = "0123456789ABCDEF"[a[5 - i] >> 4]; o[3 * i + 1] = "0123456789ABCDEF"[a[5 - i] & 15];
        o[3 * i + 2] = i < 5 ? ':' : 0;
    }
    return o;
}

/* ---- Commands: sent while the controller takes them, queued otherwise. */
static uint8_t cq[1024];
static int cqn;
void cmd(int op, const void *p, int n)
{
    uint8_t b[3 + 255] = { (uint8_t)op, (uint8_t)(op >> 8), (uint8_t)n };
    memcpy(b + 3, p, n);
    if (bt.ncmd > 0 && !cqn) { bt.ncmd--; tr_send(H4_CMD, b, 3 + n); }
    else if (cqn + 3 + n <= (int)sizeof cq) { memcpy(cq + cqn, b, 3 + n); cqn += 3 + n; }
    else bt_log("bt: command queue full, %04x dropped\n", op);
}
static void credits(int n)
{
    for (bt.ncmd = n; bt.ncmd > 0 && cqn; bt.ncmd--) {
        int l = 3 + cq[2];
        tr_send(H4_CMD, cq, l);
        memmove(cq, cq + l, cqn -= l);
    }
}

/* ---- ACL data: one queue, sent as the controller's buffers allow. */
typedef struct pkt { struct pkt *next; uint16_t handle; uint8_t le; int n; uint8_t b[]; } pkt_t;
static pkt_t *txq, **txt = &txq;
static conn_t *conn_of(int handle)
{
    for (int i = 0; i < MAXCONN; i++) if (bt.conn[i].dev && bt.conn[i].handle == handle) return &bt.conn[i];
    return 0;
}
static void acl_flush(void)
{
    while (txq) {
        pkt_t *p = txq;
        int *cr = p->le && bt.le_mtu ? &bt.le_free : &bt.acl_free;
        conn_t *c = conn_of(p->handle);
        if (c && *cr <= 0) break;
        if (c) { (*cr)--; c->inflight++; tr_send(H4_ACL, p->b, p->n); }
        if (!(txq = p->next)) txt = &txq;
        free(p);
    }
}
void acl_send(conn_t *c, int cid, const void *data, int n)
{
    static uint8_t f[L2_MTU + 64];
    if (n + 4 > (int)sizeof f) return;
    p16(f, n); p16(f + 2, cid); memcpy(f + 4, data, n);
    int mtu = c->le && bt.le_mtu ? bt.le_mtu : bt.acl_mtu;
    for (int off = 0; off < n + 4; off += mtu) {
        int k = n + 4 - off < mtu ? n + 4 - off : mtu;
        pkt_t *p = malloc(sizeof *p + 4 + k);
        if (!p) return;
        p->next = 0; p->handle = c->handle; p->le = c->le; p->n = 4 + k;
        p16(p->b, c->handle | (off ? 1 : c->le ? 0 : 2) << 12);   /* continuing; LE start; classic start */
        p16(p->b + 2, k);
        memcpy(p->b + 4, f + off, k);
        *txt = p; txt = &p->next;
    }
    acl_flush();
}
static void acl_rx(const uint8_t *p, int n)
{
    if (n < 4) return;
    conn_t *c = conn_of(g16(p) & 0xFFF);
    int pb = p[1] >> 4 & 3, len = g16(p + 2);
    if (!c || len + 4 > n) return;
    if (pb != 1) c->rxn = 0;                              /* a frame starts */
    else if (!c->rxn) return;
    if (c->rxn + len > (int)sizeof c->rx) { c->rxn = 0; return; }
    memcpy(c->rx + c->rxn, p + 4, len);
    c->rxn += len;
    if (c->rxn >= 4 && c->rxn >= 4 + g16(c->rx)) { c->rxn = 0; l2_rx(c, c->rx, 4 + g16(c->rx)); }
}

/* ---- Devices. */
static btdev_t *dev_find(const uint8_t *addr)
{
    for (int i = 0; i < bt.ndev; i++) if (!memcmp(bt.dev[i].d.addr, addr, 6)) return &bt.dev[i];
    return 0;
}
btdev_t *dev_add(const uint8_t *addr, int type)
{
    btdev_t *d = dev_find(addr);
    if (!d && bt.ndev < BT_MAXDEV) {
        d = &bt.dev[bt.ndev++];
        memset(d, 0, sizeof *d);
        memcpy(d->d.addr, addr, 6);
        d->d.type = (uint8_t)type;
    }
    return d;
}
static int pairing(btdev_t *d) { return d && d == bt.pdev && (bt.pair == BTP_BUSY || bt.pair == BTP_CONFIRM || bt.pair == BTP_PASSKEY); }
static conn_t *conn_dev(btdev_t *d)
{
    for (int i = 0; i < MAXCONN; i++) if (bt.conn[i].dev == d) return &bt.conn[i];
    return 0;
}
static int cod_kind(const uint8_t *c)
{
    int major = c[1] & 31, minor = c[0] >> 2;
    if (major == 5) return minor & 0x10 ? BTK_KEYBOARD : minor & 0x20 ? BTK_MOUSE : BTK_OTHER;
    return major == 4 ? BTK_AUDIO : major == 2 ? BTK_PHONE : major == 1 ? BTK_COMPUTER : BTK_OTHER;
}
/* Advertising data or an extended inquiry response: the name, what it is.
 * -> its flags (LE: discoverable, 1 or 2), -1 if it gave none. */
static int ad(btdev_t *d, const uint8_t *p, int n)
{
    int flags = -1;
    for (int i = 0; i + 1 < n && p[i] && i + 1 + p[i] <= n; i += 1 + p[i]) {
        int l = p[i] - 1, t = p[i + 1];
        const uint8_t *v = p + i + 2;
        if ((t == 8 && !d->d.name[0]) || t == 9) {
            int k = l < 31 ? l : 31;
            memcpy(d->d.name, v, k); d->d.name[k] = 0;
        } else if (t == 1 && l) flags = v[0] & 3;
        else if (t == 0x19 && l >= 2) {                   /* appearance */
            int a = g16(v);
            d->d.kind = a == 0x3C1 ? BTK_KEYBOARD : a == 0x3C2 ? BTK_MOUSE : a >> 6 == 1 ? BTK_PHONE : a >> 6 == 2 ? BTK_COMPUTER : d->d.kind;
        } else if (t == 0x0D && l >= 3) d->d.kind = (uint8_t)cod_kind(v);
    }
    return flags;
}

/* ---- Connections. */
static conn_t *conn_new(int handle, btdev_t *d, int le)
{
    for (int i = 0; i < MAXCONN; i++)
        if (!bt.conn[i].dev) {
            conn_t *c = &bt.conn[i];
            memset(c, 0, sizeof *c);
            c->handle = (uint16_t)handle; c->dev = d; c->le = (uint8_t)le;
            d->d.flags |= BTF_CONNECTED;
            return c;
        }
    return 0;
}
void bt_disconnect(conn_t *c, int reason)
{
    uint8_t p[3] = { (uint8_t)c->handle, (uint8_t)(c->handle >> 8), (uint8_t)reason };
    cmd(0x0406, p, 3);
}
void le_encrypt(conn_t *c, const uint8_t *ltk, const uint8_t *rand, int ediv)
{
    uint8_t p[28] = { 0 };
    p16(p, c->handle);
    if (rand) memcpy(p + 2, rand, 8);
    p16(p + 10, ediv);
    memcpy(p + 12, ltk, 16);
    cmd(0x2019, p, 28);
}
/* LE Create Connection: to d, or (0) to whichever device of the accept list advertises. */
static void le_create(btdev_t *d)
{
    uint8_t p[25] = { 0x60, 0, 0x30, 0, (uint8_t)!d, (uint8_t)(d ? d->d.type - 1 : 0) };
    if (d) memcpy(p + 6, d->d.addr, 6);
    p16(p + 13, 6); p16(p + 15, 12);                      /* every 7.5 to 15 ms (input devices) */
    p16(p + 19, 400);                                     /* lost after 4 s of silence */
    cmd(0x200D, p, 25);
}
/* Bonded LE devices not connected: in the accept list, one connection
 * request pending for them all. */
static void le_auto(void)
{
    if (!bt.on || bt.le_busy || bt.state != BTS_ON) return;
    for (int i = 0, n = 0; i < bt.ndev; i++) {
        btdev_t *d = &bt.dev[i];
        if (d->d.type == BTT_CLASSIC || !(d->d.flags & BTF_BONDED) || (d->d.flags & BTF_CONNECTED)) continue;
        uint8_t a[7] = { (uint8_t)(d->d.type - 1) };
        memcpy(a + 1, d->d.addr, 6);
        if (!n++) cmd(0x2010, 0, 0);                      /* (cleared first) */
        cmd(0x2011, a, 7);
        bt.le_busy = 1;
    }
    if (bt.le_busy) le_create(0);
}

void pair_end(int ok, const char *why)
{
    if (bt.pair != BTP_BUSY && bt.pair != BTP_CONFIRM && bt.pair != BTP_PASSKEY) return;
    bt.pair = ok ? BTP_DONE : BTP_FAILED;
    if (!ok) strlcpy(bt.why, why, sizeof bt.why);
    bt_log(ok ? "bt: %s paired (%s)\n" : "bt: pairing with %s failed: %s\n", hex6(bt.pdev->d.addr), ok ? bt.pdev->d.name : why);
}

static void scan_stop(void)
{
    if (bt.scan_left) { uint8_t p[2] = { 0 }; cmd(0x200C, p, 2); bt.scan_left = 0; }
    if (bt.inquiring) { cmd(0x0402, 0, 0); bt.inquiring = 0; }
}

/* After an inquiry: ask the classic devices that gave no name for it, one at a time. */
static void names(void)
{
    for (int i = 0; i < bt.ndev; i++) {
        btdev_t *d = &bt.dev[i];
        if (d->d.type != BTT_CLASSIC || d->d.name[0] || d->named) continue;
        uint8_t p[10];
        memcpy(p, d->d.addr, 6); p[6] = d->psrm; p[7] = 0; p16(p + 8, d->clkoff | 0x8000);
        cmd(0x0419, p, 10);
        d->named = 1;
        return;
    }
}

static void inquiry_result(const uint8_t *e, int ext)
{
    btdev_t *d = dev_add(e + 1, BTT_CLASSIC);
    if (!d) return;
    d->psrm = e[7];
    d->d.kind = (uint8_t)cod_kind(e + (ext ? 9 : 10));
    d->clkoff = g16(e + (ext ? 12 : 13));
    if (ext) d->d.rssi = (int8_t)e[14];
    if (ext == 2) ad(d, e + 15, 240);
}

static void adv_report(const uint8_t *e, int len)
{
    const uint8_t *p = e + 1;
    for (int k = 0; k < e[0] && p + 10 <= e + len; k++) {
        int type = p[0], at = p[1], n = p[8];
        if (p + 10 + n > e + len) return;
        btdev_t *d = dev_find(p + 2);
        if (type <= 1 && !d && at <= 1) {                 /* connectable: listed if discoverable */
            btdev_t t = { .d = { .type = (uint8_t)(at + 1) } };
            if (ad(&t, p + 9, n) > 0 && (d = dev_add(p + 2, at + 1))) { memcpy(d->d.name, t.d.name, 32); d->d.kind = t.d.kind; }
        } else if (d && d->d.type != BTT_CLASSIC) ad(d, p + 9, n);
        if (d && d->d.type != BTT_CLASSIC) d->d.rssi = (int8_t)p[9 + n];
        p += 10 + n;
    }
}

static void le_conn(const uint8_t *e)
{
    int st = e[1], h = g16(e + 2);
    btdev_t *d = dev_find(e + 6);
    int was = bt.le_busy;
    bt.le_busy = 0;
    if (st) {
        if (was == 2 && d == bt.pdev) pair_end(0, "the device did not connect");
        else if (bt.pdev && bt.pair == BTP_BUSY && bt.pdev->d.type != BTT_CLASSIC && !conn_dev(bt.pdev)) {
            le_create(bt.pdev); bt.le_busy = 2;           /* the auto-connection was cancelled for it */
        } else le_auto();
        return;
    }
    if (!d) d = dev_add(e + 6, e[5] + 1);
    conn_t *c = d ? conn_new(h, d, 1) : 0;
    if (!c) { uint8_t p[3] = { (uint8_t)h, (uint8_t)(h >> 8), 0x13 }; cmd(0x0406, p, 3); return; }
    c->master = !e[4];
    c->out = 1;
    bt_log("bt: LE connection to %s (%s), handle %d\n", hex6(d->d.addr), d->d.name, h);
    if (d == bt.pdev && bt.pair == BTP_BUSY) smp_start(c);
    else if (d->d.flags & BTF_BONDED) le_encrypt(c, d->key, d->rand, d->ediv);
    le_auto();
}

static void le_meta(const uint8_t *e, int len)
{
    if (e[0] == 0x01 && len >= 19) le_conn(e);
    else if (e[0] == 0x02) adv_report(e + 1, len - 1);
}

static void conn_complete(const uint8_t *e)
{
    btdev_t *d = dev_find(e + 3);
    if (e[0]) {
        if (d && d == bt.pdev) pair_end(0, "the device did not answer");
        return;
    }
    if (!d) d = dev_add(e + 3, BTT_CLASSIC);
    conn_t *c = d ? conn_new(g16(e + 1), d, 0) : 0;
    if (!c) { uint8_t p[3] = { e[1], e[2], 0x13 }; cmd(0x0406, p, 3); return; }
    c->out = d == bt.pdev && bt.pair == BTP_BUSY;
    c->master = 1;
    bt_log("bt: connected to %s (%s), handle %d\n", hex6(d->d.addr), d->d.name, c->handle);
    uint8_t p[2]; p16(p, c->handle);
    cmd(0x0411, p, 2);                                    /* Authentication Requested: pair, or prove the bond */
}

static void disconnected(const uint8_t *e)
{
    conn_t *c = conn_of(g16(e + 1));
    if (!c) return;
    btdev_t *d = c->dev;
    bt_log("bt: %s disconnected (reason %02x)\n", hex6(d->d.addr), e[3]);
    if (c->le && bt.le_mtu) bt.le_free += c->inflight; else bt.acl_free += c->inflight;
    l2_gone(c);
    input_gone(c);
    smp_end(c);
    if (pairing(d)) pair_end(0, "the device disconnected");
    d->d.flags &= ~(BTF_CONNECTED | BTF_INPUT);
    c->dev = 0;
    acl_flush();                                          /* (its queued packets go) */
    if (c->le) le_auto();
}

static void link_key(const uint8_t *e, int note)
{
    btdev_t *d = dev_find(e);
    if (note) {                                           /* Link Key Notification: a new bond */
        if (!d) return;
        memcpy(d->key, e + 6, 16);
        d->d.flags |= BTF_BONDED;
        bt_log("bt: %s: link key kept (type %d)\n", hex6(d->d.addr), e[22]);
        bonds_save();
        return;
    }
    if (d && (d->d.flags & BTF_BONDED) && !pairing(d)) {
        uint8_t p[22];
        memcpy(p, e, 6); memcpy(p + 6, d->key, 16);
        cmd(0x040B, p, 22);
    } else cmd(0x040C, e, 6);
}

/* Secure Simple Pairing's questions, for the device being paired only. */
static void ssp(int code, const uint8_t *e)
{
    if (code == 0x36) e++;                                /* (after its status) */
    btdev_t *d = dev_find(e);
    conn_t *c = d ? conn_dev(d) : 0;
    int ours = pairing(d) && c;
    switch (code) {
    case 0x31:                                            /* IO Capability Request */
        if (ours) { uint8_t p[9]; memcpy(p, e, 6); p[6] = 1; p[7] = 0; p[8] = 3; cmd(0x042B, p, 9); }   /* display yes/no, MITM, dedicated bonding */
        else { uint8_t p[7]; memcpy(p, e, 6); p[6] = 0x18; cmd(0x0434, p, 7); }   /* pairing not allowed */
        break;
    case 0x32: if (c) c->peer_io = e[6]; break;           /* IO Capability Response */
    case 0x33:                                            /* User Confirmation Request */
        if (!ours) { cmd(0x042D, e, 6); break; }
        if (c->peer_io == 3) { cmd(0x042C, e, 6); break; } /* it has no display nor buttons: just works */
        bt.pair = BTP_CONFIRM; bt.code = g32(e + 6);
        bt_log("bt: %s: does it show %06u? (bt pair confirms)\n", hex6(e), bt.code);
        break;
    case 0x3B:                                            /* User Passkey Notification: type it on the device */
        if (ours) { bt.pair = BTP_PASSKEY; bt.code = g32(e + 6); bt_log("bt: %s: type %06u on it, then Enter\n", hex6(e), bt.code); }
        break;
    case 0x34: cmd(0x042F, e, 6); break;                  /* User Passkey Request: we have no keyboard to type on */
    case 0x16: {                                          /* PIN Code Request (pre-2.1 devices): 0000 */
        uint8_t p[23] = { 0 };
        memcpy(p, e, 6); p[6] = 4; memcpy(p + 7, "0000", 4);
        if (ours) cmd(0x040D, p, 23); else cmd(0x040E, e, 6);
        break;
    }
    case 0x36: if (ours && e[-1]) pair_end(0, "the pairing was refused"); break;  /* Simple Pairing Complete */
    }
}

static void cmd_failed(int op, int st)
{
    bt_log("bt: command %04x failed (%02x)\n", op, st);
    if (op == 0x0405 || op == 0x200D) {                   /* a connection could not even start */
        if (op == 0x200D) bt.le_busy = 0;
        if (bt.pair == BTP_BUSY) pair_end(0, "could not connect");
    }
}

void bt_rx(int type, const uint8_t *p, int n)
{
    if (type == H4_ACL) { acl_rx(p, n); return; }
    if (type != H4_EVT || n < 2 || p[1] + 2 > n) return;
    const uint8_t *e = p + 2;
    int len = p[1];
    conn_t *c;
    switch (p[0]) {
    case 0x0E: if (len >= 4 && e[3]) cmd_failed(g16(e + 1), e[3]); credits(e[0]); break;
    case 0x0F: if (e[0]) cmd_failed(g16(e + 2), e[0]); credits(e[1]); break;
    case 0x13:                                            /* Number Of Completed Packets */
        for (int i = 0; i < e[0] && 1 + 4 * i + 4 <= len; i++)
            if ((c = conn_of(g16(e + 1 + 4 * i)))) {
                int k = g16(e + 3 + 4 * i);
                c->inflight -= k;
                if (c->le && bt.le_mtu) bt.le_free += k; else bt.acl_free += k;
            }
        acl_flush();
        break;
    case 0x01: bt.inquiring = 0; names(); break;          /* Inquiry Complete */
    case 0x02: if (e[0]) inquiry_result(e, 0); break;
    case 0x22: if (e[0]) inquiry_result(e, 1); break;
    case 0x2F: inquiry_result(e, 2); break;
    case 0x07: {                                          /* Remote Name Request Complete */
        btdev_t *d = dev_find(e + 1);
        if (d && !e[0]) { memcpy(d->d.name, e + 7, 31); d->d.name[31] = 0; }
        names();
        break;
    }
    case 0x03: conn_complete(e); break;
    case 0x04: {                                          /* Connection Request: bonded devices only */
        btdev_t *d = dev_find(e);
        if (bt.on && d && (d->d.flags & BTF_BONDED) && e[9] == 1) { uint8_t q[7]; memcpy(q, e, 6); q[6] = 0; cmd(0x0409, q, 7); }
        else { uint8_t q[7]; memcpy(q, e, 6); q[6] = 0x0F; cmd(0x040A, q, 7); }
        break;
    }
    case 0x05: if (!e[0]) disconnected(e); break;
    case 0x06:                                            /* Authentication Complete */
        if (!(c = conn_of(g16(e + 1)))) break;
        if (e[0]) {
            bt_log("bt: %s: authentication failed (%02x)\n", hex6(c->dev->d.addr), e[0]);
            if (c->dev == bt.pdev) pair_end(0, "authentication failed");
            bt_disconnect(c, 0x05);
        } else {
            uint8_t q[3] = { e[1], e[2], 1 };
            cmd(0x0413, q, 3);                            /* encryption on */
            if (c->dev == bt.pdev) pair_end(1, 0);
        }
        break;
    case 0x08:                                            /* Encryption Change */
        if (!(c = conn_of(g16(e + 1)))) break;
        c->enc = !e[0] && e[3];
        bt_log("bt: %s: encryption %s (%02x)\n", hex6(c->dev->d.addr), c->enc ? "on" : "off", e[0]);
        if (c->le && c->smp) smp_encrypted(c, c->enc);
        else if (c->le && c->enc) hogp_start(c);
        else if (c->le) { bt_log("bt: %s has lost our keys: forget it and pair again\n", hex6(c->dev->d.addr)); bt_disconnect(c, 0x05); }
        else if (c->enc && c->out) hidp_start(c);
        break;
    case 0x17: link_key(e, 0); break;
    case 0x18: link_key(e, 1); break;
    case 0x16: case 0x31: case 0x32: case 0x33: case 0x34: case 0x36: case 0x3B: ssp(p[0], e); break;
    case 0x3E: le_meta(e, len); break;
    case 0x10: bt_log("bt: the controller reports a hardware error (%02x)\n", e[0]); break;
    }
}

/* ---- Requests (btd.c, with the lock). */
void bt_ready(void)
{
    bt.state = BTS_ON;
    input_init();
    bt_power(bt.on);
}

void bt_tick(void)
{
    if (bt.scan_left && !--bt.scan_left) { uint8_t p[2] = { 0 }; cmd(0x200C, p, 2); }
    if (bt.pair_left && !--bt.pair_left && bt.pair != BTP_DONE && bt.pair != BTP_FAILED) {
        conn_t *c = conn_dev(bt.pdev);
        pair_end(0, "it took too long");
        if (c) bt_disconnect(c, 0x13);
    }
}

int bt_scan(void)
{
    if (bt.state != BTS_ON || !bt.on) return -EAGAIN;
    int k = 0;
    for (int i = 0; i < bt.ndev; i++)                     /* forget what the last scan found */
        if (bt.dev[i].d.flags & (BTF_BONDED | BTF_CONNECTED) || &bt.dev[i] == bt.pdev) {
            if (bt.pdev == &bt.dev[i]) bt.pdev = &bt.dev[k];
            for (int j = 0; j < MAXCONN; j++) if (bt.conn[j].dev == &bt.dev[i]) bt.conn[j].dev = &bt.dev[k];
            bt.dev[k++] = bt.dev[i];
        }
    bt.ndev = k;
    static const uint8_t inq[5] = { 0x33, 0x8B, 0x9E, 8, 0 };   /* GIAC, 10.24 s */
    static const uint8_t par[7] = { 1, 0x60, 0, 0x30, 0, 0, 0 }; /* active, every 60 ms for 30 */
    static const uint8_t on[2] = { 1, 1 };
    if (!bt.inquiring) cmd(0x0401, inq, 5);
    cmd(0x200B, par, 7);
    cmd(0x200C, on, 2);
    bt.inquiring = 1; bt.scan_left = 11;
    bt_log("bt: scanning\n");
    return 0;
}

int bt_pair(const bt_dev_t *want)
{
    btdev_t *d = dev_find(want->addr);
    if (bt.state != BTS_ON || !bt.on) return -EAGAIN;
    if (!d) return -ENOENT;
    if (bt.pair == BTP_BUSY || bt.pair == BTP_CONFIRM || bt.pair == BTP_PASSKEY) return -EBUSY;
    conn_t *c = conn_dev(d);
    if (c) bt_disconnect(c, 0x13);
    if (d->d.flags & BTF_BONDED) { d->d.flags &= ~BTF_BONDED; bonds_save(); }   /* pairing again: the old keys go */
    scan_stop();
    bt.pdev = d; bt.pair = BTP_BUSY; bt.pair_left = 60; bt.code = 0; bt.why[0] = 0;
    bt_log("bt: pairing with %s (%s)\n", hex6(d->d.addr), d->d.name);
    if (d->d.type == BTT_CLASSIC) {
        uint8_t p[13];
        memcpy(p, d->d.addr, 6); p16(p + 6, 0xCC18); p[8] = d->psrm; p[9] = 0; p16(p + 10, d->clkoff | 0x8000); p[12] = 1;
        cmd(0x0405, p, 13);
    } else if (bt.le_busy == 1) cmd(0x200E, 0, 0);       /* cancel the auto-connection; le_conn() goes on */
    else if (!bt.le_busy) { le_create(d); bt.le_busy = 2; }
    return 0;
}

int bt_confirm(int ok)
{
    btdev_t *d = bt.pdev;
    conn_t *c = d ? conn_dev(d) : 0;
    if (bt.pair != BTP_CONFIRM || !c) return -EINVAL;
    bt_log("bt: %s: the user says the code %s\n", hex6(d->d.addr), ok ? "matches" : "does not match");
    bt.pair = BTP_BUSY;
    if (c->le) return smp_confirm(c, ok);
    cmd(ok ? 0x042C : 0x042D, d->d.addr, 6);
    return 0;
}

int bt_forget(const bt_dev_t *want)
{
    btdev_t *d = dev_find(want->addr);
    if (!d || !(d->d.flags & BTF_BONDED)) return -ENOENT;
    conn_t *c = conn_dev(d);
    if (c) bt_disconnect(c, 0x13);
    d->d.flags &= ~BTF_BONDED;
    memset(d->key, 0, 16);
    bt_log("bt: %s forgotten\n", hex6(d->d.addr));
    bonds_save();
    return 0;
}

int bt_power(int on)
{
    uint8_t s = on ? 2 : 0;                               /* page scan: bonded classic devices may connect */
    bt.on = on;
    if (bt.state != BTS_ON) return 0;
    cmd(0x0C1A, &s, 1);
    if (on) le_auto();
    else {
        scan_stop();
        if (bt.le_busy) { cmd(0x200E, 0, 0); bt.le_busy = 0; }
        for (int i = 0; i < MAXCONN; i++) if (bt.conn[i].dev) bt_disconnect(&bt.conn[i], 0x15);
    }
    bt_log("bt: %s\n", on ? "on" : "off");
    return 0;
}

int bt_list(bt_dev_t *out)
{
    for (int i = 0; i < bt.ndev; i++) out[i] = bt.dev[i].d;
    return bt.ndev;
}
