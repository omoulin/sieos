/*
 * sha2.c - SHA-256 and SHA-384/512 (FIPS 180-4), HMAC (RFC 2104) and HKDF
 * (RFC 5869): the hashes and key derivation TLS 1.3 is built on.
 *
 * A hash reads its input in blocks (64 bytes for SHA-256, 128 for SHA-512)
 * and stirs each one into a small state with additions, rotations and
 * bitwise functions; the final state, after a padding block that encodes
 * the length, is the digest. SHA-384 is SHA-512 with other starting values,
 * cut to 48 bytes.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk/crypto.h"
#include "mk/lib.h"

#define ROR32(x, n) ((x) >> (n) | (x) << (32 - (n)))
#define ROR64(x, n) ((x) >> (n) | (x) << (64 - (n)))

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static const uint64_t K512[80] = {
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538,
    0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242, 0x12835b0145706fbe,
    0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2, 0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235,
    0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65,
    0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5, 0x983e5152ee66dfab,
    0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725,
    0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed,
    0x53380d139d95b3df, 0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
    0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218,
    0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8, 0x19a4c116b8d2d0c8, 0x1e376c085141ab53,
    0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373,
    0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b, 0xca273eceea26619c,
    0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6,
    0x113f9804bef90dae, 0x1b710b35131c471b, 0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc,
    0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817,
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static void block256(uint32_t h[8], const uint8_t *b)
{
    uint32_t w[64], s[8];
    for (int i = 0; i < 16; i++) w[i] = be32(b + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ w[i - 15] >> 3;
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ w[i - 2] >> 10;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(s, h, sizeof s);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = s[7] + (ROR32(s[4], 6) ^ ROR32(s[4], 11) ^ ROR32(s[4], 25)) +
                      ((s[4] & s[5]) ^ (~s[4] & s[6])) + K256[i] + w[i];
        uint32_t t2 = (ROR32(s[0], 2) ^ ROR32(s[0], 13) ^ ROR32(s[0], 22)) +
                      ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
        memmove(s + 1, s, 7 * sizeof s[0]);
        s[4] += t1;
        s[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) h[i] += s[i];
}

static void block512(uint64_t h[8], const uint8_t *b)
{
    uint64_t w[80], s[8];
    for (int i = 0; i < 16; i++) w[i] = be64(b + 8 * i);
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ w[i - 15] >> 7;
        uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ w[i - 2] >> 6;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(s, h, sizeof s);
    for (int i = 0; i < 80; i++) {
        uint64_t t1 = s[7] + (ROR64(s[4], 14) ^ ROR64(s[4], 18) ^ ROR64(s[4], 41)) +
                      ((s[4] & s[5]) ^ (~s[4] & s[6])) + K512[i] + w[i];
        uint64_t t2 = (ROR64(s[0], 28) ^ ROR64(s[0], 34) ^ ROR64(s[0], 39)) +
                      ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
        memmove(s + 1, s, 7 * sizeof s[0]);
        s[4] += t1;
        s[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) h[i] += s[i];
}

void sha_init(sha_t *s, int alg)
{
    static const uint32_t i256[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    static const uint64_t i384[8] = { 0xcbbb9d5dc1059ed8, 0x629a292a367cd507, 0x9159015a3070dd17,
                                      0x152fecd8f70e5939, 0x67332667ffc00b31, 0x8eb44a8768581511,
                                      0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4 };
    static const uint64_t i512[8] = { 0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b,
                                      0xa54ff53a5f1d36f1, 0x510e527fade682d1, 0x9b05688c2b3e6c1f,
                                      0x1f83d9abfb41bd6b, 0x5be0cd19137e2179 };
    memset(s, 0, sizeof *s);
    s->alg = alg;
    if (alg == SHA256) { memcpy(s->h32, i256, sizeof i256); s->bs = 64; s->len = 32; }
    else { memcpy(s->h64, alg == SHA384 ? i384 : i512, 64); s->bs = 128; s->len = alg == SHA384 ? 48 : 64; }
}

void sha_update(sha_t *s, const void *in, size_t n)
{
    const uint8_t *p = in;
    s->total += n;
    while (n) {
        size_t c = s->bs - s->fill < n ? s->bs - s->fill : n;
        memcpy(s->buf + s->fill, p, c);
        s->fill += c; p += c; n -= c;
        if (s->fill == s->bs) {
            if (s->bs == 64) block256(s->h32, s->buf); else block512(s->h64, s->buf);
            s->fill = 0;
        }
    }
}

void sha_final(sha_t *s, void *out)
{
    uint64_t bits = s->total * 8;
    uint8_t pad = 0x80, z = 0, l[16] = { 0 };
    for (int i = 0; i < 8; i++) l[15 - i] = bits >> (8 * i);
    size_t lenbytes = s->bs == 64 ? 8 : 16;
    sha_update(s, &pad, 1);
    while (s->fill != s->bs - lenbytes) sha_update(s, &z, 1);
    sha_update(s, l + 16 - lenbytes, lenbytes);
    uint8_t *o = out;
    for (size_t i = 0; i < s->len; i++)
        o[i] = s->bs == 64 ? s->h32[i / 4] >> (24 - 8 * (i % 4)) : s->h64[i / 8] >> (56 - 8 * (i % 8));
}

void sha(int alg, void *out, const void *in, size_t n)
{
    sha_t s;
    sha_init(&s, alg);
    sha_update(&s, in, n);
    sha_final(&s, out);
}

/* ---- HMAC: H((K ^ opad) || H((K ^ ipad) || message)). */
void hmac_init(hmac_t *h, int alg, const void *key, size_t klen)
{
    uint8_t k[128] = { 0 }, pad[128];
    sha_init(&h->inner, alg);
    if (klen > h->inner.bs) { sha(alg, k, key, klen); klen = h->inner.len; }
    else memcpy(k, key, klen);
    for (size_t i = 0; i < h->inner.bs; i++) pad[i] = k[i] ^ 0x36;
    sha_update(&h->inner, pad, h->inner.bs);
    sha_init(&h->outer, alg);
    for (size_t i = 0; i < h->outer.bs; i++) pad[i] = k[i] ^ 0x5c;
    sha_update(&h->outer, pad, h->outer.bs);
    wipe(k, sizeof k);
}
void hmac_update(hmac_t *h, const void *in, size_t n) { sha_update(&h->inner, in, n); }
void hmac_final(hmac_t *h, void *out)
{
    uint8_t d[64];
    sha_final(&h->inner, d);
    sha_update(&h->outer, d, h->inner.len);
    sha_final(&h->outer, out);
}
void hmac(int alg, void *out, const void *key, size_t klen, const void *in, size_t n)
{
    hmac_t h;
    hmac_init(&h, alg, key, klen);
    hmac_update(&h, in, n);
    hmac_final(&h, out);
}

/* ---- HKDF: Extract = HMAC(salt, input key material); Expand = a chain of
 * HMACs over (previous block, info, counter), cut to the length wanted. */
void hkdf_extract(int alg, void *prk, const void *salt, size_t slen, const void *ikm, size_t ilen)
{
    uint8_t zero[64] = { 0 };
    if (!salt) { salt = zero; slen = sha_len(alg); }
    hmac(alg, prk, salt, slen, ikm, ilen);
}

void hkdf_expand(int alg, void *out, size_t olen, const void *prk, size_t plen, const void *info, size_t ilen)
{
    uint8_t t[64], c = 0, *o = out;
    size_t tl = 0, hl = sha_len(alg);
    while (olen) {
        hmac_t h;
        c++;
        hmac_init(&h, alg, prk, plen);
        hmac_update(&h, t, tl);
        hmac_update(&h, info, ilen);
        hmac_update(&h, &c, 1);
        hmac_final(&h, t);
        tl = hl;
        size_t n = olen < hl ? olen : hl;
        memcpy(o, t, n);
        o += n; olen -= n;
    }
}

size_t sha_len(int alg) { return alg == SHA256 ? 32 : alg == SHA384 ? 48 : 64; }
