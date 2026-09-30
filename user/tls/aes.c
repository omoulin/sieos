/*
 * aes.c - AES-128/256 encryption (FIPS 197) and GCM (NIST SP 800-38D).
 *
 * Only the forward cipher is needed: GCM uses AES in counter mode.
 * The S-box is computed once from its definition (inverse in GF(2^8)
 * followed by the affine map) rather than stored.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "crypto.h"

static uint8_t sbox[256];
static bool sbox_ready;

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) {
        if (b & 1)
            r ^= a;
        a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
        b >>= 1;
    }
    return r;
}

static void sbox_init(void)
{
    for (int x = 0; x < 256; x++) {
        uint8_t inv = 0;
        if (x)
            for (int y = 1; y < 256; y++)
                if (gmul((uint8_t)x, (uint8_t)y) == 1) {
                    inv = (uint8_t)y;
                    break;
                }
        uint8_t s = inv;
        for (int i = 1; i <= 4; i++)
            s ^= (uint8_t)((inv << i) | (inv >> (8 - i)));
        sbox[x] = s ^ 0x63;
    }
    sbox_ready = true;
}

static uint32_t sub_word(uint32_t w)
{
    return (uint32_t)sbox[w >> 24] << 24 | (uint32_t)sbox[(w >> 16) & 255] << 16 |
           (uint32_t)sbox[(w >> 8) & 255] << 8 | sbox[w & 255];
}

void aes_setkey(struct aes_key *k, const uint8_t *key, size_t keylen)
{
    if (!sbox_ready)
        sbox_init();
    int nk = (int)keylen / 4;
    k->rounds = nk + 6;
    int total = 4 * (k->rounds + 1);
    for (int i = 0; i < nk; i++)
        k->rk[i] = (uint32_t)key[4 * i] << 24 | (uint32_t)key[4 * i + 1] << 16 | (uint32_t)key[4 * i + 2] << 8 |
                   key[4 * i + 3];
    uint8_t rcon = 1;
    for (int i = nk; i < total; i++) {
        uint32_t t = k->rk[i - 1];
        if (i % nk == 0) {
            t = sub_word((t << 8) | (t >> 24)) ^ ((uint32_t)rcon << 24);
            rcon = gmul(rcon, 2);
        } else if (nk > 6 && i % nk == 4) {
            t = sub_word(t);
        }
        k->rk[i] = k->rk[i - nk] ^ t;
    }
}

void aes_encrypt_block(const struct aes_key *k, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    for (int i = 0; i < 16; i++)
        s[i] = in[i] ^ (uint8_t)(k->rk[i / 4] >> (24 - 8 * (i % 4)));
    for (int r = 1; r <= k->rounds; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++)                   /* SubBytes + ShiftRows */
            t[i] = sbox[s[(i + 4 * (i % 4)) % 16]];
        if (r != k->rounds) {                          /* MixColumns */
            for (int c = 0; c < 4; c++) {
                uint8_t *col = t + 4 * c;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3;
                col[1] = a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3;
                col[2] = a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3);
                col[3] = gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2);
            }
        }
        for (int i = 0; i < 16; i++)
            s[i] = t[i] ^ (uint8_t)(k->rk[4 * r + i / 4] >> (24 - 8 * (i % 4)));
    }
    memcpy(out, s, 16);
}

/* ---------------- GCM ---------------- */

/* x = x * h in GF(2^128) with the GCM bit order. */
static void ghash_mul(uint8_t x[16], const uint8_t h[16])
{
    uint8_t z[16] = { 0 }, v[16];
    memcpy(v, h, 16);
    for (int i = 0; i < 128; i++) {
        if (x[i / 8] & (0x80 >> (i % 8)))
            for (int j = 0; j < 16; j++)
                z[j] ^= v[j];
        bool lsb = v[15] & 1;
        for (int j = 15; j > 0; j--)
            v[j] = (uint8_t)((v[j] >> 1) | (v[j - 1] << 7));
        v[0] >>= 1;
        if (lsb)
            v[0] ^= 0xE1;
    }
    memcpy(x, z, 16);
}

static void ghash_update(uint8_t y[16], const uint8_t h[16], const uint8_t *data, size_t len)
{
    while (len) {
        size_t n = len < 16 ? len : 16;
        for (size_t i = 0; i < n; i++)
            y[i] ^= data[i];
        ghash_mul(y, h);
        data += n;
        len -= n;
    }
}

void gcm_init(struct gcm *g, const uint8_t *key, size_t keylen)
{
    uint8_t zero[16] = { 0 };
    aes_setkey(&g->aes, key, keylen);
    aes_encrypt_block(&g->aes, zero, g->h);
}

static void gcm_ctr(const struct gcm *g, const uint8_t iv[12], const uint8_t *in, size_t len, uint8_t *out)
{
    uint8_t ctr[16], ks[16];
    memcpy(ctr, iv, 12);
    uint32_t n = 2;                                    /* counter 1 is used for the tag */
    for (size_t off = 0; off < len; off += 16, n++) {
        ctr[12] = (uint8_t)(n >> 24);
        ctr[13] = (uint8_t)(n >> 16);
        ctr[14] = (uint8_t)(n >> 8);
        ctr[15] = (uint8_t)n;
        aes_encrypt_block(&g->aes, ctr, ks);
        size_t m = len - off < 16 ? len - off : 16;
        for (size_t i = 0; i < m; i++)
            out[off + i] = in[off + i] ^ ks[i];
    }
}

static void gcm_tag(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t aadlen,
                    const uint8_t *ct, size_t len, uint8_t tag[16])
{
    uint8_t y[16] = { 0 }, lens[16], j0[16];
    ghash_update(y, g->h, aad, aadlen);
    ghash_update(y, g->h, ct, len);
    uint64_t abits = (uint64_t)aadlen * 8, cbits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) {
        lens[i] = (uint8_t)(abits >> (56 - 8 * i));
        lens[8 + i] = (uint8_t)(cbits >> (56 - 8 * i));
    }
    ghash_update(y, g->h, lens, 16);
    memcpy(j0, iv, 12);
    j0[12] = j0[13] = j0[14] = 0;
    j0[15] = 1;
    uint8_t ek[16];
    aes_encrypt_block(&g->aes, j0, ek);
    for (int i = 0; i < 16; i++)
        tag[i] = ek[i] ^ y[i];
}

void gcm_seal(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t aadlen, const uint8_t *in,
              size_t len, uint8_t *out, uint8_t tag[16])
{
    gcm_ctr(g, iv, in, len, out);
    gcm_tag(g, iv, aad, aadlen, out, len, tag);
}

bool gcm_open(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t aadlen, const uint8_t *in,
              size_t len, const uint8_t tag[16], uint8_t *out)
{
    uint8_t expect[16];
    gcm_tag(g, iv, aad, aadlen, in, len, expect);
    if (!ct_equal(expect, tag, 16))
        return false;
    gcm_ctr(g, iv, in, len, out);
    return true;
}
