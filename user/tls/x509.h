/*
 * x509.h - X.509 certificates: DER parsing, chain and host-name checks.
 */
#ifndef TLS_X509_H
#define TLS_X509_H

#include "crypto.h"

struct der {
    const uint8_t *p;
    size_t len;
};

struct x509 {
    struct der raw;              /* whole certificate */
    struct der tbs;              /* signed part */
    struct der issuer, subject;  /* raw Name encodings */
    int sig_hash;                /* enum hash_alg, -1 = unsupported algorithm */
    bool sig_ecdsa;              /* ecdsa-with-SHA*, else sha*WithRSAEncryption */
    struct der sig;
    long not_before, not_after;
    bool is_rsa;
    struct rsa_pub key;
    struct der spki_bits;        /* the subjectPublicKey BIT STRING contents */
    bool is_ec;                  /* an EC key on P-256 or P-384 */
    int ec_curve;                /* ECDSA_P256 / ECDSA_P384 */
    struct der ec_point;         /* 04 || X || Y */
    bool is_ca;
    struct der san;              /* SubjectAltName extension value (GeneralNames) */
    char cn[128];                /* subject common name, for messages */
};

bool x509_parse(struct x509 *c, const uint8_t *der, size_t len);
bool x509_host_matches(const struct x509 *c, const char *host);

/* Trusted roots, loaded from a PEM bundle. */
int  x509_load_roots(const char *path);               /* adds roots; returns how many, -1 on error */
int  x509_root_count(void);

/* Verify certs[0..n-1] (leaf first) up to a trusted root, for host at time now.
 * On failure, err describes the problem. */
bool x509_verify_chain(struct x509 *certs, int n, const char *host, long now, char *err, size_t errlen);

/* PEM helper: decode the next "-----BEGIN CERTIFICATE-----" block. */
long pem_next_cert(const char **text, uint8_t *out, size_t outmax);

#endif
