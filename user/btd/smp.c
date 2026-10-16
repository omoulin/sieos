/*
 * smp.c - LE pairing (Security Manager, Core spec Vol 3 Part H), as the
 * central (we start it). Our IO capability: a display and yes/no.
 *
 *   Pairing Request / Response   what each side can do; Secure Connections
 *                                if both say so, else legacy (old devices)
 *   Secure Connections           P-256 public keys, then by the IO capabilities:
 *     just works / numeric compare  its confirm, our nonce, its nonce; the
 *                                6 digits (g2) for the user to compare
 *     passkey (a keyboard)       we show 6 digits, the user types them: 20
 *                                rounds, one bit each, of confirm and nonce
 *     then                       f5: the LTK; f6: both sides check (DHKey Check)
 *   legacy                       TK = 0 or the passkey; confirm (c1), nonce;
 *                                STK = s1; encrypted, the device sends its LTK,
 *                                EDIV and Rand
 * Encryption on with the LTK (or STK), the keys kept: bonded. Every value
 * little-endian, as sent (lib/btcrypt.c).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "btd.h"

enum { JUST_WORKS, COMPARE, PASSKEY };
typedef struct smp {
    uint8_t preq[7], pres[7], sc, method, round, waiting, keysize;
    uint8_t priv[32], pk[64], pkb[64], dh[32];
    uint8_t na[16], nb[16], cb[16], tk[16], mackey[16], ltk[16];
    uint32_t passkey;
} smp_t;

static void send(conn_t *c, int op, const void *d, int n)
{
    uint8_t b[65] = { (uint8_t)op };
    memcpy(b + 1, d, n);
    acl_send(c, 6, b, 1 + n);
}

static void fail(conn_t *c, int reason, const char *why)
{
    uint8_t r = (uint8_t)reason;
    if (reason) send(c, 0x05, &r, 1);
    smp_end(c);
    pair_end(0, why);
    bt_disconnect(c, 0x05);
}

void smp_end(conn_t *c)
{
    if (c->smp) { wipe(c->smp, sizeof *c->smp); free(c->smp); c->smp = 0; }
}

void smp_start(conn_t *c)
{
    static const uint8_t req[6] = { 0x01, 0x00, 0x0D, 16, 0x00, 0x01 };   /* display yes/no; bonding, MITM, SC; their LTK */
    smp_t *s = c->smp = malloc(sizeof *s);
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->preq[0] = 0x01; memcpy(s->preq + 1, req, 6);
    send(c, 0x01, req, 6);
    bt_log("bt: LE pairing: request sent\n");
}

/* The addresses as f5, f6 and c1 take them: 6 bytes, then the type (ours: public). */
static void addrs(conn_t *c, uint8_t a[7], uint8_t b[7])
{
    memcpy(a, bt.addr, 6); a[6] = 0;
    memcpy(b, c->dev->d.addr, 6); b[6] = (uint8_t)(c->dev->d.type == BTT_LE_RANDOM);
}

static void confirm_legacy(conn_t *c, uint8_t *out, const uint8_t *r)
{
    uint8_t a[7], b[7];
    addrs(c, a, b);
    bt_c1(out, c->smp->tk, r, c->smp->preq, c->smp->pres, a[6], a, b[6], b);
}

static void passkey_round(conn_t *c)                     /* SC passkey: our commitment for bit `round` */
{
    smp_t *s = c->smp;
    uint8_t ca[16];
    bt_random(s->na, 16);
    bt_f4(ca, s->pk, s->pkb, s->na, (uint8_t)(0x80 | (s->passkey >> s->round & 1)));
    send(c, 0x03, ca, 16);
}

static void dhkey_check(conn_t *c)                       /* f5, then our check value Ea */
{
    smp_t *s = c->smp;
    uint8_t a[7], b[7], r[16] = { 0 }, ea[16];
    addrs(c, a, b);
    if (s->method == PASSKEY) memcpy(r, &(uint32_t){ s->passkey }, 4);
    bt_f5(s->mackey, s->ltk, s->dh, s->na, s->nb, a, b);
    bt_f6(ea, s->mackey, s->na, s->nb, r, s->preq + 1, a, b);
    send(c, 0x0D, ea, 16);
}

static void keep(conn_t *c, const uint8_t *ltk, const uint8_t *rand, int ediv)
{
    btdev_t *d = c->dev;
    memcpy(d->key, ltk, 16);
    memset(d->rand, 0, 8);
    if (rand) memcpy(d->rand, rand, 8);
    d->ediv = (uint16_t)ediv;
    d->keysize = c->smp->keysize;
    d->d.flags |= BTF_BONDED;
    bt_log("bt: LE pairing: keys kept (%s, %d-byte key)\n", c->smp->sc ? "Secure Connections" : "legacy", d->keysize);
    smp_end(c);
    bonds_save();
    pair_end(1, 0);
    hogp_start(c);
}

void smp_encrypted(conn_t *c, int ok)
{
    smp_t *s = c->smp;
    if (!ok) { fail(c, 0, "encryption failed"); return; }
    if (s->sc) keep(c, s->ltk, 0, 0);
    else if (!(s->pres[6] & 1)) fail(c, 0, "the device gives no key to keep");
    /* (legacy: its LTK, EDIV and Rand come next) */
}

int smp_confirm(conn_t *c, int ok)
{
    smp_t *s = c->smp;
    if (!s || !s->waiting) return -EINVAL;
    s->waiting = 0;
    if (ok) dhkey_check(c); else fail(c, 0x0C, "the codes did not match");
    return 0;
}

void smp_rx(conn_t *c, const uint8_t *p, int n)
{
    smp_t *s = c->smp;
    uint8_t chk[16], a[7], b[7], r[16] = { 0 };
    if (n < 1) return;
    if (p[0] == 0x0B) {                                   /* Security Request: encrypt (bonded) or pair (being paired) */
        if (c->dev->d.flags & BTF_BONDED) le_encrypt(c, c->dev->key, c->dev->rand, c->dev->ediv);
        else if (!s && c->dev == bt.pdev && bt.pair == BTP_BUSY) smp_start(c);
        return;
    }
    if (!s) return;
    switch (p[0]) {
    case 0x02:                                            /* Pairing Response */
        if (n < 7) break;
        memcpy(s->pres, p, 7);
        s->sc = (s->preq[3] & p[3] & 8) != 0;
        s->keysize = p[4] < 16 ? p[4] : 16;
        if (s->keysize < 7) { fail(c, 0x06, "the device's key is too short"); return; }
        if (!(s->preq[3] & p[3] & 4)) s->method = JUST_WORKS;   /* nobody wants protection against a man in the middle */
        else if (p[1] == 2 || (!s->sc && p[1] == 4)) s->method = PASSKEY;   /* a keyboard: it types what we show */
        else s->method = s->sc && (p[1] == 1 || p[1] == 4) ? COMPARE : JUST_WORKS;
        bt_log("bt: LE pairing: %s, %s (its IO capability %d)\n", s->sc ? "Secure Connections" : "legacy",
               s->method == PASSKEY ? "passkey" : s->method == COMPARE ? "numeric comparison" : "just works", p[1]);
        if (s->method == PASSKEY) {
            bt_random(&s->passkey, 4);
            s->passkey %= 1000000;
            bt.pair = BTP_PASSKEY; bt.code = s->passkey;
            bt_log("bt: type %06u on the device, then Enter\n", s->passkey);
        }
        if (s->sc) {
            bt_random(s->priv, 32);
            if (bt_p256_public(s->pk, s->priv)) { fail(c, 0x08, "no key pair"); return; }
            send(c, 0x0C, s->pk, 64);
        } else {
            memset(s->tk, 0, 16);
            if (s->method == PASSKEY) memcpy(s->tk, &s->passkey, 4);
            bt_random(s->na, 16);
            confirm_legacy(c, chk, s->na);
            send(c, 0x03, chk, 16);
        }
        break;
    case 0x0C:                                            /* Pairing Public Key */
        if (n < 65 || !s->sc) break;
        memcpy(s->pkb, p + 1, 64);
        if (!memcmp(s->pkb, s->pk, 32) || bt_p256_dh(s->dh, s->priv, s->pkb)) { fail(c, 0x0A, "a bad public key"); return; }
        if (s->method == PASSKEY) passkey_round(c);       /* (else its confirm comes first) */
        break;
    case 0x03:                                            /* Pairing Confirm */
        if (n < 17) break;
        memcpy(s->cb, p + 1, 16);
        if (s->sc && s->method != PASSKEY) bt_random(s->na, 16);
        send(c, 0x04, s->na, 16);
        break;
    case 0x04:                                            /* Pairing Random */
        if (n < 17) break;
        memcpy(s->nb, p + 1, 16);
        if (!s->sc) {
            confirm_legacy(c, chk, s->nb);
            if (!ct_equal(chk, s->cb, 16)) { fail(c, 0x04, "the device's confirm value is wrong"); return; }
            bt_s1(s->ltk, s->tk, s->nb, s->na);
            memset(s->ltk + s->keysize, 0, 16 - s->keysize);
            le_encrypt(c, s->ltk, 0, 0);                  /* with the STK */
            break;
        }
        bt_f4(chk, s->pkb, s->pk, s->nb, s->method == PASSKEY ? (uint8_t)(0x80 | (s->passkey >> s->round & 1)) : 0);
        if (!ct_equal(chk, s->cb, 16)) { fail(c, 0x04, "the device's confirm value is wrong"); return; }
        if (s->method == PASSKEY && ++s->round < 20) { passkey_round(c); break; }
        if (s->method == COMPARE) {
            bt.code = bt_g2(s->pk, s->pkb, s->na, s->nb);
            bt.pair = BTP_CONFIRM; s->waiting = 1;
            bt_log("bt: does the device show %06u? (bt pair confirms)\n", bt.code);
            break;
        }
        dhkey_check(c);
        break;
    case 0x0D:                                            /* DHKey Check: its Eb */
        if (n < 17) break;
        addrs(c, a, b);
        if (s->method == PASSKEY) memcpy(r, &(uint32_t){ s->passkey }, 4);
        bt_f6(chk, s->mackey, s->nb, s->na, r, s->pres + 1, b, a);
        if (!ct_equal(chk, p + 1, 16)) { fail(c, 0x0B, "the DHKey check failed"); return; }
        memset(s->ltk + s->keysize, 0, 16 - s->keysize);
        le_encrypt(c, s->ltk, 0, 0);
        break;
    case 0x06: if (n >= 17) memcpy(s->ltk, p + 1, 16); break;   /* Encryption Information: its LTK */
    case 0x07:                                            /* Central Identification: EDIV, Rand */
        if (n >= 11 && !s->sc) keep(c, s->ltk, p + 3, g16(p + 1));
        break;
    case 0x05:                                            /* Pairing Failed */
        bt_log("bt: LE pairing: the device says no (%02x)\n", n > 1 ? p[1] : 0);
        fail(c, 0, "the device refused");
        break;
    }
}
