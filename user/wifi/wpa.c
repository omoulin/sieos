/*
 * wpa.c - WPA2-PSK's handshakes on the host (wpa.h), from IEEE 802.11-2020
 * 12.7: EAPOL-Key frames with key descriptor version 2 (HMAC-SHA-1 MICs, AES
 * key wrap), the PRF-384 pairwise key expansion, the GTK key data element.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "wpa.h"
#include "mk/crypto.h"
#include "mk/lib.h"

/* An EAPOL-Key frame: 802.1X header (4), then these offsets from its start. */
enum { K_INFO = 5, K_LEN = 7, K_REPLAY = 9, K_NONCE = 17, K_MIC = 81, K_DLEN = 97, K_DATA = 99 };
enum { I_VER2 = 2, I_PAIR = 1 << 3, I_INSTALL = 1 << 6, I_ACK = 1 << 7, I_MIC = 1 << 8, I_SECURE = 1 << 9, I_ENC = 1 << 12 };

const uint8_t wpa_ie[22] = { 0x30, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4,
                             1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };

void wpa_pmk(uint8_t pmk[32], const char *pass, const char *ssid)
{
    pbkdf2_sha1(pmk, 32, pass, strlen(pass), ssid, strlen(ssid), 4096);
}

const uint8_t *ie_find(const uint8_t *ie, int len, int id)
{
    for (const uint8_t *e = ie + len; ie + 2 <= e && ie + 2 + ie[1] <= e; ie += 2 + ie[1])
        if (ie[0] == id) return ie;
    return 0;
}

static int has_suite(const uint8_t *p, const uint8_t *e, int type, const uint8_t **next)
{
    int n = p + 2 <= e ? p[0] | p[1] << 8 : 0, ok = 0;
    for (p += 2; n-- > 0 && p + 4 <= e; p += 4) ok |= !memcmp(p, "\x00\x0f\xac", 3) && p[3] == type;
    *next = p;
    return ok;
}
int wpa_rsn_ok(const uint8_t *ie)
{
    const uint8_t *p = ie + 4, *e = ie + 2 + ie[1];
    if (ie[1] < 18 || memcmp(p, "\x00\x0f\xac\x04", 4)) return 0;        /* group cipher: CCMP */
    if (!has_suite(p + 4, e, 4, &p) || !has_suite(p, e, 2, &p)) return 0;  /* pairwise CCMP, AKM PSK */
    return p + 2 > e || !(p[0] & 0x40);                                    /* management frame protection not required */
}

static void ptk_derive(wpa_t *w)
{
    uint8_t m[23 + 76 + 1], *d = m + 23;
    int a = memcmp(w->aa, w->spa, 6) < 0, n = memcmp(w->anonce, w->snonce, 32) < 0;
    memcpy(m, "Pairwise key expansion", 23);
    memcpy(d, a ? w->aa : w->spa, 6); memcpy(d + 6, a ? w->spa : w->aa, 6);
    memcpy(d + 12, n ? w->anonce : w->snonce, 32); memcpy(d + 44, n ? w->snonce : w->anonce, 32);
    uint8_t out[60];
    for (int i = 0; i < 3; i++) { m[99] = (uint8_t)i; hmac_sha1(out + 20 * i, w->pmk, 32, m, sizeof m); }
    memcpy(w->ptk, out, 48);
    wipe(out, sizeof out);
}

static void mic(const wpa_t *w, uint8_t *f, int len, uint8_t out[16])
{
    uint8_t d[20], keep[16];
    memcpy(keep, f + K_MIC, 16); memset(f + K_MIC, 0, 16);
    hmac_sha1(d, w->ptk, 16, f, (size_t)len);
    memcpy(f + K_MIC, keep, 16);
    memcpy(out, d, 16);
}

/* An answer: key info, the replay counter echoed, a nonce, key data; MIC'd. */
static int reply(const wpa_t *w, const uint8_t *in, int info, const uint8_t *nonce, const uint8_t *data, int dlen, uint8_t *o)
{
    int len = K_DATA + dlen;
    memset(o, 0, (size_t)len);
    o[0] = in[0]; o[1] = 3; o[2] = (uint8_t)((len - 4) >> 8); o[3] = (uint8_t)(len - 4);
    o[4] = 2; o[K_INFO] = (uint8_t)(info >> 8); o[K_INFO + 1] = (uint8_t)info;
    memcpy(o + K_REPLAY, in + K_REPLAY, 8);
    if (nonce) memcpy(o + K_NONCE, nonce, 32);
    o[K_DLEN] = (uint8_t)(dlen >> 8); o[K_DLEN + 1] = (uint8_t)dlen;
    if (dlen) memcpy(o + K_DATA, data, (size_t)dlen);
    mic(w, o, len, o + K_MIC);
    return len;
}

/* The wrapped key data of messages 3 and group 1: the GTK element (dd, len,
 * 00-0f-ac 01, key id, reserved, key); in message 3 also the AP's RSN element. */
static int keydata(wpa_t *w, const uint8_t *in, int len, int need_rsn)
{
    uint8_t kd[256];
    int n = in[K_DLEN] << 8 | in[K_DLEN + 1];
    if (n < 24 || n > (int)sizeof kd + 8 || K_DATA + n > len || aes_unwrap(w->ptk + 16, 16, in + K_DATA, (size_t)n, kd)) return -1;
    n -= 8;
    if (need_rsn && w->ap_rsn && (!ie_find(kd, n, 0x30) || memcmp(ie_find(kd, n, 0x30), w->ap_rsn, w->ap_rsn[1] + 2u)))
        return -1;                                     /* not what the beacon said: a downgrade */
    for (const uint8_t *e = kd; e + 2 <= kd + n && e + 2 + e[1] <= kd + n; e += 2 + e[1])
        if (e[0] == 0xdd && e[1] >= 6 + 16 && !memcmp(e + 2, "\x00\x0f\xac\x01", 4)) {
            w->gtkid = e[6] & 3; w->gtklen = e[1] - 6 > 32 ? 32 : e[1] - 6;
            memcpy(w->gtk, e + 8, (size_t)w->gtklen);
            wipe(kd, sizeof kd);
            return 0;
        }
    wipe(kd, sizeof kd);
    return -1;
}

int wpa_rx(wpa_t *w, const uint8_t *in, int len, uint8_t *out, int *done)
{
    *done = 0;
    if (len < K_DATA || in[1] != 3 || in[4] != 2) return 0;          /* not an RSN EAPOL-Key frame */
    int info = in[K_INFO] << 8 | in[K_INFO + 1];
    if ((info & 7) != I_VER2 || !(info & I_ACK)) return -1;
    if (w->have_replay && memcmp(in + K_REPLAY, w->replay, 8) <= 0 && (info & I_MIC)) return -1;   /* replayed */
    if ((info & (I_PAIR | I_MIC)) == I_PAIR) {                       /* message 1: the AP's nonce */
        memcpy(w->anonce, in + K_NONCE, 32);
        ptk_derive(w);
        return reply(w, in, I_VER2 | I_PAIR | I_MIC, w->snonce, wpa_ie, sizeof wpa_ie, out);
    }
    uint8_t m[16];
    mic(w, (uint8_t *)in, len, m);
    if (!(info & I_MIC) || !ct_equal(m, in + K_MIC, 16) || !(info & I_ENC)) return -1;
    if (info & I_PAIR) {                                             /* message 3: install, the GTK */
        if (!(info & I_INSTALL) || memcmp(w->anonce, in + K_NONCE, 32) || keydata(w, in, len, 1)) return -1;
        *done = WPA_PTK;
    } else {                                                         /* group message 1: a new GTK */
        if (keydata(w, in, len, 0)) return -1;
        *done = WPA_GTK;
    }
    memcpy(w->replay, in + K_REPLAY, 8); w->have_replay = 1;
    return reply(w, in, I_VER2 | I_MIC | I_SECURE | (info & I_PAIR), 0, 0, 0, out);
}
