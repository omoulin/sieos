/*
 * tls.c - TLS 1.3 client (RFC 8446).
 *
 * Handshake: ClientHello (x25519 and P-256 key shares, AES-GCM suites, ECDSA
 * and RSA signature algorithms, SNI, ALPN http/1.1) -> ServerHello ->
 * EncryptedExtensions -> Certificate -> CertificateVerify -> Finished,
 * then the client's Finished.  No 0-RTT, no client certificates.
 *
 * Resumption: NewSessionTicket messages received after the handshake are
 * kept in a small in-memory cache keyed by host name.  The next connection
 * to that host offers the newest one (a ticket is used once) as a PSK with
 * psk_dhe_ke, so the key exchange still happens; when the server accepts
 * it, Certificate and CertificateVerify are skipped (the ticket came from a
 * connection whose certificate chain was verified for the same host).
 * KeyUpdate is honoured.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "tls.h"
#include "crypto.h"
#include "x509.h"

#define REC_MAX (16384 + 256)
#define MAX_CERTS 8
#define NTICKETS 8
#define TICKET_MAX 2048
#define TICKET_LIFE_MAX (7 * 24 * 3600)     /* seconds (RFC 8446 4.6.1) */

enum { CT_CCS = 20, CT_ALERT = 21, CT_HANDSHAKE = 22, CT_APPDATA = 23 };
enum {
    HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_NEW_SESSION_TICKET = 4, HS_ENCRYPTED_EXTENSIONS = 8,
    HS_CERTIFICATE = 11, HS_CERTIFICATE_REQUEST = 13, HS_CERTIFICATE_VERIFY = 15, HS_FINISHED = 20,
    HS_KEY_UPDATE = 24,
};

struct dir {                         /* one direction of the record layer */
    bool on;
    struct gcm gcm;
    uint8_t iv[12];
    uint64_t seq;
    uint8_t secret[HASH_MAX];
};

struct tls {
    int fd;
    char err[160];
    enum hash_alg hash;
    size_t hs, keylen;
    uint16_t suite;
    struct dir rd, wr;
    uint8_t rec[5 + REC_MAX + 16];   /* receive buffer (application data is returned from it) */
    uint8_t wrec[5 + 16384 + 1 + 16]; /* send buffer */
    uint8_t *app;                    /* decrypted application data not yet returned */
    size_t app_len;
    bool eof;
    uint8_t *hsbuf;                  /* handshake reassembly */
    size_t hslen, hscap;
    uint8_t *tr;                     /* transcript of handshake messages */
    size_t trlen, trcap;
    char host[256];                  /* for the session tickets */
    uint8_t res_master[HASH_MAX];    /* resumption_master_secret */
    bool resumed;
};

/* ---------------- session tickets ---------------- */

static struct ticket {
    bool used;
    char host[256];
    enum hash_alg hash;
    uint16_t suite;
    uint8_t psk[HASH_MAX];
    uint32_t age_add;
    uint64_t received, expires;      /* tls_ms() */
    size_t len;
    uint8_t data[TICKET_MAX];
} tickets[NTICKETS];

/* The newest unexpired ticket for host, removed from the cache; false if none. */
static bool take_ticket(const char *host, struct ticket *out)
{
    uint64_t now = tls_ms();
    struct ticket *best = NULL;
    for (int i = 0; i < NTICKETS; i++) {
        struct ticket *k = &tickets[i];
        if (k->used && now >= k->expires) {
            memset(k, 0, sizeof(*k));
            continue;
        }
        if (k->used && !strcmp(k->host, host) && (!best || k->received > best->received))
            best = k;
    }
    if (!best)
        return false;
    *out = *best;
    memset(best, 0, sizeof(*best));
    return true;
}

static void save_ticket(const struct ticket *k)
{
    struct ticket *slot = &tickets[0];
    for (int i = 0; i < NTICKETS; i++) {
        if (!tickets[i].used) {
            slot = &tickets[i];
            break;
        }
        if (tickets[i].received < slot->received)
            slot = &tickets[i];                        /* all in use: the oldest makes way */
    }
    *slot = *k;
    slot->used = true;
}

void tls_forget_sessions(void)
{
    memset(tickets, 0, sizeof(tickets));
}

static bool fail(struct tls *t, const char *msg)
{
    if (!t->err[0])
        snprintf(t->err, sizeof(t->err), "%s", msg);
    return false;
}

static bool append(uint8_t **buf, size_t *len, size_t *cap, const void *data, size_t n)
{
    if (*len + n > *cap) {
        size_t nc = (*cap ? *cap * 2 : 4096);
        while (nc < *len + n)
            nc *= 2;
        uint8_t *b = realloc(*buf, nc);
        if (!b)
            return false;
        *buf = b;
        *cap = nc;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    return true;
}

static bool read_full(int fd, uint8_t *buf, size_t n)
{
    while (n) {
        long r = read(fd, buf, n);
        if (r <= 0)
            return false;
        buf += r;
        n -= r;
    }
    return true;
}

static bool write_full(int fd, const uint8_t *buf, size_t n)
{
    while (n) {
        long r = write(fd, buf, n);
        if (r <= 0)
            return false;
        buf += r;
        n -= r;
    }
    return true;
}

static void nonce(const struct dir *d, uint8_t out[12])
{
    memcpy(out, d->iv, 12);
    for (int i = 0; i < 8; i++)
        out[11 - i] ^= (uint8_t)(d->seq >> (8 * i));
}

static void set_keys(struct tls *t, struct dir *d, const uint8_t *secret)
{
    uint8_t key[32];
    memcpy(d->secret, secret, t->hs);
    hkdf_expand_label(t->hash, secret, "key", NULL, 0, key, t->keylen);
    hkdf_expand_label(t->hash, secret, "iv", NULL, 0, d->iv, 12);
    gcm_init(&d->gcm, key, t->keylen);
    d->seq = 0;
    d->on = true;
    memset(key, 0, sizeof(key));
}

/* ---------------- record layer ---------------- */

static bool send_record(struct tls *t, int type, const uint8_t *data, size_t len)
{
    while (len || type != CT_APPDATA) {
        size_t n = len > 16384 ? 16384 : len;
        uint8_t *r = t->wrec;
        if (!t->wr.on) {
            r[0] = (uint8_t)type;
            r[1] = 3;
            r[2] = type == CT_HANDSHAKE ? 1 : 3;
            r[3] = (uint8_t)(n >> 8);
            r[4] = (uint8_t)n;
            memcpy(r + 5, data, n);
            if (!write_full(t->fd, r, 5 + n))
                return fail(t, "connection lost while sending");
        } else {
            static uint8_t plain[16384 + 1];
            memcpy(plain, data, n);
            plain[n] = (uint8_t)type;
            size_t clen = n + 1 + 16;
            r[0] = CT_APPDATA;
            r[1] = 3;
            r[2] = 3;
            r[3] = (uint8_t)(clen >> 8);
            r[4] = (uint8_t)clen;
            uint8_t iv[12];
            nonce(&t->wr, iv);
            gcm_seal(&t->wr.gcm, iv, r, 5, plain, n + 1, r + 5, r + 5 + n + 1);
            t->wr.seq++;
            if (!write_full(t->fd, r, 5 + clen))
                return fail(t, "connection lost while sending");
        }
        data += n;
        len -= n;
        if (type != CT_APPDATA || !len)
            break;
    }
    return true;
}

/* Read one record; decrypts it when the read direction is protected. */
static bool recv_record(struct tls *t, int *type, uint8_t **body, size_t *len)
{
    for (;;) {
        uint8_t *r = t->rec;
        if (!read_full(t->fd, r, 5))
            return fail(t, "connection closed by the server");
        size_t n = (size_t)r[3] << 8 | r[4];
        if (n > REC_MAX)
            return fail(t, "record too large");
        if (!read_full(t->fd, r + 5, n))
            return fail(t, "connection closed by the server");
        int ct = r[0];
        if (ct == CT_CCS)
            continue;                                  /* compatibility-mode ChangeCipherSpec */
        if (!t->rd.on || ct != CT_APPDATA) {
            if (t->rd.on && ct != CT_ALERT)
                return fail(t, "unexpected plaintext record");
            *type = ct;
            *body = r + 5;
            *len = n;
            return true;
        }
        if (n < 17)
            return fail(t, "short encrypted record");
        uint8_t iv[12];
        nonce(&t->rd, iv);
        if (!gcm_open(&t->rd.gcm, iv, r, 5, r + 5, n - 16, r + 5 + n - 16, r + 5))
            return fail(t, "record authentication failed");
        t->rd.seq++;
        size_t m = n - 16;
        while (m && r[5 + m - 1] == 0)
            m--;
        if (!m)
            return fail(t, "record with no content type");
        *type = r[5 + m - 1];
        *body = r + 5;
        *len = m - 1;
        return true;
    }
}

static bool alert(struct tls *t, const uint8_t *b, size_t len)
{
    if (len >= 2 && b[1] == 0) {                       /* close_notify */
        t->eof = true;
        return fail(t, "connection closed");
    }
    snprintf(t->err, sizeof(t->err), "server sent TLS alert %d", len >= 2 ? b[1] : -1);
    return false;
}

/* Next complete handshake message (type, body) from the handshake stream. */
static bool next_handshake(struct tls *t, int *type, uint8_t **msg, size_t *msglen)
{
    for (;;) {
        if (t->hslen >= 4) {
            size_t n = (size_t)t->hsbuf[1] << 16 | (size_t)t->hsbuf[2] << 8 | t->hsbuf[3];
            if (n > 65536)
                return fail(t, "handshake message too large");
            if (t->hslen >= 4 + n) {
                *type = t->hsbuf[0];
                *msg = t->hsbuf;
                *msglen = 4 + n;
                return true;
            }
        }
        int ct;
        uint8_t *body;
        size_t len;
        if (!recv_record(t, &ct, &body, &len))
            return false;
        if (ct == CT_ALERT)
            return alert(t, body, len);
        if (ct != CT_HANDSHAKE)
            return fail(t, "unexpected record during the handshake");
        if (!append(&t->hsbuf, &t->hslen, &t->hscap, body, len))
            return fail(t, "out of memory");
    }
}

static void consume_handshake(struct tls *t, size_t n)
{
    memmove(t->hsbuf, t->hsbuf + n, t->hslen - n);
    t->hslen -= n;
}

static void transcript_hash(struct tls *t, uint8_t *out)
{
    hash_once(t->hash, t->tr, t->trlen, out);
}

/* ---------------- ClientHello ---------------- */

static void put16(uint8_t *p, size_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* The PSK binder: an HMAC over the ClientHello up to its binders (RFC 8446 4.2.11.2). */
static void psk_binder(const struct ticket *k, const uint8_t *ch, size_t trunc, uint8_t *out)
{
    size_t hs = hash_size(k->hash);
    uint8_t early[HASH_MAX], empty[HASH_MAX], bkey[HASH_MAX], fkey[HASH_MAX], th[HASH_MAX];
    hkdf_extract(k->hash, NULL, 0, k->psk, hs, early);
    hash_once(k->hash, "", 0, empty);
    hkdf_expand_label(k->hash, early, "res binder", empty, hs, bkey, hs);
    hkdf_expand_label(k->hash, bkey, "finished", NULL, 0, fkey, hs);
    hash_once(k->hash, ch, trunc, th);
    hmac(k->hash, fkey, hs, th, hs, out);
}

static size_t build_client_hello(uint8_t *m, const char *host, const uint8_t pub[32], const uint8_t pub256[65],
                                 const uint8_t random[32], const uint8_t sid[32], const struct ticket *k)
{
    size_t n = 4;
    m[n++] = 3;
    m[n++] = 3;                                        /* legacy_version TLS 1.2 */
    memcpy(m + n, random, 32);
    n += 32;
    m[n++] = 32;
    memcpy(m + n, sid, 32);
    n += 32;
    static const uint8_t suites[] = { 0, 4, 0x13, 0x01, 0x13, 0x02 };
    static const uint8_t suites384[] = { 0, 4, 0x13, 0x02, 0x13, 0x01 };   /* resuming a SHA-384 session */
    memcpy(m + n, k && k->suite == 0x1302 ? suites384 : suites, sizeof(suites));
    n += sizeof(suites);
    m[n++] = 1;
    m[n++] = 0;                                        /* null compression */
    size_t extlen_at = n;
    n += 2;
    /* server_name */
    size_t hl = strlen(host);
    put16(m + n, 0);
    put16(m + n + 2, hl + 5);
    put16(m + n + 4, hl + 3);
    m[n + 6] = 0;
    put16(m + n + 7, hl);
    memcpy(m + n + 9, host, hl);
    n += 9 + hl;
    /* supported_groups: x25519, secp256r1 */
    static const uint8_t groups[] = { 0, 10, 0, 6, 0, 4, 0, 0x1d, 0, 23 };
    memcpy(m + n, groups, sizeof(groups));
    n += sizeof(groups);
    /* signature_algorithms: ecdsa_secp256r1_sha256, ecdsa_secp384r1_sha384,
       rsa_pss_rsae_sha256/384/512, rsa_pkcs1_sha256/384/512 */
    static const uint8_t sigalgs[] = { 0, 13, 0, 18, 0, 16, 4, 3, 5, 3, 8, 4, 8, 5, 8, 6, 4, 1, 5, 1, 6, 1 };
    memcpy(m + n, sigalgs, sizeof(sigalgs));
    n += sizeof(sigalgs);
    /* supported_versions: TLS 1.3 */
    static const uint8_t versions[] = { 0, 43, 0, 3, 2, 3, 4 };
    memcpy(m + n, versions, sizeof(versions));
    n += sizeof(versions);
    /* ALPN: http/1.1 */
    static const uint8_t alpn[] = { 0, 16, 0, 11, 0, 9, 8, 'h', 't', 't', 'p', '/', '1', '.', '1' };
    memcpy(m + n, alpn, sizeof(alpn));
    n += sizeof(alpn);
    /* key_share: x25519 and secp256r1 */
    put16(m + n, 51);
    put16(m + n + 2, 2 + 36 + 69);
    put16(m + n + 4, 36 + 69);
    put16(m + n + 6, 0x1d);
    put16(m + n + 8, 32);
    memcpy(m + n + 10, pub, 32);
    put16(m + n + 42, 23);
    put16(m + n + 44, 65);
    memcpy(m + n + 46, pub256, 65);
    n += 4 + 2 + 36 + 69;
    /* psk_key_exchange_modes: psk_dhe_ke (without it servers send no tickets) */
    static const uint8_t modes[] = { 0, 45, 0, 2, 1, 1 };
    memcpy(m + n, modes, sizeof(modes));
    n += sizeof(modes);
    size_t binder_at = 0, hs = 0;
    if (k) {
        /* pre_shared_key (must be last): one identity and its binder */
        hs = hash_size(k->hash);
        uint32_t age = (uint32_t)(tls_ms() - k->received) + k->age_add;
        put16(m + n, 41);
        put16(m + n + 2, 2 + 2 + k->len + 4 + 2 + 1 + hs);
        put16(m + n + 4, 2 + k->len + 4);
        put16(m + n + 6, k->len);
        memcpy(m + n + 8, k->data, k->len);
        n += 8 + k->len;
        m[n] = (uint8_t)(age >> 24);
        m[n + 1] = (uint8_t)(age >> 16);
        m[n + 2] = (uint8_t)(age >> 8);
        m[n + 3] = (uint8_t)age;
        n += 4;
        binder_at = n;
        put16(m + n, 1 + hs);
        m[n + 2] = (uint8_t)hs;
        n += 3 + hs;
    }
    put16(m + extlen_at, n - extlen_at - 2);
    m[0] = HS_CLIENT_HELLO;
    m[1] = 0;
    put16(m + 2, n - 4);
    if (k)
        psk_binder(k, m, binder_at, m + binder_at + 3);   /* lengths above are final: the binder covers them */
    return n;
}

/* ---------------- handshake ---------------- */

static bool parse_server_hello(struct tls *t, const uint8_t *m, size_t len, const uint8_t priv[32],
                               const uint8_t priv256[32], uint8_t shared[32], bool offered_psk)
{
    static const uint8_t hrr[32] = { 0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C,
                                     0x02, 0x1E, 0x65, 0xB8, 0x91, 0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB,
                                     0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C };
    const uint8_t *p = m + 4, *end = m + len;
    if (end - p < 2 + 32 + 1)
        return fail(t, "malformed ServerHello");
    p += 2;
    if (memcmp(p, hrr, 32) == 0)
        return fail(t, "server requested a different key exchange (HelloRetryRequest)");
    p += 32;
    size_t sidlen = *p++;
    if ((size_t)(end - p) < sidlen + 3 + 2)
        return fail(t, "malformed ServerHello");
    p += sidlen;
    t->suite = (uint16_t)(p[0] << 8 | p[1]);
    p += 3;                                            /* suite + compression */
    size_t extlen = (size_t)p[0] << 8 | p[1];
    p += 2;
    if ((size_t)(end - p) < extlen)
        return fail(t, "malformed ServerHello");
    bool v13 = false, have_share = false;
    const uint8_t *e = p, *eend = p + extlen;
    while (eend - e >= 4) {
        int type = e[0] << 8 | e[1];
        size_t l = (size_t)e[2] << 8 | e[3];
        e += 4;
        if ((size_t)(eend - e) < l)
            return fail(t, "malformed ServerHello extension");
        if (type == 43 && l == 2 && e[0] == 3 && e[1] == 4)
            v13 = true;
        if (type == 51 && l == 36 && e[0] == 0 && e[1] == 0x1d && e[2] == 0 && e[3] == 32) {
            x25519(shared, priv, e + 4);
            have_share = true;
        }
        if (type == 51 && l == 69 && e[0] == 0 && e[1] == 23 && e[2] == 0 && e[3] == 65) {
            if (!p256_shared(shared, priv256, e + 4))
                return fail(t, "invalid P-256 key share");
            have_share = true;
        }
        if (type == 41) {
            if (!offered_psk || l != 2 || e[0] || e[1])
                return fail(t, "server selected a pre-shared key we did not offer");
            t->resumed = true;
        }
        e += l;
    }
    if (!v13)
        return fail(t, "server does not support TLS 1.3");
    if (!have_share)
        return fail(t, "server did not send a usable key share");
    if (t->suite == 0x1301) {
        t->hash = HASH_SHA256;
        t->keylen = 16;
    } else if (t->suite == 0x1302) {
        t->hash = HASH_SHA384;
        t->keylen = 32;
    } else {
        return fail(t, "server chose an unsupported cipher suite");
    }
    t->hs = hash_size(t->hash);
    uint8_t zero[32] = { 0 };
    for (int i = 0; i < 32; i++)
        zero[0] |= shared[i];
    if (!zero[0])
        return fail(t, "invalid x25519 share");
    return true;
}

static bool parse_certificates(struct tls *t, const uint8_t *m, size_t len, struct x509 *certs, int *ncerts)
{
    const uint8_t *p = m + 4, *end = m + len;
    if (end - p < 4)
        return fail(t, "malformed Certificate");
    size_t ctx = *p++;
    p += ctx;
    if (end - p < 3)
        return fail(t, "malformed Certificate");
    size_t total = (size_t)p[0] << 16 | (size_t)p[1] << 8 | p[2];
    p += 3;
    if ((size_t)(end - p) < total)
        return fail(t, "malformed Certificate");
    *ncerts = 0;
    while (end - p >= 5 && *ncerts < MAX_CERTS) {
        size_t cl = (size_t)p[0] << 16 | (size_t)p[1] << 8 | p[2];
        p += 3;
        if ((size_t)(end - p) < cl + 2)
            return fail(t, "malformed Certificate entry");
        if (!x509_parse(&certs[*ncerts], p, cl))
            return fail(t, "cannot parse a server certificate");
        (*ncerts)++;
        p += cl;
        size_t el = (size_t)p[0] << 8 | p[1];
        p += 2 + el;
    }
    return *ncerts > 0 || fail(t, "server sent no certificate");
}

static bool verify_certificate_verify(struct tls *t, const uint8_t *m, size_t len, const struct x509 *leaf,
                                      const uint8_t *th)
{
    if (len < 8)
        return fail(t, "malformed CertificateVerify");
    int alg = m[4] << 8 | m[5];
    size_t sl = (size_t)m[6] << 8 | m[7];
    if (len < 8 + sl)
        return fail(t, "malformed CertificateVerify");
    enum hash_alg h;
    int curve = -1;
    if (alg == 0x0403)
        h = HASH_SHA256, curve = ECDSA_P256;
    else if (alg == 0x0503)
        h = HASH_SHA384, curve = ECDSA_P384;
    else if (alg == 0x0804)
        h = HASH_SHA256;
    else if (alg == 0x0805)
        h = HASH_SHA384;
    else if (alg == 0x0806)
        h = HASH_SHA512;
    else
        return fail(t, "unsupported CertificateVerify algorithm");
    if (curve >= 0 ? !leaf->is_ec || leaf->ec_curve != curve : !leaf->is_rsa)
        return fail(t, "CertificateVerify algorithm does not match the server key");
    uint8_t content[64 + 34 + HASH_MAX], digest[HASH_MAX];
    memset(content, 0x20, 64);
    memcpy(content + 64, "TLS 1.3, server CertificateVerify", 34);   /* includes the NUL separator */
    memcpy(content + 98, th, t->hs);
    hash_once(h, content, 98 + t->hs, digest);
    if (curve >= 0 ? !ecdsa_verify(curve, leaf->ec_point.p, leaf->ec_point.len, digest, hash_size(h), m + 8, sl)
                   : !rsa_verify_pss(&leaf->key, h, digest, m + 8, sl))
        return fail(t, "server signature (CertificateVerify) is invalid");
    return true;
}

static bool handshake(struct tls *t, const char *host)
{
    uint8_t priv[32], pub[32], priv256[32], pub256[65], rnd[32], sid[32];
    if (!tls_random(priv, 32) || !tls_random(rnd, 32) || !tls_random(sid, 32))
        return fail(t, "no random number source (/dev/urandom)");
    do {
        if (!tls_random(priv256, 32))
            return fail(t, "no random number source (/dev/urandom)");
    } while (!p256_valid_scalar(priv256));
    x25519_base(pub, priv);
    if (!p256_public(pub256, priv256))
        return fail(t, "P-256 key generation failed");
    snprintf(t->host, sizeof(t->host), "%s", host);
    static struct ticket tk;
    bool offer = take_ticket(host, &tk);
    static uint8_t ch[1024 + TICKET_MAX];
    size_t chlen = build_client_hello(ch, host, pub, pub256, rnd, sid, offer ? &tk : NULL);
    if (!append(&t->tr, &t->trlen, &t->trcap, ch, chlen))
        return fail(t, "out of memory");
    if (!send_record(t, CT_HANDSHAKE, ch, chlen))
        return false;

    int type;
    uint8_t *msg;
    size_t len;
    if (!next_handshake(t, &type, &msg, &len))
        return false;
    if (type != HS_SERVER_HELLO)
        return fail(t, "expected ServerHello");
    uint8_t shared[32];
    if (!parse_server_hello(t, msg, len, priv, priv256, shared, offer))
        return false;
    if (t->resumed && (t->hash != tk.hash))
        return fail(t, "server resumed with a cipher suite of another hash");
    memset(priv, 0, sizeof(priv));
    memset(priv256, 0, sizeof(priv256));
    append(&t->tr, &t->trlen, &t->trcap, msg, len);
    consume_handshake(t, len);

    /* key schedule */
    size_t hs = t->hs;
    uint8_t early[HASH_MAX], derived[HASH_MAX], hsecret[HASH_MAX], empty[HASH_MAX], th[HASH_MAX];
    uint8_t zeros[HASH_MAX] = { 0 };
    hkdf_extract(t->hash, NULL, 0, t->resumed ? tk.psk : zeros, hs, early);
    memset(&tk, 0, sizeof(tk));
    hash_once(t->hash, "", 0, empty);
    hkdf_expand_label(t->hash, early, "derived", empty, hs, derived, hs);
    hkdf_extract(t->hash, derived, hs, shared, 32, hsecret);
    transcript_hash(t, th);
    uint8_t c_hs[HASH_MAX], s_hs[HASH_MAX];
    hkdf_expand_label(t->hash, hsecret, "c hs traffic", th, hs, c_hs, hs);
    hkdf_expand_label(t->hash, hsecret, "s hs traffic", th, hs, s_hs, hs);
    set_keys(t, &t->rd, s_hs);

    static struct x509 certs[MAX_CERTS];
    static uint8_t *certmsg;
    int ncerts = 0;
    bool got_cert = false, got_cv = false;
    for (;;) {
        if (!next_handshake(t, &type, &msg, &len))
            return false;
        if (type == HS_ENCRYPTED_EXTENSIONS) {
            /* nothing we need (ALPN, if any, is http/1.1) */
        } else if (type == HS_CERTIFICATE_REQUEST) {
            return fail(t, "server requires a client certificate");
        } else if ((type == HS_CERTIFICATE || type == HS_CERTIFICATE_VERIFY) && t->resumed) {
            return fail(t, "server sent a certificate in a resumed handshake");
        } else if (type == HS_CERTIFICATE) {
            /* the parsed certificates point into the message: keep a private copy */
            free(certmsg);
            if (!(certmsg = malloc(len)))
                return fail(t, "out of memory");
            memcpy(certmsg, msg, len);
            if (!parse_certificates(t, certmsg, len, certs, &ncerts))
                return false;
            if (x509_root_count() == 0) {
                x509_load_roots(TLS_ROOTS_PATH);
                x509_load_roots(TLS_LOCAL_ROOTS_PATH);
            }
            if (x509_root_count() == 0)
                return fail(t, "no trusted root certificates (" TLS_ROOTS_PATH ")");
            if (!x509_verify_chain(certs, ncerts, host, tls_now(), t->err, sizeof(t->err)))
                return false;
            got_cert = true;
        } else if (type == HS_CERTIFICATE_VERIFY) {
            if (!got_cert)
                return fail(t, "CertificateVerify before Certificate");
            transcript_hash(t, th);
            if (!verify_certificate_verify(t, msg, len, &certs[0], th))
                return false;
            got_cv = true;
        } else if (type == HS_FINISHED) {
            if (!got_cv && !t->resumed)
                return fail(t, "server did not authenticate");
            uint8_t fkey[HASH_MAX], expect[HASH_MAX];
            hkdf_expand_label(t->hash, s_hs, "finished", NULL, 0, fkey, hs);
            transcript_hash(t, th);
            hmac(t->hash, fkey, hs, th, hs, expect);
            if (len != 4 + hs || !ct_equal(expect, msg + 4, hs))
                return fail(t, "server Finished is invalid");
            append(&t->tr, &t->trlen, &t->trcap, msg, len);
            consume_handshake(t, len);
            break;
        } else {
            return fail(t, "unexpected handshake message");
        }
        append(&t->tr, &t->trlen, &t->trcap, msg, len);
        consume_handshake(t, len);
    }

    /* application traffic secrets (transcript through server Finished) */
    uint8_t master[HASH_MAX], c_ap[HASH_MAX], s_ap[HASH_MAX];
    hkdf_expand_label(t->hash, hsecret, "derived", empty, hs, derived, hs);
    hkdf_extract(t->hash, derived, hs, zeros, hs, master);
    transcript_hash(t, th);
    hkdf_expand_label(t->hash, master, "c ap traffic", th, hs, c_ap, hs);
    hkdf_expand_label(t->hash, master, "s ap traffic", th, hs, s_ap, hs);

    /* ChangeCipherSpec (middlebox compatibility), then our Finished */
    static const uint8_t ccs[] = { CT_CCS, 3, 3, 0, 1, 1 };
    if (!write_full(t->fd, ccs, sizeof(ccs)))
        return fail(t, "connection lost while sending");
    set_keys(t, &t->wr, c_hs);
    uint8_t fkey[HASH_MAX], fin[4 + HASH_MAX];
    hkdf_expand_label(t->hash, c_hs, "finished", NULL, 0, fkey, hs);
    fin[0] = HS_FINISHED;
    fin[1] = 0;
    fin[2] = 0;
    fin[3] = (uint8_t)hs;
    hmac(t->hash, fkey, hs, th, hs, fin + 4);
    if (!send_record(t, CT_HANDSHAKE, fin, 4 + hs))
        return false;
    /* resumption_master_secret (transcript through the client Finished), for tickets */
    append(&t->tr, &t->trlen, &t->trcap, fin, 4 + hs);
    transcript_hash(t, th);
    hkdf_expand_label(t->hash, master, "res master", th, hs, t->res_master, hs);
    set_keys(t, &t->wr, c_ap);
    set_keys(t, &t->rd, s_ap);
    free(certmsg);
    certmsg = NULL;
    memset(hsecret, 0, sizeof(hsecret));
    memset(master, 0, sizeof(master));
    free(t->tr);
    t->tr = NULL;
    t->trlen = t->trcap = 0;
    return true;
}

struct tls *tls_connect(int fd, const char *host, char *err, size_t errlen)
{
    struct tls *t = calloc(1, sizeof(*t));
    if (!t) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    t->fd = fd;
    if (!handshake(t, host)) {
        snprintf(err, errlen, "TLS: %s", t->err);
        free(t->hsbuf);
        free(t->tr);
        free(t);
        return NULL;
    }
    return t;
}

static void key_update(struct tls *t, struct dir *d)
{
    uint8_t next[HASH_MAX];
    hkdf_expand_label(t->hash, d->secret, "traffic upd", NULL, 0, next, t->hs);
    set_keys(t, d, next);
}

/* NewSessionTicket: keep it for the next connection to this host. */
static void new_session_ticket(struct tls *t, const uint8_t *m, size_t n)
{
    if (n < 4 + 4 + 1)
        return;
    uint32_t life = (uint32_t)m[0] << 24 | (uint32_t)m[1] << 16 | (uint32_t)m[2] << 8 | m[3];
    size_t nl = m[8];
    if (n < 9 + nl + 2)
        return;
    const uint8_t *nonce = m + 9;
    size_t tl = (size_t)m[9 + nl] << 8 | m[10 + nl];
    if (n < 11 + nl + tl || !tl || tl > TICKET_MAX || !life)
        return;                                        /* malformed, too big for us, or not to be kept */
    static struct ticket k;
    memset(&k, 0, sizeof(k));
    snprintf(k.host, sizeof(k.host), "%s", t->host);
    k.hash = t->hash;
    k.suite = t->suite;
    k.age_add = (uint32_t)m[4] << 24 | (uint32_t)m[5] << 16 | (uint32_t)m[6] << 8 | m[7];
    hkdf_expand_label(t->hash, t->res_master, "resumption", nonce, nl, k.psk, t->hs);
    k.received = tls_ms();
    k.expires = k.received + (uint64_t)(life < TICKET_LIFE_MAX ? life : TICKET_LIFE_MAX) * 1000;
    k.len = tl;
    memcpy(k.data, m + 11 + nl, tl);
    save_ticket(&k);
    memset(&k, 0, sizeof(k));
}

/* Handle post-handshake messages. */
static bool post_handshake(struct tls *t)
{
    while (t->hslen >= 4) {
        size_t n = (size_t)t->hsbuf[1] << 16 | (size_t)t->hsbuf[2] << 8 | t->hsbuf[3];
        if (t->hslen < 4 + n)
            break;
        if (t->hsbuf[0] == HS_KEY_UPDATE && n == 1) {
            bool requested = t->hsbuf[4] == 1;
            key_update(t, &t->rd);
            if (requested) {
                uint8_t ku[5] = { HS_KEY_UPDATE, 0, 0, 1, 0 };
                if (!send_record(t, CT_HANDSHAKE, ku, 5))
                    return false;
                key_update(t, &t->wr);
            }
        } else if (t->hsbuf[0] == HS_NEW_SESSION_TICKET) {
            new_session_ticket(t, t->hsbuf + 4, n);
        }
        consume_handshake(t, 4 + n);                   /* anything else: ignored */
    }
    return true;
}

long tls_read(struct tls *t, void *buf, size_t n)
{
    while (!t->app_len) {
        if (t->eof)
            return 0;
        int ct;
        uint8_t *body;
        size_t len;
        if (!recv_record(t, &ct, &body, &len))
            return t->eof ? 0 : -1;
        if (ct == CT_APPDATA) {
            t->app = body;
            t->app_len = len;
        } else if (ct == CT_HANDSHAKE) {
            if (!append(&t->hsbuf, &t->hslen, &t->hscap, body, len) || !post_handshake(t))
                return -1;
        } else if (ct == CT_ALERT) {
            alert(t, body, len);
            return t->eof ? 0 : -1;
        }
    }
    size_t m = n < t->app_len ? n : t->app_len;
    memcpy(buf, t->app, m);
    t->app += m;
    t->app_len -= m;
    return (long)m;
}

long tls_write(struct tls *t, const void *buf, size_t n)
{
    if (!send_record(t, CT_APPDATA, buf, n))
        return -1;
    return (long)n;
}

void tls_close(struct tls *t)
{
    if (!t)
        return;
    static const uint8_t close_notify[2] = { 1, 0 };
    if (t->wr.on && !t->eof)
        send_record(t, CT_ALERT, close_notify, 2);
    free(t->hsbuf);
    free(t->tr);
    memset(t, 0, sizeof(*t));
    free(t);
}

const char *tls_error(struct tls *t)
{
    return t->err;
}

bool tls_resumed(struct tls *t)
{
    return t->resumed;
}

const char *tls_cipher_name(struct tls *t)
{
    return t->suite == 0x1302 ? "TLS_AES_256_GCM_SHA384" : "TLS_AES_128_GCM_SHA256";
}
