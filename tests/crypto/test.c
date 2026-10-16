/*
 * test.c - The crypto library (lib/blake2b.c, chacha20.c, argon2.c, and SSH's
 * ed25519.c and Poly1305 from aead.c) against the official test vectors of
 * RFC 7693, RFC 8439, RFC 9106 and RFC 8032, plus a timing of Argon2id at
 * the parameters SIEOS uses; SHA-1 (FIPS 180's examples), CRC-32's check
 * value, Bluetooth pairing's functions (lib/btcrypt.c: the spec's sample
 * data; the P-256 key exchange, pubkey.c), and zlib streams made by the reference zlib (stored, fixed and
 * dynamic blocks) inflated (lib/sha1.c, zlib.c).
 * With "-x": print BLAKE2b digests of a fixed pattern, for check.py to
 * compare with an independent implementation; "-z" / "-u": deflate /
 * inflate standard input to standard output (check.py against the host's zlib).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mk/crypto.h"
#include "mk/zlib.h"

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

static size_t unhex(uint8_t *o, const char *h) { size_t n = 0; for (; h[0] && h[1]; h += 2) sscanf(h, "%2hhx", &o[n++]); return n; }

static int filter(int z)                             /* -z, -u: stdin -> stdout */
{
    size_t n = 0, cap = 1 << 20;
    uint8_t *in = malloc(cap);
    for (size_t r; (r = fread(in + n, 1, cap - n, stdin)) > 0; ) if ((n += r) == cap) in = realloc(in, cap *= 2);
    size_t ocap = z ? ZBOUND(n) : 64 << 20, used;
    uint8_t *out = malloc(ocap);
    long len = z ? (long)zdeflate(in, n, out) : zinflate(in, n, out, ocap, &used);
    if (len < 0 || (!z && used != n)) { fprintf(stderr, "bad stream\n"); return 1; }
    fwrite(out, 1, (size_t)len, stdout);
    return 0;
}

static void check(const char *what, int ok, const char *more)
{
    printf("  %-34s %s%s\n", what, ok ? "ok" : "FAIL", more);
    fails += !ok;
}

int main(int argc, char **argv)
{
    uint8_t out[128], key[64], in[256];
    if (argc > 1 && (!strcmp(argv[1], "-z") || !strcmp(argv[1], "-u"))) return filter(argv[1][1] == 'z');
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

    printf("SHA-1 (FIPS 180-4 examples)\n");
    sha1(out, "abc", 3);
    expect("SHA-1(\"abc\")", out, 20, "a9993e364706816aba3e25717850c26c9cd0d89d");
    sha1(out, "", 0);
    expect("SHA-1(\"\")", out, 20, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    sha1(out, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
    expect("SHA-1(448 bits)", out, 20, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    sha1_t s1;
    sha1_init(&s1);
    memset(in, 'a', 250);
    for (int i = 0; i < 4000; i++) sha1_update(&s1, in, 250);
    sha1_final(&s1, out);
    expect("SHA-1(a million 'a')", out, 20, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");

    printf("CRC-32 and zlib (streams made by the reference zlib)\n");
    uint32_t c32 = crc32(0, "123456789", 9);
    expect("CRC-32(\"123456789\")", (uint8_t[]){ c32 >> 24, c32 >> 16, c32 >> 8, c32 }, 4, "cbf43926");
    static uint8_t zs[2048], zo[4096], text[2048];
    size_t used, tl = 0;
    long zl = zinflate(zs, unhex(zs, "7801010d00f2ff53746f72656420626c6f636b2e22cb04cb"), zo, sizeof zo, &used);
    expect("inflate a stored block", zo, zl > 0 ? (size_t)zl : 0, "53746f72656420626c6f636b2e");
    zl = zinflate(zs, unhex(zs, "78da4bcbac484d5148ce4f492dd65148c3c15104000ab20d74"), zo, sizeof zo, &used);
    zo[zl > 0 ? zl : 0] = 0;
    check("inflate fixed codes", zl == 38 && used == 25 && !strcmp((char *)zo, "fixed codes, fixed codes, fixed codes!"), "");
    for (int i = 0; i < 40; i++) tl += (size_t)sprintf((char *)text + tl, "line %d of a text with dynamic Huffman codes\n", i);
    size_t zn = unhex(zs, "78da95d4cd0dc2301044e13b556c09ec2c7fe9206d58892d2c25ce218e80ee111df0eeeff66966a92ddbd9b662c97a7e"
        "777bd5feb4f9d3d25a271b8f52d6d46cdae6bc9f965feba0156803b417d05e417b03ed1db40fd00ec402c1113927744eec9ce039d1"
        "73c2e7c4cf09a013411141a1ed114111411141114111411141114111c12082410403dd27110c22184430886010c12082f1a7e017330a6b57");
    zl = zinflate(zs, zn, zo, sizeof zo, &used);
    check("inflate dynamic codes", zl == (long)tl && used == zn && !memcmp(zo, text, tl), "");
    zl = zinflate(zs, zn, zo, 32, &used);
    check("inflate into a short buffer", zl == 32 && used == 0 && !memcmp(zo, text, 32), "");
    zs[zn - 1] ^= 1;
    check("a wrong checksum is refused", zinflate(zs, zn, zo, sizeof zo, &used) == -1, "");
    zn = zdeflate(text, tl, zs);
    char more[64];
    snprintf(more, sizeof more, " (%zu -> %zu bytes; zlib -9: 157)", tl, zn);
    check("deflate, inflated again", zinflate(zs, zn, zo, sizeof zo, &used) == (long)tl && used == zn && !memcmp(zo, text, tl), more);


    printf("Bluetooth pairing (Core spec Vol 3 Part H D.1-D.7, Vol 2 Part G 7.1; SMP's byte order)\n");
    {
        uint8_t u[32], v[32], x[16], y[16], w[32], n1[16], n2[16], a1[7], a2[7], r[16], mk[16], ltk[16], k0[16] = { 0 };
        uint8_t pa[32], pb[32], qa[64], qb[64], dh[32], ia[6], ra[6], preq[7], pres[7], io[3];
        unhex(u, "e69d350e480103ccdbfdf4ac1191f4efb9a5f9e9a7832c5e2cbe97f2d203b020");
        unhex(v, "fdc57ff449dd4f6bfb7c9df1c29acb592ae7d4eefbfc0a909abbf6323d8b1855");
        unhex(x, "abae2b71ecb2ffff3e7377d15484cbd5");
        unhex(y, "cfc43dfff78365216e5fa725cce7e8a6");
        bt_f4(out, u, v, x, 0);
        expect("f4", out, 16, "2d8774a9bea1edf11cbda907f116c9f2");
        unhex(w, "98a6bf73f3348d86f166f8b4136b79999b7d390aa610103405adc857a33402ec");
        unhex(a1, "cebf3737125600"); unhex(a2, "c1cf2d7013a700");
        bt_f5(mk, ltk, w, x, y, a1, a2);
        expect("f5 MacKey", mk, 16, "206e63ce206a3ffd024a08a176f16529");
        expect("f5 LTK", ltk, 16, "380a7594b522059823cdd76911798669");
        unhex(r, "c80f2d0cd242da0854bb53b43b34a312"); unhex(io, "020101");
        bt_f6(out, mk, x, y, r, io, a1, a2);
        expect("f6", out, 16, "618f95da090b6cd2c5e8d09c9873c4e3");
        check("g2", bt_g2(u, v, x, y) == 0x2f9ed5ba % 1000000, "");
        unhex(r, "e02e70c64e2788630e6fad5621d58357");
        unhex(preq, "01010000100707"); unhex(pres, "02030000080005");
        unhex(ia, "a6a5a4a3a2a1"); unhex(ra, "b6b5b4b3b2b1");
        bt_c1(out, k0, r, preq, pres, 1, ia, 0, ra);
        expect("c1", out, 16, "863bf1bec54da7d2ea888987ef3f1e1e");
        memset(x, 0, 16); memset(y, 0, 16);
        unhex(x, "8877665544332211"); unhex(y, "00ffeeddccbbaa99");
        bt_s1(out, k0, x, y);
        expect("s1", out, 16, "62a06d79ae16425b9bf4b0e8f0e11f9a");
        static const char *vec[2][5] = {
            { "bd1a3ccda6b8995899b740eb7b60ff4a503f10d2e3b3c974385fc5a3d4f6493f",
              "fdc57ff449dd4f6bfb7c9df1c29acb592ae7d4eefbfc0a909abbf6323d8b1855",
              "e69d350e480103ccdbfdf4ac1191f4efb9a5f9e9a7832c5e2cbe97f2d203b020"
              "8bd28915d08e1c742430ed8fc24563765c15525abf9a32636deb2a65499c80dc",
              "90a1aa2fb27790559fa61586fd8ab547004c9ef184225909961daf1ff0f0a11e"
              "4a21b115f9af895f76368ee230112d476051b89a3a70567337ad9d423ef3554c",
              "98a6bf73f3348d86f166f8b4136b79999b7d390aa610103405adc857a33402ec" },
            { "637645d0f773acb7ffdd0372b97285b441b65d0c5d5484601aa39a3c6916a506",
              "ba30555019a2caa3a52908c6b503887e032b5073d42e509764cd720d67a09a52",
              "dd785c74039b7e98cb94874aadfaf8d5433e5cafeab54cf49e8079577ba4312c"
              "4f5d71437743f8ead43ebd17911021d01f87438e40e252cdbedf983818129591",
              "cc0065e1f56c0dcfec96472066c9db848175a84dc0dfc79d1b3f3df23fe465f4"
              "79b2ecd8ca55a1a8434d6bca10b0c201c2334e1624c4efee99d8bbbc48d00102",
              "69eb2132f2c605416019cd5e94e1e65f3307e3384b68e5623f886d2f3a8485ab" } };
        for (int t = 0; t < 2; t++) {
            unhex(pa, vec[t][0]); unhex(pb, vec[t][1]); unhex(qb, vec[t][3]);
            check(t ? "P-256 public key A (set 2)" : "P-256 public key A (set 1)", !bt_p256_public(qa, pa), "");
            expect("  matches", qa, 64, vec[t][2]);
            check("P-256 DHKey from A", !bt_p256_dh(dh, pa, qb), "");
            expect("  matches", dh, 32, vec[t][4]);
            check("P-256 DHKey from B", !bt_p256_dh(dh, pb, qa), "");
            expect("  matches", dh, 32, vec[t][4]);
        }
        qb[5] ^= 1;
        check("P-256 point off the curve refused", bt_p256_dh(dh, pa, qb) == -1, "");
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
