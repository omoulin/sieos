/*
 * pubkey.c - Public-key signature checks for TLS and certificates, and the
 * P-256 key exchange:
 *   RSA (RFC 8017): s^e mod n must be the expected encoding of the hash,
 *     PKCS #1 v1.5 (a fixed layout) or PSS (salted, then masked);
 *   ECDSA (FIPS 186) on the NIST curves P-256 and P-384: the point
 *     u1*G + u2*Q must have the signature's r as its x coordinate.
 *
 * Big numbers are arrays of 32-bit "limbs", least significant first. All
 * modular arithmetic is in Montgomery form (x*R mod m, R = 2^(32n)), which
 * replaces divisions by shifts. Checking a signature uses only public data,
 * so speed matters more than constant time here; the one secret (the P-256
 * key exchange's scalar) is a fresh random number used once.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk/crypto.h"
#include "mk/lib.h"

#define BN 130                                   /* limbs: 4096 bits + room */
typedef struct { int n; uint32_t m[BN], rr[BN], one[BN], m0i; } mont_t;

static void bn_from(uint32_t *r, int n, const uint8_t *b, size_t len)   /* big-endian bytes */
{
    memset(r, 0, n * 4);
    for (size_t i = 0; i < len && i / 4 < (size_t)n; i++)
        r[i / 4] |= (uint32_t)b[len - 1 - i] << (8 * (i % 4));
}
static void bn_to(uint8_t *b, size_t len, const uint32_t *a, int n)
{
    for (size_t i = 0; i < len; i++) b[len - 1 - i] = i / 4 < (size_t)n ? (uint8_t)(a[i / 4] >> (8 * (i % 4))) : 0;
}
static int bn_cmp(const uint32_t *a, const uint32_t *b, int n)
{
    for (int i = n - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
static int bn_zero(const uint32_t *a, int n) { for (int i = 0; i < n; i++) if (a[i]) return 0; return 1; }
static uint32_t bn_add(uint32_t *r, const uint32_t *a, const uint32_t *b, int n)
{
    uint64_t c = 0;
    for (int i = 0; i < n; i++) { c += (uint64_t)a[i] + b[i]; r[i] = (uint32_t)c; c >>= 32; }
    return (uint32_t)c;
}
static uint32_t bn_sub(uint32_t *r, const uint32_t *a, const uint32_t *b, int n)
{
    int64_t c = 0;
    for (int i = 0; i < n; i++) { c += (int64_t)a[i] - b[i]; r[i] = (uint32_t)c; c >>= 32; }
    return (uint32_t)(c & 1);
}

/* r = a*b*R^-1 mod m ("CIOS": multiply and reduce one limb at a time). */
static void mont_mul(const mont_t *c, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    int n = c->n;
    uint32_t t[BN + 2] = { 0 };
    for (int i = 0; i < n; i++) {
        uint64_t C = 0;
        for (int j = 0; j < n; j++) { C += t[j] + (uint64_t)a[j] * b[i]; t[j] = (uint32_t)C; C >>= 32; }
        C += t[n]; t[n] = (uint32_t)C; t[n + 1] = (uint32_t)(C >> 32);
        uint32_t q = t[0] * c->m0i;
        C = t[0] + (uint64_t)q * c->m[0]; C >>= 32;
        for (int j = 1; j < n; j++) { C += t[j] + (uint64_t)q * c->m[j]; t[j - 1] = (uint32_t)C; C >>= 32; }
        C += t[n]; t[n - 1] = (uint32_t)C; t[n] = t[n + 1] + (uint32_t)(C >> 32);
    }
    if (t[n] || bn_cmp(t, c->m, n) >= 0) bn_sub(t, t, c->m, n);
    memcpy(r, t, n * 4);
}

/* Set up arithmetic modulo the odd number mod (big-endian bytes). */
static int mont_init(mont_t *c, const uint8_t *mod, size_t len)
{
    while (len && !*mod) { mod++; len--; }
    if (!len || len > (BN - 2) * 4 || !(mod[len - 1] & 1)) return -1;
    c->n = (int)(len + 3) / 4;
    bn_from(c->m, c->n, mod, len);
    uint32_t x = 1;                               /* -m^-1 mod 2^32 by Newton's iteration */
    for (int i = 0; i < 5; i++) x *= 2 - c->m[0] * x;
    c->m0i = -x;
    memset(c->rr, 0, sizeof c->rr);               /* R^2 mod m, by doubling 1 64n times */
    c->rr[0] = 1;
    for (int i = 0; i < 64 * c->n; i++) {
        uint32_t carry = bn_add(c->rr, c->rr, c->rr, c->n);
        if (carry || bn_cmp(c->rr, c->m, c->n) >= 0) bn_sub(c->rr, c->rr, c->m, c->n);
    }
    uint32_t one[BN] = { 1 };
    mont_mul(c, c->one, one, c->rr);              /* 1 in Montgomery form: R mod m */
    return 0;
}

static void to_mont(const mont_t *c, uint32_t *r, const uint32_t *a) { mont_mul(c, r, a, c->rr); }
static void from_mont(const mont_t *c, uint32_t *r, const uint32_t *a) { uint32_t one[BN] = { 1 }; mont_mul(c, r, a, one); }

/* r = x^e in Montgomery form (x in Montgomery form, e big-endian bytes). */
static void mont_pow(const mont_t *c, uint32_t *r, const uint32_t *x, const uint8_t *e, size_t elen)
{
    uint32_t acc[BN];
    memcpy(acc, c->one, c->n * 4);
    for (size_t i = 0; i < elen; i++)
        for (int b = 7; b >= 0; b--) {
            mont_mul(c, acc, acc, acc);
            if (e[i] >> b & 1) mont_mul(c, acc, acc, x);
        }
    memcpy(r, acc, c->n * 4);
}

static void mod_add(const mont_t *c, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    if (bn_add(r, a, b, c->n) || bn_cmp(r, c->m, c->n) >= 0) bn_sub(r, r, c->m, c->n);
}
static void mod_sub(const mont_t *c, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    if (bn_sub(r, a, b, c->n)) bn_add(r, r, c->m, c->n);
}

/* x^-1 mod m (m prime), x in Montgomery form: Fermat, x^(m-2). */
static void mont_inv(const mont_t *c, uint32_t *r, const uint32_t *x)
{
    uint32_t e[BN], two[BN] = { 2 };
    uint8_t eb[BN * 4];
    bn_sub(e, c->m, two, c->n);
    bn_to(eb, c->n * 4, e, c->n);
    mont_pow(c, r, x, eb, c->n * 4);
}

/* ---- RSA */
static int rsa_em(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                  const uint8_t *sig, size_t slen, uint8_t *em, size_t *k, int *bits)
{
    static mont_t c;                              /* 1.5 KiB: kept off the stack */
    while (nlen && !*n) { n++; nlen--; }
    if (mont_init(&c, n, nlen) || slen > nlen || elen > 8) return -1;
    uint32_t s[BN], x[BN];
    bn_from(s, c.n, sig, slen);
    if (bn_cmp(s, c.m, c.n) >= 0) return -1;      /* the signature must be below n */
    to_mont(&c, x, s);
    mont_pow(&c, x, x, e, elen);
    from_mont(&c, s, x);
    *k = nlen;
    *bits = (int)nlen * 8;
    for (uint8_t t = n[0]; !(t & 0x80); t <<= 1) (*bits)--;
    bn_to(em, nlen, s, c.n);
    return 0;
}

static const uint8_t *digest_info(int alg, size_t *len)
{
    static const uint8_t p256[] = { 0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20 };
    static const uint8_t p384[] = { 0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30 };
    static const uint8_t p512[] = { 0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40 };
    *len = sizeof p256;
    return alg == SHA256 ? p256 : alg == SHA384 ? p384 : p512;
}

int rsa_verify_pkcs1(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                     int alg, const uint8_t *hash, const uint8_t *sig, size_t slen)
{
    static uint8_t em[BN * 4], want[BN * 4];
    size_t k, dl, hl = sha_len(alg);
    int bits;
    if (rsa_em(n, nlen, e, elen, sig, slen, em, &k, &bits)) return -1;
    const uint8_t *di = digest_info(alg, &dl);
    if (k < dl + hl + 11) return -1;
    want[0] = 0; want[1] = 1;                     /* 00 01 FF..FF 00 DigestInfo hash */
    memset(want + 2, 0xff, k - dl - hl - 3);
    want[k - dl - hl - 1] = 0;
    memcpy(want + k - dl - hl, di, dl);
    memcpy(want + k - hl, hash, hl);
    return memcmp(em, want, k) ? -1 : 0;
}

static void mgf1(int alg, uint8_t *mask, size_t len, const uint8_t *seed, size_t slen)
{
    uint8_t c[4] = { 0 }, h[64];
    for (size_t off = 0; off < len; off += sha_len(alg)) {
        sha_t s;
        sha_init(&s, alg);
        sha_update(&s, seed, slen);
        sha_update(&s, c, 4);
        sha_final(&s, h);
        for (size_t i = 0; i < sha_len(alg) && off + i < len; i++) mask[off + i] ^= h[i];
        if (!++c[3]) c[2]++;
    }
}

int rsa_verify_pss(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                   int alg, const uint8_t *hash, const uint8_t *sig, size_t slen)
{
    static uint8_t em[BN * 4];
    size_t k, hl = sha_len(alg), sl = hl;
    int bits;
    if (rsa_em(n, nlen, e, elen, sig, slen, em, &k, &bits)) return -1;
    int embits = bits - 1;
    size_t emlen = (embits + 7) / 8;
    uint8_t *m = em + (k - emlen);                /* a leading 0 byte when emLen < k */
    if (k > emlen && em[0]) return -1;
    if (emlen < hl + sl + 2 || m[emlen - 1] != 0xbc) return -1;
    size_t dblen = emlen - hl - 1;
    uint8_t *db = m, *h = m + dblen, topmask = (uint8_t)(0xff >> (8 * emlen - embits));
    if (db[0] & ~topmask) return -1;
    mgf1(alg, db, dblen, h, hl);                  /* unmask DB in place */
    db[0] &= topmask;
    for (size_t i = 0; i < dblen - sl - 1; i++) if (db[i]) return -1;
    if (db[dblen - sl - 1] != 1) return -1;
    uint8_t z[8] = { 0 }, h2[64];
    sha_t s;
    sha_init(&s, alg);
    sha_update(&s, z, 8);
    sha_update(&s, hash, hl);
    sha_update(&s, db + dblen - sl, sl);
    sha_final(&s, h2);
    return memcmp(h, h2, hl) ? -1 : 0;
}

/* ---- Elliptic curves y^2 = x^3 - 3x + b over a prime field. */
typedef struct {
    int len;                                      /* bytes per coordinate */
    const char *p, *b, *gx, *gy, *n;              /* hex */
} curve_def_t;
static const curve_def_t CURVES[] = {
    { 32, "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
          "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
          "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
          "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5",
          "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551" },
    { 48, "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
          "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
          "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
          "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f",
          "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973" },
};

typedef struct {
    mont_t p, n;
    uint32_t b[12], gx[12], gy[12];               /* Montgomery form */
    int len, ready;
} curve_t;
static curve_t curves[2];

static void hexbytes(uint8_t *o, const char *h, int len)
{
    for (int i = 0; i < len; i++) {
        int v = 0;
        for (int j = 0; j < 2; j++) { char c = h[2 * i + j]; v = v * 16 + (c <= '9' ? c - '0' : c - 'a' + 10); }
        o[i] = (uint8_t)v;
    }
}

static curve_t *curve(int id)
{
    curve_t *c = &curves[id - 1];
    if (c->ready) return c;
    const curve_def_t *d = &CURVES[id - 1];
    uint8_t t[48] = { 0 };
    uint32_t x[12];
    c->len = d->len;
    hexbytes(t, d->p, d->len); mont_init(&c->p, t, d->len);
    hexbytes(t, d->n, d->len); mont_init(&c->n, t, d->len);
    hexbytes(t, d->b, d->len);  bn_from(x, c->p.n, t, d->len); to_mont(&c->p, c->b, x);
    hexbytes(t, d->gx, d->len); bn_from(x, c->p.n, t, d->len); to_mont(&c->p, c->gx, x);
    hexbytes(t, d->gy, d->len); bn_from(x, c->p.n, t, d->len); to_mont(&c->p, c->gy, x);
    c->ready = 1;
    return c;
}

typedef struct { uint32_t x[12], y[12], z[12]; } pt_t;   /* Jacobian: (X/Z^2, Y/Z^3); Z = 0: infinity */

static void pt_dbl(const mont_t *f, pt_t *r, const pt_t *p)
{
    int n = f->n;
    if (bn_zero(p->z, n)) { *r = *p; return; }
    uint32_t d[12], g[12], be[12], al[12], t[12], u[12];
    mont_mul(f, d, p->z, p->z);                   /* delta = Z^2 */
    mont_mul(f, g, p->y, p->y);                   /* gamma = Y^2 */
    mont_mul(f, be, p->x, g);                     /* beta = X*gamma */
    mod_sub(f, t, p->x, d); mod_add(f, u, p->x, d);
    mont_mul(f, al, t, u);
    mod_add(f, t, al, al); mod_add(f, al, t, al); /* alpha = 3(X-delta)(X+delta) */
    pt_t o;
    mod_add(f, t, p->y, p->z); mont_mul(f, t, t, t);
    mod_sub(f, t, t, g); mod_sub(f, o.z, t, d);   /* Z3 = (Y+Z)^2 - gamma - delta */
    mont_mul(f, o.x, al, al);
    mod_add(f, t, be, be); mod_add(f, t, t, t);   /* 4 beta */
    mod_sub(f, o.x, o.x, t); mod_sub(f, o.x, o.x, t);   /* X3 = alpha^2 - 8 beta */
    mod_sub(f, t, t, o.x);
    mont_mul(f, o.y, al, t);
    mont_mul(f, g, g, g);
    mod_add(f, g, g, g); mod_add(f, g, g, g); mod_add(f, g, g, g);   /* 8 gamma^2 */
    mod_sub(f, o.y, o.y, g);
    *r = o;
}

static void pt_add(const mont_t *f, pt_t *r, const pt_t *p, const pt_t *q)
{
    int n = f->n;
    if (bn_zero(p->z, n)) { *r = *q; return; }
    if (bn_zero(q->z, n)) { *r = *p; return; }
    uint32_t z1z1[12], z2z2[12], u1[12], u2[12], s1[12], s2[12], h[12], rr[12], i[12], j[12], v[12], t[12];
    mont_mul(f, z1z1, p->z, p->z); mont_mul(f, z2z2, q->z, q->z);
    mont_mul(f, u1, p->x, z2z2);   mont_mul(f, u2, q->x, z1z1);
    mont_mul(f, s1, p->y, q->z);   mont_mul(f, s1, s1, z2z2);
    mont_mul(f, s2, q->y, p->z);   mont_mul(f, s2, s2, z1z1);
    mod_sub(f, h, u2, u1);
    mod_sub(f, rr, s2, s1);
    if (bn_zero(h, n)) {
        if (bn_zero(rr, n)) { pt_dbl(f, r, p); return; }
        memset(r, 0, sizeof *r); return;          /* P + (-P) = infinity */
    }
    mod_add(f, rr, rr, rr);
    mod_add(f, i, h, h); mont_mul(f, i, i, i);    /* I = (2H)^2 */
    mont_mul(f, j, h, i);                         /* J = H*I */
    mont_mul(f, v, u1, i);                        /* V = U1*I */
    pt_t o;
    mont_mul(f, o.x, rr, rr); mod_sub(f, o.x, o.x, j);
    mod_sub(f, o.x, o.x, v); mod_sub(f, o.x, o.x, v);
    mod_sub(f, t, v, o.x); mont_mul(f, o.y, rr, t);
    mont_mul(f, t, s1, j); mod_add(f, t, t, t); mod_sub(f, o.y, o.y, t);
    mod_add(f, t, p->z, q->z); mont_mul(f, t, t, t);
    mod_sub(f, t, t, z1z1); mod_sub(f, t, t, z2z2); mont_mul(f, o.z, t, h);
    *r = o;
}

/* r = k1*P + k2*Q (Shamir's trick: one pass over both scalars); k2 may be 0. */
static void pt_mul2(const mont_t *f, pt_t *r, const uint8_t *k1, const pt_t *p, const uint8_t *k2, const pt_t *q, int len)
{
    pt_t acc, pq;
    memset(&acc, 0, sizeof acc);
    if (k2) pt_add(f, &pq, p, q);
    for (int i = 0; i < len * 8; i++) {
        pt_dbl(f, &acc, &acc);
        int b1 = k1[i / 8] >> (7 - i % 8) & 1, b2 = k2 ? k2[i / 8] >> (7 - i % 8) & 1 : 0;
        if (b1 && b2) pt_add(f, &acc, &acc, &pq);
        else if (b1) pt_add(f, &acc, &acc, p);
        else if (b2) pt_add(f, &acc, &acc, q);
    }
    *r = acc;
}

static int pt_affine_x(const curve_t *c, uint8_t *x, uint8_t *y, const pt_t *p)
{
    const mont_t *f = &c->p;
    if (bn_zero(p->z, f->n)) return -1;
    uint32_t zi[12], t[12], v[12];
    mont_inv(f, zi, p->z);
    mont_mul(f, t, zi, zi);
    mont_mul(f, v, p->x, t); from_mont(f, v, v); bn_to(x, c->len, v, f->n);
    if (y) { mont_mul(f, t, t, zi); mont_mul(f, v, p->y, t); from_mont(f, v, v); bn_to(y, c->len, v, f->n); }
    return 0;
}

/* Read an uncompressed point and check it is on the curve. */
static int pt_load(const curve_t *c, pt_t *p, const uint8_t *b, size_t blen)
{
    const mont_t *f = &c->p;
    uint32_t x[12], y[12], l[12], r[12], t[12];
    if (blen != 1 + 2 * (size_t)c->len || b[0] != 4) return -1;
    bn_from(x, f->n, b + 1, c->len); bn_from(y, f->n, b + 1 + c->len, c->len);
    if (bn_cmp(x, f->m, f->n) >= 0 || bn_cmp(y, f->m, f->n) >= 0) return -1;
    to_mont(f, p->x, x); to_mont(f, p->y, y);
    memcpy(p->z, f->one, f->n * 4);
    mont_mul(f, l, p->y, p->y);                   /* y^2 */
    mont_mul(f, r, p->x, p->x); mont_mul(f, r, r, p->x);   /* x^3 */
    mod_sub(f, r, r, p->x); mod_sub(f, r, r, p->x); mod_sub(f, r, r, p->x);
    mod_add(f, r, r, c->b);                       /* x^3 - 3x + b */
    (void)t;
    return bn_cmp(l, r, f->n) ? -1 : 0;
}

int ecdsa_verify(int id, const uint8_t *pub, size_t publen, const uint8_t *hash, size_t hlen,
                 const uint8_t *rb, size_t rlen, const uint8_t *sb, size_t slen)
{
    if (id != P256 && id != P384) return -1;
    curve_t *c = curve(id);
    const mont_t *nm = &c->n;
    int L = c->len, n = nm->n;
    pt_t q, g, R;
    if (pt_load(c, &q, pub, publen)) return -1;
    while (rlen && !*rb) { rb++; rlen--; }        /* DER integers may carry a leading 0 */
    while (slen && !*sb) { sb++; slen--; }
    if (rlen > (size_t)L || slen > (size_t)L) return -1;
    uint32_t r[12], s[12], e[12], w[12], u1[12], u2[12], t[12];
    bn_from(r, n, rb, rlen); bn_from(s, n, sb, slen);
    if (bn_zero(r, n) || bn_zero(s, n) || bn_cmp(r, nm->m, n) >= 0 || bn_cmp(s, nm->m, n) >= 0) return -1;
    bn_from(e, n, hash, hlen < (size_t)L ? hlen : (size_t)L);   /* the hash's leftmost bits */
    if (bn_cmp(e, nm->m, n) >= 0) bn_sub(e, e, nm->m, n);
    to_mont(nm, t, s); mont_inv(nm, w, t);        /* w = s^-1 (Montgomery form) */
    to_mont(nm, t, e); mont_mul(nm, u1, t, w); from_mont(nm, u1, u1);
    to_mont(nm, t, r); mont_mul(nm, u2, t, w); from_mont(nm, u2, u2);
    uint8_t k1[48], k2[48], x[48];
    bn_to(k1, L, u1, n); bn_to(k2, L, u2, n);
    memcpy(g.x, c->gx, sizeof g.x); memcpy(g.y, c->gy, sizeof g.y); memcpy(g.z, c->p.one, sizeof g.z);
    pt_mul2(&c->p, &R, k1, &g, k2, &q, L);
    if (pt_affine_x(c, x, 0, &R)) return -1;
    bn_from(t, n, x, L);
    if (bn_cmp(t, nm->m, n) >= 0) bn_sub(t, t, nm->m, n);
    return bn_cmp(t, r, n) ? -1 : 0;
}

static void p256_scalar(const curve_t *c, uint8_t k[32], const uint8_t priv[32])
{
    uint32_t d[12] = { 0 };
    bn_from(d, c->n.n, priv, 32);
    if (bn_cmp(d, c->n.m, c->n.n) >= 0) bn_sub(d, d, c->n.m, c->n.n);
    if (bn_zero(d, c->n.n)) d[0] = 1;
    bn_to(k, 32, d, c->n.n);
}

int p256_public(uint8_t pub[65], const uint8_t priv[32])
{
    curve_t *c = curve(P256);
    pt_t g, r;
    uint8_t k[32];
    p256_scalar(c, k, priv);
    memcpy(g.x, c->gx, sizeof g.x); memcpy(g.y, c->gy, sizeof g.y); memcpy(g.z, c->p.one, sizeof g.z);
    pt_mul2(&c->p, &r, k, &g, 0, 0, 32);
    pub[0] = 4;
    wipe(k, sizeof k);
    return pt_affine_x(c, pub + 1, pub + 33, &r);
}

int p256_shared(uint8_t out[32], const uint8_t priv[32], const uint8_t *peer, size_t plen)
{
    curve_t *c = curve(P256);
    pt_t q, r;
    uint8_t k[32];
    if (pt_load(c, &q, peer, plen)) return -1;
    p256_scalar(c, k, priv);
    pt_mul2(&c->p, &r, k, &q, 0, 0, 32);
    wipe(k, sizeof k);
    return pt_affine_x(c, out, 0, &r);
}
