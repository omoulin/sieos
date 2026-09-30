/*
 * sha2.c - SHA-256, SHA-384, SHA-512 (FIPS 180-4), HMAC and HKDF.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "crypto.h"

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
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};
static const uint32_t H256[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
static const uint64_t H512[8] = {
    0x6a09e667f3bcc908ULL,
    0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL,
    0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL,
    0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL,
    0x5be0cd19137e2179ULL };
static const uint64_t H384[8] = {
    0xcbbb9d5dc1059ed8ULL,
    0x629a292a367cd507ULL,
    0x9159015a3070dd17ULL,
    0x152fecd8f70e5939ULL,
    0x67332667ffc00b31ULL,
    0x8eb44a8768581511ULL,
    0xdb0c2e0d64f98fa7ULL,
    0x47b5481dbefa4fa4ULL };

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void sha256_compress(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha512_compress(uint64_t h[8], const uint8_t *p)
{
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        uint64_t v = 0;
        for (int j = 0; j < 8; j++)
            v = v << 8 | p[8 * i + j];
        w[i] = v;
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t t1 = hh + (ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
        uint64_t t2 = (ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

size_t hash_size(enum hash_alg alg)
{
    return alg == HASH_SHA256 ? 32 : alg == HASH_SHA384 ? 48 : 64;
}

size_t hash_block(enum hash_alg alg)
{
    return alg == HASH_SHA256 ? 64 : 128;
}

void hash_init(struct hash_ctx *c, enum hash_alg alg)
{
    memset(c, 0, sizeof(*c));
    c->alg = alg;
    if (alg == HASH_SHA256)
        memcpy(c->s.h32, H256, sizeof(H256));
    else
        memcpy(c->s.h64, alg == HASH_SHA384 ? H384 : H512, sizeof(H512));
}

static void compress(struct hash_ctx *c, const uint8_t *p)
{
    if (c->alg == HASH_SHA256)
        sha256_compress(c->s.h32, p);
    else
        sha512_compress(c->s.h64, p);
}

void hash_update(struct hash_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t bs = hash_block(c->alg);
    c->total += len;
    if (c->buflen) {
        size_t n = bs - c->buflen < len ? bs - c->buflen : len;
        memcpy(c->buf + c->buflen, p, n);
        c->buflen += n;
        p += n;
        len -= n;
        if (c->buflen == bs) {
            compress(c, c->buf);
            c->buflen = 0;
        }
    }
    while (len >= bs) {
        compress(c, p);
        p += bs;
        len -= bs;
    }
    memcpy(c->buf, p, len);
    c->buflen += len;
}

void hash_final(struct hash_ctx *c, uint8_t *out)
{
    size_t bs = hash_block(c->alg), lenbytes = bs == 64 ? 8 : 16;
    uint64_t bits = c->total * 8;
    uint8_t pad[HASH_BLOCK_MAX * 2];
    size_t padlen = bs - c->buflen;
    if (padlen < lenbytes + 1)
        padlen += bs;
    memset(pad, 0, padlen);
    pad[0] = 0x80;
    for (int i = 0; i < 8; i++)
        pad[padlen - 1 - i] = (uint8_t)(bits >> (8 * i));
    hash_update(c, pad, padlen);
    size_t n = hash_size(c->alg);
    for (size_t i = 0; i < n; i++)
        out[i] = c->alg == HASH_SHA256 ? (uint8_t)(c->s.h32[i / 4] >> (24 - 8 * (i % 4)))
                                       : (uint8_t)(c->s.h64[i / 8] >> (56 - 8 * (i % 8)));
    memset(c, 0, sizeof(*c));
}

void hash_once(enum hash_alg alg, const void *data, size_t len, uint8_t *out)
{
    struct hash_ctx c;
    hash_init(&c, alg);
    hash_update(&c, data, len);
    hash_final(&c, out);
}

void hmac(enum hash_alg alg, const uint8_t *key, size_t keylen, const void *msg, size_t len, uint8_t *out)
{
    size_t bs = hash_block(alg), hs = hash_size(alg);
    uint8_t k[HASH_BLOCK_MAX] = { 0 }, pad[HASH_BLOCK_MAX], inner[HASH_MAX];
    if (keylen > bs)
        hash_once(alg, key, keylen, k);
    else
        memcpy(k, key, keylen);
    struct hash_ctx c;
    for (size_t i = 0; i < bs; i++)
        pad[i] = k[i] ^ 0x36;
    hash_init(&c, alg);
    hash_update(&c, pad, bs);
    hash_update(&c, msg, len);
    hash_final(&c, inner);
    for (size_t i = 0; i < bs; i++)
        pad[i] = k[i] ^ 0x5c;
    hash_init(&c, alg);
    hash_update(&c, pad, bs);
    hash_update(&c, inner, hs);
    hash_final(&c, out);
    memset(k, 0, sizeof(k));
}

void hkdf_extract(enum hash_alg alg, const uint8_t *salt, size_t saltlen, const uint8_t *ikm, size_t ikmlen,
                  uint8_t *prk)
{
    uint8_t zero[HASH_MAX] = { 0 };
    if (!salt) {
        salt = zero;
        saltlen = hash_size(alg);
    }
    hmac(alg, salt, saltlen, ikm, ikmlen, prk);
}

void hkdf_expand(enum hash_alg alg, const uint8_t *prk, const uint8_t *info, size_t infolen, uint8_t *out,
                 size_t outlen)
{
    size_t hs = hash_size(alg);
    uint8_t t[HASH_MAX], buf[HASH_MAX + 256 + 1];
    size_t tlen = 0;
    for (uint8_t i = 1; outlen; i++) {
        memcpy(buf, t, tlen);
        memcpy(buf + tlen, info, infolen);
        buf[tlen + infolen] = i;
        hmac(alg, prk, hs, buf, tlen + infolen + 1, t);
        tlen = hs;
        size_t n = outlen < hs ? outlen : hs;
        memcpy(out, t, n);
        out += n;
        outlen -= n;
    }
}

void hkdf_expand_label(enum hash_alg alg, const uint8_t *secret, const char *label, const uint8_t *ctx,
                       size_t ctxlen, uint8_t *out, size_t outlen)
{
    uint8_t info[2 + 1 + 255 + 1 + 64];
    size_t ll = strlen(label), n = 0;
    info[n++] = (uint8_t)(outlen >> 8);
    info[n++] = (uint8_t)outlen;
    info[n++] = (uint8_t)(6 + ll);
    memcpy(info + n, "tls13 ", 6);
    n += 6;
    memcpy(info + n, label, ll);
    n += ll;
    info[n++] = (uint8_t)ctxlen;
    memcpy(info + n, ctx, ctxlen);
    n += ctxlen;
    hkdf_expand(alg, secret, info, n, out, outlen);
}

bool ct_equal(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++)
        d |= x[i] ^ y[i];
    return d == 0;
}
