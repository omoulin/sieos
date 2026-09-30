/*
 * wpa.c - The cryptography of WPA2-Personal (IEEE 802.11-2016, 12.7): the
 * passphrase's key (PBKDF2-HMAC-SHA1, 4096 rounds, RFC 8018), the PRF that
 * expands the pairwise key (12.7.1.2), the EAPOL-Key MIC (HMAC-SHA1-128)
 * and the unwrapping of the group key (AES key wrap, RFC 3394).
 *
 * SHA-1 is FIPS 180-4's, AES FIPS 197's (the tables computed at first use).
 * The frames' encryption itself (CCMP) is done by the Wi-Fi device.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "kernel.h"
#include "wpa.h"

/* ---------------------------------------------------------------- SHA-1 */

struct sha1 {
    uint32_t h[5];
    uint8_t buf[64];
    uint32_t n;
    uint64_t total;
};

static inline uint32_t rol(uint32_t x, int n) { return x << n | x >> (32 - n); }

static void sha1_block(struct sha1 *s, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)
            f = (b & c) | (~b & d), k = 0x5A827999;
        else if (i < 40)
            f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (i < 60)
            f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else
            f = b ^ c ^ d, k = 0xCA62C1D6;
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e;
}

static void sha1_init(struct sha1 *s)
{
    static const uint32_t iv[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    memcpy(s->h, iv, sizeof(iv));
    s->n = 0;
    s->total = 0;
}

static void sha1_update(struct sha1 *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->total += len;
    while (len) {
        uint32_t k = 64 - s->n < len ? 64 - s->n : (uint32_t)len;
        memcpy(s->buf + s->n, p, k);
        s->n += k, p += k, len -= k;
        if (s->n == 64) {
            sha1_block(s, s->buf);
            s->n = 0;
        }
    }
}

static void sha1_final(struct sha1 *s, uint8_t out[20])
{
    uint64_t bits = s->total * 8;
    uint8_t pad = 0x80, zero = 0;
    sha1_update(s, &pad, 1);
    while (s->n != 56)
        sha1_update(s, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++)
        len[i] = bits >> (56 - 8 * i);
    sha1_update(s, len, 8);
    for (int i = 0; i < 5; i++)
        out[4 * i] = s->h[i] >> 24, out[4 * i + 1] = s->h[i] >> 16, out[4 * i + 2] = s->h[i] >> 8,
        out[4 * i + 3] = s->h[i];
}

/* HMAC-SHA1 (RFC 2104) over up to three pieces. */
static void hmac_sha1_3(const uint8_t *key, size_t klen, const void *a, size_t alen, const void *b, size_t blen,
                        const void *c, size_t clen, uint8_t out[20])
{
    uint8_t k[64] = { 0 }, pad[64], inner[20];
    if (klen > 64) {
        struct sha1 s;
        sha1_init(&s);
        sha1_update(&s, key, klen);
        sha1_final(&s, k);
    } else {
        memcpy(k, key, klen);
    }
    struct sha1 s;
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x36;
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    sha1_update(&s, a, alen);
    if (blen)
        sha1_update(&s, b, blen);
    if (clen)
        sha1_update(&s, c, clen);
    sha1_final(&s, inner);
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x5C;
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    sha1_update(&s, inner, 20);
    sha1_final(&s, out);
}

void hmac_sha1(const uint8_t *key, size_t klen, const void *msg, size_t len, uint8_t out[20])
{
    hmac_sha1_3(key, klen, msg, len, NULL, 0, NULL, 0, out);
}

/* The PMK from a passphrase (8-63 characters) and the SSID: PBKDF2-HMAC-SHA1, 4096 rounds, 256 bits. */
void wpa_passphrase_pmk(const char *pass, const uint8_t *ssid, size_t ssid_len, uint8_t pmk[32])
{
    size_t plen = strlen(pass);
    for (uint32_t blk = 1; blk <= 2; blk++) {
        uint8_t cnt[4] = { blk >> 24, blk >> 16, blk >> 8, blk }, u[20], t[20];
        hmac_sha1_3((const uint8_t *)pass, plen, ssid, ssid_len, cnt, 4, NULL, 0, u);
        memcpy(t, u, 20);
        for (int i = 1; i < 4096; i++) {
            hmac_sha1((const uint8_t *)pass, plen, u, 20, u);
            for (int j = 0; j < 20; j++)
                t[j] ^= u[j];
        }
        memcpy(pmk + (blk - 1) * 20, t, blk == 1 ? 20 : 12);
    }
}

/* The 802.11 PRF: HMAC-SHA1(K, label || 0 || data || i), i = 0, 1, ... */
void wpa_prf(const uint8_t *key, size_t klen, const char *label, const uint8_t *data, size_t dlen, uint8_t *out,
             size_t olen)
{
    uint8_t lab[64], dig[20];
    size_t ll = strlen(label);
    memcpy(lab, label, ll);
    lab[ll] = 0;
    for (uint8_t i = 0; olen; i++) {
        hmac_sha1_3(key, klen, lab, ll + 1, data, dlen, &i, 1, dig);
        size_t k = olen < 20 ? olen : 20;
        memcpy(out, dig, k);
        out += k, olen -= k;
    }
}

/* ---------------------------------------------------------------- AES-128 */

static uint8_t sbox[256], inv_sbox[256];

static uint8_t xt(uint8_t x) { return (uint8_t)(x << 1) ^ (x & 0x80 ? 0x1B : 0); }

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) {
        if (b & 1)
            r ^= a;
        a = xt(a);
        b >>= 1;
    }
    return r;
}

static void aes_tables(void)
{
    if (sbox[0])
        return;
    for (int i = 0; i < 256; i++) {
        uint8_t inv = 0;                         /* the multiplicative inverse in GF(2^8) */
        for (int j = 1; j < 256 && i; j++)
            if (gmul((uint8_t)i, (uint8_t)j) == 1) {
                inv = (uint8_t)j;
                break;
            }
        uint8_t s = inv ^ (uint8_t)(inv << 1 | inv >> 7) ^ (uint8_t)(inv << 2 | inv >> 6) ^
                    (uint8_t)(inv << 3 | inv >> 5) ^ (uint8_t)(inv << 4 | inv >> 4) ^ 0x63;
        sbox[i] = s;
        inv_sbox[s] = (uint8_t)i;
    }
}

static void aes_expand(const uint8_t key[16], uint8_t rk[176])
{
    aes_tables();
    memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };
        if (i % 16 == 0) {
            uint8_t a = t[0];
            t[0] = sbox[t[1]] ^ rcon, t[1] = sbox[t[2]], t[2] = sbox[t[3]], t[3] = sbox[a];
            rcon = xt(rcon);
        }
        for (int j = 0; j < 4; j++)
            rk[i + j] = rk[i - 16 + j] ^ t[j];
    }
}

static void aes_decrypt(const uint8_t rk[176], uint8_t b[16])
{
    for (int j = 0; j < 16; j++)
        b[j] ^= rk[160 + j];
    for (int r = 9; r >= 0; r--) {
        uint8_t t[16];
        for (int c = 0; c < 4; c++)              /* inverse shift rows, inverse substitution */
            for (int row = 0; row < 4; row++)
                t[4 * ((c + row) % 4) + row] = inv_sbox[b[4 * c + row]];
        for (int j = 0; j < 16; j++)
            t[j] ^= rk[16 * r + j];
        if (r) {                                 /* inverse mix columns */
            for (int c = 0; c < 4; c++) {
                uint8_t *m = t + 4 * c, a0 = m[0], a1 = m[1], a2 = m[2], a3 = m[3];
                m[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                m[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                m[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                m[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
        }
        memcpy(b, t, 16);
    }
}

/* AES key unwrap (RFC 3394) with a 128-bit KEK: n 64-bit blocks out of n + 1; false if the check fails. */
bool wpa_aes_unwrap(const uint8_t kek[16], const uint8_t *in, size_t len, uint8_t *out)
{
    if (len < 24 || len % 8)
        return false;
    size_t n = len / 8 - 1;
    uint8_t rk[176], a[8], b[16];
    aes_expand(kek, rk);
    memcpy(a, in, 8);
    memmove(out, in + 8, n * 8);
    for (int j = 5; j >= 0; j--)
        for (size_t i = n; i >= 1; i--) {
            uint64_t t = (uint64_t)n * j + i;
            memcpy(b, a, 8);
            for (int k = 0; k < 8; k++)
                b[7 - k] ^= t >> (8 * k);
            memcpy(b + 8, out + (i - 1) * 8, 8);
            aes_decrypt(rk, b);
            memcpy(a, b, 8);
            memcpy(out + (i - 1) * 8, b + 8, 8);
        }
    for (int k = 0; k < 8; k++)
        if (a[k] != 0xA6)
            return false;
    return true;
}
