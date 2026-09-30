/*
 * crypt.c - SHA-256 and the SIEOS password hash (libsieos).
 *
 * Hash format: "$5a$<salt>$<hex>", where
 *     h = SHA256(salt "$" password), then 5000 times h = SHA256(h password salt).
 * This is an SIEOS-specific scheme (tools/mkshadow.py implements the same),
 * not glibc's sha256-crypt.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256(const void *data, size_t len, uint8_t out[32])
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const uint8_t *p = data;
    size_t left = len;
    while (left >= 64) {
        sha256_block(h, p);
        p += 64;
        left -= 64;
    }
    uint8_t tail[128];
    memset(tail, 0, sizeof(tail));
    memcpy(tail, p, left);
    tail[left] = 0x80;
    size_t tlen = left + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        tail[tlen - 1 - i] = bits >> (8 * i);
    sha256_block(h, tail);
    if (tlen == 128)
        sha256_block(h, tail + 64);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = h[i] >> 24;
        out[i * 4 + 1] = h[i] >> 16;
        out[i * 4 + 2] = h[i] >> 8;
        out[i * 4 + 3] = h[i];
    }
}

char *crypt_password(const char *password, const char *salt)
{
    static char result[128];
    char buf[256];
    uint8_t h[32];
    size_t pl = strlen(password), sl = strlen(salt);
    if (pl > 100 || sl > 32)
        return NULL;
    int n = snprintf(buf, sizeof(buf), "%s$%s", salt, password);
    sha256(buf, n, h);
    for (int i = 0; i < 5000; i++) {
        memcpy(buf, h, 32);
        memcpy(buf + 32, password, pl);
        memcpy(buf + 32 + pl, salt, sl);
        sha256(buf, 32 + pl + sl, h);
    }
    int off = snprintf(result, sizeof(result), "$5a$%s$", salt);
    for (int i = 0; i < 32; i++)
        off += snprintf(result + off, sizeof(result) - off, "%02x", h[i]);
    return result;
}

bool check_password(const char *password, const char *hash)
{
    if (!hash || strncmp(hash, "$5a$", 4) != 0)
        return hash && hash[0] == 0;          /* empty field = no password */
    const char *salt = hash + 4;
    const char *end = strchr(salt, '$');
    if (!end || end - salt > 32)
        return false;
    char s[33];
    memcpy(s, salt, end - salt);
    s[end - salt] = 0;
    char *c = crypt_password(password, s);
    return c && strcmp(c, hash) == 0;
}
