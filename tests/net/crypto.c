/*
 * tests/net/crypto.c - Checks the TLS crypto (lib/sha2.c, aead.c, x25519.c,
 * pubkey.c) against the official test vectors (FIPS 180-4, RFC 4231, 5869,
 * 8439, 7748, the GCM specification) and against signatures made by the
 * host's openssl (tests/net/gen.py). Usage: crypto-test VECTORS-FILE
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mk/crypto.h"
#include "../../user/wifi/wpa.h"

static int fails, checks;

static size_t unhex(uint8_t *o, const char *h)
{
    size_t n = 0;
    for (; h[0] && h[1]; h += 2) { unsigned v; sscanf(h, "%2x", &v); o[n++] = (uint8_t)v; }
    return n;
}

static void same(const char *what, const uint8_t *got, const char *hex)
{
    uint8_t want[256];
    size_t n = unhex(want, hex);
    checks++;
    if (memcmp(got, want, n)) { fails++; printf("FAIL %s\n", what); }
}

static void hashes(void)
{
    uint8_t d[64];
    sha(SHA256, d, "abc", 3); same("sha256 abc", d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha(SHA384, d, "abc", 3); same("sha384 abc", d, "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
    sha(SHA512, d, "abc", 3); same("sha512 abc", d, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha(SHA256, d, two, strlen(two)); same("sha256 448 bits", d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    sha_t s;
    sha_init(&s, SHA256);
    for (int i = 0; i < 10000; i++) sha_update(&s, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 100);
    sha_final(&s, d); same("sha256 million a", d, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    hmac(SHA256, d, "Jefe", 4, "what do ya want for nothing?", 28);
    same("hmac-sha256 rfc4231 #2", d, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    hmac(SHA384, d, "Jefe", 4, "what do ya want for nothing?", 28);
    same("hmac-sha384 rfc4231 #2", d, "af45d2e376484031617f78d2b58a6b1b9c7ef464f5a01b47e42ec3736322445e8e2240ca5e69e2c78b3239ecfab21649");
    uint8_t ikm[22], salt[13], info[10], prk[32], okm[42];
    memset(ikm, 0x0b, 22);
    for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
    for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
    hkdf_extract(SHA256, prk, salt, 13, ikm, 22);
    same("hkdf prk rfc5869 #1", prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
    hkdf_expand(SHA256, okm, 42, prk, 32, info, 10);
    same("hkdf okm rfc5869 #1", okm, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
}

static void aeads(void)
{
    uint8_t key[32], nonce[12], ad[64], pt[128], ct[160], back[128];
    /* RFC 8439 2.8.2 */
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x80 + i);
    unhex(nonce, "070000004041424344454647");
    size_t adl = unhex(ad, "50515253c0c1c2c3c4c5c6c7");
    const char *msg = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    size_t n = strlen(msg);
    aead_t a;
    aead_init(&a, AEAD_CHACHA20_POLY1305, key);
    aead_seal(&a, nonce, ad, adl, msg, n, ct);
    same("chacha20-poly1305 ciphertext", ct, "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116");
    same("chacha20-poly1305 tag", ct + n, "1ae10b594f09e26a7e902ecbd0600691");
    checks++; if (aead_open(&a, nonce, ad, adl, ct, n + 16, back) || memcmp(back, msg, n)) { fails++; puts("FAIL chacha open"); }
    ct[3] ^= 1;
    checks++; if (!aead_open(&a, nonce, ad, adl, ct, n + 16, back)) { fails++; puts("FAIL chacha forged accepted"); }
    /* GCM specification, test case 4 */
    unhex(key, "feffe9928665731c6d6a8f9467308308");
    unhex(nonce, "cafebabefacedbaddecaf888");
    adl = unhex(ad, "feedfacedeadbeeffeedfacedeadbeefabaddad2");
    n = unhex(pt, "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
    aead_init(&a, AEAD_AES128_GCM, key);
    aead_seal(&a, nonce, ad, adl, pt, n, ct);
    same("aes-128-gcm ciphertext", ct, "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091");
    same("aes-128-gcm tag", ct + n, "5bc94fbc3221a5db94fae95ae7121a47");
    checks++; if (aead_open(&a, nonce, ad, adl, ct, n + 16, back) || memcmp(back, pt, n)) { fails++; puts("FAIL gcm open"); }
    ct[n] ^= 0x80;
    checks++; if (!aead_open(&a, nonce, ad, adl, ct, n + 16, back)) { fails++; puts("FAIL gcm forged accepted"); }
    /* GCM test case 2: empty key, one zero block */
    memset(key, 0, 16); memset(nonce, 0, 12); memset(pt, 0, 16);
    aead_init(&a, AEAD_AES128_GCM, key);
    aead_seal(&a, nonce, 0, 0, pt, 16, ct);
    same("aes-128-gcm tc2", ct, "0388dace60b6a392f328c2b971b2fe78ab6e47d42cec13bdf53a67b21257bddf");
    /* AES-256-GCM: test cases 14 (zero key, one zero block) and 16 */
    memset(key, 0, 32);
    aead_init(&a, AEAD_AES256_GCM, key);
    aead_seal(&a, nonce, 0, 0, pt, 16, ct);
    same("aes-256-gcm tc14", ct, "cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919");
    unhex(key, "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
    unhex(nonce, "cafebabefacedbaddecaf888");
    adl = unhex(ad, "feedfacedeadbeeffeedfacedeadbeefabaddad2");
    n = unhex(pt, "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
    aead_init(&a, AEAD_AES256_GCM, key);
    aead_seal(&a, nonce, ad, adl, pt, n, ct);
    same("aes-256-gcm tc16 ciphertext", ct, "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662");
    same("aes-256-gcm tc16 tag", ct + n, "76fc6ece0f4e1768cddf8853bb2d551b");
    checks++; if (aead_open(&a, nonce, ad, adl, ct, n + 16, back) || memcmp(back, pt, n)) { fails++; puts("FAIL gcm-256 open"); }
}

static void dh(void)
{
    uint8_t a[32], b[32], pa[32], pb[32], s1[32], s2[32];
    unhex(a, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    unhex(b, "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    x25519_base(pa, a); same("x25519 alice public", pa, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    x25519_base(pb, b); same("x25519 bob public", pb, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    x25519(s1, a, pb); same("x25519 shared (alice)", s1, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    x25519(s2, b, pa); same("x25519 shared (bob)", s2, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    /* RFC 7748 5.2: 1 and 1000 iterations */
    uint8_t k[32] = { 9 }, u[32] = { 9 }, t[32];
    for (int i = 0; i < 1000; i++) {
        x25519(t, k, u);
        memcpy(u, k, 32); memcpy(k, t, 32);
        if (i == 0) same("x25519 iterated x1", k, "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079");
    }
    same("x25519 iterated x1000", k, "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51");
    /* P-256 and P-384 key exchange: both sides agree; a point off the curve is refused */
    for (int cv = P256, L = 32; cv <= P384; cv++, L = 48) {
        uint8_t da[48], db[48], qa[97], qb[97], z1[48], z2[48];
        for (int i = 0; i < L; i++) { da[i] = (uint8_t)rand(); db[i] = (uint8_t)rand(); }
        checks++;
        if (ec_public(cv, qa, da) || ec_public(cv, qb, db) || ec_shared(cv, z1, da, qb, 2 * L + 1) ||
            ec_shared(cv, z2, db, qa, 2 * L + 1) || memcmp(z1, z2, L)) { fails++; printf("FAIL p%d key exchange\n", L * 8); }
        qb[40] ^= 1;
        checks++; if (!ec_shared(cv, z1, da, qb, 2 * L + 1)) { fails++; printf("FAIL p%d off-curve point accepted\n", L * 8); }
    }
}

static int halg(const char *h) { return !strcmp(h, "sha256") ? SHA256 : !strcmp(h, "sha384") ? SHA384 : SHA512; }

static void signatures(const char *file)
{
    FILE *f = fopen(file, "r");
    if (!f) { printf("no %s\n", file); fails++; return; }
    static char line[16384];
    int ec = 0, rsa = 0;
    double t_ec = 0, t_rsa = 0;
    while (fgets(line, sizeof line, f)) {
        char kind[16], sub[16], h[16];
        static char a[4096], b[4096], c[4096], d[4096];
        static uint8_t A[1024], B[1024], C[1024], D[1024], hash[64];
        if (sscanf(line, "%15s %15s %15s %4095s %4095s %4095s %4095s", kind, sub, h, a, b, c, d) != 7) continue;
        size_t la = unhex(A, a), lb = unhex(B, b), lc = unhex(C, c), ld = unhex(D, d);
        int alg = halg(h), ok, bad;
        clock_t t0 = clock();
        if (!strcmp(kind, "ecdsa")) {                 /* pub, msg, r, s */
            int cv = !strcmp(sub, "P256") ? P256 : P384;
            sha(alg, hash, B, lb);
            ok = !ecdsa_verify(cv, A, la, hash, sha_len(alg), C, lc, D, ld);
            hash[0] ^= 1;
            bad = !ecdsa_verify(cv, A, la, hash, sha_len(alg), C, lc, D, ld);
            ec++; t_ec += clock() - t0;
        } else {                                      /* n, e, msg, sig */
            sha(alg, hash, C, lc);
            int pss = !strcmp(sub, "pss");
            ok = !(pss ? rsa_verify_pss : rsa_verify_pkcs1)(A, la, B, lb, alg, hash, D, ld);
            D[ld / 2] ^= 4;
            bad = !(pss ? rsa_verify_pss : rsa_verify_pkcs1)(A, la, B, lb, alg, hash, D, ld);
            rsa++; t_rsa += clock() - t0;
        }
        checks += 2;
        if (!ok) { fails++; printf("FAIL %s %s %s: valid signature refused\n", kind, sub, h); }
        if (bad) { fails++; printf("FAIL %s %s %s: damaged signature accepted\n", kind, sub, h); }
    }
    fclose(f);
    printf("  %d ECDSA (%.2f ms each, 2 checks), %d RSA (%.2f ms each, 2 checks)\n", ec,
           ec ? 1000 * t_ec / CLOCKS_PER_SEC / ec : 0.0, rsa, rsa ? 1000 * t_rsa / CLOCKS_PER_SEC / rsa : 0.0);
}

/* WPA2 (user/wifi/wpa.c and its crypto): RFC 2202, 6070 and 3394, the
 * IEEE 802.11 passphrase vector (J.4), then a 4-way and a group handshake
 * against frames made by an access point written apart (tests/net/wpagen.py:
 * hashlib, openssl's AES), each answer compared byte for byte; damaged or
 * replayed frames refused. */
static void wpa(void)
{
    uint8_t d[64], f[256], o[256];
    hmac_sha1(d, "Jefe", 4, "what do ya want for nothing?", 28);
    same("hmac-sha1 rfc2202 #2", d, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    pbkdf2_sha1(d, 20, "password", 8, "salt", 4, 4096);
    same("pbkdf2-sha1 rfc6070 #3", d, "4b007901b765489abead49d926f721d065a429c1");
    pbkdf2_sha1(d, 25, "passwordPASSWORDpassword", 24, "saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096);
    same("pbkdf2-sha1 rfc6070 #5", d, "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038");
    wpa_pmk(d, "password", "IEEE");
    same("wpa psk ieee 802.11 j.4", d, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");
    uint8_t kek[16], key[16];
    for (int i = 0; i < 16; i++) kek[i] = (uint8_t)i;
    unhex(f, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5");
    checks++; if (aes_unwrap(kek, 16, f, 24, key)) { fails++; printf("FAIL aes unwrap rfc3394 4.1\n"); }
    same("aes unwrap rfc3394 4.1", key, "00112233445566778899aabbccddeeff");
    f[3] ^= 1; checks++; if (!aes_unwrap(kek, 16, f, 24, key)) { fails++; printf("FAIL aes unwrap damaged\n"); }

    static const uint8_t ap_rsn[22] = { 0x30, 20, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0, 0 };
    wpa_t w = { .aa = { 2, 0, 0, 0, 0, 1 }, .spa = { 2, 0x11, 0x22, 0x33, 0x44, 0x55 }, .ap_rsn = ap_rsn };
    for (int i = 0; i < 32; i++) w.snonce[i] = (uint8_t)(32 + i);
    wpa_pmk(w.pmk, "correct horse", "SIEOS-test");
    same("wpa pmk", w.pmk, "651423552fc2ffdb35d12450833dbe042867b32232498a88fbf5d23523505ef4");
    checks++; if (!wpa_rsn_ok(ap_rsn)) { fails++; printf("FAIL rsn ok\n"); }
    int done, n = (int)unhex(f, "0203005f02008a00000000000000000001000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000");
    int r = wpa_rx(&w, f, n, o, &done);
    same("wpa ptk", w.ptk, "40cf12fd240577c909d6be705f122ffec9b19071fe6ae6d0ce5a4b5dd9a87aee7ea2e44ca84b80677951911f4bf92294");
    checks++; if (r != 121 || done) { fails++; printf("FAIL wpa msg 2 length\n"); }
    same("wpa msg 2", o, "0203007502010a00000000000000000001202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e"
        "3f00000000000000000000000000000000000000000000000000000000000000007892251071f2616a2074064cd7c38a"
        "ca001630140100000fac040100000fac040100000fac020000");
    n = (int)unhex(f, "020300970213ca00000000000000000002000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e"
        "1f0000000000000000000000000000000000000000000000000000000000000000db466af84234890dd30d20f6bb0995"
        "5f00385750e318da8ac6bb9284b2eff7c53acc6875879e51dd8997750bd0362567f3f89339d11293d8d9619d17d6aa74"
        "e9b932e2168d06b78c25a0");
    f[100] ^= 1; checks++; if (wpa_rx(&w, f, n, o, &done) != -1) { fails++; printf("FAIL wpa damaged msg 3\n"); }
    f[100] ^= 1; r = wpa_rx(&w, f, n, o, &done);
    checks++; if (r != 99 || done != WPA_PTK || w.gtkid != 1 || w.gtklen != 16) { fails++; printf("FAIL wpa msg 3\n"); }
    same("wpa msg 4", o, "0203005f02030a00000000000000000002000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001f83fe741492b9f955eaf07def5a3fc10000");
    same("wpa gtk", w.gtk, "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf");
    checks++; if (wpa_rx(&w, f, n, o, &done) != -1) { fails++; printf("FAIL wpa replayed msg 3\n"); }
    n = (int)unhex(f, "020300870213820000000000000000000300000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000009ff4a09e0e40d6adbbecd2e3017d47"
        "940028bcf253a58d8cf56880641032de469e233f1a9b62ae039e498f5eceaa6ad7ecfb6eb7b3bd4c0aae95");
    r = wpa_rx(&w, f, n, o, &done);
    checks++; if (r != 99 || done != WPA_GTK || w.gtkid != 2) { fails++; printf("FAIL wpa group msg 1\n"); }
    same("wpa group msg 2", o, "0203005f0203020000000000000000000300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000c8a1954eb7dba889d43d119560395ebd0000");
    same("wpa new gtk", w.gtk, "afaeadacabaaa9a8a7a6a5a4a3a2a1a0");
}

int main(int argc, char **argv)
{
    hashes();
    wpa();
    aeads();
    dh();
    if (argc > 1) signatures(argv[1]);
    /* speed of the record ciphers */
    static uint8_t buf[1 << 20], out[(1 << 20) + 16];
    uint8_t key[32] = { 1 }, nonce[12] = { 2 };
    for (int alg = AEAD_CHACHA20_POLY1305; alg <= AEAD_AES256_GCM; alg++) {
        aead_t a;
        aead_init(&a, alg, key);
        clock_t t0 = clock();
        for (int i = 0; i < 8; i++) aead_seal(&a, nonce, 0, 0, buf, sizeof buf, out);
        double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("  %s: %.0f MB/s\n", alg == AEAD_AES256_GCM ? "aes-256-gcm" : alg == AEAD_AES128_GCM ? "aes-128-gcm" : "chacha20-poly1305", 8 / s);
    }
    printf("net crypto: %d of %d checks passed\n", checks - fails, checks);
    return fails != 0;
}
