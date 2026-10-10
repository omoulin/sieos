/*
 * test.c - The crypto library (lib/blake2b.c, chacha20.c, argon2.c) against
 * the official test vectors of RFC 7693, RFC 8439 and RFC 9106, plus a
 * timing of Argon2id at the parameters SIEOS uses.
 * With "-x": print BLAKE2b digests of a fixed pattern, for check.py to
 * compare with an independent implementation.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mk/crypto.h"

static int fails;

static void hex(char *o, const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) sprintf(o + 2 * i, "%02x", p[i]); }

static void expect(const char *what, const uint8_t *got, size_t n, const char *want)
{
    char h[300];
    hex(h, got, n);
    int ok = !strcmp(h, want);
    printf("  %-34s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) { printf("    got  %s\n    want %s\n", h, want); fails++; }
}

int main(int argc, char **argv)
{
    uint8_t out[128], key[64], in[256];
    if (argc > 1 && !strcmp(argv[1], "-x")) {         /* digests for check.py */
        for (int i = 0; i < 256; i++) in[i] = (uint8_t)(i * 7 + 1);
        for (int i = 0; i < 64; i++) key[i] = (uint8_t)i;
        for (int n = 0; n <= 256; n += 3)
            for (int ol = 1; ol <= 64; ol += 21) {
                blake2b_t s;
                char h[200];
                blake2b_init(&s, ol, key, n % 65);
                blake2b_update(&s, in, n);
                blake2b_final(&s, out);
                hex(h, out, ol);
                printf("%d %d %d %s\n", n, ol, n % 65, h);
            }
        return 0;
    }

    printf("BLAKE2b (RFC 7693)\n");
    blake2b(out, 64, "abc", 3);
    expect("BLAKE2b-512(\"abc\")", out, 64,
        "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923");
    blake2b(out, 64, "", 0);
    expect("BLAKE2b-512(\"\")", out, 64,
        "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce");

    printf("ChaCha20 (RFC 8439, 2.3.2)\n");
    uint32_t k[8], nonce[3] = { 0x09000000, 0x4a000000, 0 };
    for (int i = 0; i < 32; i++) ((uint8_t *)k)[i] = (uint8_t)i;
    chacha20_block(k, 1, nonce, out);
    expect("block, counter 1", out, 64,
        "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4ed2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e");

    printf("Argon2 (RFC 9106, 5.1-5.3: m=32 KiB, t=3, p=4, 32-byte tag)\n");
    uint8_t pwd[32], salt[16], secret[8], ad[12];
    memset(pwd, 1, 32); memset(salt, 2, 16); memset(secret, 3, 8); memset(ad, 4, 12);
    static const char *want[3] = {
        "512b391b6f1162975371d30919734294f868e3be3984f3c1a13a4db9fabe4acb",
        "c814d9d1dc7f37aa13f0d77f2494bda1c8de6b016dd388d29952a4c4672b6ce8",
        "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659" };
    static const char *name[3] = { "Argon2d", "Argon2i", "Argon2id" };
    void *mem = malloc(32 << 10);
    for (int ty = 0; ty < 3; ty++) {
        argon2_t a = { pwd, salt, secret, ad, 32, 16, 8, 12, 3, 32, 4, (uint32_t)ty };
        argon2(&a, out, 32, mem);
        expect(name[ty], out, 32, want[ty]);
    }
    free(mem);

    printf("Argon2id, 16 MiB (SIEOS uses t = 24), on the host with its compiler's vector code\n");
    int ms[] = { 16384 };
    for (int i = 0; i < 1; i++)
        for (uint32_t t = 3; t <= 24; t += 21) {
            argon2_t a = { "correct horse", salt, 0, 0, 13, 16, 0, 0, t, (uint32_t)ms[i], 1, ARGON2_ID };
            mem = malloc((size_t)ms[i] << 10);
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            argon2(&a, out, 32, mem);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            printf("  m = %5d KiB, t = %u: %6.1f ms\n", ms[i], t, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
            free(mem);
        }
    printf(fails ? "crypto tests: %d FAILED\n" : "crypto tests: all passed\n", fails);
    return fails != 0;
}
