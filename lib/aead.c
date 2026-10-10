/*
 * aead.c - Authenticated encryption for TLS 1.3: ChaCha20-Poly1305
 * (RFC 8439) and AES-128-GCM (NIST SP 800-38D).
 *
 * Both encrypt with a keystream (ChaCha20 blocks, or AES applied to a
 * counter) and append a 16-byte tag: a keyed checksum of the associated
 * data and the ciphertext (Poly1305, or GHASH) that nobody without the key
 * can forge. A receiver checks the tag before trusting a single byte.
 *
 * Integer code only. AES here uses its S-box table: lookups that depend on
 * the key, which a program sharing the processor's cache could in theory
 * observe; ChaCha20-Poly1305 has no such tables and is offered first.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk/crypto.h"
#include "mk/lib.h"

typedef unsigned __int128 u128;

static uint64_t le64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

/* ---- Poly1305: a polynomial over the prime 2^130 - 5, evaluated at the
 * secret point r, plus the secret s. The 130-bit accumulator h is kept in
 * three limbs of 44, 44 and 42 bits so products fit in 128 bits. */
typedef struct { uint64_t r[3], h[3], pad[2]; uint8_t buf[16]; size_t fill; } poly_t;
#define M44 0xfffffffffffULL
#define M42 0x3ffffffffffULL

static void poly_init(poly_t *p, const uint8_t key[32])
{
    uint64_t t0 = le64(key), t1 = le64(key + 8);
    p->r[0] = t0 & 0xffc0fffffffULL;
    p->r[1] = (t0 >> 44 | t1 << 20) & 0xfffffc0ffffULL;
    p->r[2] = (t1 >> 24) & 0x00ffffffc0fULL;
    p->h[0] = p->h[1] = p->h[2] = 0;
    p->pad[0] = le64(key + 16);
    p->pad[1] = le64(key + 24);
    p->fill = 0;
}

static void poly_blocks(poly_t *p, const uint8_t *m, size_t n, uint64_t hibit)
{
    uint64_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], s1 = r1 * 20, s2 = r2 * 20;
    uint64_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2];
    for (; n >= 16; m += 16, n -= 16) {
        uint64_t t0 = le64(m), t1 = le64(m + 8);
        h0 += t0 & M44;
        h1 += (t0 >> 44 | t1 << 20) & M44;
        h2 += (t1 >> 24 & M42) | hibit;
        u128 d0 = (u128)h0 * r0 + (u128)h1 * s2 + (u128)h2 * s1;
        u128 d1 = (u128)h0 * r1 + (u128)h1 * r0 + (u128)h2 * s2;
        u128 d2 = (u128)h0 * r2 + (u128)h1 * r1 + (u128)h2 * r0;
        uint64_t c = (uint64_t)(d0 >> 44); h0 = (uint64_t)d0 & M44;
        d1 += c; c = (uint64_t)(d1 >> 44); h1 = (uint64_t)d1 & M44;
        d2 += c; c = (uint64_t)(d2 >> 42); h2 = (uint64_t)d2 & M42;
        h0 += c * 5; c = h0 >> 44; h0 &= M44; h1 += c;
    }
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2;
}

static void poly_update(poly_t *p, const void *in, size_t n)
{
    const uint8_t *m = in;
    if (p->fill) {
        size_t c = 16 - p->fill < n ? 16 - p->fill : n;
        memcpy(p->buf + p->fill, m, c);
        p->fill += c; m += c; n -= c;
        if (p->fill < 16) return;
        poly_blocks(p, p->buf, 16, 1ULL << 40);
        p->fill = 0;
    }
    poly_blocks(p, m, n & ~15UL, 1ULL << 40);
    memcpy(p->buf, m + (n & ~15UL), n & 15);
    p->fill = n & 15;
}

static void poly_pad16(poly_t *p)                 /* zeros up to a 16-byte boundary */
{
    static const uint8_t z[16];
    if (p->fill) poly_update(p, z, 16 - p->fill);
}

static void poly_final(poly_t *p, uint8_t tag[16])
{
    if (p->fill) {                                /* last partial block: 1, then zeros */
        p->buf[p->fill] = 1;
        memset(p->buf + p->fill + 1, 0, 15 - p->fill);
        poly_blocks(p, p->buf, 16, 0);
    }
    uint64_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], c;
    c = h1 >> 44; h1 &= M44; h2 += c; c = h2 >> 42; h2 &= M42; h0 += c * 5;
    c = h0 >> 44; h0 &= M44; h1 += c; c = h1 >> 44; h1 &= M44; h2 += c;
    c = h2 >> 42; h2 &= M42; h0 += c * 5; c = h0 >> 44; h0 &= M44; h1 += c;
    /* h - p = h + 5 - 2^130: keep it if it is not negative (constant time) */
    uint64_t g0 = h0 + 5; c = g0 >> 44; g0 &= M44;
    uint64_t g1 = h1 + c; c = g1 >> 44; g1 &= M44;
    uint64_t g2 = h2 + c - (1ULL << 42);
    uint64_t mask = (g2 >> 63) - 1;
    h0 = (h0 & ~mask) | (g0 & mask);
    h1 = (h1 & ~mask) | (g1 & mask);
    h2 = (h2 & ~mask) | (g2 & mask);
    uint64_t t0 = p->pad[0], t1 = p->pad[1];      /* + s */
    h0 += t0 & M44; c = h0 >> 44; h0 &= M44;
    h1 += ((t0 >> 44 | t1 << 20) & M44) + c; c = h1 >> 44; h1 &= M44;
    h2 += (t1 >> 24 & M42) + c; h2 &= M42;
    put64(tag, h0 | h1 << 44);
    put64(tag + 8, h1 >> 20 | h2 << 24);
    wipe(p, sizeof *p);
}

/* ---- ChaCha20 keystream from block counter `ctr`, xor-ed into data. */
static void chacha_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t ctr,
                       const uint8_t *in, uint8_t *out, size_t n)
{
    uint32_t k[8], no[3];
    uint8_t ks[64];
    memcpy(k, key, 32);
    memcpy(no, nonce, 12);
    for (size_t off = 0; off < n; off += 64, ctr++) {
        chacha20_block(k, ctr, no, ks);
        size_t c = n - off < 64 ? n - off : 64;
        for (size_t i = 0; i < c; i++) out[off + i] = in[off + i] ^ ks[i];
    }
    wipe(ks, sizeof ks);
    wipe(k, sizeof k);
}

static void cp_tag(const uint8_t key[32], const uint8_t nonce[12], const void *ad, size_t adlen,
                   const uint8_t *ct, size_t n, uint8_t tag[16])
{
    uint8_t pk[64], lens[16];
    uint32_t k[8], no[3];
    memcpy(k, key, 32);
    memcpy(no, nonce, 12);
    chacha20_block(k, 0, no, pk);                 /* block 0: the one-time Poly1305 key */
    poly_t p;
    poly_init(&p, pk);
    poly_update(&p, ad, adlen); poly_pad16(&p);
    poly_update(&p, ct, n);     poly_pad16(&p);
    put64(lens, adlen); put64(lens + 8, n);
    poly_update(&p, lens, 16);
    poly_final(&p, tag);
    wipe(pk, sizeof pk);
    wipe(k, sizeof k);
}

/* ---- AES-128 (FIPS 197), encryption only: GCM never decrypts with AES. */
static const uint8_t SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static uint32_t subword(uint32_t w)
{
    return (uint32_t)SBOX[w >> 24] << 24 | (uint32_t)SBOX[w >> 16 & 255] << 16 |
           (uint32_t)SBOX[w >> 8 & 255] << 8 | SBOX[w & 255];
}

static void aes_expand(uint32_t rk[44], const uint8_t key[16])
{
    static const uint8_t rcon[10] = { 1, 2, 4, 8, 16, 32, 64, 128, 0x1b, 0x36 };
    for (int i = 0; i < 4; i++)
        rk[i] = (uint32_t)key[4 * i] << 24 | key[4 * i + 1] << 16 | key[4 * i + 2] << 8 | key[4 * i + 3];
    for (int i = 4; i < 44; i++) {
        uint32_t t = rk[i - 1];
        if (i % 4 == 0) t = subword(t << 8 | t >> 24) ^ (uint32_t)rcon[i / 4 - 1] << 24;
        rk[i] = rk[i - 4] ^ t;
    }
}

static uint8_t xt(uint8_t b) { return (uint8_t)(b << 1 ^ ((b >> 7) * 0x1b)); }   /* times 2 */

static void aes_encrypt(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ (uint8_t)(rk[i / 4] >> (24 - 8 * (i % 4)));
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < 16; i++) t[i] = SBOX[s[(i + 4 * (i % 4)) % 16]];   /* SubBytes + ShiftRows */
        if (r < 10)
            for (int c = 0; c < 4; c++) {                                     /* MixColumns */
                uint8_t *p = t + 4 * c, a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3], x = a0 ^ a1 ^ a2 ^ a3;
                p[0] ^= x ^ xt(a0 ^ a1); p[1] ^= x ^ xt(a1 ^ a2);
                p[2] ^= x ^ xt(a2 ^ a3); p[3] ^= x ^ xt(a3 ^ a0);
            }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ (uint8_t)(rk[4 * r + i / 4] >> (24 - 8 * (i % 4)));
    }
    memcpy(out, s, 16);
}

/* ---- GHASH: multiplication in GF(2^128) by H, bit by bit and in
 * constant time (masks instead of branches). Blocks are big-endian. */
static uint64_t be64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; return v; }
static void putbe64(uint8_t *p, uint64_t v) { for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; } }

static void gmul(uint64_t *xh, uint64_t *xl, uint64_t hh, uint64_t hl)
{
    uint64_t zh = 0, zl = 0, vh = hh, vl = hl, x[2] = { *xh, *xl };
    for (int i = 0; i < 128; i++) {
        uint64_t bit = x[i / 64] >> (63 - i % 64) & 1, m = -bit;
        zh ^= vh & m; zl ^= vl & m;
        uint64_t lsb = -(vl & 1);
        vl = vl >> 1 | vh << 63;
        vh = (vh >> 1) ^ (0xe100000000000000ULL & lsb);
    }
    *xh = zh; *xl = zl;
}

static void ghash(const aead_t *a, uint64_t *yh, uint64_t *yl, const uint8_t *p, size_t n)
{
    uint8_t b[16];
    for (size_t off = 0; off < n; off += 16) {
        size_t c = n - off < 16 ? n - off : 16;
        memset(b, 0, 16);
        memcpy(b, p + off, c);
        *yh ^= be64(b); *yl ^= be64(b + 8);
        gmul(yh, yl, a->hh, a->hl);
    }
}

static void gcm_ctr(const aead_t *a, const uint8_t nonce[12], uint32_t ctr, const uint8_t *in, uint8_t *out, size_t n)
{
    uint8_t cb[16], ks[16];
    memcpy(cb, nonce, 12);
    for (size_t off = 0; off < n; off += 16, ctr++) {
        cb[12] = ctr >> 24; cb[13] = ctr >> 16; cb[14] = ctr >> 8; cb[15] = (uint8_t)ctr;
        aes_encrypt(a->rk, cb, ks);
        size_t c = n - off < 16 ? n - off : 16;
        for (size_t i = 0; i < c; i++) out[off + i] = in[off + i] ^ ks[i];
    }
}

static void gcm_tag(const aead_t *a, const uint8_t nonce[12], const void *ad, size_t adlen,
                    const uint8_t *ct, size_t n, uint8_t tag[16])
{
    uint64_t yh = 0, yl = 0;
    uint8_t l[16], j0[16];
    ghash(a, &yh, &yl, ad, adlen);
    ghash(a, &yh, &yl, ct, n);
    putbe64(l, (uint64_t)adlen * 8); putbe64(l + 8, (uint64_t)n * 8);
    ghash(a, &yh, &yl, l, 16);
    gcm_ctr(a, nonce, 1, (const uint8_t[16]){ 0 }, j0, 16);   /* E(K, J0) */
    putbe64(tag, yh); putbe64(tag + 8, yl);
    for (int i = 0; i < 16; i++) tag[i] ^= j0[i];
}

/* ---- The common interface. */
void aead_init(aead_t *a, int alg, const uint8_t *key)
{
    memset(a, 0, sizeof *a);
    a->alg = alg;
    if (alg == AEAD_CHACHA20_POLY1305) { memcpy(a->key, key, 32); return; }
    aes_expand(a->rk, key);
    uint8_t h[16];
    aes_encrypt(a->rk, (const uint8_t[16]){ 0 }, h);         /* H = E(K, 0) */
    a->hh = be64(h); a->hl = be64(h + 8);
}

void aead_seal(const aead_t *a, const uint8_t nonce[12], const void *ad, size_t adlen,
               const void *in, size_t n, uint8_t *out)
{
    if (a->alg == AEAD_CHACHA20_POLY1305) {
        chacha_xor(a->key, nonce, 1, in, out, n);
        cp_tag(a->key, nonce, ad, adlen, out, n, out + n);
    } else {
        gcm_ctr(a, nonce, 2, in, out, n);
        gcm_tag(a, nonce, ad, adlen, out, n, out + n);
    }
}

int aead_open(const aead_t *a, const uint8_t nonce[12], const void *ad, size_t adlen,
              const void *in, size_t n, uint8_t *out)
{
    uint8_t tag[16];
    const uint8_t *c = in;
    if (n < 16) return -1;
    n -= 16;
    if (a->alg == AEAD_CHACHA20_POLY1305) cp_tag(a->key, nonce, ad, adlen, c, n, tag);
    else gcm_tag(a, nonce, ad, adlen, c, n, tag);
    if (!ct_equal(tag, c + n, 16)) return -1;                 /* forged or damaged */
    if (a->alg == AEAD_CHACHA20_POLY1305) chacha_xor(a->key, nonce, 1, c, out, n);
    else gcm_ctr(a, nonce, 2, c, out, n);
    return 0;
}
