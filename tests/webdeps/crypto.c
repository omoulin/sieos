/* pkg-config: libgcrypt libtasn1 */
/*
 * wtest-crypto (make webdeps-test): libgcrypt as WebKit's WebCrypto uses it
 * (SHA-256, HMAC, AES-GCM, an ECDSA P-256 key made from SIEOS's random
 * numbers, signed and verified) and libtasn1 (DER encoded and decoded).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <gcrypt.h>
#include <libtasn1.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char *what, int ok) { printf("wtest crypto: %s: %s\n", what, ok ? "ok" : "FAIL"); fails += !ok; }
static int hex(const unsigned char *b, const char *want)
{
    char s[129];
    size_t n = strlen(want) / 2;
    for (size_t i = 0; i < n; i++) snprintf(s + 2 * i, 3, "%02x", b[i]);
    return !strcmp(s, want);
}

/* AES-128-GCM: NIST's test case 3 */
static const unsigned char key[16] = {0xfe,0xff,0xe9,0x92,0x86,0x65,0x73,0x1c,0x6d,0x6a,0x8f,0x94,0x67,0x30,0x83,0x08};
static const unsigned char iv[12] = {0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad,0xde,0xca,0xf8,0x88};
static const unsigned char pt[64] = {
    0xd9,0x31,0x32,0x25,0xf8,0x84,0x06,0xe5,0xa5,0x59,0x09,0xc5,0xaf,0xf5,0x26,0x9a,
    0x86,0xa7,0xa9,0x53,0x15,0x34,0xf7,0xda,0x2e,0x4c,0x30,0x3d,0x8a,0x31,0x8a,0x72,
    0x1c,0x3c,0x0c,0x95,0x95,0x68,0x09,0x53,0x2f,0xcf,0x0e,0x24,0x49,0xa6,0xb5,0x25,
    0xb1,0x6a,0xed,0xf5,0xaa,0x0d,0xe6,0x57,0xba,0x63,0x7b,0x39,0x1a,0xaf,0xd2,0x55};

int main(void)
{
    check("version", gcry_check_version(GCRYPT_VERSION) != 0);
    gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);

    unsigned char d[32];
    gcry_md_hash_buffer(GCRY_MD_SHA256, d, "abc", 3);
    check("SHA-256", hex(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    gcry_md_hd_t h;
    int ok = !gcry_md_open(&h, GCRY_MD_SHA256, GCRY_MD_FLAG_HMAC) && !gcry_md_setkey(h, "key", 3);
    if (ok) {
        gcry_md_write(h, "The quick brown fox jumps over the lazy dog", 43);
        ok = hex(gcry_md_read(h, 0), "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
        gcry_md_close(h);
    }
    check("HMAC-SHA-256", ok);

    unsigned char ct[64], tag[16];
    gcry_cipher_hd_t c;
    ok = !gcry_cipher_open(&c, GCRY_CIPHER_AES128, GCRY_CIPHER_MODE_GCM, 0) && !gcry_cipher_setkey(c, key, 16) &&
         !gcry_cipher_setiv(c, iv, 12) && !gcry_cipher_encrypt(c, ct, 64, pt, 64) && !gcry_cipher_gettag(c, tag, 16);
    check("AES-GCM", ok && hex(ct, "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e") &&
                     hex(tag, "4d5c2af327cd64a62cf35abd2ba6fab4"));
    gcry_cipher_close(c);

    gcry_sexp_t parms, kp, pub, sec, data, sig;
    ok = !gcry_sexp_build(&parms, 0, "(genkey (ecc (curve \"NIST P-256\")))") && !gcry_pk_genkey(&kp, parms);
    check("ECDSA P-256 key", ok);
    if (ok) {
        pub = gcry_sexp_find_token(kp, "public-key", 0);
        sec = gcry_sexp_find_token(kp, "private-key", 0);
        gcry_sexp_build(&data, 0, "(data (flags raw) (value %b))", 32, d);
        ok = !gcry_pk_sign(&sig, data, sec);
        check("ECDSA sign, verify", ok && !gcry_pk_verify(sig, data, pub));
        d[0] ^= 1;
        gcry_sexp_release(data);
        gcry_sexp_build(&data, 0, "(data (flags raw) (value %b))", 32, d);
        check("ECDSA wrong data refused", ok && gcry_pk_verify(sig, data, pub) != 0);
    }

    unsigned char der[64];  /* the tag and length libtasn1 makes, then the value */
    unsigned int n = sizeof der, vn;
    const unsigned char *v;
    ok = asn1_encode_simple_der(ASN1_ETYPE_OCTET_STRING, (const unsigned char *)"SIEOS", 5, der, &n) == ASN1_SUCCESS &&
         n == 2 && der[0] == 4 && der[1] == 5 && (memcpy(der + 2, "SIEOS", 5), n += 5) &&
         asn1_decode_simple_der(ASN1_ETYPE_OCTET_STRING, der, n, &v, &vn) == ASN1_SUCCESS && vn == 5 && !memcmp(v, "SIEOS", 5);
    check("libtasn1 DER", ok);

    printf("wtest crypto: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
