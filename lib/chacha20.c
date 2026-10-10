/*
 * chacha20.c - The ChaCha20 block function (RFC 8439): a 16-word state
 * (constants, 256-bit key, block counter, 96-bit nonce) goes through 20
 * rounds of add-rotate-xor; added to its starting value, it is 64 bytes of
 * keystream that cannot be predicted without the key.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk/crypto.h"
#include "mk/lib.h"

#define ROTL(x, n) ((x) << (n) | (x) >> (32 - (n)))
#define QR(a, b, c, d) do { a += b; d = ROTL(d ^ a, 16); c += d; b = ROTL(b ^ c, 12); \
                            a += b; d = ROTL(d ^ a, 8);  c += d; b = ROTL(b ^ c, 7); } while (0)

void chacha20_block(const uint32_t key[8], uint32_t counter, const uint32_t nonce[3], uint8_t out[64])
{
    uint32_t s[16] = { 0x61707865, 0x3320646E, 0x79622D32, 0x6B206574 };   /* "expand 32-byte k" */
    memcpy(s + 4, key, 32);
    s[12] = counter;
    memcpy(s + 13, nonce, 12);
    uint32_t x[16];
    memcpy(x, s, sizeof x);
    for (int i = 0; i < 10; i++) {                 /* 10 double rounds: columns, then diagonals */
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) x[i] += s[i];
    memcpy(out, x, 64);                            /* little-endian words (x86) */
}
