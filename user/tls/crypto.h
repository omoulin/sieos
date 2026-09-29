/*
 * crypto.h - Primitives used by the TLS 1.3 client.
 *
 * SHA-256/384/512, HMAC, HKDF (RFC 5869, TLS 1.3 labels), AES-GCM,
 * X25519 (RFC 7748), P-256 ECDH, and signature verification: RSA (PKCS #1 v1.5 and PSS)
 * and ECDSA on P-256 and P-384.
 */
#ifndef TLS_CRYPTO_H
#define TLS_CRYPTO_H

#include "port.h"

/* ---- hashes ---- */
#define HASH_MAX 64
#define HASH_BLOCK_MAX 128

enum hash_alg { HASH_SHA256, HASH_SHA384, HASH_SHA512 };

struct hash_ctx {
    enum hash_alg alg;
    union {
        uint32_t h32[8];
        uint64_t h64[8];
    } s;
    uint8_t buf[128];
    size_t buflen;
    uint64_t total;
};

size_t hash_size(enum hash_alg alg);
size_t hash_block(enum hash_alg alg);
void hash_init(struct hash_ctx *c, enum hash_alg alg);
void hash_update(struct hash_ctx *c, const void *data, size_t len);
void hash_final(struct hash_ctx *c, uint8_t *out);
void hash_once(enum hash_alg alg, const void *data, size_t len, uint8_t *out);

void hmac(enum hash_alg alg, const uint8_t *key, size_t keylen, const void *msg, size_t len, uint8_t *out);
void hkdf_extract(enum hash_alg alg, const uint8_t *salt, size_t saltlen, const uint8_t *ikm, size_t ikmlen,
                  uint8_t *prk);
void hkdf_expand(enum hash_alg alg, const uint8_t *prk, const uint8_t *info, size_t infolen, uint8_t *out,
                 size_t outlen);
/* HKDF-Expand-Label(secret, "tls13 " + label, context, length) */
void hkdf_expand_label(enum hash_alg alg, const uint8_t *secret, const char *label, const uint8_t *ctx,
                       size_t ctxlen, uint8_t *out, size_t outlen);

/* ---- AES-GCM ---- */
struct aes_key {
    uint32_t rk[60];
    int rounds;
};
void aes_setkey(struct aes_key *k, const uint8_t *key, size_t keylen);   /* 16 or 32 bytes */
void aes_encrypt_block(const struct aes_key *k, const uint8_t in[16], uint8_t out[16]);

struct gcm {
    struct aes_key aes;
    uint8_t h[16];
};
void gcm_init(struct gcm *g, const uint8_t *key, size_t keylen);
void gcm_seal(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t aadlen, const uint8_t *in,
              size_t len, uint8_t *out, uint8_t tag[16]);
bool gcm_open(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t aadlen, const uint8_t *in,
              size_t len, const uint8_t tag[16], uint8_t *out);

/* ---- X25519 ---- */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

/* ---- P-256 ECDH ---- */
bool p256_valid_scalar(const uint8_t k[32]);
bool p256_public(uint8_t out[65], const uint8_t k[32]);                       /* 04 || X || Y */
bool p256_shared(uint8_t out[32], const uint8_t k[32], const uint8_t peer[65]); /* X coordinate */

/* ---- ECDSA verification (P-256, P-384) ---- */
enum { ECDSA_P256, ECDSA_P384 };
/* pub: 04 || X || Y; sig: DER SEQUENCE { r, s }; digest: the message's hash */
bool ecdsa_verify(int curve, const uint8_t *pub, size_t publen, const uint8_t *digest, size_t dlen,
                  const uint8_t *sig, size_t siglen);

/* ---- RSA ---- */
struct rsa_pub {
    const uint8_t *n;     /* big-endian modulus, no leading zeros */
    size_t nlen;
    const uint8_t *e;
    size_t elen;
};
bool rsa_verify_pkcs1(const struct rsa_pub *k, enum hash_alg alg, const uint8_t *digest, const uint8_t *sig,
                      size_t siglen);
bool rsa_verify_pss(const struct rsa_pub *k, enum hash_alg alg, const uint8_t *digest, const uint8_t *sig,
                    size_t siglen);

/* constant-time comparison */
bool ct_equal(const void *a, const void *b, size_t n);

#endif
