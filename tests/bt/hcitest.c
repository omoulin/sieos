/*
 * hcitest.c - btd's stack (user/btd: hci.c, l2cap.c, smp.c, input.c, with
 * user/lib/hidrep.c) on the host, driven by scripted HCI traces through a
 * fake transport: what the stack sends is checked packet by packet, and
 * the test answers as the controller and the remote device would.
 *
 *   classic  scan (an extended inquiry result: a keyboard), pairing (SSP,
 *            passkey shown), the link key kept, encryption, the two HID
 *            channels opened and configured, boot protocol, a key typed and
 *            a mouse moved (CON_INJECT checked), flow control, disconnection
 *   LE       an advertisement (a mouse), LE connection, SMP Secure
 *            Connections (the test is the responder, with lib/btcrypt.c:
 *            confirm, nonces, DHKey checks, the LTK compared), encryption,
 *            HID over GATT (service, attributes, report map in pieces,
 *            report reference, notifications on), a notification moving
 *            the pointer; a bonded device reconnected by the accept list
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include "btd.h"

static int fails, checks;
static void check(const char *what, int ok)
{
    checks++;
    if (!ok) { fails++; printf("  FAIL %s\n", what); }
}

/* ---- The platform btd.c would give. */
static struct { int type, n; uint8_t b[300]; } sent[256];
static int nsent, taken, saves;
static con_inject_t inj[64];
static int ninj;
void tr_send(int type, const uint8_t *p, int n)
{
    if (nsent < 256) { sent[nsent].type = type; sent[nsent].n = n; memcpy(sent[nsent].b, p, n < 300 ? n : 300); nsent++; }
}
void bt_log(const char *f, ...) { va_list ap; va_start(ap, f); if (getenv("BTLOG")) vprintf(f, ap); va_end(ap); }
void bt_random(void *p, int n) { static uint8_t x = 7; for (int i = 0; i < n; i++) ((uint8_t *)p)[i] = x = (uint8_t)(x * 33 + 11); }
void bonds_save(void) { saves++; }
long call_named(long *port, const char *name, msg_t *m, int repeat)
{
    (void)port; (void)name; (void)repeat;
    for (size_t i = 0; m->w[0] == CON_INJECT && i < m->slen / sizeof(con_inject_t) && ninj < 64; i++) inj[ninj++] = ((const con_inject_t *)m->sbuf)[i];
    return 0;
}
long thread_start(void (*fn)(void *), void *arg, size_t stack) { (void)fn; (void)arg; (void)stack; return 99; }
long sys_clock(void) { return 0; }
long sys_sleep(uint64_t ns) { (void)ns; return 0; }
long sys_wake(int tid) { (void)tid; return 0; }
long sys_yield(void) { return 0; }

/* ---- Script helpers. */
static void evt(int code, const uint8_t *p, int n)
{
    uint8_t b[260] = { (uint8_t)code, (uint8_t)n };
    memcpy(b + 2, p, n);
    bt_rx(H4_EVT, b, n + 2);
}
#define EVT(code, ...) do { const uint8_t e_[] = { __VA_ARGS__ }; evt(code, e_, sizeof e_); } while (0)
static const uint8_t *next(int type, int *n)
{
    if (taken >= nsent || sent[taken].type != type) return 0;
    *n = sent[taken].n;
    return sent[taken++].b;
}
/* The next command must be op: -> its parameters. */
static const uint8_t *cmd_is(int op, const char *what)
{
    int n;
    const uint8_t *p = next(H4_CMD, &n);
    int ok = p && g16(p) == op;
    if (!ok) printf("  (wanted command %04x, got %s %04x)\n", op, p ? "command" : "nothing or data", p ? g16(p) : 0);
    check(what, ok);
    return ok ? p + 3 : 0;
}
/* The next packet must be an L2CAP frame on cid (one ACL packet): -> its payload. */
static const uint8_t *frame(int cid, int *len, const char *what)
{
    int n;
    const uint8_t *p = next(H4_ACL, &n);
    int ok = p && n >= 8 && g16(p + 6) == cid && g16(p + 4) == n - 8;
    if (!ok) printf("  (wanted a frame on channel %04x, got %s)\n", cid, p ? "another" : "nothing");
    check(what, ok);
    if (ok) *len = n - 8;
    if (p) { uint8_t e[5] = { 1, p[0], (uint8_t)(p[1] & 15), 1, 0 }; evt(0x13, e, 5); }   /* the controller sent it */
    return ok ? p + 8 : 0;
}
static void acl_in(int handle, int cid, const uint8_t *d, int n)
{
    uint8_t b[300];
    p16(b, handle | 2 << 12); p16(b + 2, n + 4); p16(b + 4, n); p16(b + 6, cid);
    memcpy(b + 8, d, n);
    bt_rx(H4_ACL, b, n + 8);
}
#define ACL(h, cid, ...) do { const uint8_t a_[] = { __VA_ARGS__ }; acl_in(h, cid, a_, sizeof a_); } while (0)
static int injected(int kind, int sc, int dx, int dy, int buttons)
{
    for (int i = 0; i < ninj; i++)
        if (inj[i].kind == kind && (kind != INJ_KEY || inj[i].sc == sc) && (kind != INJ_REL || (inj[i].dx == dx && inj[i].dy == dy && inj[i].buttons == buttons)))
            return 1;
    return 0;
}

static const uint8_t K[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 }, M[6] = { 0x11, 0x12, 0x13, 0x14, 0x15, 0xC6 };

static void classic(void)
{
    printf("classic: scan, pairing, HID\n");
    check("scan starts", !bt_scan());
    cmd_is(0x0401, "inquiry"); cmd_is(0x200B, "LE scan parameters"); cmd_is(0x200C, "LE scan on");
    uint8_t ir[255] = { 1 };                              /* an extended inquiry result */
    memcpy(ir + 1, K, 6); ir[7] = 1; ir[9] = 0x40; ir[10] = 0x05; ir[11] = 0x00; p16(ir + 12, 0x1234); ir[14] = (uint8_t)-50;
    memcpy(ir + 15, "\x0e\x09Keyboard K380", 15);
    evt(0x2F, ir, 255);
    bt_dev_t list[BT_MAXDEV];
    int n = bt_list(list);
    check("the keyboard is listed, with its name and kind", n == 1 && !strcmp(list[0].name, "Keyboard K380") && list[0].kind == BTK_KEYBOARD && list[0].rssi == -50);
    bt_dev_t want = { 0 };
    memcpy(want.addr, K, 6);
    check("pair", !bt_pair(&want));
    cmd_is(0x200C, "LE scan off"); cmd_is(0x0402, "inquiry cancelled");
    const uint8_t *p = cmd_is(0x0405, "Create Connection");
    check("  to the keyboard, with its clock offset", p && !memcmp(p, K, 6) && g16(p + 10) == (0x1234 | 0x8000));
    EVT(0x03, 0, 0x40, 0x00, 1, 2, 3, 4, 5, 6, 1, 0);
    cmd_is(0x0411, "Authentication Requested");
    EVT(0x17, 1, 2, 3, 4, 5, 6);
    cmd_is(0x040C, "no link key yet");
    EVT(0x31, 1, 2, 3, 4, 5, 6);
    p = cmd_is(0x042B, "IO capability reply");
    check("  display yes/no, MITM", p && p[6] == 1 && p[8] == 3);
    EVT(0x32, 1, 2, 3, 4, 5, 6, 2, 0, 3);                 /* it is a keyboard */
    EVT(0x3B, 1, 2, 3, 4, 5, 6, 0x40, 0xE2, 0x01, 0x00);  /* passkey 123456 */
    check("the passkey is shown", bt.pair == BTP_PASSKEY && bt.code == 123456);
    EVT(0x36, 0, 1, 2, 3, 4, 5, 6);
    uint8_t lk[23] = { 1, 2, 3, 4, 5, 6 };
    memset(lk + 6, 0xAB, 16); lk[22] = 5;
    evt(0x18, lk, 23);
    check("the link key is kept", saves == 1 && (bt.dev[0].d.flags & BTF_BONDED) && bt.dev[0].key[0] == 0xAB);
    EVT(0x06, 0, 0x40, 0x00);
    cmd_is(0x0413, "encryption on");
    check("paired", bt.pair == BTP_DONE);
    EVT(0x08, 0, 0x40, 0x00, 1);
    int len;
    p = frame(1, &len, "L2CAP connection request");
    check("  for HID control (0x11)", p && p[0] == 2 && g16(p + 4) == 0x11 && g16(p + 6) == 0x40);
    ACL(0x40, 1, 3, p ? p[1] : 0, 8, 0, 0x50, 0, 0x40, 0, 0, 0, 0, 0);   /* response: their channel 0x50 */
    p = frame(1, &len, "our configuration request");
    check("  with our MTU", p && p[0] == 4 && g16(p + 4) == 0x50 && p[8] == 1 && g16(p + 10) == L2_MTU);
    int id = p ? p[1] : 0;
    ACL(0x40, 1, 4, 9, 8, 0, 0x40, 0, 0, 0, 1, 2, 0xA0, 0x02);       /* theirs: MTU 672 */
    p = frame(1, &len, "configuration accepted");
    check("  (success)", p && p[0] == 5 && p[1] == 9 && g16(p + 8) == 0);
    ACL(0x40, 1, 5, (uint8_t)id, 6, 0, 0x40, 0, 0, 0, 0, 0);
    p = frame(1, &len, "then the interrupt channel (0x13)");
    check("  from channel 0x41", p && p[0] == 2 && g16(p + 4) == 0x13 && g16(p + 6) == 0x41);
    ACL(0x40, 1, 3, p ? p[1] : 0, 8, 0, 0x51, 0, 0x41, 0, 0, 0, 0, 0);
    p = frame(1, &len, "its configuration request");
    id = p ? p[1] : 0;
    ACL(0x40, 1, 4, 10, 4, 0, 0x41, 0, 0, 0);
    frame(1, &len, "configuration accepted");
    ACL(0x40, 1, 5, (uint8_t)id, 6, 0, 0x41, 0, 0, 0, 0, 0);
    p = frame(0x50, &len, "SET_PROTOCOL(boot) on the control channel");
    check("  (0x70)", p && len == 1 && p[0] == 0x70);
    check("the keyboard is in use", (bt.dev[0].d.flags & BTF_INPUT) && bt.conn[0].hid);
    ninj = 0;
    ACL(0x40, 0x41, 0xA1, 1, 0, 0, 4, 0, 0, 0, 0, 0);      /* 'a' down */
    ACL(0x40, 0x41, 0xA1, 1, 0, 0, 0, 0, 0, 0, 0, 0);      /* up */
    check("a key typed: 'a' pressed and released", injected(INJ_KEY, 0x1E, 0, 0, 0) && injected(INJ_KEY, 0x9E, 0, 0, 0));
    ACL(0x40, 0x41, 0xA1, 2, 1, 5, 0xFB, 0);               /* boot mouse report: left, +5, -5 */
    check("a mouse report: moved, left button", injected(INJ_REL, 0, 5, -5, MB_LEFT));
    int before = bt.acl_free;
    EVT(0x13, 1, 0x40, 0x00, 2, 0);
    check("flow control: completed packets give buffers back", bt.acl_free == before + 2);
    EVT(0x05, 0, 0x40, 0x00, 0x13);
    check("disconnected: no longer connected nor in use", !(bt.dev[0].d.flags & (BTF_CONNECTED | BTF_INPUT)) && !bt.conn[0].dev);
    EVT(0x04, 1, 2, 3, 4, 5, 6, 0x40, 0x05, 0, 1);         /* it comes back */
    p = cmd_is(0x0409, "a bonded device's connection is accepted");
    check("  as central", p && p[6] == 0);
    EVT(0x03, 0, 0x42, 0x00, 1, 2, 3, 4, 5, 6, 1, 0);
    cmd_is(0x0411, "Authentication Requested");
    EVT(0x17, 1, 2, 3, 4, 5, 6);
    p = cmd_is(0x040B, "its link key given");
    check("  the kept key", p && p[6] == 0xAB);
    EVT(0x05, 0, 0x42, 0x00, 0x13);
}

static void le(void)
{
    printf("LE: advertisement, SMP Secure Connections, HID over GATT\n");
    uint8_t adv[60] = { 0x02, 1, 0, 1 }, *a = adv + 4;    /* LE meta: one report, ADV_IND, random address */
    memcpy(a, M, 6); a += 6;
    static const uint8_t data[] = { 2, 1, 6, 3, 0x19, 0xC2, 0x03, 9, 9, 'M', 'X', ' ', 'M', 'o', 'u', 's', 'e' };
    *a++ = sizeof data; memcpy(a, data, sizeof data); a += sizeof data; *a++ = (uint8_t)-60;
    evt(0x3E, adv, (int)(a - adv));
    bt_dev_t list[BT_MAXDEV], want = { 0 };
    int n = bt_list(list), k = n - 1;
    check("the mouse is listed (LE, random address, its name and kind)", n >= 2 && list[k].type == BTT_LE_RANDOM && !strcmp(list[k].name, "MX Mouse") && list[k].kind == BTK_MOUSE);
    memcpy(want.addr, M, 6);
    check("pair", !bt_pair(&want));
    const uint8_t *p = cmd_is(0x200D, "LE Create Connection");
    check("  to the mouse", p && p[4] == 0 && p[5] == 1 && !memcmp(p + 6, M, 6));
    uint8_t lc[19] = { 0x01, 0, 0x41, 0x00, 0, 1 };
    memcpy(lc + 6, M, 6);
    evt(0x3E, lc, 19);
    int len;
    p = frame(6, &len, "SMP Pairing Request");
    uint8_t preq[7], pres[7] = { 0x02, 0x03, 0x00, 0x09, 16, 0x00, 0x01 };   /* the mouse: no IO, bonding, SC */
    if (p) memcpy(preq, p, 7);
    check("  bonding, MITM, Secure Connections; its LTK wanted", p && len == 7 && p[1] == 1 && p[3] == 0x0D && p[6] == 1);
    acl_in(0x41, 6, pres, 7);
    p = frame(6, &len, "our public key");
    uint8_t pka[64], pkb[64], privb[32], dh[32], nb[16], na[16], cb[16], mk[16], ltk[16], ea[16], eb[16], r0[16] = { 0 };
    uint8_t A[7], B[7];
    memcpy(A, bt.addr, 6); A[6] = 0; memcpy(B, M, 6); B[6] = 1;
    if (p) memcpy(pka, p + 1, 64);
    for (int i = 0; i < 32; i++) privb[i] = (uint8_t)(i * 7 + 1);
    check("  on the curve; the DHKey computed", p && len == 65 && !bt_p256_public(pkb, privb) && !bt_p256_dh(dh, privb, pka));
    uint8_t m[65] = { 0x0C };
    memcpy(m + 1, pkb, 64); acl_in(0x41, 6, m, 65);
    memset(nb, 0x5A, 16);
    bt_f4(cb, pkb, pka, nb, 0);
    m[0] = 0x03; memcpy(m + 1, cb, 16); acl_in(0x41, 6, m, 17);
    p = frame(6, &len, "our nonce (Pairing Random)");
    if (p) memcpy(na, p + 1, 16);
    m[0] = 0x04; memcpy(m + 1, nb, 16); acl_in(0x41, 6, m, 17);
    p = frame(6, &len, "our DHKey check");
    bt_f5(mk, ltk, dh, na, nb, A, B);
    bt_f6(ea, mk, na, nb, r0, preq + 1, A, B);
    check("  right (f5, f6)", p && p[0] == 0x0D && !memcmp(p + 1, ea, 16));
    bt_f6(eb, mk, nb, na, r0, pres + 1, B, A);
    m[0] = 0x0D; memcpy(m + 1, eb, 16); acl_in(0x41, 6, m, 17);
    p = cmd_is(0x2019, "LE Start Encryption");
    check("  with the LTK both sides computed", p && !memcmp(p + 12, ltk, 16));
    EVT(0x08, 0, 0x41, 0x00, 1);
    btdev_t *d = 0;
    for (int i = 0; i < bt.ndev; i++) if (!memcmp(bt.dev[i].d.addr, M, 6)) d = &bt.dev[i];
    check("bonded, the LTK kept", d && (d->d.flags & BTF_BONDED) && !memcmp(d->key, ltk, 16) && bt.pair == BTP_DONE);

    p = frame(4, &len, "ATT: find the HID service");
    check("  (Find By Type Value 2800 = 1812)", p && p[0] == 0x06 && g16(p + 5) == 0x2800 && g16(p + 7) == 0x1812);
    ACL(0x41, 4, 0x07, 0x10, 0, 0x20, 0);
    p = frame(4, &len, "ATT: Find Information over it");
    check("  0x10 to 0x20", p && p[0] == 0x04 && g16(p + 1) == 0x10 && g16(p + 3) == 0x20);
    ACL(0x41, 4, 0x05, 1, 0x11, 0, 0x03, 0x28, 0x12, 0, 0x4B, 0x2A, 0x13, 0, 0x03, 0x28, 0x14, 0, 0x4D, 0x2A,
        0x15, 0, 0x02, 0x29, 0x16, 0, 0x08, 0x29);
    p = frame(4, &len, "ATT: Find Information goes on");
    check("  from 0x17", p && g16(p + 1) == 0x17);
    ACL(0x41, 4, 0x01, 0x04, 0x17, 0, 0x0A);
    static const uint8_t map[] = { 0x05, 1, 0x09, 2, 0xA1, 1, 0x85, 2, 0x09, 1, 0xA1, 0, 0x05, 9, 0x19, 1, 0x29, 3, 0x15, 0, 0x25, 1,
        0x95, 3, 0x75, 1, 0x81, 2, 0x95, 1, 0x75, 5, 0x81, 1, 0x05, 1, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F,
        0x75, 8, 0x95, 2, 0x81, 6, 0xC0, 0xC0 };
    p = frame(4, &len, "ATT: read the report map");
    check("  (handle 0x12)", p && p[0] == 0x0A && g16(p + 1) == 0x12);
    for (int off = 0; off < (int)sizeof map; off += 22) {
        int k2 = (int)sizeof map - off < 22 ? (int)sizeof map - off : 22;
        m[0] = off ? 0x0D : 0x0B; memcpy(m + 1, map + off, k2); acl_in(0x41, 4, m, 1 + k2);
        if (k2 == 22) { p = frame(4, &len, "ATT: Read Blob for the rest"); check("  at the right offset", p && p[0] == 0x0C && g16(p + 3) == off + 22); }
    }
    p = frame(4, &len, "ATT: read the report reference");
    check("  (handle 0x16)", p && p[0] == 0x0A && g16(p + 1) == 0x16);
    ACL(0x41, 4, 0x0B, 2, 1);                              /* report 2, input */
    p = frame(4, &len, "ATT: notifications on");
    check("  (0x0001 into 0x15)", p && p[0] == 0x12 && g16(p + 1) == 0x15 && g16(p + 3) == 1);
    ACL(0x41, 4, 0x13);
    check("the mouse is in use", d && (d->d.flags & BTF_INPUT));
    ninj = 0;
    ACL(0x41, 4, 0x1B, 0x14, 0, 1, 3, 0xFD);
    check("a notification moves the pointer", injected(INJ_REL, 0, 3, -3, MB_LEFT));
    ACL(0x41, 4, 0x02, 0x40, 0);                          /* the mouse asks our MTU */
    p = frame(4, &len, "our MTU told");
    check("  23", p && p[0] == 0x03 && g16(p + 1) == 23);
    ACL(0x41, 5, 0x12, 1, 8, 0, 6, 0, 12, 0, 0, 0, 0x90, 1);  /* connection parameters */
    frame(5, &len, "parameter update accepted");
    cmd_is(0x2013, "LE Connection Update");

    EVT(0x05, 0, 0x41, 0x00, 0x08);
    cmd_is(0x2010, "lost: the accept list cleared");
    p = cmd_is(0x2011, "  the mouse added");
    check("  (random, its address)", p && p[0] == 1 && !memcmp(p + 1, M, 6));
    p = cmd_is(0x200D, "  and connected when it advertises");
    check("  (accept list)", p && p[4] == 1);
    lc[2] = 0x43;
    evt(0x3E, lc, 19);
    p = cmd_is(0x2019, "back: encryption with the kept LTK");
    check("  the LTK", p && !memcmp(p + 12, ltk, 16));
}

int main(void)
{
    memcpy(bt.addr, (const uint8_t[6]){ 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6 }, 6);
    bt.acl_mtu = 1021; bt.acl_free = 8; bt.le_mtu = 251; bt.le_free = 8; bt.ncmd = 1000; bt.on = 1;
    bt_ready();
    cmd_is(0x0C1A, "page scan on");
    classic();
    le();
    printf(fails ? "bt stack: %d of %d checks FAILED\n" : "bt stack: all %d checks passed\n", fails ? fails : checks, checks);
    return fails != 0;
}
