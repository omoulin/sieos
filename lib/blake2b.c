/*
 * blake2b.c - BLAKE2b (RFC 7693): a 128-byte block is mixed into an 8-word
 * state by 12 rounds of the "G" function; the last block is flagged, and
 * the digest is the state's first outlen bytes. Used by the kernel (to mix
 * entropy) and by programs (Argon2).
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk/crypto.h"
#include "mk/lib.h"

static const uint64_t IV[8] = {
    0x6A09E667F3BCC908, 0xBB67AE8584CAA73B, 0x3C6EF372FE94F82B, 0xA54FF53A5F1D36F1,
    0x510E527FADE682D1, 0x9B05688C2B3E6C1F, 0x1F83D9ABFB41BD6B, 0x5BE0CD19137E2179,
};
/* The order in which each round reads the 16 message words. */
static const uint8_t SIGMA[12][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 }, { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
    { 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 }, { 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
    { 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 }, { 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
    { 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 }, { 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
    { 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 }, { 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 }, { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
};
#define ROTR(x, n) ((x) >> (n) | (x) << (64 - (n)))
#define G(a, b, c, d, x, y) do { a += b + x; d = ROTR(d ^ a, 32); c += d; b = ROTR(b ^ c, 24); \
                                 a += b + y; d = ROTR(d ^ a, 16); c += d; b = ROTR(b ^ c, 63); } while (0)

static void compress(blake2b_t *s, int last)
{
    uint64_t v[16], m[16];
    memcpy(m, s->b, sizeof m);                 /* little-endian words (x86) */
    for (int i = 0; i < 8; i++) { v[i] = s->h[i]; v[i + 8] = IV[i]; }
    v[12] ^= s->t;                             /* bytes so far (inputs here stay below 2^64) */
    if (last) v[14] = ~v[14];
    for (int r = 0; r < 12; r++) {
        const uint8_t *z = SIGMA[r];
        G(v[0], v[4], v[8],  v[12], m[z[0]],  m[z[1]]);
        G(v[1], v[5], v[9],  v[13], m[z[2]],  m[z[3]]);
        G(v[2], v[6], v[10], v[14], m[z[4]],  m[z[5]]);
        G(v[3], v[7], v[11], v[15], m[z[6]],  m[z[7]]);
        G(v[0], v[5], v[10], v[15], m[z[8]],  m[z[9]]);
        G(v[1], v[6], v[11], v[12], m[z[10]], m[z[11]]);
        G(v[2], v[7], v[8],  v[13], m[z[12]], m[z[13]]);
        G(v[3], v[4], v[9],  v[14], m[z[14]], m[z[15]]);
    }
    for (int i = 0; i < 8; i++) s->h[i] ^= v[i] ^ v[i + 8];
}

void blake2b_init(blake2b_t *s, size_t outlen, const void *key, size_t keylen)
{
    memcpy(s->h, IV, sizeof IV);
    s->h[0] ^= 0x01010000 ^ keylen << 8 ^ outlen;   /* the parameter block: depth 1, fanout 1 */
    s->t = s->c = 0;
    s->outlen = outlen;
    if (keylen) {                                  /* a key is a first, zero-padded block */
        memset(s->b, 0, sizeof s->b);
        memcpy(s->b, key, keylen);
        s->c = 128;
    }
}

void blake2b_update(blake2b_t *s, const void *in, size_t n)
{
    const uint8_t *p = in;
    while (n) {
        if (s->c == 128) { s->t += 128; compress(s, 0); s->c = 0; }   /* keep the last block for final */
        size_t k = 128 - s->c < n ? 128 - s->c : n;
        memcpy(s->b + s->c, p, k);
        s->c += k; p += k; n -= k;
    }
}

void blake2b_final(blake2b_t *s, void *out)
{
    s->t += s->c;
    memset(s->b + s->c, 0, 128 - s->c);
    compress(s, 1);
    memcpy(out, s->h, s->outlen);
}

void blake2b(void *out, size_t outlen, const void *in, size_t n)
{
    blake2b_t s;
    blake2b_init(&s, outlen, 0, 0);
    blake2b_update(&s, in, n);
    blake2b_final(&s, out);
}
