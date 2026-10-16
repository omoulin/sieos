/*
 * test.c - The crypto library (lib/blake2b.c, chacha20.c, argon2.c, and SSH's
 * ed25519.c and Poly1305 from aead.c) against the official test vectors of
 * RFC 7693, RFC 8439, RFC 9106 and RFC 8032, plus a timing of Argon2id at
 * the parameters SIEOS uses.
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

static void unhex(uint8_t *o, const char *h) { for (; h[0] && h[1]; h += 2) { unsigned v; sscanf(h, "%2x", &v); *o++ = (uint8_t)v; } }

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

    printf("Poly1305 (RFC 8439, 2.5.2)\n");
    unhex(key, "85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
    poly1305(out, key, "Cryptographic Forum Research Group", 34);
    expect("tag", out, 16, "a8061dc1305136c6c22b8baf0c0127a9");

    printf("Ed25519 (RFC 8032, 7.1)\n");
    static const char *ed[4][4] = {     /* secret key, message, public key, signature */
        { "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "",
          "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
          "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
        { "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "72",
          "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
          "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
        { "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7", "af82",
          "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
          "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
        { "833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
          "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
          "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704" } };
    for (int t = 0; t < 4; t++) {
        uint8_t sk[32], pk[32], sig[64], msg[64];
        size_t ml = strlen(ed[t][1]) / 2;
        char what[40];
        unhex(sk, ed[t][0]); unhex(msg, ed[t][1]);
        ed25519_public(pk, sk);
        snprintf(what, sizeof what, "test %d: public key", t + 1); expect(what, pk, 32, ed[t][2]);
        ed25519_sign(sig, msg, ml, sk, pk);
        snprintf(what, sizeof what, "test %d: signature", t + 1); expect(what, sig, 64, ed[t][3]);
        int ok = !ed25519_verify(sig, msg, ml, pk);
        sig[t * 9] ^= 4;                              /* a damaged signature (R or S) */
        int bad = ed25519_verify(sig, msg, ml, pk);
        sig[t * 9] ^= 4; sig[63] |= 0xe0;             /* S >= L */
        int big = ed25519_verify(sig, msg, ml, pk);
        snprintf(what, sizeof what, "test %d: verify, damaged, S >= L", t + 1);
        printf("  %-34s %s\n", what, ok && bad && big ? "ok" : "FAIL");
        if (!(ok && bad && big)) fails++;
    }

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
