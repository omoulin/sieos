/*
 * avrcp.c - Remote control: AVRCP 1.4 over AVCTP 1.3 (L2CAP PSM 0x17),
 * with the A2DP sink; one control channel, opened by the sink, or by us
 * 2 s after A2DP is ready if it has not.
 *
 *   target      the headset's buttons: PASS THROUGH play, pause, stop, next,
 *               previous, volume up and down; each press goes to audiod as a
 *               media key (AUDIO_KEY, the operation id). Pause and stop also
 *               suspend the stream at once: the headset sees it stopped, so
 *               its button then sends play. UNIT INFO, SUBUNIT INFO and
 *               GetCapabilities (no events) answered; the rest refused
 *   controller  absolute volume: RegisterNotification (volume changed) when
 *               the channel opens; an INTERIM answer: the sink scales the
 *               sound, so SIEOS's volume goes to it (SetAbsoluteVolume,
 *               0-100% as 0-127) and the encoder runs at full scale; each
 *               CHANGED (its own buttons) sets SIEOS's volume, and we
 *               register again
 * A frame: AVCTP's 3 bytes (label, packet type, command/response; the
 * profile 110E), then AV/C's: ctype, subunit (0x48: panel), opcode,
 * operands; vendor-dependent ones (opcode 0) carry the Bluetooth SIG's
 * company id 001958, a PDU id, its parameters' length.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "btd.h"

static struct {
    conn_t *c;                                   /* the sink, while we wait to open the channel */
    l2ch_t *h;
    int wait, label, abs, vol;                   /* abs: the sink sets the volume; vol: SIEOS's (0-100), -1: not known */
} rc = { .vol = -1 };

static void send(int label, int resp, const uint8_t *av, int n)
{
    uint8_t b[24] = { (uint8_t)(label << 4 | resp << 1), 0x11, 0x0E };
    memcpy(b + 3, av, n);
    l2_send(rc.h, b, 3 + n);
}
static void vendor(int label, int resp, int ctype, int pdu, const uint8_t *p, int n)
{
    uint8_t b[16] = { (uint8_t)ctype, 0x48, 0, 0, 0x19, 0x58, (uint8_t)pdu, 0, 0, (uint8_t)n };
    memcpy(b + 10, p, n);
    send(label, resp, b, 10 + n);
}
static void ask(int ctype, int pdu, const uint8_t *p, int n) { rc.label = (rc.label + 1) & 15; vendor(rc.label, 0, ctype, pdu, p, n); }
static void reg(void) { static const uint8_t e[5] = { 0x0D }; ask(3, 0x31, e, 5); }   /* NOTIFY: volume changed */
static void set_abs(void) { uint8_t v = (uint8_t)((rc.vol * 127 + 50) / 100); ask(0, 0x50, &v, 1); }   /* CONTROL: SetAbsoluteVolume */

static void rc_recv(l2ch_t *h, const uint8_t *p, int n)
{
    if (h != rc.h || n < 6 || p[0] & 0x0C) return;      /* (single packets only: ours are all small) */
    int label = p[0] >> 4, ct = p[3], op = p[5], id = n > 6 ? p[6] & 0x7F : 0, pdu = n >= 13 ? p[9] : -1;
    if (p[0] & 2) {                                      /* an answer to ours: RegisterNotification's */
        if (op || pdu != 0x31) return;
        if ((ct != 0x0F && ct != 0x0D) || n < 15 || p[13] != 0x0D) { bt_log("bt: AVRCP: no absolute volume (%02x)\n", ct); return; }
        if (!rc.abs) { rc.abs = 1; bt_log("bt: %s: AVRCP: absolute volume\n", h->c->dev->d.name); if (rc.vol >= 0) set_abs(); }
        if (ct == 0x0D) { rc.vol = ((p[14] & 0x7F) * 100 + 63) / 127; to_audiod(AUDIO_VOL, rc.vol); reg(); }   /* CHANGED */
        return;
    }
    if ((p[1] << 8 | p[2]) != 0x110E) { uint8_t b[3] = { (uint8_t)(label << 4 | 3), p[1], p[2] }; l2_send(h, b, 3); return; }   /* (not ours: invalid profile) */
    uint8_t r[16];
    int k = n - 3 < 16 ? n - 3 : 16;
    memcpy(r, p + 3, k);
    if (op == 0x7C && n >= 8 && ((id >= MK_VOLUP && id <= MK_PAUSE && id != 0x43) || id == MK_NEXT || id == MK_PREV)) {   /* PASS THROUGH */
        r[0] = 0x09;                                     /* accepted */
        if (!(p[6] & 0x80)) {                            /* (pressed; not released) */
            bt_log("bt: AVRCP: key %02x\n", id);
            to_audiod(AUDIO_KEY, id);
            if (id == MK_PAUSE || id == MK_STOP) a2dp_suspend();
        }
    } else if (op == 0x30 || op == 0x31) {               /* UNIT INFO, SUBUNIT INFO: a panel */
        static const uint8_t u[5] = { 0x07, 0x48, 0xFF, 0xFF, 0xFF };
        r[0] = 0x0C; r[1] = 0xFF; memcpy(r + 3, u, 5); k = 8;
    } else if (op == 0 && pdu >= 0) {                    /* GetCapabilities (company ids: the SIG's; events: none); else rejected */
        uint8_t c[5] = { n > 13 ? p[13] : 0, 1, 0, 0x19, 0x58 };
        if (pdu == 0x10 && (c[0] == 2 || c[0] == 3)) { c[1] = c[0] == 2; vendor(label, 1, 0x0C, pdu, c, c[1] ? 5 : 2); }
        else { c[0] = pdu == 0x10 || pdu == 0x31; vendor(label, 1, 0x0A, pdu, c, 1); }   /* (1: invalid parameter, 0: command) */
        return;
    } else r[0] = 0x08;                                  /* not implemented */
    send(label, 1, r, k);
}
static void rc_open(l2ch_t *h)
{
    if (rc.h && rc.h != h) { l2_close(h); return; }      /* (one at a time) */
    rc.h = h; rc.abs = rc.wait = 0;
    bt_log("bt: %s: AVRCP connected\n", h->c->dev->d.name);
    reg();
}
static void rc_close(l2ch_t *h) { if (h == rc.h) rc.h = 0, rc.abs = 0; }
static const l2ops_t ops = { rc_open, rc_recv, rc_close };

void avrcp_init(void) { l2_listen(0x17, &ops); }
void avrcp_link(conn_t *c) { if (!rc.h) rc.c = c, rc.wait = 2; }
void avrcp_tick(void)
{
    if (rc.wait && !--rc.wait && !rc.h && rc.c->dev && rc.c->enc) rc.h = l2_open(rc.c, 0x17, &ops);
}
int avrcp_vol(int vol)
{
    vol &= 0xFF;
    if (vol != rc.vol) { rc.vol = vol; if (rc.abs) set_abs(); }
    return rc.abs;
}
