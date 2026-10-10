/*
 * mk/crypto.h - The cryptographic building blocks, written for SIEOS from
 * their public specifications and checked against their official test
 * vectors (tests/crypto):
 *   BLAKE2b   (RFC 7693) a fast hash, also the base of Argon2;
 *   ChaCha20  (RFC 8439) the stream cipher behind the kernel's random numbers;
 *   Argon2    (RFC 9106) password hashing that needs a lot of memory, so that
 *             guessing passwords with many machines in parallel is expensive.
 * All are plain 64-bit integer code: no floating point, no vector registers.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* ---- BLAKE2b: digests of 1 to 64 bytes, optionally keyed (a MAC). */
typedef struct { uint64_t h[8], t; uint8_t b[128]; size_t c, outlen; } blake2b_t;
void blake2b_init(blake2b_t *s, size_t outlen, const void *key, size_t keylen);
void blake2b_update(blake2b_t *s, const void *in, size_t n);
void blake2b_final(blake2b_t *s, void *out);
void blake2b(void *out, size_t outlen, const void *in, size_t n);

/* ---- ChaCha20: one 64-byte block of keystream for (key, counter, nonce). */
void chacha20_block(const uint32_t key[8], uint32_t counter, const uint32_t nonce[3], uint8_t out[64]);

/* ---- Argon2. mem must hold m_kib KiB (8-byte aligned); the caller owns it
 * (and should wipe and free it right after: it holds password-derived data).
 * Returns 0, or -1 for parameters out of range. */
enum { ARGON2_D = 0, ARGON2_I = 1, ARGON2_ID = 2 };
typedef struct {
    const void *pwd, *salt, *secret, *ad;       /* secret and ad may be 0 */
    uint32_t pwdlen, saltlen, secretlen, adlen;
    uint32_t t, m_kib, p, type;                 /* passes, memory in KiB, lanes, ARGON2_* */
} argon2_t;
int argon2(const argon2_t *a, void *out, uint32_t outlen, void *mem);

/* Compare two secrets in time that does not depend on where they differ;
 * erase a secret (the compiler may not skip it). */
int  ct_equal(const void *a, const void *b, size_t n);
void wipe(void *p, size_t n);

/* ---- For TLS 1.3 (user/lib/tls.c): hashes, MACs, key derivation,
 * authenticated encryption, key exchange and signature checks. */

/* SHA-2 (FIPS 180-4): SHA256 (32-byte digest), SHA384 (48), SHA512 (64). */
enum { SHA256 = 1, SHA384, SHA512 };
typedef struct {
    uint32_t h32[8]; uint64_t h64[8];
    uint8_t buf[128]; size_t fill, bs, len; uint64_t total; int alg;
} sha_t;
void   sha_init(sha_t *s, int alg);
void   sha_update(sha_t *s, const void *in, size_t n);
void   sha_final(sha_t *s, void *out);
void   sha(int alg, void *out, const void *in, size_t n);
size_t sha_len(int alg);
/* HMAC (RFC 2104) and HKDF (RFC 5869) over those hashes. */
typedef struct { sha_t inner, outer; } hmac_t;
void hmac_init(hmac_t *h, int alg, const void *key, size_t klen);
void hmac_update(hmac_t *h, const void *in, size_t n);
void hmac_final(hmac_t *h, void *out);
void hmac(int alg, void *out, const void *key, size_t klen, const void *in, size_t n);
void hkdf_extract(int alg, void *prk, const void *salt, size_t slen, const void *ikm, size_t ilen);
void hkdf_expand(int alg, void *out, size_t olen, const void *prk, size_t plen, const void *info, size_t ilen);

/* Authenticated encryption with a 12-byte nonce and a 16-byte tag:
 * ChaCha20-Poly1305 (RFC 8439) or AES-128-GCM (NIST SP 800-38D).
 * seal: out = ciphertext (n bytes) + tag; open: in = ciphertext + tag
 * (n bytes in all), out = n - 16 bytes; -1 if the tag is wrong (out wiped). */
enum { AEAD_CHACHA20_POLY1305 = 1, AEAD_AES128_GCM };
typedef struct { int alg; uint8_t key[32]; uint32_t rk[44]; uint64_t hh, hl; } aead_t;
void aead_init(aead_t *a, int alg, const uint8_t *key);
void aead_seal(const aead_t *a, const uint8_t nonce[12], const void *ad, size_t adlen,
               const void *in, size_t n, uint8_t *out);
int  aead_open(const aead_t *a, const uint8_t nonce[12], const void *ad, size_t adlen,
               const void *in, size_t n, uint8_t *out);

/* X25519 (RFC 7748): out = scalar * point (32-byte little-endian values);
 * x25519_base: scalar * the standard base point. -1 if the result is all
 * zeros (a malicious peer's point). */
int x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
int x25519_base(uint8_t out[32], const uint8_t scalar[32]);

/* Elliptic curves P-256 and P-384 (FIPS 186): ECDSA signature checks
 * (for certificates and TLS), and P-256 key exchange. Points are
 * "uncompressed": 04 || x || y. Return 0 if valid, -1 if not. */
enum { P256 = 1, P384 };
int ecdsa_verify(int curve, const uint8_t *pub, size_t publen, const uint8_t *hash, size_t hlen,
                 const uint8_t *r, size_t rlen, const uint8_t *s, size_t slen);
int p256_public(uint8_t pub[65], const uint8_t priv[32]);     /* priv: 32 random bytes */
int p256_shared(uint8_t out[32], const uint8_t priv[32], const uint8_t *peer, size_t plen);

/* RSA signature checks (RFC 8017): PKCS #1 v1.5 and PSS (MGF1 with the same
 * hash, salt as long as the hash), moduli up to 4096 bits. 0 if valid. */
int rsa_verify_pkcs1(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                     int alg, const uint8_t *hash, const uint8_t *sig, size_t slen);
int rsa_verify_pss(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                   int alg, const uint8_t *hash, const uint8_t *sig, size_t slen);
