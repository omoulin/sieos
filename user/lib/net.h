/*
 * net.h - The network for programs: sockets (through the network server,
 * port "net"), an HTTP/1.1 client, and TLS 1.3 for https (docs/net.md).
 * Errors are -E... (mk/abi.h, mk/proto.h).
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "mk.h"

/* ---- net.c: sockets. Timeouts in milliseconds (0: the default). */
long net_resolve(const char *host, uint32_t *ip, unsigned timeout_ms);
long net_connect(uint32_t ip, uint16_t port, unsigned timeout_ms);       /* -> socket */
long net_listen(uint16_t port, int backlog);                             /* -> socket */
long net_accept(long s, uint32_t *ip, uint16_t *port, unsigned timeout_ms);
long net_send(long s, const void *buf, size_t n);                        /* all of it, or -error */
long net_recv(long s, void *buf, size_t n, unsigned timeout_ms);         /* -> bytes, 0 = closed */
long net_udp(uint16_t port);                                             /* -> socket */
long net_sendto(long s, uint32_t ip, uint16_t port, const void *buf, size_t n);
long net_recvfrom(long s, void *buf, size_t n, uint32_t *ip, uint16_t *port, unsigned timeout_ms);
long net_close(long s);
long net_ping(uint32_t ip, unsigned timeout_ms);                         /* -> round trip in µs */
long net_info(net_info_t *in);
const char *net_ntoa(uint32_t ip, char buf[16]);                         /* "a.b.c.d" */
const char *net_strerror(long e);                                        /* a few words */

/* ---- tls.c: TLS 1.3 client (certificates checked against /etc/ssl/roots).
 * flags: TLS_INSECURE skips the certificate check (tests only). */
typedef struct tls tls_t;
enum { TLS_INSECURE = 1 };
tls_t *tls_connect(long sock, const char *host, int flags, long *err);
long tls_write(tls_t *t, const void *buf, size_t n);
long tls_read(tls_t *t, void *buf, size_t n, unsigned timeout_ms);       /* 0: closed */
void tls_close(tls_t *t);                                                /* also closes the socket */
const char *tls_cipher(tls_t *t);

/* ---- http.c: HTTP/1.1 client, http:// and https://. A request returns a
 * response whose body is read in pieces (chunked or not). */
typedef struct {
    int status;                        /* 200, 404, ... */
    char headers[4096];                /* the raw response headers */
    long sock;
    tls_t *tls;
    int chunked, done;
    int64_t left;                      /* body bytes left (-1: until closed) */
    uint8_t buf[16384];                /* read-ahead */
    size_t pos, len;
    unsigned timeout_ms;
} http_t;
/* method "GET"/"POST"; extra: header lines ("Name: value\r\n"...) or 0;
 * follows up to 5 redirects for GET. -> 0 or -error (h->status set). */
long http_request(http_t *h, const char *method, const char *url, const char *extra,
                  const void *body, size_t blen, int flags, unsigned timeout_ms);
long http_read(http_t *h, void *buf, size_t n);                          /* body bytes, 0 = end */
long http_line(http_t *h, char *buf, size_t n);  /* one body line (SSE): length, -1 at the end */
const char *http_header(http_t *h, const char *name, char *val, size_t n);
void http_close(http_t *h);

/* ---- x509.c: certificates (used by tls.c). */
typedef struct {
    const uint8_t *tbs; size_t tbslen;                   /* the signed part */
    const uint8_t *issuer, *subject; size_t ilen, slen;  /* raw DER names */
    const uint8_t *sig; size_t siglen; int sigalg;       /* X509_* */
    int keytype;                                         /* X509_RSA, X509_P256, X509_P384 */
    const uint8_t *key, *mod, *exp; size_t keylen, modlen, explen;
    int64_t not_before, not_after;                       /* seconds since 1970 */
    int ca, pathlen;
    const uint8_t *san; size_t sanlen;                   /* subjectAltName's GeneralNames */
    const uint8_t *cn; size_t cnlen;
} x509_t;
enum { X509_RSA = 1, X509_P256, X509_P384 };
enum { X509_RSA_SHA256 = 1, X509_RSA_SHA384, X509_RSA_SHA512, X509_ECDSA_SHA256, X509_ECDSA_SHA384,
       X509_ECDSA_SHA512 };
int x509_parse(x509_t *c, const uint8_t *der, size_t len);
int x509_check_sig(const x509_t *c, const x509_t *issuer);              /* 0: signed by issuer */
int x509_host(const x509_t *c, const char *host);                       /* 0: valid for host */
/* Verify a chain (certs[0] the server's) against the roots file, at time
 * now: 0, or -1 with a reason in why. */
int x509_verify_chain(const uint8_t **certs, const size_t *lens, int n, const char *host,
                      int64_t now, const char *roots_path, const char **why);
int ecdsa_der_verify(int curve, const uint8_t *pub, size_t publen, int alg, const uint8_t *hash,
                     const uint8_t *der, size_t dlen);
