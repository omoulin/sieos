/*
 * tls.h - A small TLS 1.3 client (RFC 8446).
 *
 * X25519 key exchange, TLS_AES_128_GCM_SHA256 / TLS_AES_256_GCM_SHA384,
 * RSA server certificates (RSA-PSS handshake signatures) verified against
 * the roots in /etc/ssl/certs.pem, with host-name checking.
 */
#ifndef TLS_TLS_H
#define TLS_TLS_H

#include "port.h"

#define TLS_ROOTS_PATH "/etc/ssl/certs.pem"
#define TLS_LOCAL_ROOTS_PATH "/etc/ssl/local.pem"   /* optional site-local additions */

struct tls;

/* Handshake over a connected TCP socket.  NULL on failure (err explains). */
struct tls *tls_connect(int fd, const char *host, char *err, size_t errlen);
long tls_read(struct tls *t, void *buf, size_t n);          /* 0 = closed, -1 = error */
long tls_write(struct tls *t, const void *buf, size_t n);   /* -1 = error */
void tls_close(struct tls *t);                               /* sends close_notify, frees; fd stays open */
const char *tls_error(struct tls *t);
const char *tls_cipher_name(struct tls *t);

#endif
