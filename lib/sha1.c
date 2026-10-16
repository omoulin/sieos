/*
 * sha1.c - SHA-1 (FIPS 180-4): the hash that names git's objects. Too weak
 * for signatures today, it is only used where git's format needs it (the
 * name of a file, a tree, a commit; a pack's and an index's checksum).
 *
 * Like SHA-256 (sha2.c): 64-byte blocks stirred into a state of five
 * 32-bit words, 80 rounds each; a padding block with the length ends it.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk/crypto.h"
#include "mk/lib.h"

#define ROL(x, n) ((x) << (n) | (x) >> (32 - (n)))

static void block(uint32_t h[5], const uint8_t *b)
{
    uint32_t w[80], a = h[0], c1 = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4 * i] << 24 | b[4 * i + 1] << 16 | b[4 * i + 2] << 8 | b[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f = i < 20 ? ((c1 & c) | (~c1 & d)) + 0x5a827999 : i < 40 ? (c1 ^ c ^ d) + 0x6ed9eba1 :
                     i < 60 ? ((c1 & c) | (c1 & d) | (c & d)) + 0x8f1bbcdc : (c1 ^ c ^ d) + 0xca62c1d6;
        uint32_t t = ROL(a, 5) + f + e + w[i];
        e = d; d = c; c = ROL(c1, 30); c1 = a; a = t;
    }
    h[0] += a; h[1] += c1; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_init(sha1_t *s)
{
    static const uint32_t iv[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    memcpy(s->h, iv, sizeof iv);
    s->fill = 0; s->total = 0;
}

void sha1_update(sha1_t *s, const void *in, size_t n)
{
    const uint8_t *p = in;
    s->total += n;
    if (s->fill) {
        size_t c = 64 - s->fill < n ? 64 - s->fill : n;
        memcpy(s->buf + s->fill, p, c);
        s->fill += c; p += c; n -= c;
        if (s->fill < 64) return;
        block(s->h, s->buf);
        s->fill = 0;
    }
    for (; n >= 64; n -= 64, p += 64) block(s->h, p);
    memcpy(s->buf, p, n);
    s->fill = n;
}

void sha1_final(sha1_t *s, uint8_t out[20])
{
    uint64_t bits = s->total * 8;
    uint8_t pad[72] = { 0x80 };
    size_t n = (s->fill < 56 ? 56 : 120) - s->fill;
    for (int i = 0; i < 8; i++) pad[n + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, pad, n + 8);
    for (int i = 0; i < 20; i++) out[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

void sha1(uint8_t out[20], const void *in, size_t n)
{
    sha1_t s;
    sha1_init(&s);
    sha1_update(&s, in, n);
    sha1_final(&s, out);
}
