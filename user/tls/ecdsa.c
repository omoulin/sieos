/*
 * ecdsa.c - ECDSA signature verification (FIPS 186-4) on NIST P-256 and
 * P-384: certificate signatures and TLS 1.3 CertificateVerify.
 *
 * Generic Montgomery arithmetic on 32-bit limbs (up to 12, for 384-bit
 * moduli) serves both the field (p) and the group order (n); points are in
 * Jacobian coordinates (curves with a = -3).  Only public values are
 * handled, so nothing here needs to be constant time.
 */
#include "crypto.h"

#define MAXL 12

struct mont {
    int n;                        /* limbs */
    uint32_t m[MAXL];             /* the modulus, little-endian limbs */
    uint32_t minv;                /* -m^-1 mod 2^32 */
    uint32_t r2[MAXL];            /* R^2 mod m, R = 2^(32n) */
    uint32_t one[MAXL];           /* R mod m (1 in Montgomery form) */
};

typedef uint32_t num[MAXL];

static void from_be(const struct mont *M, uint32_t *r, const uint8_t *b, size_t len)
{
    memset(r, 0, MAXL * 4);
    for (size_t i = 0; i < len && i < (size_t)M->n * 4; i++)
        r[i / 4] |= (uint32_t)b[len - 1 - i] << (8 * (i % 4));
}

static int cmp(const struct mont *M, const uint32_t *a, const uint32_t *b)
{
    for (int i = M->n - 1; i >= 0; i--)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

static bool is_zero(const struct mont *M, const uint32_t *a)
{
    uint32_t x = 0;
    for (int i = 0; i < M->n; i++)
        x |= a[i];
    return !x;
}

static uint32_t sub_raw(const struct mont *M, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint64_t borrow = 0;
    for (int i = 0; i < M->n; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        r[i] = (uint32_t)d;
        borrow = (d >> 32) & 1;
    }
    return (uint32_t)borrow;
}

static void mod_add(const struct mont *M, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint64_t carry = 0;
    num t;
    for (int i = 0; i < M->n; i++) {
        uint64_t s = (uint64_t)a[i] + b[i] + carry;
        t[i] = (uint32_t)s;
        carry = s >> 32;
    }
    num u;
    uint32_t borrow = sub_raw(M, u, t, M->m);
    memcpy(r, (carry || !borrow) ? u : t, MAXL * 4);
}

static void mod_sub(const struct mont *M, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    num t;
    if (sub_raw(M, t, a, b)) {                   /* went negative: add m back */
        uint64_t carry = 0;
        for (int i = 0; i < M->n; i++) {
            uint64_t s = (uint64_t)t[i] + M->m[i] + carry;
            t[i] = (uint32_t)s;
            carry = s >> 32;
        }
    }
    memcpy(r, t, MAXL * 4);
}

/* r = a * b / R mod m (CIOS) */
static void mont_mul(const struct mont *M, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint32_t t[MAXL + 2];
    memset(t, 0, sizeof(t));
    int n = M->n;
    for (int i = 0; i < n; i++) {
        uint64_t c = 0;
        for (int j = 0; j < n; j++) {
            uint64_t x = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + c;
            t[j] = (uint32_t)x;
            c = x >> 32;
        }
        uint64_t x = (uint64_t)t[n] + c;
        t[n] = (uint32_t)x;
        t[n + 1] = (uint32_t)(x >> 32);
        uint32_t q = t[0] * M->minv;
        c = ((uint64_t)t[0] + (uint64_t)q * M->m[0]) >> 32;
        for (int j = 1; j < n; j++) {
            x = (uint64_t)t[j] + (uint64_t)q * M->m[j] + c;
            t[j - 1] = (uint32_t)x;
            c = x >> 32;
        }
        x = (uint64_t)t[n] + c;
        t[n - 1] = (uint32_t)x;
        t[n] = t[n + 1] + (uint32_t)(x >> 32);
    }
    num u;
    uint32_t borrow = sub_raw(M, u, t, M->m);
    memcpy(r, (t[n] || !borrow) ? u : t, MAXL * 4);
}

static void mont_setup(struct mont *M, const uint8_t *mod_be, int bytes)
{
    memset(M, 0, sizeof(*M));
    M->n = bytes / 4;
    from_be(M, M->m, mod_be, bytes);
    uint32_t inv = 1;                            /* Newton: inv = m0^-1 mod 2^32 */
    for (int i = 0; i < 5; i++)
        inv *= 2 - M->m[0] * inv;
    M->minv = -inv;
    num x = { 1 };                               /* 2^(64n) mod m by doubling */
    for (int i = 0; i < 64 * M->n; i++)
        mod_add(M, x, x, x);
    memcpy(M->r2, x, sizeof(x));
    num o = { 1 };
    mont_mul(M, M->one, o, M->r2);               /* R mod m */
}

static void to_mont(const struct mont *M, uint32_t *r, const uint32_t *a) { mont_mul(M, r, a, M->r2); }

static void from_mont(const struct mont *M, uint32_t *r, const uint32_t *a)
{
    num o = { 1 };
    mont_mul(M, r, a, o);
}

/* r = a^(m-2): the inverse (m prime), all in Montgomery form */
static void mont_inv(const struct mont *M, uint32_t *r, const uint32_t *a)
{
    num e, two = { 2 }, res;
    sub_raw(M, e, M->m, two);
    memcpy(res, M->one, sizeof(res));
    for (int i = M->n * 32 - 1; i >= 0; i--) {
        mont_mul(M, res, res, res);
        if ((e[i / 32] >> (i % 32)) & 1)
            mont_mul(M, res, res, a);
    }
    memcpy(r, res, sizeof(res));
}

/* ---------------- curves ---------------- */

struct curve {
    int bytes;
    struct mont p, n;
    num b, gx, gy;                               /* Montgomery form (mod p) */
    bool ready;
};

static const uint8_t P256_P[] = { 0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff };
static const uint8_t P256_N[] = { 0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51 };
static const uint8_t P256_B[] = { 0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,0x76,0x98,0x86,0xbc,0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b };
static const uint8_t P256_GX[] = { 0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96 };
static const uint8_t P256_GY[] = { 0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5 };

static const uint8_t P384_P[] = { 0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff };
static const uint8_t P384_N[] = { 0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xc7,0x63,0x4d,0x81,0xf4,0x37,0x2d,0xdf,0x58,0x1a,0x0d,0xb2,0x48,0xb0,0xa7,0x7a,0xec,0xec,0x19,0x6a,0xcc,0xc5,0x29,0x73 };
static const uint8_t P384_B[] = { 0xb3,0x31,0x2f,0xa7,0xe2,0x3e,0xe7,0xe4,0x98,0x8e,0x05,0x6b,0xe3,0xf8,0x2d,0x19,0x18,0x1d,0x9c,0x6e,0xfe,0x81,0x41,0x12,0x03,0x14,0x08,0x8f,0x50,0x13,0x87,0x5a,0xc6,0x56,0x39,0x8d,0x8a,0x2e,0xd1,0x9d,0x2a,0x85,0xc8,0xed,0xd3,0xec,0x2a,0xef };
static const uint8_t P384_GX[] = { 0xaa,0x87,0xca,0x22,0xbe,0x8b,0x05,0x37,0x8e,0xb1,0xc7,0x1e,0xf3,0x20,0xad,0x74,0x6e,0x1d,0x3b,0x62,0x8b,0xa7,0x9b,0x98,0x59,0xf7,0x41,0xe0,0x82,0x54,0x2a,0x38,0x55,0x02,0xf2,0x5d,0xbf,0x55,0x29,0x6c,0x3a,0x54,0x5e,0x38,0x72,0x76,0x0a,0xb7 };
static const uint8_t P384_GY[] = { 0x36,0x17,0xde,0x4a,0x96,0x26,0x2c,0x6f,0x5d,0x9e,0x98,0xbf,0x92,0x92,0xdc,0x29,0xf8,0xf4,0x1d,0xbd,0x28,0x9a,0x14,0x7c,0xe9,0xda,0x31,0x13,0xb5,0xf0,0xb8,0xc0,0x0a,0x60,0xb1,0xce,0x1d,0x7e,0x81,0x9d,0x7a,0x43,0x1d,0x7c,0x90,0xea,0x0e,0x5f };

static struct curve curves[2];

static struct curve *get_curve(int c)
{
    struct curve *C = &curves[c];
    if (C->ready)
        return C;
    const uint8_t *p = c ? P384_P : P256_P, *n = c ? P384_N : P256_N, *b = c ? P384_B : P256_B;
    const uint8_t *gx = c ? P384_GX : P256_GX, *gy = c ? P384_GY : P256_GY;
    C->bytes = c ? 48 : 32;
    mont_setup(&C->p, p, C->bytes);
    mont_setup(&C->n, n, C->bytes);
    num t;
    from_be(&C->p, t, b, C->bytes);
    to_mont(&C->p, C->b, t);
    from_be(&C->p, t, gx, C->bytes);
    to_mont(&C->p, C->gx, t);
    from_be(&C->p, t, gy, C->bytes);
    to_mont(&C->p, C->gy, t);
    C->ready = true;
    return C;
}

struct jp {
    num x, y, z;                                 /* z == 0: the point at infinity */
};

static void jp_double(const struct mont *M, struct jp *r, const struct jp *a)
{
    if (is_zero(M, a->z) || is_zero(M, a->y)) {
        memset(r, 0, sizeof(*r));
        return;
    }
    num delta, gamma, beta, alpha, t1, t2, x3, y3, z3;
    mont_mul(M, delta, a->z, a->z);
    mont_mul(M, gamma, a->y, a->y);
    mont_mul(M, beta, a->x, gamma);
    mod_sub(M, t1, a->x, delta);
    mod_add(M, t2, a->x, delta);
    mont_mul(M, alpha, t1, t2);
    mod_add(M, t1, alpha, alpha);
    mod_add(M, alpha, alpha, t1);                /* alpha = 3 (x - delta)(x + delta) */
    mont_mul(M, x3, alpha, alpha);
    mod_add(M, t1, beta, beta);
    mod_add(M, t1, t1, t1);                      /* 4 beta */
    mod_add(M, t2, t1, t1);                      /* 8 beta */
    mod_sub(M, x3, x3, t2);
    mod_add(M, z3, a->y, a->z);
    mont_mul(M, z3, z3, z3);
    mod_sub(M, z3, z3, gamma);
    mod_sub(M, z3, z3, delta);
    mod_sub(M, t1, t1, x3);
    mont_mul(M, y3, alpha, t1);
    mont_mul(M, t2, gamma, gamma);
    mod_add(M, t2, t2, t2);
    mod_add(M, t2, t2, t2);
    mod_add(M, t2, t2, t2);                      /* 8 gamma^2 */
    mod_sub(M, y3, y3, t2);
    memcpy(r->x, x3, sizeof(num));
    memcpy(r->y, y3, sizeof(num));
    memcpy(r->z, z3, sizeof(num));
}

static void jp_add(const struct mont *M, struct jp *r, const struct jp *a, const struct jp *b)
{
    if (is_zero(M, a->z)) {
        *r = *b;
        return;
    }
    if (is_zero(M, b->z)) {
        *r = *a;
        return;
    }
    num z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, t;
    mont_mul(M, z1z1, a->z, a->z);
    mont_mul(M, z2z2, b->z, b->z);
    mont_mul(M, u1, a->x, z2z2);
    mont_mul(M, u2, b->x, z1z1);
    mont_mul(M, t, b->z, z2z2);
    mont_mul(M, s1, a->y, t);
    mont_mul(M, t, a->z, z1z1);
    mont_mul(M, s2, b->y, t);
    mod_sub(M, h, u2, u1);
    mod_sub(M, rr, s2, s1);
    if (is_zero(M, h)) {
        if (is_zero(M, rr))
            jp_double(M, r, a);                  /* the same point */
        else
            memset(r, 0, sizeof(*r));            /* opposite points */
        return;
    }
    mod_add(M, i, h, h);
    mont_mul(M, i, i, i);                        /* I = (2H)^2 */
    mont_mul(M, j, h, i);                        /* J = H I */
    mod_add(M, rr, rr, rr);                      /* r = 2 (S2 - S1) */
    mont_mul(M, v, u1, i);                       /* V = U1 I */
    struct jp o;
    mont_mul(M, o.x, rr, rr);
    mod_sub(M, o.x, o.x, j);
    mod_sub(M, o.x, o.x, v);
    mod_sub(M, o.x, o.x, v);                     /* X3 = r^2 - J - 2V */
    mod_sub(M, t, v, o.x);
    mont_mul(M, o.y, rr, t);
    mont_mul(M, t, s1, j);
    mod_add(M, t, t, t);
    mod_sub(M, o.y, o.y, t);                     /* Y3 = r (V - X3) - 2 S1 J */
    mod_add(M, t, a->z, b->z);
    mont_mul(M, t, t, t);
    mod_sub(M, t, t, z1z1);
    mod_sub(M, t, t, z2z2);
    mont_mul(M, o.z, t, h);                      /* Z3 = ((Z1+Z2)^2 - Z1Z1 - Z2Z2) H */
    *r = o;
}

static bool on_curve(struct curve *C, const uint32_t *x, const uint32_t *y)
{
    const struct mont *M = &C->p;
    num y2, x3, t;
    mont_mul(M, y2, y, y);
    mont_mul(M, x3, x, x);
    mont_mul(M, x3, x3, x);
    mod_add(M, t, x, x);
    mod_add(M, t, t, x);                         /* 3x */
    mod_sub(M, x3, x3, t);
    mod_add(M, x3, x3, C->b);
    return cmp(M, y2, x3) == 0;
}

/* DER INTEGER at *p (within end) into a big-endian buffer of len bytes */
static bool der_int(const uint8_t **p, const uint8_t *end, uint8_t *out, size_t len)
{
    const uint8_t *q = *p;
    if (end - q < 2 || q[0] != 0x02 || q[1] > end - q - 2 || q[1] == 0 || q[1] >= 0x80)
        return false;
    size_t n = q[1];
    q += 2;
    while (n > 1 && *q == 0) {
        q++;
        n--;
    }
    if (n > len)
        return false;
    memset(out, 0, len);
    memcpy(out + len - n, q, n);
    *p = q + n;
    return true;
}

bool ecdsa_verify(int curve, const uint8_t *pub, size_t publen, const uint8_t *digest, size_t dlen,
                  const uint8_t *sig, size_t siglen)
{
    struct curve *C = get_curve(curve == ECDSA_P384);
    size_t bytes = C->bytes;
    if (publen != 1 + 2 * bytes || pub[0] != 4)
        return false;                            /* uncompressed points only */
    const struct mont *P = &C->p, *N = &C->n;
    /* the signature: SEQUENCE { INTEGER r, INTEGER s } */
    const uint8_t *q = sig, *end = sig + siglen;
    if (siglen < 8 || q[0] != 0x30)
        return false;
    size_t slen = q[1];
    q += 2;
    if (slen & 0x80) {
        if (slen != 0x81 || end - q < 1)
            return false;
        slen = *q++;
    }
    if ((size_t)(end - q) < slen)
        return false;
    end = q + slen;
    uint8_t rb[48], sb[48];
    if (!der_int(&q, end, rb, bytes) || !der_int(&q, end, sb, bytes) || q != end)
        return false;
    num r, s, e, qx, qy;
    from_be(N, r, rb, bytes);
    from_be(N, s, sb, bytes);
    if (is_zero(N, r) || is_zero(N, s) || cmp(N, r, N->m) >= 0 || cmp(N, s, N->m) >= 0)
        return false;
    /* e: the digest's leftmost bits (both curves are whole bytes) */
    uint8_t eb[48];
    memset(eb, 0, sizeof(eb));
    size_t take = dlen < bytes ? dlen : bytes;
    memcpy(eb + bytes - take, digest, take);
    from_be(N, e, eb, bytes);
    if (cmp(N, e, N->m) >= 0)
        sub_raw(N, e, e, N->m);
    /* the public key, on the curve */
    num t;
    from_be(P, t, pub + 1, bytes);
    if (cmp(P, t, P->m) >= 0)
        return false;
    to_mont(P, qx, t);
    from_be(P, t, pub + 1 + bytes, bytes);
    if (cmp(P, t, P->m) >= 0)
        return false;
    to_mont(P, qy, t);
    if (!on_curve(C, qx, qy))
        return false;
    /* w = 1/s, u1 = e w, u2 = r w (mod n) */
    num sm, w, em, rm, u1, u2;
    to_mont(N, sm, s);
    mont_inv(N, w, sm);
    to_mont(N, em, e);
    to_mont(N, rm, r);
    mont_mul(N, u1, em, w);
    mont_mul(N, u2, rm, w);
    from_mont(N, u1, u1);
    from_mont(N, u2, u2);
    /* R = u1 G + u2 Q (Shamir's trick) */
    struct jp G = { { 0 }, { 0 }, { 0 } }, Q = G, GQ, R;
    memcpy(G.x, C->gx, sizeof(num));
    memcpy(G.y, C->gy, sizeof(num));
    memcpy(G.z, P->one, sizeof(num));
    memcpy(Q.x, qx, sizeof(num));
    memcpy(Q.y, qy, sizeof(num));
    memcpy(Q.z, P->one, sizeof(num));
    jp_add(P, &GQ, &G, &Q);
    memset(&R, 0, sizeof(R));
    for (int i = (int)bytes * 8 - 1; i >= 0; i--) {
        jp_double(P, &R, &R);
        int b1 = (u1[i / 32] >> (i % 32)) & 1, b2 = (u2[i / 32] >> (i % 32)) & 1;
        if (b1 && b2)
            jp_add(P, &R, &R, &GQ);
        else if (b1)
            jp_add(P, &R, &R, &G);
        else if (b2)
            jp_add(P, &R, &R, &Q);
    }
    if (is_zero(P, R.z))
        return false;
    /* x = X / Z^2, affine, then mod n */
    num zi, z2, x;
    mont_inv(P, zi, R.z);
    mont_mul(P, z2, zi, zi);
    mont_mul(P, x, R.x, z2);
    from_mont(P, x, x);
    if (cmp(N, x, N->m) >= 0)
        sub_raw(N, x, x, N->m);
    return cmp(N, x, r) == 0;
}
