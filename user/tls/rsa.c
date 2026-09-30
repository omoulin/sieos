/*
 * rsa.c - RSA signature verification (RFC 8017): RSASSA-PKCS1-v1_5 and
 * RSASSA-PSS with MGF1.  Public-key operations only, moduli up to 4096
 * bits, Montgomery multiplication on 32-bit limbs.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "crypto.h"

#define MAXLIMBS 128                       /* 4096 bits */

struct mont {
    int n;                                 /* limbs */
    uint32_t m[MAXLIMBS];                  /* modulus, little-endian limbs */
    uint32_t minv;                         /* -m^-1 mod 2^32 */
};

static void from_bytes(uint32_t *x, int limbs, const uint8_t *b, size_t len)
{
    memset(x, 0, limbs * 4);
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        x[bit / 32] |= (uint32_t)b[i] << (bit % 32);
    }
}

static void to_bytes(uint8_t *b, size_t len, const uint32_t *x)
{
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        b[i] = (uint8_t)(x[bit / 32] >> (bit % 32));
    }
}

static int cmp(const uint32_t *a, const uint32_t *b, int n)
{
    for (int i = n - 1; i >= 0; i--)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

static void sub_in(uint32_t *a, const uint32_t *b, int n)
{
    uint64_t borrow = 0;
    for (int i = 0; i < n; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)d;
        borrow = (d >> 63) & 1;
    }
}

/* r = a * b * R^-1 mod m  (CIOS) */
static void mont_mul(const struct mont *M, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    int n = M->n;
    uint32_t t[MAXLIMBS + 2] = { 0 };
    for (int i = 0; i < n; i++) {
        uint64_t c = 0;
        for (int j = 0; j < n; j++) {
            uint64_t s = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + c;
            t[j] = (uint32_t)s;
            c = s >> 32;
        }
        uint64_t s = (uint64_t)t[n] + c;
        t[n] = (uint32_t)s;
        t[n + 1] = (uint32_t)(s >> 32);
        uint32_t q = t[0] * M->minv;
        c = ((uint64_t)t[0] + (uint64_t)q * M->m[0]) >> 32;
        for (int j = 1; j < n; j++) {
            s = (uint64_t)t[j] + (uint64_t)q * M->m[j] + c;
            t[j - 1] = (uint32_t)s;
            c = s >> 32;
        }
        s = (uint64_t)t[n] + c;
        t[n - 1] = (uint32_t)s;
        t[n] = t[n + 1] + (uint32_t)(s >> 32);
    }
    if (t[n] || cmp(t, M->m, n) >= 0)
        sub_in(t, M->m, n);
    memcpy(r, t, n * 4);
}

/* out = base^e mod m, all big-endian byte strings of the modulus length. */
static bool rsa_public(const struct rsa_pub *k, const uint8_t *sig, size_t siglen, uint8_t *out)
{
    size_t nlen = k->nlen;
    while (nlen && !k->n[k->nlen - nlen])
        nlen--;
    if (!nlen || nlen > MAXLIMBS * 4 || siglen != nlen || !(k->n[k->nlen - 1] & 1))
        return false;
    const uint8_t *nb = k->n + (k->nlen - nlen);
    static struct mont M;
    M.n = (int)((nlen + 3) / 4);
    from_bytes(M.m, M.n, nb, nlen);
    uint32_t inv = 1;                                  /* Newton: inv = m0^-1 mod 2^32 */
    for (int i = 0; i < 5; i++)
        inv *= 2 - M.m[0] * inv;
    M.minv = -inv;

    static uint32_t s[MAXLIMBS], r2[MAXLIMBS], x[MAXLIMBS], acc[MAXLIMBS];
    from_bytes(s, M.n, sig, siglen);
    if (cmp(s, M.m, M.n) >= 0)
        return false;
    /* r2 = R^2 mod m by doubling 1 (2 * 32 * n) times */
    memset(r2, 0, sizeof(r2));
    r2[0] = 1;
    for (int i = 0; i < 64 * M.n; i++) {
        uint32_t carry = 0;
        for (int j = 0; j < M.n; j++) {
            uint32_t v = r2[j];
            r2[j] = (v << 1) | carry;
            carry = v >> 31;
        }
        if (carry || cmp(r2, M.m, M.n) >= 0)
            sub_in(r2, M.m, M.n);
    }
    mont_mul(&M, x, s, r2);                            /* x = s in Montgomery form */
    memcpy(acc, x, M.n * 4);
    /* exponent bits after the leading one */
    int started = 0;
    for (size_t i = 0; i < k->elen; i++)
        for (int b = 7; b >= 0; b--) {
            int bit = (k->e[i] >> b) & 1;
            if (!started) {
                started = bit;
                continue;
            }
            mont_mul(&M, acc, acc, acc);
            if (bit)
                mont_mul(&M, acc, acc, x);
        }
    if (!started)
        return false;
    uint32_t one[MAXLIMBS] = { 1 };
    mont_mul(&M, acc, acc, one);
    to_bytes(out, nlen, acc);
    return true;
}

static const uint8_t di_sha256[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65,
                                     0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
static const uint8_t di_sha384[] = { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65,
                                     0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 };
static const uint8_t di_sha512[] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65,
                                     0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };

bool rsa_verify_pkcs1(const struct rsa_pub *k, enum hash_alg alg, const uint8_t *digest, const uint8_t *sig,
                      size_t siglen)
{
    static uint8_t em[MAXLIMBS * 4];
    if (!rsa_public(k, sig, siglen, em))
        return false;
    const uint8_t *di = alg == HASH_SHA256 ? di_sha256 : alg == HASH_SHA384 ? di_sha384 : di_sha512;
    size_t dilen = sizeof(di_sha256), hs = hash_size(alg);
    size_t tlen = dilen + hs;
    if (siglen < tlen + 11)
        return false;
    size_t ps = siglen - tlen - 3;
    if (em[0] != 0 || em[1] != 1 || em[2 + ps] != 0)
        return false;
    for (size_t i = 0; i < ps; i++)
        if (em[2 + i] != 0xFF)
            return false;
    return memcmp(em + 3 + ps, di, dilen) == 0 && ct_equal(em + 3 + ps + dilen, digest, hs);
}

static void mgf1(enum hash_alg alg, const uint8_t *seed, size_t seedlen, uint8_t *mask, size_t len)
{
    uint8_t buf[HASH_MAX + 4], h[HASH_MAX];
    size_t hs = hash_size(alg);
    memcpy(buf, seed, seedlen);
    for (uint32_t c = 0; len; c++) {
        buf[seedlen] = (uint8_t)(c >> 24);
        buf[seedlen + 1] = (uint8_t)(c >> 16);
        buf[seedlen + 2] = (uint8_t)(c >> 8);
        buf[seedlen + 3] = (uint8_t)c;
        hash_once(alg, buf, seedlen + 4, h);
        size_t n = len < hs ? len : hs;
        for (size_t i = 0; i < n; i++)
            *mask++ ^= h[i];
        len -= n;
    }
}

/* RSASSA-PSS with MGF1 of the same hash and salt length = hash length. */
bool rsa_verify_pss(const struct rsa_pub *k, enum hash_alg alg, const uint8_t *digest, const uint8_t *sig,
                    size_t siglen)
{
    static uint8_t em[MAXLIMBS * 4];
    if (!rsa_public(k, sig, siglen, em))
        return false;
    size_t nlen = siglen, hs = hash_size(alg), slen = hs;
    /* modBits: bit length of n */
    size_t off = k->nlen - nlen;
    int top = 8;
    while (top && !(k->n[off] & (1 << (top - 1))))
        top--;
    size_t modbits = (nlen - 1) * 8 + top;
    size_t embits = modbits - 1, emlen = (embits + 7) / 8;
    const uint8_t *e = em + (nlen - emlen);
    if (nlen > emlen && em[0] != 0)
        return false;
    if (emlen < hs + slen + 2 || e[emlen - 1] != 0xBC)
        return false;
    size_t dblen = emlen - hs - 1;
    static uint8_t db[MAXLIMBS * 4];
    memcpy(db, e, dblen);
    const uint8_t *hh = e + dblen;
    uint8_t topmask = (uint8_t)(0xFF >> (8 * emlen - embits));
    if (db[0] & ~topmask)
        return false;
    mgf1(alg, hh, hs, db, dblen);
    db[0] &= topmask;
    size_t ps = dblen - slen - 1;
    for (size_t i = 0; i < ps; i++)
        if (db[i])
            return false;
    if (db[ps] != 1)
        return false;
    uint8_t mprime[8 + HASH_MAX + HASH_MAX] = { 0 }, h2[HASH_MAX];
    memcpy(mprime + 8, digest, hs);
    memcpy(mprime + 8 + hs, db + dblen - slen, slen);
    hash_once(alg, mprime, 8 + hs + slen, h2);
    return ct_equal(h2, hh, hs);
}
