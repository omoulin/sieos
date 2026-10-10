/*
 * x25519.c - The X25519 key exchange (RFC 7748). Each side picks a random
 * 32-byte secret, sends secret * base point, and multiplies the other's
 * point by its own secret: both get the same shared value, which an
 * eavesdropper cannot compute.
 *
 * Numbers modulo p = 2^255 - 19 are kept in five 51-bit limbs, so limb
 * products fit in 128 bits. The Montgomery ladder does the same operations
 * whatever the secret's bits are (a constant-time swap instead of branches).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk/crypto.h"
#include "mk/lib.h"

typedef unsigned __int128 u128;
typedef uint64_t fe[5];
#define M51 0x7ffffffffffffULL

static void fe_load(fe r, const uint8_t b[32])
{
    uint64_t w[4];
    memcpy(w, b, 32);
    r[0] = w[0] & M51;
    r[1] = (w[0] >> 51 | w[1] << 13) & M51;
    r[2] = (w[1] >> 38 | w[2] << 26) & M51;
    r[3] = (w[2] >> 25 | w[3] << 39) & M51;
    r[4] = w[3] >> 12 & M51;                      /* the top bit is ignored */
}

static void fe_carry(fe r)
{
    for (int i = 0; i < 4; i++) { r[i + 1] += r[i] >> 51; r[i] &= M51; }
    r[0] += 19 * (r[4] >> 51); r[4] &= M51;
    r[1] += r[0] >> 51; r[0] &= M51;
}

static void fe_add(fe r, const fe a, const fe b) { for (int i = 0; i < 5; i++) r[i] = a[i] + b[i]; fe_carry(r); }
static void fe_sub(fe r, const fe a, const fe b)  /* + 4p, so nothing goes negative */
{
    r[0] = a[0] + 0x1fffffffffffb4 - b[0];
    for (int i = 1; i < 5; i++) r[i] = a[i] + 0x1ffffffffffffc - b[i];
    fe_carry(r);
}

static void fe_mul(fe r, const fe a, const fe b)
{
    u128 t[5];
    uint64_t b19[5];
    for (int i = 1; i < 5; i++) b19[i] = 19 * b[i];   /* 2^255 = 19 (mod p) */
    t[0] = (u128)a[0] * b[0] + (u128)a[1] * b19[4] + (u128)a[2] * b19[3] + (u128)a[3] * b19[2] + (u128)a[4] * b19[1];
    t[1] = (u128)a[0] * b[1] + (u128)a[1] * b[0] + (u128)a[2] * b19[4] + (u128)a[3] * b19[3] + (u128)a[4] * b19[2];
    t[2] = (u128)a[0] * b[2] + (u128)a[1] * b[1] + (u128)a[2] * b[0] + (u128)a[3] * b19[4] + (u128)a[4] * b19[3];
    t[3] = (u128)a[0] * b[3] + (u128)a[1] * b[2] + (u128)a[2] * b[1] + (u128)a[3] * b[0] + (u128)a[4] * b19[4];
    t[4] = (u128)a[0] * b[4] + (u128)a[1] * b[3] + (u128)a[2] * b[2] + (u128)a[3] * b[1] + (u128)a[4] * b[0];
    for (int i = 0; i < 4; i++) { t[i + 1] += (uint64_t)(t[i] >> 51); t[i] &= M51; }
    uint64_t c = (uint64_t)(t[4] >> 51);
    for (int i = 0; i < 5; i++) r[i] = (uint64_t)t[i] & M51;
    r[0] += 19 * c;
    r[1] += r[0] >> 51; r[0] &= M51;
}

static void fe_mul_small(fe r, const fe a, uint64_t k)
{
    u128 c = 0;
    for (int i = 0; i < 5; i++) { c += (u128)a[i] * k; r[i] = (uint64_t)c & M51; c >>= 51; }
    r[0] += 19 * (uint64_t)c;
    r[1] += r[0] >> 51; r[0] &= M51;
}

static void fe_inv(fe r, const fe z)              /* z^(p-2), p-2 = 2^255 - 21 */
{
    fe x;
    memcpy(x, z, sizeof x);
    for (int i = 253; i >= 0; i--) {              /* bits 254..0 of p-2: all 1 except 2 and 4 */
        fe_mul(x, x, x);
        if (i != 2 && i != 4) fe_mul(x, x, z);
    }
    memcpy(r, x, sizeof x);
}

static void fe_store(uint8_t b[32], const fe a)
{
    fe t;
    memcpy(t, a, sizeof t);
    fe_carry(t); fe_carry(t);
    /* subtract p if t >= p: add 19, see whether bit 255 appears */
    uint64_t c = (t[0] + 19) >> 51;
    for (int i = 1; i < 5; i++) c = (t[i] + c) >> 51;
    t[0] += 19 * c;
    for (int i = 0; i < 4; i++) { t[i + 1] += t[i] >> 51; t[i] &= M51; }
    t[4] &= M51;
    uint64_t w[4] = { t[0] | t[1] << 51, t[1] >> 13 | t[2] << 38, t[2] >> 26 | t[3] << 25, t[3] >> 39 | t[4] << 12 };
    memcpy(b, w, 32);
}

static void cswap(fe a, fe b, uint64_t bit)
{
    uint64_t m = -bit;
    for (int i = 0; i < 5; i++) { uint64_t t = m & (a[i] ^ b[i]); a[i] ^= t; b[i] ^= t; }
}

int x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    uint8_t k[32];
    memcpy(k, scalar, 32);
    k[0] &= 248; k[31] &= 127; k[31] |= 64;        /* "clamping" */
    fe x1, x2 = { 1 }, z2 = { 0 }, x3, z3 = { 1 }, a, aa, b, bb, e, c, d, da, cb;
    fe_load(x1, point);
    memcpy(x3, x1, sizeof x3);
    uint64_t swap = 0;
    for (int t = 254; t >= 0; t--) {
        uint64_t kt = k[t / 8] >> (t % 8) & 1;
        swap ^= kt;
        cswap(x2, x3, swap); cswap(z2, z3, swap);
        swap = kt;
        fe_add(a, x2, z2); fe_mul(aa, a, a);
        fe_sub(b, x2, z2); fe_mul(bb, b, b);
        fe_sub(e, aa, bb);
        fe_add(c, x3, z3); fe_sub(d, x3, z3);
        fe_mul(da, d, a); fe_mul(cb, c, b);
        fe_add(x3, da, cb); fe_mul(x3, x3, x3);
        fe_sub(z3, da, cb); fe_mul(z3, z3, z3); fe_mul(z3, z3, x1);
        fe_mul(x2, aa, bb);
        fe_mul_small(z2, e, 121665); fe_add(z2, z2, aa); fe_mul(z2, z2, e);
    }
    cswap(x2, x3, swap); cswap(z2, z3, swap);
    fe_inv(z2, z2);
    fe_mul(x2, x2, z2);
    fe_store(out, x2);
    wipe(k, sizeof k);
    uint8_t any = 0;
    for (int i = 0; i < 32; i++) any |= out[i];
    return any ? 0 : -1;
}

int x25519_base(uint8_t out[32], const uint8_t scalar[32])
{
    static const uint8_t nine[32] = { 9 };
    return x25519(out, scalar, nine);
}
