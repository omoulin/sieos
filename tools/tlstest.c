/*
 * tlstest.c - Host-side test driver for user/tls (built with -DTLS_HOSTED).
 *
 *   tlstest hash ALG HEX            -> digest
 *   tlstest hmac ALG KEYHEX HEX     -> mac
 *   tlstest hkdf ALG SALT IKM INFO LEN
 *   tlstest gcm KEY IV AAD PT       -> ciphertext||tag   (and checks gcm_open)
 *   tlstest x25519 SCALAR POINT
 *   tlstest p256pub SCALAR / tlstest p256ecdh SCALAR PEERPOINT
 *   tlstest pkcs1 ALG N E DIGEST SIG / tlstest pss ALG N E DIGEST SIG -> ok|bad
 *   tlstest get HOST PATH [ROOTS]   -> HTTPS GET over TLS 1.3, prints the response
 *                                      (TLSPORT=port, TLSREPEAT=n connections)
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include "../user/tls/crypto.h"
#include "../user/tls/tls.h"
#include "../user/tls/x509.h"

static size_t unhex(const char *s, uint8_t *out)
{
    size_t n = 0;
    for (; s[0] && s[1]; s += 2) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (uint8_t)v;
    }
    return n;
}

static void hex(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        printf("%02x", p[i]);
    printf("\n");
}

static enum hash_alg alg_of(const char *s)
{
    return !strcmp(s, "sha256") ? HASH_SHA256 : !strcmp(s, "sha384") ? HASH_SHA384 : HASH_SHA512;
}

static uint8_t a[70000], b[70000], c[70000], d[70000], e[70000];

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    const char *op = argv[1];
    if (!strcmp(op, "hash")) {
        size_t n = unhex(argv[3], a);
        hash_once(alg_of(argv[2]), a, n, b);
        hex(b, hash_size(alg_of(argv[2])));
    } else if (!strcmp(op, "hmac")) {
        size_t kn = unhex(argv[3], a), n = unhex(argv[4], b);
        hmac(alg_of(argv[2]), a, kn, b, n, c);
        hex(c, hash_size(alg_of(argv[2])));
    } else if (!strcmp(op, "hkdf")) {
        enum hash_alg h = alg_of(argv[2]);
        size_t sn = unhex(argv[3], a), in = unhex(argv[4], b), fn = unhex(argv[5], c);
        size_t len = atoi(argv[6]);
        uint8_t prk[64];
        hkdf_extract(h, sn ? a : NULL, sn, b, in, prk);
        hkdf_expand(h, prk, c, fn, d, len);
        hex(d, len);
    } else if (!strcmp(op, "gcm")) {
        size_t kn = unhex(argv[2], a), ivn = unhex(argv[3], b), an = unhex(argv[4], c), pn = unhex(argv[5], d);
        (void)ivn;
        struct gcm g;
        gcm_init(&g, a, kn);
        uint8_t tag[16];
        gcm_seal(&g, b, c, an, d, pn, e, tag);
        static uint8_t back[70000];
        if (!gcm_open(&g, b, c, an, e, pn, tag, back) || memcmp(back, d, pn))
            printf("OPEN-FAILED ");
        tag[0] ^= 1;
        if (gcm_open(&g, b, c, an, e, pn, tag, back))
            printf("FORGERY-ACCEPTED ");
        tag[0] ^= 1;
        memcpy(e + pn, tag, 16);
        hex(e, pn + 16);
    } else if (!strcmp(op, "x25519")) {
        unhex(argv[2], a);
        unhex(argv[3], b);
        x25519(c, a, b);
        hex(c, 32);
    } else if (!strcmp(op, "p256pub")) {
        unhex(argv[2], a);
        if (!p256_public(b, a))
            printf("FAIL\n");
        else
            hex(b, 65);
    } else if (!strcmp(op, "p256ecdh")) {
        unhex(argv[2], a);
        unhex(argv[3], b);
        if (!p256_shared(c, a, b))
            printf("FAIL\n");
        else
            hex(c, 32);
    } else if (!strcmp(op, "pkcs1") || !strcmp(op, "pss")) {
        struct rsa_pub k;
        k.nlen = unhex(argv[3], a);
        k.n = a;
        k.elen = unhex(argv[4], b);
        k.e = b;
        unhex(argv[5], c);
        size_t sl = unhex(argv[6], d);
        bool ok = op[1] == 'k' ? rsa_verify_pkcs1(&k, alg_of(argv[2]), c, d, sl)
                               : rsa_verify_pss(&k, alg_of(argv[2]), c, d, sl);
        printf("%s\n", ok ? "ok" : "bad");
    } else if (!strcmp(op, "get")) {
        const char *host = argv[2], *path = argv[3];
        if (argc > 4 && x509_load_roots(argv[4]) <= 0) {
            printf("cannot load roots\n");
            return 1;
        }
        struct hostent *he = gethostbyname(host);
        if (!he)
            return 1;
        int repeat = getenv("TLSREPEAT") ? atoi(getenv("TLSREPEAT")) : 1;
        for (int round = 0; round < repeat; round++) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(getenv("TLSPORT") ? atoi(getenv("TLSPORT")) : 443) };
            memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
            if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
                return 1;
            char err[200];
            struct tls *t = tls_connect(fd, host, err, sizeof(err));
            if (!t) {
                printf("FAIL %s\n", err);
                return 1;
            }
            printf("handshake ok (%s%s)\n", tls_cipher_name(t), tls_resumed(t) ? ", resumed" : "");
            char req[512];
            int n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
            tls_write(t, req, n);
            long r;
            char buf[4096];
            while ((r = tls_read(t, buf, sizeof(buf))) > 0)
                fwrite(buf, 1, r, stdout);
            printf("\n[read returned %ld]\n", r);
            tls_close(t);
            close(fd);
        }
    }
    return 0;
}
