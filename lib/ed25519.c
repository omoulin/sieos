/*
 * ed25519.c - Ed25519 signatures (RFC 8032): SSH's host and user keys.
 *
 * The curve -x^2 + y^2 = 1 + d x^2 y^2 over the field of X25519 (whose
 * arithmetic this file shares, lib/fe25519.h). A secret key is 32 random
 * bytes; SHA-512 of it gives the secret scalar a and a prefix. The public
 * key is A = aB (B the standard base point). A signature of M is R = rB,
 * with r = SHA-512(prefix || M) (no random number needed: the same message
 * always gets the same r), and S = r + SHA-512(R || A || M) a modulo the
 * group order L. A verifier checks SB = R + SHA-512(R || A || M) A.
 *
 * Points are kept in extended coordinates (X:Y:Z:T), x = X/Z, y = Y/Z,
 * xy = T/Z, where one addition formula serves for doubling too. Multiplying
 * a point by a scalar does the same operations whatever the scalar's bits
 * (constant-time swaps), so signing reveals nothing of the key through its
 * timing.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk/crypto.h"
#include "mk/lib.h"
#include "fe25519.h"

typedef struct { fe x, y, z, t; } ge;

static const fe D  = { 0x34dca135978a3, 0x1a8283b156ebd, 0x5e7a26001c029, 0x739c663a03cbb, 0x52036cee2b6ff };
static const fe D2 = { 0x69b9426b2f159, 0x35050762add7a, 0x3cf44c0038052, 0x6738cc7407977, 0x2406d9dc56dff };
static const fe SQRTM1 = { 0x61b274a0ea0b0, 0xd5a5fc8f189d, 0x7ef5e9cbd0c60, 0x78595a6804c9e, 0x2b8324804fc1d };
static const uint8_t L[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9,
                               0xde, 0x14, [31] = 0x10 };      /* 2^252 + 27742317777372353535851937790883648493 */

static void ge_add(ge *r, const ge *p, const ge *q)  /* RFC 8032, 5.1.4 */
{
    fe a, b, c, d, e, f, g, h;
    fe_sub(a, p->y, p->x); fe_sub(h, q->y, q->x); fe_mul(a, a, h);
    fe_add(b, p->y, p->x); fe_add(h, q->y, q->x); fe_mul(b, b, h);
    fe_mul(c, p->t, q->t); fe_mul(c, c, D2);
    fe_mul(d, p->z, q->z); fe_add(d, d, d);
    fe_sub(e, b, a); fe_sub(f, d, c); fe_add(g, d, c); fe_add(h, b, a);
    fe_mul(r->x, e, f); fe_mul(r->y, g, h); fe_mul(r->t, e, h); fe_mul(r->z, f, g);
}

/* r = s p (s: 32 little-endian bytes). r - q stays p: each step either
 * doubles r and adds p to q, or the other way round, after a swap. */
static void ge_mul(ge *r, const ge *p, const uint8_t s[32])
{
    ge q = *p;
    *r = (ge){ .y = { 1 }, .z = { 1 } };          /* the neutral point (0, 1) */
    for (int i = 255; i >= 0; i--) {
        uint64_t b = s[i / 8] >> (i & 7) & 1;
        fe_cswap(r->x, q.x, b); fe_cswap(r->y, q.y, b); fe_cswap(r->z, q.z, b); fe_cswap(r->t, q.t, b);
        ge_add(&q, &q, r);
        ge_add(r, r, r);
        fe_cswap(r->x, q.x, b); fe_cswap(r->y, q.y, b); fe_cswap(r->z, q.z, b); fe_cswap(r->t, q.t, b);
    }
}

/* A point as 32 bytes: y, and x's lowest bit in the top bit. */
static void ge_pack(uint8_t out[32], const ge *p)
{
    fe zi, x, y;
    uint8_t xb[32];
    fe_inv(zi, p->z);
    fe_mul(x, p->x, zi); fe_mul(y, p->y, zi);
    fe_store(out, y); fe_store(xb, x);
    out[31] |= (uint8_t)(xb[0] << 7);
}

static int fe_same(const fe a, const fe b)
{
    uint8_t x[32], y[32];
    fe_store(x, a); fe_store(y, b);
    return !memcmp(x, y, 32);
}

/* The other way (RFC 8032, 5.1.3): x from y, x^2 = (y^2 - 1) / (d y^2 + 1),
 * a square root found as u v^3 (u v^7)^((p-5)/8). -1 if not a point. */
static int ge_unpack(ge *p, const uint8_t in[32])
{
    fe u, v, v3, x, t, one = { 1 }, zero = { 0 };
    uint8_t b[32];
    fe_load(p->y, in);
    fe_store(b, p->y);
    b[31] |= in[31] & 0x80;
    if (memcmp(b, in, 32)) return -1;             /* y >= p: not the one encoding */
    fe_mul(u, p->y, p->y); fe_mul(v, u, D);
    fe_sub(u, u, one); fe_add(v, v, one);
    fe_mul(v3, v, v); fe_mul(v3, v3, v);
    fe_mul(x, v3, v3); fe_mul(x, x, v); fe_mul(x, x, u);
    fe_pow(x, x, 251, 1 << 1);                    /* ^(2^252 - 3) */
    fe_mul(x, x, v3); fe_mul(x, x, u);
    fe_mul(t, x, x); fe_mul(t, t, v);
    if (!fe_same(t, u)) {
        fe_add(t, t, u);                          /* v x^2 = -u: x times sqrt(-1) */
        if (!fe_same(t, zero)) return -1;
        fe_mul(x, x, SQRTM1);
    }
    fe_store(b, x);
    if ((b[0] & 1) != in[31] >> 7) {
        if (fe_same(x, zero)) return -1;          /* "-0" */
        fe_sub(x, zero, x);
    }
    memcpy(p->x, x, sizeof x);
    memcpy(p->z, one, sizeof one);
    fe_mul(p->t, x, p->y);
    return 0;
}

static void base(ge *b)
{
    static const uint8_t enc[32] = { 0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
        0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66 };
    ge_unpack(b, enc);                            /* y = 4/5 */
}

/* x (64 limbs of about a byte each, signed) modulo L, as 32 bytes. Limb i
 * >= 32 is worth x[i] 2^(8i) = 16 x[i] 2^(8i-260) 2^252, and 2^252 = -(L -
 * 2^252) modulo L: folded into the limbs below, then the top 4 bits too. */
static void mod_l(uint8_t r[32], int64_t x[64])
{
    int64_t c;
    for (int i = 63; i >= 32; i--) {
        int j;
        c = 0;
        for (j = i - 32; j < i - 12; j++) {
            x[j] += c - 16 * x[i] * L[j - (i - 32)];
            c = (x[j] + 128) >> 8;
            x[j] -= c * 256;
        }
        x[j] += c;
        x[i] = 0;
    }
    c = 0;
    for (int j = 0; j < 32; j++) {
        x[j] += c - (x[31] >> 4) * L[j];
        c = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; j++) x[j] -= c * L[j];
    for (int i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 255);
    }
}

/* SHA-512 of a || b || m, modulo L. */
static void hash_l(uint8_t r[32], const uint8_t *a, const uint8_t *b, const void *m, size_t n)
{
    uint8_t h[64];
    int64_t x[64];
    sha_t s;
    sha_init(&s, SHA512);
    sha_update(&s, a, 32);
    if (b) sha_update(&s, b, 32);
    sha_update(&s, m, n);
    sha_final(&s, h);
    for (int i = 0; i < 64; i++) x[i] = h[i];
    mod_l(r, x);
}

/* The secret scalar a (clamped) and the prefix, from the secret key. */
static void expand(uint8_t h[64], const uint8_t seed[32])
{
    sha(SHA512, h, seed, 32);
    h[0] &= 248; h[31] &= 127; h[31] |= 64;
}

void ed25519_public(uint8_t pub[32], const uint8_t seed[32])
{
    uint8_t h[64];
    ge b, a;
    expand(h, seed);
    base(&b);
    ge_mul(&a, &b, h);
    ge_pack(pub, &a);
    wipe(h, sizeof h);
}

void ed25519_sign(uint8_t sig[64], const void *m, size_t n, const uint8_t seed[32], const uint8_t pub[32])
{
    uint8_t h[64], r[32], k[32];
    int64_t x[64] = { 0 };
    ge b, p;
    expand(h, seed);
    hash_l(r, h + 32, 0, m, n);                   /* r = SHA-512(prefix || M) */
    base(&b);
    ge_mul(&p, &b, r);
    ge_pack(sig, &p);                             /* R */
    hash_l(k, sig, pub, m, n);
    for (int i = 0; i < 32; i++) x[i] = r[i];     /* S = r + k a */
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++) x[i + j] += (int64_t)k[i] * h[j];
    mod_l(sig + 32, x);
    wipe(h, sizeof h); wipe(r, sizeof r); wipe(x, sizeof x);
}

int ed25519_verify(const uint8_t sig[64], const void *m, size_t n, const uint8_t pub[32])
{
    uint8_t k[32], chk[32];
    ge a, b, sb, ka;
    int i = 31;
    while (i > 0 && sig[32 + i] == L[i]) i--;
    if (sig[32 + i] >= L[i]) return -1;           /* S >= L: not a valid signature */
    if (ge_unpack(&a, pub)) return -1;
    fe zero = { 0 };
    fe_sub(a.x, zero, a.x); fe_sub(a.t, zero, a.t);   /* -A */
    hash_l(k, sig, pub, m, n);
    base(&b);
    ge_mul(&sb, &b, sig + 32);
    ge_mul(&ka, &a, k);
    ge_add(&sb, &sb, &ka);                        /* SB - kA, which must be R */
    ge_pack(chk, &sb);
    return memcmp(chk, sig, 32) ? -1 : 0;
}
