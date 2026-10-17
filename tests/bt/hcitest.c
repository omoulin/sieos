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
 *   A2DP     bt connect to bonded headphones: SDP for their sink (with a
 *            continuation), AVDTP discover (an AAC endpoint skipped for the
 *            SBC one), capabilities, configuration (bitpool), open, the media
 *            channel; sound put in, start, packets paced by the media clock:
 *            RTP headers, SBC frame headers and CRCs, the frames decoded (a
 *            float decoder here, and the host's libsbc if there) and compared
 *            to the input (SNR); suspend when the sound stops
 *   AVRCP    we open the control channel; absolute volume (registered, the
 *            interim answer, SetAbsoluteVolume, the headset's change to
 *            audiod and registered again); UNIT INFO, GetCapabilities; the
 *            buttons (pass through) to audiod, pause suspending the stream
 *   SDP      a phone asks our records: Object Push's channel, a cut answer
 *            and its continuation, Service Search, one attribute
 *   OBEX     a phone pushes a file (RFCOMM: multiplexer, PN, DLC, MSC,
 *            credits; OBEX connect, put in two packets; the user asked, the
 *            answer held until accepted): the file checked; two more
 *            declined (by the user; no answer in 30 s);
 *            then we send one to it (SDP finds its channel, RFCOMM with few
 *            credits, PUTs as big as it takes): every byte checked
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <math.h>
#include <dlfcn.h>
#include "btd.h"

static int fails, checks;
static void check(const char *what, int ok)
{
    checks++;
    if (!ok) { fails++; printf("  FAIL %s\n", what); }
}

/* ---- The platform btd.c would give. */
static struct { int type, n; uint8_t b[1100]; } sent[4096];
static int nsent, taken, saves;
static con_inject_t inj[64];
static int ninj;
void tr_send(int type, const uint8_t *p, int n)
{
    if (nsent < 4096) { sent[nsent].type = type; sent[nsent].n = n; memcpy(sent[nsent].b, p, n < 1100 ? n : 1100); nsent++; }
}
void bt_log(const char *f, ...) { va_list ap; va_start(ap, f); if (getenv("BTLOG")) vprintf(f, ap); va_end(ap); }
void bt_random(void *p, int n) { static uint8_t x = 7; for (int i = 0; i < n; i++) ((uint8_t *)p)[i] = x = (uint8_t)(x * 33 + 11); }
void bonds_save(void) { saves++; }
static char fname[64];                                    /* the file received */
static uint8_t fdata[4096];
static int fn, fstate;                                    /* 1 open, 2 complete, 3 broken off */
static char asked[64];                                    /* the file the user is asked about */
static int nasked, keys[16], nkeys, avol = -1, aout;      /* (to audiod: the headset's keys, its volume; headphones ready) */
void file_ask(const btdev_t *from, const char *name, uint32_t size) { (void)from; (void)size; strlcpy(asked, name, sizeof asked); nasked++; }
int file_open(const btdev_t *from, const char *name, uint32_t size) { (void)from; (void)size; strlcpy(fname, name, sizeof fname); fn = 0; fstate = 1; return 0; }
int file_write(const void *p, int n) { if (fn + n > (int)sizeof fdata) return -1; memcpy(fdata + fn, p, n); fn += n; return n; }
void file_close(int ok) { fstate = ok > 0 ? 2 : ok ? 4 : 3; }
void to_audiod(int op, int arg) { if (op == AUDIO_VOL) avol = arg; else if (op == AUDIO_OUT) aout = arg == AUDIO_BT; else if (nkeys < 16) keys[nkeys++] = arg; }
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
    uint8_t b[1100];
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

/* ---- Stage 2: channels, as the peer sees them. */
static void link_up(int h, const uint8_t *a, int incoming)   /* a bonded classic device's link, up to encryption */
{
    if (incoming) {
        uint8_t q[10] = { 0 };
        memcpy(q, a, 6); q[6] = 0x0C; q[7] = 0x02; q[8] = 0x5A; q[9] = 1;
        evt(0x04, q, 10);
        cmd_is(0x0409, "its connection accepted");
    } else cmd_is(0x0405, "Create Connection");
    uint8_t c[11] = { 0, (uint8_t)h, (uint8_t)(h >> 8) };
    memcpy(c + 3, a, 6); c[9] = 1;
    evt(0x03, c, 11);
    cmd_is(0x0411, "Authentication Requested");
    evt(0x17, a, 6);
    cmd_is(0x040B, "its link key given");
    EVT(0x06, 0, (uint8_t)h, (uint8_t)(h >> 8));
    cmd_is(0x0413, "encryption on");
    EVT(0x08, 0, (uint8_t)h, (uint8_t)(h >> 8), 1);
}
static int open_out(int h, int psm, int rcid, int mtu)     /* we open a channel -> our cid */
{
    int len, lcid, id;
    const uint8_t *p = frame(1, &len, "L2CAP connection request");
    check("  for the PSM", p && p[0] == 2 && g16(p + 4) == psm);
    lcid = p ? g16(p + 6) : 0;
    uint8_t r[12] = { 3, p ? p[1] : 0, 8, 0 };
    p16(r + 4, rcid); p16(r + 6, lcid); acl_in(h, 1, r, 12);
    p = frame(1, &len, "our configuration request");
    id = p ? p[1] : 0;
    uint8_t c[12] = { 4, 0x33, 8, 0 };
    p16(c + 4, lcid); c[8] = 1; c[9] = 2; p16(c + 10, mtu); acl_in(h, 1, c, 12);
    frame(1, &len, "its configuration accepted");
    uint8_t a[10] = { 5, (uint8_t)id, 6, 0 };
    p16(a + 4, lcid); acl_in(h, 1, a, 10);
    return lcid;
}
static int open_in(int h, int psm, int rcid, int mtu)      /* the peer opens one -> our cid */
{
    int len, lcid, id;
    uint8_t r[8] = { 2, 0x21, 4, 0 };
    p16(r + 4, psm); p16(r + 6, rcid); acl_in(h, 1, r, 8);
    const uint8_t *p = frame(1, &len, "L2CAP connection response");
    check("  accepted", p && p[0] == 3 && g16(p + 8) == 0);
    lcid = p ? g16(p + 4) : 0;
    p = frame(1, &len, "our configuration request");
    id = p ? p[1] : 0;
    uint8_t c[12] = { 4, 0x34, 8, 0 };
    p16(c + 4, lcid); c[8] = 1; c[9] = 2; p16(c + 10, mtu); acl_in(h, 1, c, 12);
    frame(1, &len, "its configuration accepted");
    uint8_t a[10] = { 5, (uint8_t)id, 6, 0 };
    p16(a + 4, lcid); acl_in(h, 1, a, 10);
    return lcid;
}
static void closed_by_us(int h, const char *what)          /* our Disconnection Request, answered */
{
    int len;
    const uint8_t *p = frame(1, &len, what);
    check("  (disconnection request)", p && p[0] == 6);
    if (p) { uint8_t r[8] = { 7, p[1], 4, 0, p[4], p[5], p[6], p[7] }; acl_in(h, 1, r, 8); }
}

/* ---- SBC, checked from the outside: the CRC, and a float decoder (joint
 * stereo, 8 subbands, 16 blocks, loudness: what btd sends). */
static int crc_ok(const uint8_t *f)
{
    uint8_t c = 0x0F, d[11] = { f[1], f[2] };
    memcpy(d + 2, f + 4, 9);
    for (int i = 0; i < 88; i++) { int bit = d[i >> 3] >> (7 - (i & 7)) & 1; c = (uint8_t)(((c >> 7) ^ bit) ? (c << 1) ^ 0x1D : c << 1); }
    return c == f[3];
}
static const int32_t PROTO[41] = {                        /* the spec's prototype filter, Q20 */
    0, 164, 360, 582, 864, 1195, 1548, 1870, 2110, 2206, 2091, 1695, 946, -187, -1730, -3667, -5934, -8419,
    -10966, -13366, -15364, -16677, -17009, -16063, -13566, -9288, -3066, 5155, 15352, 27378, 40973, 55771,
    71302, 87016, 102315, 116598, 129252, 139738, 147591, 152452, 154094 };
static int sbc_dec(const uint8_t *f, int n, int16_t *pcm)  /* one frame -> its length (-1: not ours) */
{
    static double V[2][160];
    static const int off[8] = { -4, 0, 0, 0, 0, 0, 1, 2 };
    if (n < 13 || f[0] != 0x9C || f[1] != 0xFD) return -1;
    int bp = f[2], len = 13 + 2 * bp, sf[2][8], need[2][8], bits[2][8], pos = 13 * 8, maxn = -99;
    if (n < len) return -1;
    for (int ch = 0; ch < 2; ch++) for (int k = 0; k < 8; k++) sf[ch][k] = f[5 + ch * 4 + k / 2] >> (k & 1 ? 0 : 4) & 15;
    for (int ch = 0; ch < 2; ch++) for (int k = 0; k < 8; k++) {
        int l = sf[ch][k] - off[k];
        need[ch][k] = !sf[ch][k] ? -5 : l > 0 ? l / 2 : l;
        if (need[ch][k] > maxn) maxn = need[ch][k];
    }
    int slice = maxn + 1, count = 0, sc = 0;
    do {
        slice--; count += sc; sc = 0;
        for (int ch = 0; ch < 2; ch++) for (int k = 0; k < 8; k++)
            if (need[ch][k] > slice + 1 && need[ch][k] < slice + 16) sc++; else if (need[ch][k] == slice + 1) sc += 2;
    } while (count + sc < bp);
    if (count + sc == bp) { count += sc; slice--; }
    for (int ch = 0; ch < 2; ch++) for (int k = 0; k < 8; k++) bits[ch][k] = need[ch][k] < slice + 2 ? 0 : need[ch][k] - slice < 16 ? need[ch][k] - slice : 16;
    for (int ch = 0, k = 0; count < bp && k < 8; ) {
        if (bits[ch][k] >= 2 && bits[ch][k] < 16) { bits[ch][k]++; count++; }
        else if (need[ch][k] == slice + 1 && bp > count + 1) { bits[ch][k] = 2; count += 2; }
        if (ch) { ch = 0; k++; } else ch = 1;
    }
    for (int ch = 0, k = 0; count < bp && k < 8; ) {
        if (bits[ch][k] < 16) { bits[ch][k]++; count++; }
        if (ch) { ch = 0; k++; } else ch = 1;
    }
    for (int b = 0; b < 16; b++) {
        double s[2][8];
        for (int ch = 0; ch < 2; ch++) for (int k = 0; k < 8; k++) {
            int w = bits[ch][k], q = 0;
            for (int i = 0; i < w; i++, pos++) q = q << 1 | (f[pos >> 3] >> (7 - (pos & 7)) & 1);
            s[ch][k] = w ? ldexp(1, sf[ch][k] + 1) * ((2.0 * q + 1) / ((1 << w) - 1) - 1) : 0;
        }
        for (int k = 0; k < 7; k++) if (f[4] & 0x80 >> k) { double m = s[0][k], d = s[1][k]; s[0][k] = m + d; s[1][k] = m - d; }
        for (int ch = 0; ch < 2; ch++) {
            double *v = V[ch], u[80];
            memmove(v + 16, v, 144 * sizeof *v);
            for (int k = 0; k < 16; k++) { v[k] = 0; for (int i = 0; i < 8; i++) v[k] += cos((i + 0.5) * (k + 4) * M_PI / 8) * s[ch][i]; }
            for (int i = 0; i < 5; i++) for (int j = 0; j < 8; j++) { u[i * 16 + j] = v[i * 32 + j]; u[i * 16 + j + 8] = v[i * 32 + j + 24]; }
            for (int j = 0; j < 8; j++) {
                double x = 0;
                for (int i = 0; i < 10; i++) { int t = j + 8 * i; x += u[t] * PROTO[t <= 40 ? t : 80 - t] / 1048576.0 * (t >> 4 & 1 ? 8 : -8); }
                long y = lround(x);
                pcm[(b * 8 + j) * 2 + ch] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
            }
        }
    }
    return len;
}
static double snr(const int16_t *in, const int16_t *out, int n)   /* both channels, the codec's delay found */
{
    int best = 0;
    double bs = -1e30, sig = 0, err = 0;
    for (int d = 0; d < 300; d++) {
        double c = 0;
        for (int i = 2000; i < n - 300; i++) c += (double)in[i] * out[i + 2 * d];   /* (both channels: one delay) */
        if (c > bs) { bs = c; best = d; }
    }
    for (int i = 1000; i < n - 300; i++)
        for (int ch = 0; ch < 2; ch++) { double a = in[2 * i + ch], b = out[2 * (i + best) + ch]; sig += a * a; err += (a - b) * (a - b); }
    return 10 * log10(sig / (err + 1e-9));
}

static const uint8_t HP[6] = { 0x21, 0x22, 0x23, 0x24, 0x25, 0x26 }, PH[6] = { 0x31, 0x32, 0x33, 0x34, 0x35, 0x36 };
static btdev_t *bonded(const uint8_t *a, int kind, const char *name)
{
    btdev_t *d = dev_add(a, BTT_CLASSIC);
    d->d.flags |= BTF_BONDED; d->d.kind = (uint8_t)kind;
    strlcpy(d->d.name, name, sizeof d->d.name);
    memset(d->key, 0x77, 16);
    return d;
}
static int avdtp(int sig, int *label, int id)              /* the next AVDTP command must be id -> its index in sent[] */
{
    int len;
    const uint8_t *p = frame(sig, &len, "an AVDTP command");
    int ok = p && (p[0] & 15) == 0 && p[1] == id;
    if (!ok) printf("  (wanted AVDTP signal %d, got %d)\n", id, p ? p[1] : -1);
    check("  the right one", ok);
    if (p) *label = p[0] >> 4;
    return taken - 1;
}

static int remote(int h);
static void buttons(int h, int c, int sig);
static void audio(void)
{
    printf("A2DP: headphones: SDP, AVDTP, SBC media; AVRCP\n");
    const int h = 0x50;
    int rc;
    bonded(HP, BTK_AUDIO, "Headphones");
    bt_dev_t want = { 0 };
    memcpy(want.addr, HP, 6);
    check("bt connect", !bt_connect(&want, 1));
    link_up(h, HP, 0);
    int len, label, sdp = open_out(h, 1, 0x70, 672);
    const uint8_t *p = frame(0x70, &len, "SDP: Service Search Attribute request");
    check("  for an audio sink (110B), every attribute", p && p[0] == 0x06 && g16(p + 8) == 0x0B11 && len == 20 && p[19] == 0);
    static const uint8_t rec[] = { 0x35, 47, 0x35, 45,     /* an audio sink's record */
        0x09, 0, 0, 0x0A, 0, 1, 0, 5, 0x09, 0, 1, 0x35, 3, 0x19, 0x11, 0x0B,
        0x09, 0, 4, 0x35, 16, 0x35, 6, 0x19, 1, 0, 0x09, 0, 0x19, 0x35, 6, 0x19, 0, 0x19, 0x09, 1, 3,
        0x09, 0, 9, 0x35, 8, 0x35, 6, 0x19, 0x11, 0x0D, 0x09, 1, 3 };
    uint8_t r[80] = { 0x07, p ? p[1] : 0, p ? p[2] : 0, 0, 25, 0, 20 };
    memcpy(r + 7, rec, 20); r[27] = 2; r[28] = 0xAB; r[29] = 0xCD;
    acl_in(h, sdp, r, 30);                                  /* the first 20 bytes, and a continuation */
    p = frame(0x70, &len, "SDP: the request again, with the continuation");
    check("  (its 2 bytes)", p && len == 22 && p[19] == 2 && p[20] == 0xAB && p[21] == 0xCD);
    r[1] = p ? p[1] : 0; r[2] = p ? p[2] : 0; r[3] = 0; r[4] = (uint8_t)(sizeof rec - 20 + 3); r[5] = 0; r[6] = (uint8_t)(sizeof rec - 20);
    memcpy(r + 7, rec + 20, sizeof rec - 20); r[7 + sizeof rec - 20] = 0;
    acl_in(h, sdp, r, 8 + sizeof rec - 20);
    closed_by_us(h, "SDP done: its channel closed");
    int sig = open_out(h, 0x19, 0x71, 672);
    avdtp(0x71, &label, 0x01);                              /* Discover */
    uint8_t d[8] = { (uint8_t)(label << 4 | 2), 1, 1 << 2, 0x08, 2 << 2, 0x08, 3 << 2 | 2, 0x08 };   /* sinks 1, 2; 3 in use */
    acl_in(h, sig, d, 8);
    int k = avdtp(0x71, &label, 0x02);                      /* Get Capabilities of 1 */
    check("  of endpoint 1", sent[k].b[10] == 1 << 2);
    uint8_t aac[12] = { (uint8_t)(label << 4 | 2), 2, 1, 0, 7, 8, 0, 2, 0x80, 0x01, 0x8C, 0x00 };
    acl_in(h, sig, aac, 12);
    k = avdtp(0x71, &label, 0x02);
    check("  AAC skipped: endpoint 2", sent[k].b[10] == 2 << 2);
    uint8_t sbc[12] = { (uint8_t)(label << 4 | 2), 2, 1, 0, 7, 6, 0, 0, 0xFF, 0xFF, 2, 35 };
    acl_in(h, sig, sbc, 12);
    k = avdtp(0x71, &label, 0x03);                          /* Set Configuration */
    static const uint8_t conf[12] = { 2 << 2, 1 << 2, 1, 0, 7, 6, 0, 0, 0x11, 0x15, 2, 35 };
    check("  SBC 48 kHz joint stereo, 16 blocks, 8 subbands, loudness, bitpool 35", sent[k].n == 8 + 2 + 12 && !memcmp(sent[k].b + 10, conf, 12));
    uint8_t ok[2] = { (uint8_t)(label << 4 | 2), 3 };
    acl_in(h, sig, ok, 2);
    avdtp(0x71, &label, 0x06);                              /* Open */
    ok[0] = (uint8_t)(label << 4 | 2); ok[1] = 6;
    acl_in(h, sig, ok, 2);
    aout = 0;
    open_out(h, 0x19, 0x72, 672);
    check("the media channel: headphones ready (audiod told)", a2dp_sink() && !strcmp(a2dp_sink(), "Headphones") && aout);
    check("  in use", dev_find(HP)->d.flags & BTF_AUDIO);
    rc = remote(h);

    static int16_t in[2 * 9000], out[2 * 9000];
    for (int i = 0; i < 9000; i++) { in[2 * i] = (int16_t)(10000 * sin(2 * M_PI * 1000 * i / 48000.0)); in[2 * i + 1] = (int16_t)(7000 * sin(2 * M_PI * 3100 * i / 48000.0)); }
    int fill = 9 * 7 * 512;                                 /* 9 packets of 7 frames */
    bt.acl_free += 8;                                       /* (the controller's buffers: 9 packets unread) */
    memcpy(pcm_ring + pcm_w % PCM_RING, in, fill);
    check("sound put in", a2dp_put(pcm_w, fill, 100) == fill);
    avdtp(0x71, &label, 0x07);                              /* Start */
    check("  a2dp_pump: nothing before the sink starts", a2dp_pump(1000000000) == 0 && taken == nsent);
    ok[0] = (uint8_t)(label << 4 | 2); ok[1] = 7;
    acl_in(h, sig, ok, 2);
    int64_t t = 1000000000, per = 7 * 128 * 1000000000LL / 48000, w = a2dp_pump(t);
    check("streaming: one packet now, the next one 18.7 ms later", w == per && nsent - taken == 1);
    check("  not before", a2dp_pump(t + per - 1) == 1 && nsent - taken == 1);
    a2dp_pump(t + per);
    a2dp_pump(t + 200000000);                               /* (late: from now on) */
    for (int i = 0; i < 6; i++) a2dp_pump(t + 200000000 + (i + 1) * per);
    int np = 0, frames = 0, hdr = 1, crc = 1, seq = 1, o = 0, sn = 0;
    static uint8_t stream[9 * 7 * 83];
    for (const uint8_t *m; taken < nsent && (m = frame(0x72, &len, "a media packet")); np++) {
        if (m[0] != 0x80 || m[1] != 96 || m[12] != 7 || g16(m + 8) || (m[2] << 8 | m[3]) != np || (uint32_t)(m[4] << 24 | m[5] << 16 | m[6] << 8 | m[7]) != (uint32_t)np * 7 * 128) seq = 0;
        for (int i = 13; i + 83 <= len; i += 83, frames++) {
            if (m[i] != 0x9C || m[i + 1] != 0xFD || m[i + 2] != 35) hdr = 0;
            if (!crc_ok(m + i)) crc = 0;
            if (sbc_dec(m + i, len - i, out + o) == 83) o += 256;
            memcpy(stream + sn, m + i, 83); sn += 83;
        }
    }
    check("9 packets of 7 frames (594 bytes), all the sound", np == 9 && frames == 63 && pcm_w == pcm_r);
    check("  RTP: version 2, type 96, sequence numbers and timestamps (samples)", seq);
    check("  SBC headers: 48 kHz, 16 blocks, joint stereo, loudness, 8 subbands, bitpool 35", hdr);
    check("  their CRCs", crc);
    double q = snr(in, out, frames * 128);
    printf("  SNR of the decoded sound: %.1f dB\n", q);
    check("  decoded: the sound (SNR above 35 dB)", q > 35);
    void *lib = dlopen("libsbc.so.1", RTLD_NOW);
    if (lib) {                                              /* the host's libsbc, if there */
        struct { unsigned long flags; uint8_t f, b, s, m, a, bp, e; void *priv, *pab; } st;
        int (*init)(void *, unsigned long) = (int (*)(void *, unsigned long))dlsym(lib, "sbc_init");
        long (*dec)(void *, const void *, size_t, void *, size_t, size_t *) = (long (*)(void *, const void *, size_t, void *, size_t, size_t *))dlsym(lib, "sbc_decode");
        size_t at = 0, wr, on = 0;
        memset(out, 0, sizeof out);
        if (init && dec && !init(&st, 0))
            for (long rr; at < (size_t)sn && (rr = dec(&st, stream + at, sn - at, (uint8_t *)out + on, sizeof out - on, &wr)) > 0; at += rr) on += wr;
        q = snr(in, out, (int)(on / 4));
        printf("  SNR decoded by libsbc: %.1f dB\n", q);
        check("  libsbc decodes it too", at == (size_t)sn && q > 35);
    } else printf("  (no libsbc on this host: its check skipped)\n");
    for (int i = 0; i < 5; i++) bt_tick();
    avdtp(0x71, &label, 0x09);                              /* Suspend */
    check("  suspended after 5 s without sound", 1);
    ok[0] = (uint8_t)(label << 4 | 2); ok[1] = 9;
    acl_in(h, sig, ok, 2);
    buttons(h, rc, sig);
    EVT(0x05, 0, (uint8_t)h, 0, 0x13);
    check("disconnected: no headphones", !a2dp_sink() && a2dp_put(pcm_w, 4, 100) == -ENOENT);
}

/* ---- AVRCP, the headset's side: our frames on its channel 0x73. */
static const uint8_t *avc(int ctype, int op, int pdu, int *len, const char *what)   /* ours: AVCTP (110E), AV/C ctype, opcode, PDU */
{
    const uint8_t *p = frame(0x73, len, what);
    int ok = p && *len >= 8 && p[1] == 0x11 && p[2] == 0x0E && p[3] == ctype && p[5] == op &&
             (op || (*len >= 13 && !memcmp(p + 6, "\0\x19\x58", 3) && p[9] == pdu && *len == 13 + p[12]));
    if (!ok) printf("  (wanted ctype %x, opcode %x, PDU %x; got %d bytes: %x %x %x)\n", ctype, op, pdu, p ? *len : 0, p ? p[3] : 0, p ? p[5] : 0, p && *len > 9 ? p[9] : 0);
    check("  its AVCTP and AV/C headers", ok);
    return ok ? p : 0;
}
static int remote(int h)                                    /* -> our cid */
{
    int len;
    const uint8_t *p;
    bt_tick();
    check("AVRCP: no control channel yet: we wait", taken == nsent);
    bt_tick();
    int c = open_out(h, 0x17, 0x73, 672);                   /* (2 s: we open it) */
    p = avc(3, 0, 0x31, &len, "AVRCP: register for volume changes (NOTIFY)");
    check("  event 0D, a command", p && p[13] == 0x0D && !(p[0] & 2));
    ACL(h, c, 0x12, 0x11, 0x0E, 0x0F, 0x48, 0, 0, 0x19, 0x58, 0x31, 0, 0, 2, 0x0D, 0x40);   /* INTERIM: 0x40 */
    check("  interim: absolute volume (SIEOS's not known yet: nothing sent)", taken == nsent && avrcp_vol(50) == 1);
    p = avc(0, 0, 0x50, &len, "SetAbsoluteVolume (CONTROL)");
    check("  50% is 0x40", p && p[13] == 0x40);
    check("  the same volume again: nothing sent", avrcp_vol(50) == 1 && taken == nsent);
    ACL(h, c, 0x22, 0x11, 0x0E, 0x09, 0x48, 0, 0, 0x19, 0x58, 0x50, 0, 0, 1, 0x40);   /* (accepted) */
    ACL(h, c, 0x12, 0x11, 0x0E, 0x0D, 0x48, 0, 0, 0x19, 0x58, 0x31, 0, 0, 2, 0x0D, 0x20);   /* CHANGED: its buttons, 0x20 */
    check("the headset's volume: 0x20 is 25% for audiod", avol == 25);
    avc(3, 0, 0x31, &len, "  registered again");
    check("  audiod's echo: nothing sent", avrcp_vol(25) == 1 && taken == nsent);
    avrcp_vol(100);
    p = avc(0, 0, 0x50, &len, "SetAbsoluteVolume 100%");
    check("  0x7F", p && p[13] == 0x7F);
    ACL(h, c, 0x30, 0x11, 0x0E, 0x01, 0xFF, 0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);   /* UNIT INFO */
    p = avc(0x0C, 0x30, 0, &len, "UNIT INFO answered");
    check("  a panel, label kept", p && len == 11 && p[0] == 0x32 && p[6] == 0x07 && p[7] == 0x48);
    ACL(h, c, 0x40, 0x11, 0x0E, 0x01, 0x48, 0, 0, 0x19, 0x58, 0x10, 0, 0, 1, 3);   /* GetCapabilities: events */
    p = avc(0x0C, 0, 0x10, &len, "GetCapabilities (events) answered");
    check("  none", p && p[13] == 3 && p[14] == 0);
    ACL(h, c, 0x50, 0x11, 0x0E, 0x01, 0x48, 0, 0, 0x19, 0x58, 0x20, 0, 0, 1, 0);   /* GetElementAttributes: not ours */
    avc(0x0A, 0, 0x20, &len, "another PDU: rejected");
    ACL(h, c, 0x60, 0x11, 0x0F, 0x00, 0x48, 0x7C, 0x44, 0);  /* (another profile) */
    p = frame(0x73, &len, "another profile: invalid");
    check("  (IPID, header only)", p && len == 3 && p[0] == 0x63);
    return c;
}
static void press(int h, int c, int id, int up)
{
    uint8_t b[8] = { 0x70, 0x11, 0x0E, 0x00, 0x48, 0x7C, (uint8_t)(id | up << 7), 0 };
    acl_in(h, c, b, 8);
}
static void buttons(int h, int c, int sig)
{
    int len, label;
    const uint8_t *p;
    nkeys = 0;
    press(h, c, MK_PLAY, 0);
    p = avc(0x09, 0x7C, 0, &len, "pass through: play accepted");
    check("  echoed", p && len == 8 && p[6] == MK_PLAY);
    press(h, c, MK_PLAY, 1);
    avc(0x09, 0x7C, 0, &len, "  released");
    press(h, c, MK_NEXT, 0); avc(0x09, 0x7C, 0, &len, "next");
    press(h, c, MK_VOLUP, 0); avc(0x09, 0x7C, 0, &len, "volume up");
    press(h, c, 0x47, 0); avc(0x08, 0x7C, 0, &len, "record: not implemented");
    check("audiod given play, next, volume up (once each)", nkeys == 3 && keys[0] == MK_PLAY && keys[1] == MK_NEXT && keys[2] == MK_VOLUP);
    check("more sound", a2dp_put(pcm_w, 7 * 512, 100) == 7 * 512);
    avdtp(0x71, &label, 0x07);                              /* Start */
    uint8_t ok[2] = { (uint8_t)(label << 4 | 2), 7 };
    acl_in(h, sig, ok, 2);
    press(h, c, MK_PAUSE, 0);
    avdtp(0x71, &label, 0x09);
    check("pause: the stream suspended at once", 1);
    avc(0x09, 0x7C, 0, &len, "  accepted");
    check("  audiod given pause", nkeys == 4 && keys[3] == MK_PAUSE);
    ok[0] = (uint8_t)(label << 4 | 2); ok[1] = 9;
    acl_in(h, sig, ok, 2);
}

/* RFCOMM, the phone's side. */
static uint8_t fcs8(const uint8_t *p, int n)
{
    uint8_t c = 0xFF;
    while (n--) { c ^= *p++; for (int b = 0; b < 8; b++) c = (uint8_t)(c & 1 ? (c >> 1) ^ 0xE0 : c >> 1); }
    return (uint8_t)(0xFF - c);
}
static void rf_in(int h, int cid, int cr, int dlci, int ctl, const uint8_t *d, int n, int credits)
{
    uint8_t b[1100];
    int k = 3;
    b[0] = (uint8_t)(1 | cr << 1 | dlci << 2); b[1] = (uint8_t)(ctl | (credits >= 0 ? 0x10 : 0));
    b[2] = (uint8_t)(n << 1 | (n < 128));
    if (n >= 128) b[k++] = (uint8_t)(n >> 7);
    if (credits >= 0) b[k++] = (uint8_t)credits;
    if (n) memcpy(b + k, d, n);
    b[k + n] = fcs8(b, (ctl & 0xEF) == 0xEF ? 2 : 3);
    acl_in(h, cid, b, k + n + 1);
}
/* Our next RFCOMM frame (on the peer's channel rcid) -> its data; dlci, control, credits (-1: none). */
static const uint8_t *rf_out(int rcid, int *dlci, int *ctl, int *n, int *credits, const char *what)
{
    int len;
    const uint8_t *p = frame(rcid, &len, what);
    if (!p || len < 4) return 0;
    int k = p[2] & 1 ? 3 : 4, l = p[2] >> 1 | (k == 4 ? p[3] << 7 : 0);
    *dlci = p[0] >> 2; *ctl = p[1] & 0xEF; *credits = *ctl == 0xEF && p[1] & 0x10 && *dlci ? p[k++] : -1;
    *n = l;
    check("  its length and FCS", k + l + 1 == len && p[k + l] == fcs8(p, *ctl == 0xEF ? 2 : 3));
    return p + k;
}
static void obex_in(int h, int rf)                          /* phone -> us */
{
    int dl, ctl, n, cr;
    const uint8_t *p;
    rf_in(h, rf, 1, 0, 0x2F, 0, 0, -1);                     /* SABM 0 (the phone opens the multiplexer) */
    p = rf_out(0x61, &dl, &ctl, &n, &cr, "RFCOMM: UA for the multiplexer");
    check("  UA on DLCI 0", p && dl == 0 && ctl == 0x63);
    uint8_t pn[10] = { 0x83, 17, 18, 0xF0, 0, 0, 0x84, 0x03, 0, 5 };   /* PN: DLCI 18 (channel 9), credits, 900 bytes, 5 credits */
    rf_in(h, rf, 1, 0, 0xEF, pn, 10, -1);
    p = rf_out(0x61, &dl, &ctl, &n, &cr, "RFCOMM: PN answered");
    check("  credit flow control, 900 bytes, 7 credits for the phone", p && n == 10 && p[0] == 0x81 && p[2] == 18 && p[3] == 0xE0 && g16(p + 6) == 900 && p[9] == 7);
    rf_in(h, rf, 1, 9 << 1, 0x2F, 0, 0, -1);                /* SABM on channel 9 */
    p = rf_out(0x61, &dl, &ctl, &n, &cr, "RFCOMM: UA for channel 9");
    check("  (DLCI 18)", p && dl == 18 && ctl == 0x63);
    p = rf_out(0x61, &dl, &ctl, &n, &cr, "RFCOMM: our modem status");
    check("  MSC command, ready", p && dl == 0 && p[0] == 0xE3 && p[2] == (18 << 2 | 3));
    uint8_t msc[4] = { 0xE3, 5, 18 << 2 | 3, 0x8D };
    rf_in(h, rf, 1, 0, 0xEF, msc, 4, -1);
    p = rf_out(0x61, &dl, &ctl, &n, &cr, "RFCOMM: its modem status answered");
    check("  MSC response", p && p[0] == 0xE1);
    static const uint8_t con[7] = { 0x80, 0, 7, 0x10, 0, 0x10, 0 };
    rf_in(h, rf, 1, 18, 0xEF, con, 7, 3);
    p = rf_out(0x61, &dl, &ctl, &n, &cr, "OBEX: CONNECT answered");
    check("  success, packets up to 8192 bytes, credits given", p && dl == 18 && n == 7 && p[0] == 0xA0 && p[5] == 0x20 && p[6] == 0 && cr > 0);
    uint8_t put[64] = { 0x02, 0, 0, 0x01, 0, 23 };          /* PUT: Name "hello.txt", Length 11, Body "hello " */
    int k = 6;
    for (const char *s = "hello.txt"; *s; s++) { put[k++] = 0; put[k++] = (uint8_t)*s; }
    put[k++] = 0; put[k++] = 0;
    put[k++] = 0xC3; put[k++] = 0; put[k++] = 0; put[k++] = 0; put[k++] = 11;
    put[k++] = 0x48; put[k++] = 0; put[k++] = 9; memcpy(put + k, "hello ", 6); k += 6;
    put[2] = (uint8_t)k;
    rf_in(h, rf, 1, 18, 0xEF, put, k, -1);
    check("OBEX: the first PUT: the user asked, the answer waits", nasked == 1 && !strcmp(asked, "hello.txt") && taken == nsent && !fstate);
    check("  accepted (Atlas)", !opp_answer(1) && opp_answer(1) == -ENOENT);
    p = rf_out(0x61, &dl, &ctl, &n, &cr, "OBEX: the first PUT answered");
    check("  continue; the file opened as hello.txt", p && p[0] == 0x90 && fstate == 1 && !strcmp(fname, "hello.txt"));
    static const uint8_t fin[11] = { 0x82, 0, 11, 0x49, 0, 8, 'w', 'o', 'r', 'l', 'd' };
    rf_in(h, rf, 1, 18, 0xEF, fin, 11, -1);
    p = rf_out(0x61, &dl, &ctl, &n, &cr, "OBEX: the final PUT answered");
    check("  success; the file complete: \"hello world\"", p && p[0] == 0xA0 && fstate == 2 && fn == 11 && !memcmp(fdata, "hello world", 11));
    for (int t = 0; t < 2; t++) {                           /* two more: declined; not answered */
        static const uint8_t one[17] = { 0x82, 0, 17, 0x01, 0, 11, 0, 'x', 0, '.', 0, 'y', 0, 0, 0x49, 0, 3 };
        fstate = 0;
        rf_in(h, rf, 1, 18, 0xEF, one, 17, -1);
        check(t ? "a file nobody answers for: asked" : "a file the user declines: asked", nasked == 2 + t && !strcmp(asked, "x.y") && taken == nsent);
        if (t) for (int i = 0; i < 30; i++) opp_tick(); else opp_answer(0);
        p = rf_out(0x61, &dl, &ctl, &n, &cr, t ? "  30 s: answered" : "  answered");
        check("  forbidden, no file", p && p[0] == 0xC3 && fstate == 4 && opp_answer(1) == -ENOENT);
    }
    rf_in(h, rf, 1, 18, 0x53, 0, 0, -1);                    /* DISC 18 */
    p = rf_out(0x61, &dl, &ctl, &n, &cr, "RFCOMM: DISC answered");
    check("  UA", p && dl == 18 && ctl == 0x63);
    closed_by_us(h, "  and the L2CAP channel closed");
}
static void obex_out(int h)                                 /* us -> phone */
{
    static uint8_t file[3000], got[3000];
    int dl, ctl, n, cr, len;
    const uint8_t *p;
    for (int i = 0; i < 3000; i++) file[i] = (uint8_t)(i * 7 + i / 300);
    check("bt send: started", !opp_send(PH, "p\xC3\xAF" "c.jpg", 3000));
    check("  BT_DATA: not ready yet", opp_data(file, 100, 0) == -EAGAIN);
    int sdp = open_out(h, 1, 0x62, 672);
    p = frame(0x62, &len, "SDP: its Object Push (1105)");
    check("  asked", p && p[0] == 0x06 && g16(p + 8) == 0x0511);
    static const uint8_t rec[] = { 0x07, 0, 1, 0, 32, 0, 29, 0x35, 27, 0x35, 25,
        0x09, 0, 1, 0x35, 3, 0x19, 0x11, 0x05,
        0x09, 0, 4, 0x35, 12, 0x35, 3, 0x19, 1, 0, 0x35, 5, 0x19, 0, 3, 0x08, 12, 0 };
    uint8_t r[64];
    memcpy(r, rec, sizeof rec); r[1] = p ? p[1] : 0; r[2] = p ? p[2] : 0;
    acl_in(h, sdp, r, sizeof rec);
    closed_by_us(h, "SDP done");
    int rf = open_out(h, 3, 0x63, 600);
    p = rf_out(0x63, &dl, &ctl, &n, &cr, "RFCOMM: SABM for the multiplexer");
    check("  on DLCI 0, as initiator", p && dl == 0 && ctl == 0x2F && sent[taken - 1].b[8] == 3);
    rf_in(h, rf, 1, 0, 0x63, 0, 0, -1);                     /* UA */
    p = rf_out(0x63, &dl, &ctl, &n, &cr, "RFCOMM: PN for channel 12");
    check("  DLCI 24, credit flow control, 7 credits", p && p[0] == 0x83 && p[2] == 24 && p[3] == 0xF0 && p[9] == 7);
    uint8_t pn[10] = { 0x81, 17, 24, 0xE0, 0, 0, 0x90, 0x01, 0, 1 };   /* 400 bytes, 1 credit */
    rf_in(h, rf, 0, 0, 0xEF, pn, 10, -1);
    p = rf_out(0x63, &dl, &ctl, &n, &cr, "RFCOMM: SABM on DLCI 24");
    check("  (it)", p && dl == 24 && ctl == 0x2F);
    rf_in(h, rf, 0, 24, 0x63, 0, 0, -1);
    rf_out(0x63, &dl, &ctl, &n, &cr, "RFCOMM: our modem status");
    p = rf_out(0x63, &dl, &ctl, &n, &cr, "OBEX: CONNECT");
    check("  version 1.0, 8192 bytes (its one credit used, ours given)", p && dl == 24 && n == 7 && p[0] == 0x80 && p[3] == 0x10 && p[5] == 0x20 && cr > 0);
    static const uint8_t ca[7] = { 0xA0, 0, 7, 0x10, 0, 0x04, 0 };   /* success, 1024 bytes; 2 credits */
    rf_in(h, rf, 0, 24, 0xEF, ca, 7, 2);
    int at = 0, gn = 0, pk = 0, frames = 0, name = 0, last = 0, waits = 0;
    while (!last && pk < 10) {
        int r2 = at < 3000 ? opp_data(file + at, 3000 - at, 0) : opp_data(0, 0, 1);
        if (at < 3000) { check("  a piece taken", r2 > 0); if (r2 <= 0) break; at += r2; }
        else check("  the end: waits for the answer", r2 == -EAGAIN);
        uint8_t pkt[1100];
        int pn2 = 0, want = 3;
        while (pn2 < want) {                                 /* the packet, in frames; credits given when they stop */
            if (taken == nsent) { waits++; rf_in(h, rf, 0, 24, 0xEF, 0, 0, 3); continue; }
            p = rf_out(0x63, &dl, &ctl, &n, &cr, "OBEX: a PUT's frame");
            if (!p) break;
            if (n) frames++;
            check("  within 400 bytes", n <= 400);
            memcpy(pkt + pn2, p, n); pn2 += n;
            if (pn2 >= 3) want = pkt[1] << 8 | pkt[2];
        }
        pk++;
        last = pkt[0] == 0x82;
        check("  a PUT, at most 1024 bytes", (pkt[0] == 0x02 || pkt[0] == 0x82) && want <= 1024);
        for (int i = 3, hl; i < want; i += hl) {
            int hi = pkt[i];
            hl = hi >> 6 < 2 ? pkt[i + 1] << 8 | pkt[i + 2] : hi >> 6 == 2 ? 2 : 5;
            if (hl < 2) break;
            if (hi == 0x01) name = hl == 3 + 16 && pkt[i + 3] == 0 && pkt[i + 4] == 'p' && pkt[i + 5] == 0 && pkt[i + 6] == 0xEF && pkt[i + 7] == 0 && pkt[i + 8] == 'c';
            if (hi == 0xC3) check("  its length: 3000", pkt[i + 1] == 0 && pkt[i + 2] == 0 && (pkt[i + 3] << 8 | pkt[i + 4]) == 3000);
            if ((hi == 0x48 || hi == 0x49) && gn + hl - 3 <= 3000) { memcpy(got + gn, pkt + i + 3, hl - 3); gn += hl - 3; }
        }
        uint8_t ans[3] = { (uint8_t)(last ? 0xA0 : 0x90), 0, 3 };
        rf_in(h, rf, 0, 24, 0xEF, ans, 3, -1);
    }
    printf("  %d PUTs in %d frames, %d waits for credits\n", pk, frames, waits);
    check("all 3000 bytes sent, in order; its name in UTF-16 (\"p\\u00efc.jpg\"); waited for credits", gn == 3000 && !memcmp(got, file, 3000) && name && waits);
    p = rf_out(0x63, &dl, &ctl, &n, &cr, "OBEX: DISCONNECT");
    check("  (0x81)", p && n == 3 && p[0] == 0x81);
    p = rf_out(0x63, &dl, &ctl, &n, &cr, "RFCOMM: DISC");
    check("  on DLCI 24", p && dl == 24 && ctl == 0x43);
    closed_by_us(h, "  and the L2CAP channel");
    check("BT_DATA's end: delivered", opp_data(0, 0, 1) == 0);
}

static void phone(void)
{
    printf("SDP server, OBEX both ways (a phone)\n");
    const int h = 0x51;
    bonded(PH, BTK_PHONE, "Phone");
    link_up(h, PH, 1);
    check("  a phone: no service started", taken == nsent);
    int len, sdp = open_in(h, 1, 0x60, 672);
    static const uint8_t ssa[20] = { 0x06, 0, 9, 0, 15, 0x35, 3, 0x19, 0x11, 0x05, 0, 20, 0x35, 5, 0x0A, 0, 0, 0xFF, 0xFF, 0 };
    acl_in(h, sdp, ssa, 20);                                /* Object Push? at most 20 bytes an answer */
    static uint8_t all[256];
    int an = 0, turns = 0;
    for (const uint8_t *p; (p = frame(0x60, &len, "SDP: our answer")) && turns < 20; turns++) {
        int bc = p[5] << 8 | p[6];
        check("  Service Search Attribute response, at most 20 bytes", p[0] == 0x07 && bc <= 20 && 7 + bc < len);
        memcpy(all + an, p + 7, bc); an += bc;
        if (!p[7 + bc]) break;
        uint8_t q[24];
        memcpy(q, ssa, 19); q[19] = p[7 + bc]; memcpy(q + 20, p + 8 + bc, p[7 + bc]); q[4] = (uint8_t)(15 + p[7 + bc]);
        acl_in(h, sdp, q, 20 + p[7 + bc]);
    }
    int ch = 0;
    for (int i = 0; i + 5 <= an; i++) if (!memcmp(all + i, "\x19\x00\x03\x08", 4)) ch = all[i + 4];
    printf("  %d bytes in %d answers\n", an, turns + 1);
    check("  one record (handle 10002), RFCOMM channel 9", turns > 1 && all[0] == 0x35 && all[1] == an - 2 && ch == OPP_CH && !memcmp(all + 4, "\x09\x00\x00\x0A\x00\x01\x00\x02", 8));
    static const uint8_t ss[13] = { 0x02, 0, 10, 0, 8, 0x35, 3, 0x19, 0x11, 0x0A, 0, 5, 0 };
    acl_in(h, sdp, ss, 13);
    const uint8_t *p = frame(0x60, &len, "SDP: Service Search for an audio source (110A)");
    check("  one handle: 10001", p && p[0] == 0x03 && p[6] == 1 && p[8] == 1 && g16(p + 9) == 0x0100 && p[12] == 1);
    static const uint8_t rc[13] = { 0x02, 0, 12, 0, 8, 0x35, 3, 0x19, 0x11, 0x0E, 0, 5, 0 };
    acl_in(h, sdp, rc, 13);
    p = frame(0x60, &len, "SDP: Service Search for AVRCP (110E)");
    check("  two: 10003 (target), 10004 (controller)", p && p[0] == 0x03 && p[6] == 2 && p[8] == 2 && p[12] == 3 && p[16] == 4);
    static const uint8_t sa[17] = { 0x04, 0, 11, 0, 12, 0, 1, 0, 1, 0, 100, 0x35, 3, 0x09, 0, 4, 0 };
    acl_in(h, sdp, sa, 17);
    p = frame(0x60, &len, "SDP: its attribute 4 (protocols)");
    check("  only that: L2CAP 0x19, AVDTP 1.3", p && p[0] == 0x05 && len == 31 && p[7] == 0x35 && p[8] == 21 && p[10] == 0 && p[11] == 4 && p[28] == 1 && p[29] == 3);
    int rf = open_in(h, 3, 0x61, 1000);
    obex_in(h, rf);
    obex_out(h);
}

int main(void)
{
    memcpy(bt.addr, (const uint8_t[6]){ 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6 }, 6);
    bt.acl_mtu = 1021; bt.acl_free = 8; bt.le_mtu = 251; bt.le_free = 8; bt.ncmd = 1000; bt.on = 1;
    bt_ready();
    cmd_is(0x0C1A, "page scan on");
    classic();
    le();
    audio();
    phone();
    printf(fails ? "bt stack: %d of %d checks FAILED\n" : "bt stack: all %d checks passed\n", fails ? fails : checks, checks);
    return fails != 0;
}
