/*
 * p256.c - ECDH on NIST P-256 (secp256r1, SEC 1 / FIPS 186-4).
 *
 * Used for TLS key exchange with servers that only accept NIST curves.
 * Field arithmetic in Montgomery form on 8 x 32-bit limbs; points in
 * Jacobian coordinates (a = -3 doubling).  Only ephemeral keys are used,
 * so the scalar ladder does the same work for every bit but point
 * addition has data-dependent branches for the special cases.
 */
#include "crypto.h"

typedef uint32_t fe[8];                     /* little-endian limbs */

static const fe P = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000,
                      0x00000000, 0x00000000, 0x00000001, 0xFFFFFFFF };
static const uint8_t B_BYTES[32] = {
    0x5A, 0xC6, 0x35, 0xD8, 0xAA, 0x3A, 0x93, 0xE7, 0xB3, 0xEB, 0xBD, 0x55, 0x76, 0x98, 0x86, 0xBC,
    0x65, 0x1D, 0x06, 0xB0, 0xCC, 0x53, 0xB0, 0xF6, 0x3B, 0xCE, 0x3C, 0x3E, 0x27, 0xD2, 0x60, 0x4B };
static const uint8_t GX[32] = {
    0x6B, 0x17, 0xD1, 0xF2, 0xE1, 0x2C, 0x42, 0x47, 0xF8, 0xBC, 0xE6, 0xE5, 0x63, 0xA4, 0x40, 0xF2,
    0x77, 0x03, 0x7D, 0x81, 0x2D, 0xEB, 0x33, 0xA0, 0xF4, 0xA1, 0x39, 0x45, 0xD8, 0x98, 0xC2, 0x96 };
static const uint8_t GY[32] = {
    0x4F, 0xE3, 0x42, 0xE2, 0xFE, 0x1A, 0x7F, 0x9B, 0x8E, 0xE7, 0xEB, 0x4A, 0x7C, 0x0F, 0x9E, 0x16,
    0x2B, 0xCE, 0x33, 0x57, 0x6B, 0x31, 0x5E, 0xCE, 0xCB, 0xB6, 0x40, 0x68, 0x37, 0xBF, 0x51, 0xF5 };
static const uint8_t N_BYTES[32] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84, 0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51 };

static void fe_from_bytes(fe r, const uint8_t b[32])
{
    for (int i = 0; i < 8; i++)
        r[i] = (uint32_t)b[31 - 4 * i] | (uint32_t)b[30 - 4 * i] << 8 | (uint32_t)b[29 - 4 * i] << 16 |
               (uint32_t)b[28 - 4 * i] << 24;
}

static void fe_to_bytes(uint8_t b[32], const fe a)
{
    for (int i = 0; i < 8; i++) {
        b[31 - 4 * i] = (uint8_t)a[i];
        b[30 - 4 * i] = (uint8_t)(a[i] >> 8);
        b[29 - 4 * i] = (uint8_t)(a[i] >> 16);
        b[28 - 4 * i] = (uint8_t)(a[i] >> 24);
    }
}

static int fe_cmp(const fe a, const fe b)
{
    for (int i = 7; i >= 0; i--)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

static bool fe_is_zero(const fe a)
{
    uint32_t d = 0;
    for (int i = 0; i < 8; i++)
        d |= a[i];
    return d == 0;
}

/* r = a - b (mod 2^256); returns the borrow */
static uint32_t raw_sub(fe r, const fe a, const fe b)
{
    uint64_t borrow = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        r[i] = (uint32_t)d;
        borrow = (d >> 63) & 1;
    }
    return (uint32_t)borrow;
}

static uint32_t raw_add(fe r, const fe a, const fe b)
{
    uint64_t c = 0;
    for (int i = 0; i < 8; i++) {
        c += (uint64_t)a[i] + b[i];
        r[i] = (uint32_t)c;
        c >>= 32;
    }
    return (uint32_t)c;
}

static void fe_add(fe r, const fe a, const fe b)
{
    fe t;
    uint32_t c = raw_add(r, a, b);
    uint32_t borrow = raw_sub(t, r, P);
    if (c || !borrow)
        memcpy(r, t, sizeof(fe));
}

static void fe_sub(fe r, const fe a, const fe b)
{
    if (raw_sub(r, a, b))
        raw_add(r, r, P);
}

/* Montgomery multiplication: r = a * b * 2^-256 mod p.  -p^-1 mod 2^32 = 1. */
static void fe_mul(fe r, const fe a, const fe b)
{
    uint32_t t[10] = { 0 };
    for (int i = 0; i < 8; i++) {
        uint64_t c = 0;
        for (int j = 0; j < 8; j++) {
            uint64_t s = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + c;
            t[j] = (uint32_t)s;
            c = s >> 32;
        }
        uint64_t s = (uint64_t)t[8] + c;
        t[8] = (uint32_t)s;
        t[9] = (uint32_t)(s >> 32);
        uint32_t q = t[0];
        c = ((uint64_t)t[0] + (uint64_t)q * P[0]) >> 32;
        for (int j = 1; j < 8; j++) {
            s = (uint64_t)t[j] + (uint64_t)q * P[j] + c;
            t[j - 1] = (uint32_t)s;
            c = s >> 32;
        }
        s = (uint64_t)t[8] + c;
        t[7] = (uint32_t)s;
        t[8] = t[9] + (uint32_t)(s >> 32);
    }
    fe res;
    memcpy(res, t, sizeof(fe));
    fe sub;
    uint32_t borrow = raw_sub(sub, res, P);
    memcpy(r, (t[8] || !borrow) ? sub : res, sizeof(fe));
}

static fe R2;                                /* 2^512 mod p */
static fe ONE_M;                             /* 1 in Montgomery form */
static fe B_M;                               /* curve b in Montgomery form */
static bool ready;

static void init(void)
{
    /* R2 by doubling 1 512 times mod p */
    fe x = { 1 };
    for (int i = 0; i < 512; i++)
        fe_add(x, x, x);
    memcpy(R2, x, sizeof(fe));
    fe one = { 1 }, b;
    fe_mul(ONE_M, one, R2);
    fe_from_bytes(b, B_BYTES);
    fe_mul(B_M, b, R2);
    ready = true;
}

static void to_mont(fe r, const fe a)
{
    fe_mul(r, a, R2);
}

static void from_mont(fe r, const fe a)
{
    fe one = { 1 };
    fe_mul(r, a, one);
}

static void fe_inv(fe r, const fe a)
{
    /* a^(p-2) */
    fe e, acc;
    fe two = { 2 };
    raw_sub(e, P, two);
    memcpy(acc, ONE_M, sizeof(fe));
    for (int i = 255; i >= 0; i--) {
        fe_mul(acc, acc, acc);
        if ((e[i / 32] >> (i % 32)) & 1)
            fe_mul(acc, acc, a);
    }
    memcpy(r, acc, sizeof(fe));
}

struct jpoint {
    fe x, y, z;                              /* Montgomery form; z = 0 is infinity */
};

static void point_double(struct jpoint *r, const struct jpoint *p)
{
    if (fe_is_zero(p->z)) {
        *r = *p;
        return;
    }
    fe delta, gamma, beta, alpha, t1, t2;
    fe_mul(delta, p->z, p->z);
    fe_mul(gamma, p->y, p->y);
    fe_mul(beta, p->x, gamma);
    fe_sub(t1, p->x, delta);
    fe_add(t2, p->x, delta);
    fe_mul(alpha, t1, t2);
    fe_add(t1, alpha, alpha);
    fe_add(alpha, t1, alpha);                /* alpha = 3 (x - delta)(x + delta) */
    struct jpoint o;
    fe_mul(o.x, alpha, alpha);
    fe_add(t1, beta, beta);
    fe_add(t1, t1, t1);                      /* 4 beta */
    fe_add(t2, t1, t1);                      /* 8 beta */
    fe_sub(o.x, o.x, t2);
    fe_add(t2, p->y, p->z);
    fe_mul(o.z, t2, t2);
    fe_sub(o.z, o.z, gamma);
    fe_sub(o.z, o.z, delta);
    fe_sub(t1, t1, o.x);
    fe_mul(o.y, alpha, t1);
    fe_mul(t2, gamma, gamma);
    fe_add(t2, t2, t2);
    fe_add(t2, t2, t2);
    fe_add(t2, t2, t2);                      /* 8 gamma^2 */
    fe_sub(o.y, o.y, t2);
    *r = o;
}

static void point_add(struct jpoint *r, const struct jpoint *p, const struct jpoint *q)
{
    if (fe_is_zero(p->z)) {
        *r = *q;
        return;
    }
    if (fe_is_zero(q->z)) {
        *r = *p;
        return;
    }
    fe z1z1, z2z2, u1, u2, s1, s2, h, rr, t;
    fe_mul(z1z1, p->z, p->z);
    fe_mul(z2z2, q->z, q->z);
    fe_mul(u1, p->x, z2z2);
    fe_mul(u2, q->x, z1z1);
    fe_mul(t, q->z, z2z2);
    fe_mul(s1, p->y, t);
    fe_mul(t, p->z, z1z1);
    fe_mul(s2, q->y, t);
    fe_sub(h, u2, u1);
    fe_sub(rr, s2, s1);
    if (fe_is_zero(h)) {
        if (fe_is_zero(rr)) {
            point_double(r, p);
        } else {
            memset(r, 0, sizeof(*r));        /* P + (-P) = infinity */
        }
        return;
    }
    fe hh, hhh, v;
    fe_mul(hh, h, h);
    fe_mul(hhh, hh, h);
    fe_mul(v, u1, hh);
    struct jpoint o;
    fe_mul(o.x, rr, rr);
    fe_sub(o.x, o.x, hhh);
    fe_sub(o.x, o.x, v);
    fe_sub(o.x, o.x, v);
    fe_sub(t, v, o.x);
    fe_mul(o.y, rr, t);
    fe_mul(t, s1, hhh);
    fe_sub(o.y, o.y, t);
    fe_mul(t, p->z, q->z);
    fe_mul(o.z, t, h);
    *r = o;
}

static void cswap(struct jpoint *a, struct jpoint *b, uint32_t bit)
{
    uint32_t mask = 0u - bit;
    uint32_t *x = (uint32_t *)a, *y = (uint32_t *)b;
    for (size_t i = 0; i < sizeof(*a) / 4; i++) {
        uint32_t t = mask & (x[i] ^ y[i]);
        x[i] ^= t;
        y[i] ^= t;
    }
}

/* r = k * p (Montgomery ladder) */
static void scalar_mult(struct jpoint *r, const uint8_t k[32], const struct jpoint *p)
{
    struct jpoint r0, r1 = *p;
    memset(&r0, 0, sizeof(r0));
    for (int i = 255; i >= 0; i--) {
        uint32_t bit = (k[31 - i / 8] >> (i % 8)) & 1;
        cswap(&r0, &r1, bit);
        point_add(&r1, &r0, &r1);
        point_double(&r0, &r0);
        cswap(&r0, &r1, bit);
    }
    *r = r0;
}

static bool to_affine(uint8_t out[64], const struct jpoint *p)
{
    if (fe_is_zero(p->z))
        return false;
    fe zi, zi2, zi3, x, y;
    fe_inv(zi, p->z);
    fe_mul(zi2, zi, zi);
    fe_mul(zi3, zi2, zi);
    fe_mul(x, p->x, zi2);
    fe_mul(y, p->y, zi3);
    from_mont(x, x);
    from_mont(y, y);
    fe_to_bytes(out, x);
    fe_to_bytes(out + 32, y);
    return true;
}

/* Parse an uncompressed point (04 || X || Y) and check it lies on the curve. */
static bool load_point(struct jpoint *p, const uint8_t in[65])
{
    if (in[0] != 4)
        return false;
    fe x, y;
    fe_from_bytes(x, in + 1);
    fe_from_bytes(y, in + 33);
    if (fe_cmp(x, P) >= 0 || fe_cmp(y, P) >= 0)
        return false;
    to_mont(p->x, x);
    to_mont(p->y, y);
    memcpy(p->z, ONE_M, sizeof(fe));
    fe lhs, rhs, t;
    fe_mul(lhs, p->y, p->y);                 /* y^2 */
    fe_mul(t, p->x, p->x);
    fe_mul(rhs, t, p->x);                    /* x^3 */
    fe_sub(rhs, rhs, p->x);
    fe_sub(rhs, rhs, p->x);
    fe_sub(rhs, rhs, p->x);                  /* x^3 - 3x */
    fe_add(rhs, rhs, B_M);
    return fe_cmp(lhs, rhs) == 0;
}

/* A valid private key: 0 < k < n. */
bool p256_valid_scalar(const uint8_t k[32])
{
    bool zero = true;
    for (int i = 0; i < 32; i++)
        if (k[i])
            zero = false;
    return !zero && memcmp(k, N_BYTES, 32) < 0;
}

bool p256_public(uint8_t out[65], const uint8_t k[32])
{
    if (!ready)
        init();
    uint8_t g[65];
    g[0] = 4;
    memcpy(g + 1, GX, 32);
    memcpy(g + 33, GY, 32);
    struct jpoint gp, r;
    if (!load_point(&gp, g))
        return false;
    scalar_mult(&r, k, &gp);
    out[0] = 4;
    return to_affine(out + 1, &r);
}

bool p256_shared(uint8_t out[32], const uint8_t k[32], const uint8_t peer[65])
{
    if (!ready)
        init();
    struct jpoint q, r;
    if (!load_point(&q, peer))
        return false;
    scalar_mult(&r, k, &q);
    uint8_t xy[64];
    if (!to_affine(xy, &r))
        return false;
    memcpy(out, xy, 32);
    return true;
}
