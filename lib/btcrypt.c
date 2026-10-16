/*
 * btcrypt.c - The Bluetooth pairing functions the host computes (Core spec
 * Vol 3 Part H 2.2): LE Secure Connections' f4, f5, f6 and g2 on AES-CMAC
 * (RFC 4493), legacy pairing's c1 and s1 on AES-128, and the P-256 key
 * exchange (pubkey.c) in SMP's byte order. Classic Secure Simple Pairing
 * needs nothing here: the controller does its arithmetic.
 *
 * The spec writes these functions most significant octet first; SMP sends
 * every value least significant first. Each function takes and gives SMP's
 * order: its message is laid out back to front, then turned around once.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk/crypto.h"
#include "mk/lib.h"

static void rev(uint8_t *d, const uint8_t *s, int n) { for (int i = 0; i < n; i++) d[i] = s[n - 1 - i]; }
static void dbl(uint8_t *l)                       /* times x in GF(2^128) (CMAC's subkeys) */
{
    int c = l[0] >> 7;
    for (int i = 0; i < 15; i++) l[i] = (uint8_t)(l[i] << 1 | l[i + 1] >> 7);
    l[15] = (uint8_t)(l[15] << 1 ^ (c ? 0x87 : 0));
}

/* AES-CMAC with key over m (n bytes, at most 80 here). */
void bt_cmac(uint8_t mac[16], const uint8_t key[16], const uint8_t *m, int n)
{
    uint32_t rk[60];
    uint8_t k[16], l[16] = { 0 }, x[16] = { 0 }, b[16];
    rev(k, key, 16);
    aes_expand(rk, k, 4);
    aes_encrypt(rk, 10, l, l);                    /* L = E(K, 0) */
    int nb = n ? (n + 15) / 16 : 1;
    for (int i = 0; i < nb; i++) {
        for (int j = 0, at = 16 * i; j < 16; j++, at++) b[j] = at < n ? m[n - 1 - at] : at == n ? 0x80 : 0;
        if (i == nb - 1) {                        /* the last block: K1 if whole, else padded and K2 */
            dbl(l);
            if (!n || n % 16) dbl(l);
            for (int j = 0; j < 16; j++) b[j] ^= l[j];
        }
        for (int j = 0; j < 16; j++) b[j] ^= x[j];
        aes_encrypt(rk, 10, b, x);
    }
    rev(mac, x, 16);
    wipe(rk, sizeof rk);
}

void bt_f4(uint8_t out[16], const uint8_t u[32], const uint8_t v[32], const uint8_t x[16], uint8_t z)
{
    uint8_t m[65];
    m[0] = z; memcpy(m + 1, v, 32); memcpy(m + 33, u, 32);
    bt_cmac(out, x, m, 65);
}

void bt_f5(uint8_t mackey[16], uint8_t ltk[16], const uint8_t w[32], const uint8_t n1[16], const uint8_t n2[16],
           const uint8_t a1[7], const uint8_t a2[7])
{
    static const uint8_t salt[16] = { 0xbe, 0x83, 0x60, 0x5a, 0xdb, 0x0b, 0x37, 0x60, 0x38, 0xa5, 0xf5, 0xaa, 0x91, 0x83, 0x88, 0x6c };
    uint8_t t[16], m[53] = { 0x00, 0x01 };        /* length 256, A2, A1, N2, N1, "btle", counter */
    bt_cmac(t, salt, w, 32);
    memcpy(m + 2, a2, 7); memcpy(m + 9, a1, 7); memcpy(m + 16, n2, 16); memcpy(m + 32, n1, 16);
    memcpy(m + 48, "eltb", 4);
    m[52] = 0; bt_cmac(mackey, t, m, 53);
    m[52] = 1; bt_cmac(ltk, t, m, 53);
    wipe(t, 16);
}

void bt_f6(uint8_t out[16], const uint8_t w[16], const uint8_t n1[16], const uint8_t n2[16], const uint8_t r[16],
           const uint8_t io[3], const uint8_t a1[7], const uint8_t a2[7])
{
    uint8_t m[65];
    memcpy(m, a2, 7); memcpy(m + 7, a1, 7); memcpy(m + 14, io, 3); memcpy(m + 17, r, 16);
    memcpy(m + 33, n2, 16); memcpy(m + 49, n1, 16);
    bt_cmac(out, w, m, 65);
}

uint32_t bt_g2(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16], const uint8_t y[16])
{
    uint8_t m[80], o[16];
    memcpy(m, y, 16); memcpy(m + 16, v, 32); memcpy(m + 48, u, 32);
    bt_cmac(o, x, m, 80);
    return ((uint32_t)o[0] | o[1] << 8 | o[2] << 16 | (uint32_t)o[3] << 24) % 1000000;
}

static void e(uint8_t out[16], const uint8_t key[16], const uint8_t in[16])   /* AES-128, SMP's order */
{
    uint32_t rk[60];
    uint8_t k[16], b[16];
    rev(k, key, 16); rev(b, in, 16);
    aes_expand(rk, k, 4);
    aes_encrypt(rk, 10, b, b);
    rev(out, b, 16);
    wipe(rk, sizeof rk);
}

void bt_c1(uint8_t out[16], const uint8_t k[16], const uint8_t r[16], const uint8_t preq[7], const uint8_t pres[7],
           uint8_t iat, const uint8_t ia[6], uint8_t rat, const uint8_t ra[6])
{
    uint8_t p[16] = { iat, rat };
    memcpy(p + 2, preq, 7); memcpy(p + 9, pres, 7);
    for (int i = 0; i < 16; i++) p[i] ^= r[i];
    e(out, k, p);
    memset(p, 0, 16); memcpy(p, ra, 6); memcpy(p + 6, ia, 6);
    for (int i = 0; i < 16; i++) p[i] ^= out[i];
    e(out, k, p);
}

void bt_s1(uint8_t out[16], const uint8_t k[16], const uint8_t r1[16], const uint8_t r2[16])
{
    uint8_t r[16];
    memcpy(r, r2, 8); memcpy(r + 8, r1, 8);
    e(out, k, r);
}

int bt_p256_public(uint8_t pub[64], const uint8_t priv[32])
{
    uint8_t k[32], p[65];
    rev(k, priv, 32);
    int r = ec_public(P256, p, k);
    rev(pub, p + 1, 32); rev(pub + 32, p + 33, 32);
    wipe(k, 32);
    return r;
}

int bt_p256_dh(uint8_t dh[32], const uint8_t priv[32], const uint8_t peer[64])
{
    uint8_t k[32], p[65] = { 4 }, o[32];
    rev(k, priv, 32); rev(p + 1, peer, 32); rev(p + 33, peer + 32, 32);
    int r = ec_shared(P256, o, k, p, 65);
    rev(dh, o, 32);
    wipe(k, 32); wipe(o, 32);
    return r;
}
